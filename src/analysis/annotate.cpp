#include "analysis/annotate.hpp"

#include "analysis/demangle.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <map>
#include <set>

namespace decomp {
namespace {

std::string float_text(const Program& program, u64 va, std::string_view symbol_name) {
    bool is_double = symbol_name.size() == std::string_view("__real@").size() + 16;
    if (is_double) {
        if (auto v = program.image().read<u64>(va)) return std::format("{}", std::bit_cast<double>(*v));
    } else if (auto v = program.image().read<u32>(va)) {
        return std::format("{}f", std::bit_cast<float>(*v));
    }
    return {};
}

bool printable_string(const std::string& s) {
    if (s.size() < 2) return false;
    return std::ranges::all_of(s, [](unsigned char c) { return c >= 0x20 || c == '\n' || c == '\r' || c == '\t'; });
}

std::string short_name(const Symbol& s) {
    if (s.kind == SymbolKind::import) return s.name;
    auto q = qualified_name(s.name);
    return q.empty() ? s.name : q;
}

} // namespace

Reference describe_reference(const Program& program, u64 va) {
    Reference r;
    r.va = va;
    const Symbol* s = program.symbols().at(va);
    if (!s) {
        // Calls through linker thunks read as calls to the destination.
        if (auto dest = program.thunk_destination(va)) {
            r = describe_reference(program, *dest);
            r.va = va;
            const std::string via = std::format("via thunk at {:#x}", va);
            r.detail = r.detail.empty() ? via : r.detail + "; " + via;
            return r;
        }
    }
    i64 offset = 0;
    if (!s) {
        s = program.symbols().containing(va);
        if (s) offset = static_cast<i64>(va - s->va);
    }
    if (s) {
        r.name = offset ? std::format("{}+{:#x}", s->name, offset) : s->name;
        r.display = offset ? std::format("{}+{:#x}", short_name(*s), offset) : short_name(*s);
        r.kind = std::string(to_string(s->kind));
        if (s->kind == SymbolKind::string) {
            if (auto str = program.image().read_cstring(va)) r.detail = escape_c_string(*str);
        } else if (s->kind == SymbolKind::float_const) {
            r.detail = float_text(program, va, s->name);
        } else if (s->kind == SymbolKind::import) {
            r.detail = s->display;
        } else if (s->kind == SymbolKind::function) {
            r.detail = s->display;
        }
        return r;
    }
    r.name = std::format("unk_{:x}", va);
    r.display = std::format("{:#x}", va);
    r.kind = "unknown";
    if (program.image().is_readonly_data(va)) {
        if (auto str = program.image().read_cstring(va, 256); str && printable_string(*str)) {
            r.kind = "string";
            r.detail = escape_c_string(*str);
        }
    }
    if (const auto* sec = program.image().section_at(va); sec && r.detail.empty()) r.detail = "in " + sec->name;
    return r;
}

bool is_address_field(const Program& program, const x86::Instruction& ins, const x86::Field& field) {
    if (field.kind == x86::FieldKind::rel) return true;
    if (field.rip_relative) return true;
    if (field.size < 4) return false;
    const auto& image = program.image();
    if (image.has_relocations()) return image.is_relocated(ins.address + field.offset);
    return image.contains(field.absolute) && field.absolute >= image.image_base() + 0x1000;
}

Result<AnnotatedFunction> annotate_function(const Program& program, u64 start, bool include_callers) {
    TRY_ASSIGN(auto ext, program.function_extent(start));
    TRY_ASSIGN(auto ins, program.function_instructions(ext));
    auto cfg = build_cfg(ins, ext.jump_tables);

    AnnotatedFunction fn;
    fn.start = ext.start;
    fn.end = ext.end;
    fn.jump_tables = ext.jump_tables;
    if (const Symbol* s = program.symbols().at(start)) {
        fn.name = s->name;
        fn.display = s->display;
        fn.pdb_name = s->pdb_name;
    } else {
        fn.name = std::format("sub_{:x}", start);
        fn.display = fn.name;
    }
    fn.instruction_count = ins.size();
    fn.block_count = cfg.blocks.size();
    fn.loop_count = static_cast<usize>(std::ranges::count_if(cfg.blocks, &BasicBlock::loop_header));

    // Branch targets inside the function get labels.
    std::set<u64> labelled;
    for (const auto& x : ins)
        if (x.branch_target && ext.contains(*x.branch_target) && (x.flow == x86::Flow::jump || x.flow == x86::Flow::cond_jump))
            labelled.insert(*x.branch_target);
    for (const auto& t : ext.jump_tables)
        for (u64 target : t.targets)
            if (target) labelled.insert(target);
    auto label_for = [](u64 va) { return std::format("loc_{:x}", va); };

    std::map<u64, Reference> callees, data_refs;
    const auto rva_fields = image_relative_fields(program.image(), ins);
    const bool x64 = program.arch() == Arch::x64;
    const i64 slot = x64 ? 8 : 4;
    i64 sp = 0;  // stack pointer relative to the value at entry (return address at [sp_entry])
    std::optional<i64> frame;  // ebp/rbp value relative to entry sp, once set up

    auto frame_name = [&](i64 offset_from_entry) -> std::string {
        if (offset_from_entry >= slot) return std::format("arg_{:x}", offset_from_entry - slot);
        if (offset_from_entry < 0) return std::format("var_{:x}", -offset_from_entry);
        return "return_address";
    };

    for (usize i = 0; i < ins.size(); ++i) {
        const auto& x = ins[i];
        AnnotatedLine line;
        line.address = x.address;
        line.block = cfg.block_of[i];
        line.bytes = hex_bytes(x.bytes.data(), x.length);
        if (labelled.contains(x.address)) line.label = label_for(x.address);

        std::vector<std::string> notes;
        auto renderer = [&](const x86::Instruction& in, const x86::Field& f) -> std::optional<std::string> {
            const auto field_no = static_cast<usize>(&f - in.fields.data());
            if (f.kind != x86::FieldKind::rel && rva_fields.contains({in.address, field_no})) {
                // Image-base-relative (MSVC x64 `[r8+rcx*4+rva]`): name what the RVA points at.
                const u64 va = program.image().image_base() + static_cast<u64>(f.raw);
                if (std::ranges::any_of(ext.jump_tables, [&](const JumpTable& t) { return t.table_va == va; }))
                    return std::format("switch_table_{:x}", va);
                auto ref = describe_reference(program, va);
                data_refs.emplace(va, ref);
                if (!ref.detail.empty() && ref.kind != "function") notes.push_back(ref.detail);
                return std::format("imagerel {}", ref.display);
            }
            if (f.kind == x86::FieldKind::rel) {
                if (ext.contains(f.absolute)) return label_for(f.absolute);
                auto ref = describe_reference(program, f.absolute);
                if (in.flow == x86::Flow::call || in.flow == x86::Flow::jump || in.flow == x86::Flow::cond_jump) {
                    if (auto dest = program.thunk_destination(f.absolute))
                        if (const Symbol* slot = program.symbols().at(*dest); slot && slot->kind == SymbolKind::import) ref.import_call = "thunk";
                    callees.emplace(f.absolute, ref);
                    if (in.flow != x86::Flow::call) notes.push_back("tail call");
                }
                return ref.display;
            }
            if (!is_address_field(program, in, f)) return std::nullopt;
            if (ext.contains(f.absolute) && !std::ranges::any_of(ext.jump_tables, [&](const JumpTable& t) { return t.table_va == f.absolute; }))
                return label_for(f.absolute);
            auto ref = describe_reference(program, f.absolute);
            bool is_table = std::ranges::any_of(ext.jump_tables, [&](const JumpTable& t) { return t.table_va == f.absolute; });
            if (is_table) {
                return std::format("switch_table_{:x}", f.absolute);
            }
            // An import's slot is read to call it (`call [__imp_X]`, or `mov esi, [__imp_X]` then `call esi`).
            if (ref.kind == "import") {
                ref.import_call = "dllimport";
                callees.emplace(f.absolute, ref);
            } else if (ref.kind == "function" && in.flow == x86::Flow::indirect_call) {
                callees.emplace(f.absolute, ref);
            } else {
                data_refs.emplace(f.absolute, ref);
            }
            if (!ref.detail.empty() && ref.kind != "function") notes.push_back(ref.detail);
            return ref.display;
        };
        line.text = x86::render(x, renderer);

        // Frame slots for esp/ebp-relative operands.
        for (const auto& op : x.operands) {
            if (op.kind != x86::OperandKind::mem || !op.mem.index.empty()) continue;
            const auto& base = op.mem.base;
            if ((base == "esp" || base == "rsp")) notes.push_back(frame_name(sp + op.mem.disp));
            else if ((base == "ebp" || base == "rbp") && frame) notes.push_back(frame_name(*frame + op.mem.disp));
        }
        for (const auto& t : ext.jump_tables) {
            if (t.jump_va != x.address) continue;
            std::vector<std::string> targets;
            for (u64 target : t.targets) targets.push_back(target ? label_for(target) : "none");
            notes.push_back(std::format("switch: {} cases -> {}", t.targets.size(), join(targets, ", ")));
        }
        const auto& block = cfg.blocks[line.block];
        if (block.first == i && block.loop_header) notes.push_back(std::format("loop header (depth {})", block.loop_depth));
        for (usize s : block.successors)
            if (block.last == i && s <= line.block && cfg.blocks[s].loop_header) notes.push_back("loop back-edge");

        // Update the stack-pointer model (straight-line approximation; good enough for slot names).
        const auto& m = x.mnemonic;
        auto first_reg = [&](std::string_view r) { return !x.operands.empty() && x.operands[0].kind == x86::OperandKind::reg && x.operands[0].reg == r; };
        auto second_imm = [&]() -> std::optional<i64> {
            if (x.operands.size() == 2 && x.operands[1].kind == x86::OperandKind::imm) return x.operands[1].imm;
            return std::nullopt;
        };
        std::string spr = x64 ? "rsp" : "esp", fpr = x64 ? "rbp" : "ebp";
        if (m == "push") sp -= (x.operands.empty() ? slot : std::max<i64>(x.operands[0].size_bits / 8, 2));
        else if (m == "pop") sp += slot;
        else if (m == "sub" && first_reg(spr) && second_imm()) sp -= *second_imm();
        else if (m == "add" && first_reg(spr) && second_imm()) sp += *second_imm();
        else if (m == "mov" && first_reg(fpr) && x.operands.size() == 2 && x.operands[1].kind == x86::OperandKind::reg && x.operands[1].reg == spr) frame = sp;
        else if (m == "mov" && first_reg(spr) && x.operands.size() == 2 && x.operands[1].kind == x86::OperandKind::reg && x.operands[1].reg == fpr && frame) sp = *frame;
        else if (m == "leave" && frame) sp = *frame + slot;

        std::ranges::sort(notes);
        notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
        line.comment = join(notes, "; ");
        fn.lines.push_back(std::move(line));
    }

    for (auto& [va, ref] : callees) fn.callees.push_back(ref);
    for (auto& [va, ref] : data_refs) fn.data_refs.push_back(ref);
    if (include_callers)
        for (u64 caller : program.callers_of(start)) fn.callers.push_back(program.describe_address(caller));
    fn.virtual_slots = program.rtti().describe_slots(start);
    return fn;
}

std::string to_text(const AnnotatedFunction& fn, bool with_bytes) {
    std::string out;
    out += std::format("; function: {}\n", fn.display.empty() ? fn.name : fn.display);
    out += std::format("; symbol:   {}{}\n", fn.name, fn.pdb_name.empty() || fn.pdb_name == fn.name ? "" : std::format("  (pdb: {})", fn.pdb_name));
    out += std::format("; range:    {:#x}-{:#x} ({} bytes, {} instructions, {} blocks, {} loops)\n", fn.start, fn.end,
                       fn.end - fn.start, fn.instruction_count, fn.block_count, fn.loop_count);
    if (!fn.callers.empty()) out += std::format("; callers:  {}\n", join(fn.callers, ", "));
    for (const auto& v : fn.virtual_slots) out += std::format("; virtual:  {}\n", v);
    for (const auto& c : fn.callees)
        out += std::format("; calls:    {} = {}\n", c.display, c.detail.empty() ? c.name : c.detail);
    for (const auto& d : fn.data_refs)
        out += std::format("; data:     {} ({}){}\n", d.display, d.kind, d.detail.empty() ? "" : " " + d.detail);
    usize prev_block = static_cast<usize>(-1);
    for (const auto& l : fn.lines) {
        if (l.block != prev_block || !l.label.empty()) {
            if (!l.label.empty()) out += l.label + ":\n";
            prev_block = l.block;
        }
        std::string body = with_bytes ? std::format("  {:08x}  {:<24} {}", l.address, l.bytes, l.text)
                                      : std::format("  {:08x}  {}", l.address, l.text);
        if (!l.comment.empty()) {
            if (body.size() < 64) body.append(64 - body.size(), ' ');
            body += " ; " + l.comment;
        }
        out += body + "\n";
    }
    return out;
}

Json to_json(const AnnotatedFunction& fn) {
    Json j;
    j["start"] = fn.start;
    j["end"] = fn.end;
    j["name"] = fn.name;
    j["display"] = fn.display;
    j["pdb_name"] = fn.pdb_name;
    j["instructions"] = fn.instruction_count;
    j["blocks"] = fn.block_count;
    j["loops"] = fn.loop_count;
    j["callers"] = fn.callers;
    j["virtual_slots"] = fn.virtual_slots;
    auto refs = [](const std::vector<Reference>& list) {
        Json arr = Json::array();
        for (const auto& r : list)
            arr.push_back({{"va", r.va}, {"name", r.name}, {"display", r.display}, {"kind", r.kind}, {"detail", r.detail},
                           {"import_call", r.import_call}});
        return arr;
    };
    j["callees"] = refs(fn.callees);
    j["data"] = refs(fn.data_refs);
    Json lines = Json::array();
    for (const auto& l : fn.lines) {
        Json line{{"address", l.address}, {"bytes", l.bytes}, {"text", l.text}, {"block", l.block}};
        if (!l.label.empty()) line["label"] = l.label;
        if (!l.comment.empty()) line["comment"] = l.comment;
        lines.push_back(std::move(line));
    }
    j["lines"] = std::move(lines);
    return j;
}

} // namespace decomp
