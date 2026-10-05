#include "analysis/typeflow.hpp"

#include "analysis/demangle.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <deque>
#include <format>
#include <set>

namespace decomp {

const TypeLayout* TypeView::find(std::string_view name) const {
    if (first_)
        if (const TypeLayout* t = first_->find(name)) return t;
    if (second_)
        if (const TypeLayout* t = second_->find(name)) return t;
    return nullptr;
}

std::optional<TypeCatalog::FieldRef> TypeView::field_ref(std::string_view type, u64 offset, bool innermost) const {
    if (first_ && first_->find(type)) return first_->field_ref(type, offset, innermost);
    if (second_ && second_->find(type)) return second_->field_ref(type, offset, innermost);
    return std::nullopt;
}

StackPoints stack_points(std::span<const x86::Instruction> instructions, bool x64) {
    StackPoints out;
    const i64 slot = x64 ? 8 : 4;
    const std::string spr = x64 ? "rsp" : "esp", fpr = x64 ? "rbp" : "ebp";
    i64 sp = 0;
    std::optional<i64> frame;
    for (const x86::Instruction& x : instructions) {
        out.sp.push_back(sp);
        out.frame.push_back(frame);
        // A straight-line approximation: good enough for naming slots.
        const auto& m = x.mnemonic;
        auto first_reg = [&](std::string_view r) { return !x.operands.empty() && x.operands[0].kind == x86::OperandKind::reg && x.operands[0].reg == r; };
        auto second_imm = [&]() -> std::optional<i64> {
            if (x.operands.size() == 2 && x.operands[1].kind == x86::OperandKind::imm) return x.operands[1].imm;
            return std::nullopt;
        };
        auto second_reg = [&](std::string_view r) { return x.operands.size() == 2 && x.operands[1].kind == x86::OperandKind::reg && x.operands[1].reg == r; };
        if (m == "push") sp -= x.operands.empty() ? slot : std::max<i64>(x.operands[0].size_bits / 8, 2);
        else if (m == "pop") sp += slot;
        else if (m == "sub" && first_reg(spr) && second_imm()) sp -= *second_imm();
        else if (m == "add" && first_reg(spr) && second_imm()) sp += *second_imm();
        else if (m == "mov" && first_reg(fpr) && second_reg(spr)) frame = sp;
        else if (m == "mov" && first_reg(spr) && second_reg(fpr) && frame) sp = *frame;
        else if (m == "leave" && frame) sp = *frame + slot;
    }
    return out;
}

namespace {

constexpr usize kMaxExpression = 80;

std::string family(std::string_view reg) { return x86::gpr_family(reg); }

// The registers a call may change: the caller-saved ones.
const std::vector<std::string>& caller_saved(bool x64) {
    static const std::vector<std::string> x86 = {"rax", "rcx", "rdx"};
    static const std::vector<std::string> x64_registers = {"rax", "rcx", "rdx", "r8", "r9", "r10", "r11"};
    return x64 ? x64_registers : x86;
}

// Instructions that write registers their operands do not show (rax, rcx, rdx, rsi, rdi).
bool implicit_writes(const x86::Instruction& x) {
    static const std::set<std::string, std::less<>> kMnemonics = {
        "mul", "div", "idiv", "cdq", "cqo", "cwd", "cbw", "cwde", "cdqe", "movsb", "movsw", "movsq", "stosb", "stosw", "stosd", "stosq",
        "lodsb", "lodsw", "lodsd", "lodsq", "scasb", "scasw", "scasd", "scasq", "cmpsb", "cmpsw", "cmpsq", "cpuid", "rdtsc", "rdtscp",
        "xlat", "lahf", "loop", "loope", "loopne", "cmpxchg", "cmpxchg8b", "cmpxchg16b", "syscall", "in", "insb", "insw", "insd", "rep",
    };
    if (x.mnemonic == "imul") return x.operands.size() == 1;
    // movsd and cmpsd are SSE moves and compares when they show xmm operands.
    if (x.mnemonic == "movsd" || x.mnemonic == "cmpsd")
        return std::ranges::none_of(x.operands, [](const x86::Operand& o) { return o.kind == x86::OperandKind::reg && o.reg.starts_with("xmm"); });
    return kMnemonics.contains(x.mnemonic) || x.prefix.starts_with("rep");
}

std::string member(const TypedValue& v, const std::string& path) { return v.address ? v.expression + "." + path : v.expression + "->" + path; }

// What a value derived from `v` is called: its expression, unless that grows too long.
std::string named(const std::string& expression, const std::string& type) {
    return expression.size() <= kMaxExpression ? expression : std::format("({}*)", type);
}

// The name of the virtual method in `slot` of the type's (primary) vtable: introduced by it or by the
// base at its start.
std::string virtual_name(const TypeView& types, std::string type, u64 slot) {
    for (int depth = 0; depth < 16 && !type.empty(); ++depth) {
        const TypeLayout* t = types.find(type);
        if (!t) return {};
        for (const VirtualMethod& v : t->virtuals)
            if (v.slot == slot) return v.name;
        std::string next;
        for (const BaseLayout& b : t->bases)
            if (!b.is_virtual && b.offset == 0) {
                next = b.name;
                break;
            }
        type = next;
    }
    return {};
}

// The base class whose subobject starts at `offset` of `type`, at any depth.
std::string base_at(const TypeView& types, const std::string& type, u64 offset, int depth = 0) {
    const TypeLayout* t = types.find(type);
    if (!t || depth > 16) return {};
    for (const BaseLayout& b : t->bases) {
        if (b.is_virtual || offset < b.offset) continue;
        if (offset == b.offset && b.offset != 0) return b.name;
        if (const std::string inner = base_at(types, b.name, offset - b.offset, depth + 1); !inner.empty()) return inner;
    }
    return {};
}

std::optional<i64> stack_slot(const x86::MemOperand& m, i64 sp, std::optional<i64> frame) {
    if (!m.index.empty() || !m.segment.empty()) return std::nullopt;
    const std::string base = family(m.base);
    if (base == "rsp") return sp + m.disp;
    if (base == "rbp" && frame) return *frame + m.disp;
    return std::nullopt;
}

// The typed pointer a memory operand goes through: its base register's value (not the frame pointer's).
const TypedValue* through(const TypeState& s, const x86::MemOperand& m, std::optional<i64> frame) {
    if (m.base.empty() || (!m.segment.empty() && m.segment != "ds")) return nullptr;
    const std::string base = family(m.base);
    if (base == "rsp" || (base == "rbp" && frame) || base == "rip") return nullptr;
    const auto it = s.registers.find(base);
    return it == s.registers.end() ? nullptr : &it->second;
}

class Flow {
public:
    Flow(const TypeView& types, bool x64) : types_(types), x64_(x64), pointer_bits_(x64 ? 64 : 32), pointer_size_(x64 ? 8 : 4) {}

    // Runs `x` on the state; with `notes`, says what its memory operands reach (and with `accesses`,
    // records them as instruction `index`'s).
    void step(TypeState& s, const x86::Instruction& x, i64 sp, std::optional<i64> frame, std::vector<std::string>* notes,
              std::vector<TypedAccess>* accesses = nullptr, usize index = 0) const {
        if (notes)
            for (const x86::Operand& op : x.operands)
                if (op.kind == x86::OperandKind::mem)
                    if (const TypedValue* v = through(s, op.mem, frame)) {
                        const usize before = accesses ? accesses->size() : 0;
                        describe(*v, op.mem, x.mnemonic == "lea", *notes, accesses);
                        if (accesses)
                            for (usize i = before; i < accesses->size(); ++i) (*accesses)[i].instruction = index;
                    }

        const auto& ops = x.operands;
        const auto full_reg = [&](usize i) { return i < ops.size() && ops[i].kind == x86::OperandKind::reg && ops[i].size_bits == pointer_bits_; };
        const auto reg_value = [&](const std::string& reg) -> std::optional<TypedValue> {
            const auto it = s.registers.find(family(reg));
            return it == s.registers.end() ? std::nullopt : std::optional(it->second);
        };
        const auto set_reg = [&](const std::string& reg, std::optional<TypedValue> value) {
            if (value) s.registers[family(reg)] = std::move(*value);
            else s.registers.erase(family(reg));
        };
        const auto set_slot = [&](i64 slot, std::optional<TypedValue> value) {
            if (value) s.stack[slot] = std::move(*value);
            else s.stack.erase(slot);
        };

        bool handled = false;
        if (x.mnemonic == "mov" && ops.size() == 2) {
            if (full_reg(0) && full_reg(1)) {
                set_reg(ops[0].reg, reg_value(ops[1].reg));
                handled = true;
            } else if (full_reg(0) && ops[1].kind == x86::OperandKind::mem) {
                std::optional<TypedValue> loaded;
                if (const auto slot = stack_slot(ops[1].mem, sp, frame)) {
                    if (const auto it = s.stack.find(*slot); it != s.stack.end()) loaded = it->second;
                } else if (const TypedValue* v = through(s, ops[1].mem, frame); v && ops[1].mem.index.empty()) {
                    loaded = load(*v, ops[1].mem.disp);
                }
                set_reg(ops[0].reg, std::move(loaded));
                handled = true;
            } else if (ops[0].kind == x86::OperandKind::mem) {
                if (const auto slot = stack_slot(ops[0].mem, sp, frame)) set_slot(*slot, full_reg(1) ? reg_value(ops[1].reg) : std::nullopt);
                handled = true;
            }
        } else if (x.mnemonic == "lea" && ops.size() == 2 && full_reg(0) && ops[1].kind == x86::OperandKind::mem) {
            std::optional<TypedValue> address;
            if (const TypedValue* v = through(s, ops[1].mem, frame); v && ops[1].mem.index.empty() && !v->vtable) address = offset(*v, ops[1].mem.disp);
            set_reg(ops[0].reg, std::move(address));
            handled = true;
        } else if (x.mnemonic == "xchg" && ops.size() == 2 && full_reg(0) && full_reg(1)) {
            const auto a = reg_value(ops[0].reg), b = reg_value(ops[1].reg);
            set_reg(ops[0].reg, b);
            set_reg(ops[1].reg, a);
            handled = true;
        } else if (x.mnemonic == "push" && ops.size() == 1) {
            set_slot(sp - static_cast<i64>(pointer_size_), full_reg(0) ? reg_value(ops[0].reg) : std::nullopt);
            handled = true;
        } else if (x.mnemonic == "pop" && ops.size() == 1 && full_reg(0)) {
            const auto it = s.stack.find(sp);
            set_reg(ops[0].reg, it == s.stack.end() ? std::nullopt : std::optional(it->second));
            handled = true;
        }
        if (!handled)
            for (const x86::Operand& op : ops) {
                if (!op.write) continue;
                if (op.kind == x86::OperandKind::reg) s.registers.erase(family(op.reg));
                else if (op.kind == x86::OperandKind::mem)
                    if (const auto slot = stack_slot(op.mem, sp, frame)) s.stack.erase(*slot);
            }
        if (x.flow == x86::Flow::call || x.flow == x86::Flow::indirect_call)
            for (const std::string& reg : caller_saved(x64_)) s.registers.erase(reg);
        if (implicit_writes(x))
            for (const char* reg : {"rax", "rcx", "rdx", "rsi", "rdi"}) s.registers.erase(reg);
    }

private:
    // What `mov reg, [v + disp]` loads: a pointer field's target, or the vtable at a vfptr.
    std::optional<TypedValue> load(const TypedValue& v, i64 disp) const {
        if (v.vtable || disp < 0) return std::nullopt;
        const auto ref = types_.field_ref(v.type, static_cast<u64>(disp));
        if (!ref || !ref->exact) return std::nullopt;
        if (!ref->field && ref->path.ends_with("__vfptr")) {
            const auto scope = ref->path.rfind("::");
            return TypedValue{scope == std::string::npos ? v.type : ref->path.substr(0, scope), v.expression, v.address, true};
        }
        const FieldLayout* f = ref->field;
        if (!f || f->pointee.empty() || f->is_bitfield() || !f->dimensions.empty() || f->size != pointer_size_ || !types_.find(f->pointee))
            return std::nullopt;
        return TypedValue{f->pointee, named(member(v, ref->path), f->pointee)};
    }

    // What `lea reg, [v + disp]` makes: the address of an embedded struct, or of a base subobject.
    std::optional<TypedValue> offset(const TypedValue& v, i64 disp) const {
        if (disp == 0) return v;
        if (disp < 0) return std::nullopt;
        if (const std::string base = base_at(types_, v.type, static_cast<u64>(disp)); !base.empty()) return TypedValue{base, v.expression, v.address};
        const auto ref = types_.field_ref(v.type, static_cast<u64>(disp), false);
        if (!ref || !ref->exact || !ref->field) return std::nullopt;
        const FieldLayout& f = *ref->field;
        if (f.udt.empty() || f.is_bitfield() || !types_.find(f.udt)) return std::nullopt;
        // An array's element (items[2]) or an embedded struct.
        return TypedValue{f.udt, named(member(v, ref->path), f.udt), true};
    }

    // `address`: the operand is lea's, an address rather than an access.
    void describe(const TypedValue& v, const x86::MemOperand& m, bool address, std::vector<std::string>& notes,
                  std::vector<TypedAccess>* accesses) const {
        if (v.vtable) {
            if (!m.index.empty() || m.disp < 0 || m.disp % static_cast<i64>(pointer_size_) != 0) return;
            const u64 slot = static_cast<u64>(m.disp) / pointer_size_;
            const std::string method = virtual_name(types_, v.type, slot);
            notes.push_back(method.empty() ? std::format("{} vtable slot {}", v.type, slot)
                                           : std::format("{}->{}() (virtual, slot {})", v.expression, method, slot));
            if (accesses) accesses->push_back({0, v.type, method.empty() ? std::format("vtable slot {}", slot) : method + "()", false});
            return;
        }
        if (m.disp < 0 || (address && m.disp == 0 && m.index.empty())) return;
        const auto ref = types_.field_ref(v.type, static_cast<u64>(m.disp), !address);
        if (!ref) return;
        std::string path = ref->path;
        if (!m.index.empty()) {
            // [base+index*scale+disp] walking an array field: its element by the index register.
            const FieldLayout* f = ref->field;
            if (!f || f->dimensions.size() != 1 || f->dimensions[0] == 0 || f->size / f->dimensions[0] != m.scale) return;
            const auto open = path.find(f->name + "[");
            if (open == std::string::npos) return;
            const auto close = path.find(']', open);
            path = path.substr(0, open + f->name.size()) + "[" + m.index + "]" + path.substr(close + 1);
        }
        std::string text = (address ? "&" : "") + member(v, path);
        if (!ref->exact) text += std::format(" (+{})", ref->rest);
        notes.push_back(std::move(text));
        if (accesses) accesses->push_back({0, v.type, ref->exact ? path : std::format("{} (+{})", path, ref->rest), address});
    }

    const TypeView& types_;
    bool x64_;
    u16 pointer_bits_;
    u64 pointer_size_;
};

TypeState meet(const TypeState& a, const TypeState& b) {
    TypeState out;
    for (const auto& [reg, value] : a.registers)
        if (const auto it = b.registers.find(reg); it != b.registers.end() && it->second == value) out.registers.emplace(reg, value);
    for (const auto& [slot, value] : a.stack)
        if (const auto it = b.stack.find(slot); it != b.stack.end() && it->second == value) out.stack.emplace(slot, value);
    return out;
}

// The class of a member function with `this`, from its decorated name ("?Hit@Player@@QAEXH@Z" ->
// Player): after the name comes an access code, and these have `this`.
std::optional<std::string> member_class(std::string_view decorated) {
    if (!decorated.starts_with('?')) return std::nullopt;
    const auto at = decorated.find("@@");
    if (at == std::string_view::npos || at + 2 >= decorated.size()) return std::nullopt;
    if (std::string_view("ABEFIJMNQRUV").find(decorated[at + 2]) == std::string_view::npos) return std::nullopt;
    const std::string qualified = qualified_name(decorated);
    const auto scope = qualified.rfind("::");
    if (scope == std::string::npos) return std::nullopt;
    return qualified.substr(0, scope);
}

bool floating(const codeview::TypeStream& types, codeview::TypeIndex t) {
    const std::string name = types.name_of(t);
    return name == "float" || name == "double" || name == "long double";
}

} // namespace

EntryTypes entry_types(const Program& program, u64 function, const TypeView& types) {
    EntryTypes out;
    if (types.empty()) return out;
    const bool x64 = program.arch() == Arch::x64;
    const auto known = [&](const std::string& type) { return !type.empty() && types.find(type); };
    const auto in_register = [&](std::string_view reg, const std::string& type, const std::string& expression) {
        if (!known(type)) return;
        out.state.registers[family(reg)] = TypedValue{type, expression};
        out.description.push_back(std::format("{} = {}* ({})", expression, type, reg));
    };
    const auto on_stack = [&](i64 slot, const std::string& type, const std::string& expression) {
        if (!known(type)) return;
        out.state.stack[slot] = TypedValue{type, expression};
        out.description.push_back(std::format("{} = {}*", expression, type));
    };

    const ProgramTypes& pdb = program.pdb_types();
    if (const auto it = pdb.function_types.find(function); it != pdb.function_types.end())
        if (const auto f = pdb.stream.function(it->second)) {
            const codeview::TypeStream& s = pdb.stream;
            const std::string self = f->this_type != 0 ? s.pointee_udt(f->this_type) : std::string();
            if (x64) {
                // rcx, rdx, r8, r9 by position (`this` first), then the stack after the home space.
                static constexpr const char* kRegisters[] = {"rcx", "rdx", "r8", "r9"};
                usize position = 0;
                if (f->this_type != 0) in_register(kRegisters[position++], self, "this");
                for (const codeview::TypeIndex t : f->parameters) {
                    const std::string name = std::format("arg_{:x}", 8 * position);
                    if (position < 4) in_register(kRegisters[position], s.pointee_udt(t), name);
                    else on_stack(static_cast<i64>(8 + 8 * position), s.pointee_udt(t), name);
                    ++position;
                }
                return out;
            }
            // x86: __thiscall passes `this` in ecx, __fastcall the first two in ecx and edx, the rest the stack.
            i64 slot = 4;
            const bool fastcall = f->calling_convention == 0x04;
            std::vector<const char*> registers;
            if (fastcall) registers = {"ecx", "edx"};
            if (f->calling_convention == 0x0b) registers = {"ecx"};
            usize next = 0;
            if (f->this_type != 0) {
                if (next < registers.size()) in_register(registers[next++], self, "this");
                else {
                    on_stack(slot, self, "this");
                    slot += 4;
                }
            }
            if (!fastcall) next = registers.size();
            for (const codeview::TypeIndex t : f->parameters) {
                if (t == 0) break;  // ...
                const u64 size = s.size_of(t, 4);
                if (next < registers.size() && size > 0 && size <= 4 && !floating(s, t)) {
                    in_register(registers[next], s.pointee_udt(t), std::format("{}_arg", registers[next]));
                    ++next;
                    continue;
                }
                on_stack(slot, s.pointee_udt(t), std::format("arg_{:x}", slot - 4));
                slot += static_cast<i64>((std::max<u64>(size, 4) + 3) / 4 * 4);
            }
            return out;
        }

    // Without a PDB type: a member function's decorated name names its class.
    const Symbol* symbol = program.symbols().at(function);
    if (!symbol) return out;
    const auto cls = member_class(symbol->name);
    if (!cls) return out;
    if (x64) {
        in_register("rcx", *cls, "this");
    } else {
        const std::string signature = demangle(symbol->name).value_or("");
        if (signature.find("__thiscall") != std::string::npos) in_register("ecx", *cls, "this");
        else if (signature.find("__fastcall") != std::string::npos) in_register("ecx", *cls, "this");
        else on_stack(4, *cls, "this");
    }
    return out;
}

std::vector<std::vector<std::string>> typed_operand_notes(const Program& program, std::span<const x86::Instruction> instructions, const Cfg& cfg,
                                                          const EntryTypes& entry, const TypeView& types, std::vector<TypedAccess>* accesses) {
    std::vector<std::vector<std::string>> notes(instructions.size());
    if (instructions.empty() || cfg.blocks.empty() || (entry.state.registers.empty() && entry.state.stack.empty())) return notes;
    const bool x64 = program.arch() == Arch::x64;
    const StackPoints points = stack_points(instructions, x64);
    const Flow flow(types, x64);
    const usize n = cfg.blocks.size();
    std::vector<std::optional<TypeState>> in(n), out(n);
    const usize start = cfg.block_of[0];
    std::deque<usize> work = {start};
    std::vector<bool> queued(n, false);
    queued[start] = true;
    // Values only drop out as states meet, so this settles; the bound guards against surprises.
    for (usize steps = 0; !work.empty() && steps < n * 64; ++steps) {
        const usize b = work.front();
        work.pop_front();
        queued[b] = false;
        std::optional<TypeState> state;
        if (b == start) state = entry.state;
        for (const usize p : cfg.blocks[b].predecessors)
            if (out[p]) state = state ? meet(*state, *out[p]) : *out[p];
        if (!state) continue;
        if (in[b] && *in[b] == *state && out[b]) continue;
        in[b] = state;
        TypeState s = *state;
        for (usize i = cfg.blocks[b].first; i <= cfg.blocks[b].last; ++i) flow.step(s, instructions[i], points.sp[i], points.frame[i], nullptr);
        if (out[b] && *out[b] == s) continue;
        out[b] = std::move(s);
        for (const usize next : cfg.blocks[b].successors)
            if (!queued[next]) {
                queued[next] = true;
                work.push_back(next);
            }
    }
    for (usize b = 0; b < n; ++b) {
        if (!in[b]) continue;
        TypeState s = *in[b];
        for (usize i = cfg.blocks[b].first; i <= cfg.blocks[b].last; ++i)
            flow.step(s, instructions[i], points.sp[i], points.frame[i], &notes[i], accesses, i);
    }
    return notes;
}

std::vector<FieldUse> find_field_uses(const Program& program, const TypeView& types, std::string_view type, const std::function<bool()>& cancelled) {
    std::vector<FieldUse> out;
    if (types.empty() || !types.find(type)) return out;
    for (const Symbol* f : program.symbols().functions()) {
        if (cancelled && cancelled()) break;
        const EntryTypes entry = entry_types(program, f->va, types);
        if (entry.state.registers.empty() && entry.state.stack.empty()) continue;
        auto extent = program.function_extent(f->va);
        if (!extent) continue;
        auto instructions = program.function_instructions(*extent);
        if (!instructions || instructions->empty()) continue;
        const Cfg cfg = build_cfg(*instructions, extent->jump_tables);
        std::vector<TypedAccess> accesses;
        typed_operand_notes(program, *instructions, cfg, entry, types, &accesses);
        for (const TypedAccess& a : accesses)
            if (a.type == type) out.push_back({f->va, (*instructions)[a.instruction].address, a.path, a.address});
    }
    return out;
}

} // namespace decomp
