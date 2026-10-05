#include "analysis/jump_tables.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <string>

namespace decomp {

std::vector<std::pair<u64, u64>> JumpTable::data_ranges() const {
    std::vector<std::pair<u64, u64>> out;
    if (!targets.empty()) out.emplace_back(table_va, table_va + targets.size() * entry_size);
    if (index_va && index_entries) out.emplace_back(index_va, index_va + index_entries);
    std::ranges::sort(out);
    return out;
}

namespace {

using x86::Instruction;
using x86::Operand;
using x86::OperandKind;

constexpr usize kMaxEntries = 4096;
constexpr usize kLookBack = 16;  // instructions searched before the jump

std::string family(std::string_view reg) { return x86::gpr_family(reg); }

const Operand* memory_operand(const Instruction& ins) {
    for (const auto& op : ins.operands)
        if (op.kind == OperandKind::mem) return &op;
    return nullptr;
}

// A memory operand as text, so that a load and a compare of the same slot can be recognized.
std::string memory_key(const Operand& op) { return std::format("{}+{}*{}+{}", op.mem.base, op.mem.index, op.mem.scale, op.mem.disp); }

bool writes_register(const Instruction& ins, const std::set<std::string>& regs) {
    return !ins.operands.empty() && ins.operands[0].kind == OperandKind::reg && regs.contains(family(ins.operands[0].reg)) &&
           ins.mnemonic != "cmp" && ins.mnemonic != "test" && ins.mnemonic != "push";
}

// How many entries the switch's bounds let the table be indexed with. Walking back from
// `before[from - 1]`, it follows copies into the index (register moves, loads from a slot) and biases
// applied to it (`dec`, `add`, `lea r, [r-5]`) until what bounds the value: an unsigned
// compare-and-branch (`cmp idx, N` + `ja`: N + 1 values; `jae`: N values) or a mask (`and idx, M`:
// M + 1 values). Biases applied after the bound shift the range: [0, n) + bias.
std::optional<u64> bound_of(std::span<const Instruction> before, usize from, const std::string& index) {
    std::set<std::string> regs{index};
    std::set<std::string> slots;
    i64 bias = 0;
    auto entries = [&](u64 values) -> std::optional<u64> {
        const i64 n = static_cast<i64>(values) + bias;
        if (n <= 0 || static_cast<u64>(n) > kMaxEntries) return std::nullopt;
        return static_cast<u64>(n);
    };
    const usize stop = from > kLookBack ? from - kLookBack : 0;
    for (usize k = from; k-- > stop;) {
        const Instruction& ins = before[k];
        const std::string& m = ins.mnemonic;
        if (ins.flow == x86::Flow::cond_jump) {
            const bool above = m == "jnbe", above_equal = m == "jnb";
            if ((!above && !above_equal) || k == 0) return std::nullopt;
            const Instruction& cmp = before[k - 1];
            if (cmp.mnemonic != "cmp" || cmp.operands.size() != 2 || cmp.operands[1].kind != OperandKind::imm) return std::nullopt;
            const Operand& lhs = cmp.operands[0];
            const bool tracked = (lhs.kind == OperandKind::reg && regs.contains(family(lhs.reg))) ||
                                 (lhs.kind == OperandKind::mem && slots.contains(memory_key(lhs)));
            if (!tracked) return std::nullopt;
            const u64 n = static_cast<u64>(cmp.operands[1].imm) & (lhs.size_bits >= 64 ? ~u64{0} : (u64{1} << lhs.size_bits) - 1);
            return entries(above ? n + 1 : n);
        }
        if (ins.flow != x86::Flow::none) return std::nullopt;  // a call or another branch: the value is unknown
        if (!writes_register(ins, regs)) continue;
        if (ins.operands.size() == 1) {
            if (m == "dec") --bias;
            else if (m == "inc") ++bias;
            else return std::nullopt;
            continue;
        }
        const Operand& src = ins.operands[1];
        if (m == "and" && src.kind == OperandKind::imm) {
            const u64 mask = static_cast<u64>(src.imm) & 0xFFFFFFFFu;
            if (mask >= kMaxEntries) return std::nullopt;
            return entries(mask + 1);
        }
        if ((m == "mov" || m == "movsxd" || m == "movzx" || m == "movsx") && src.kind == OperandKind::reg) {
            regs.insert(family(src.reg));
        } else if (m == "mov" && src.kind == OperandKind::mem) {
            slots.insert(memory_key(src));
        } else if (m == "lea" && src.kind == OperandKind::mem && src.mem.index.empty() && !src.mem.base.empty()) {
            regs.insert(family(src.mem.base));
            bias += src.mem.disp;
        } else if ((m == "add" || m == "sub") && src.kind == OperandKind::imm) {
            bias += m == "add" ? src.imm : -src.imm;
        } else {
            return std::nullopt;  // the index comes from somewhere the bound does not cover
        }
    }
    return std::nullopt;
}

// A case label: code in the function. LLVM points the cases that cannot happen at an int3 (often the
// byte right after the function), so an int3 at the function's limit counts too.
bool valid_target(const JumpTableContext& ctx, u64 target) {
    if (!ctx.image->is_code(target)) return false;
    if (target >= ctx.fn_start && target < ctx.fn_limit) return true;
    return target == ctx.fn_limit && ctx.image->read<u8>(target).value_or(0) == 0xCC;
}

std::optional<u64> entry_target(const JumpTableContext& ctx, const JumpTable& t, usize i) {
    const u64 slot = t.table_va + static_cast<u64>(i) * t.entry_size;
    switch (t.encoding) {
    case TableEncoding::absolute:
        if (t.entry_size == 8) return ctx.image->read<u64>(slot);
        return ctx.image->read<u32>(slot).transform([](u32 v) { return u64{v}; });
    case TableEncoding::relative:
        return ctx.image->read<i32>(slot).transform([&](i32 v) { return t.table_va + static_cast<u64>(static_cast<i64>(v)); });
    case TableEncoding::rva:
        return ctx.image->read<u32>(slot).transform([&](u32 v) { return ctx.image->image_base() + v; });
    }
    return std::nullopt;
}

// A null entry: MSVC leaves the cases of a switch whose default is `__assume(0)` without code.
bool null_entry(const JumpTableContext& ctx, const JumpTable& t, usize i) {
    const u64 slot = t.table_va + static_cast<u64>(i) * t.entry_size;
    if (t.entry_size == 8) return ctx.image->read<u64>(slot) == u64{0};
    return ctx.image->read<u32>(slot) == u32{0};
}

// Exactly `count` entries, all inside the function or null (kept as 0); nothing when one is not.
std::optional<std::vector<u64>> read_counted(const JumpTableContext& ctx, const JumpTable& t, usize count) {
    std::vector<u64> out;
    out.reserve(count);
    for (usize i = 0; i < count; ++i) {
        if (null_entry(ctx, t, i)) {
            out.push_back(0);
            continue;
        }
        auto target = entry_target(ctx, t, i);
        if (!target || !valid_target(ctx, *target)) return std::nullopt;
        out.push_back(*target);
    }
    if (std::ranges::all_of(out, [](u64 v) { return v == 0; })) return std::nullopt;
    return out;
}

// Entries while they point into the function (or are null), up to the next named object; nulls at
// the end are not the table's.
std::vector<u64> read_while_valid(const JumpTableContext& ctx, const JumpTable& t) {
    std::vector<u64> out;
    for (usize i = 0; i < kMaxEntries; ++i) {
        const u64 slot = t.table_va + static_cast<u64>(i) * t.entry_size;
        if (i > 0 && ctx.symbols)
            if (const Symbol* s = ctx.symbols->at(slot); s && s->kind != SymbolKind::label) break;
        if (i > 0 && ctx.table_starts && ctx.table_starts->contains(slot)) break;
        // A table in the code section ends where the code it points into would begin.
        if (std::ranges::find(out, slot) != out.end()) break;
        if (null_entry(ctx, t, i)) {
            if (slot + t.entry_size > ctx.fn_limit && slot >= ctx.fn_start) break;
            out.push_back(0);
            continue;
        }
        auto target = entry_target(ctx, t, i);
        if (!target || !valid_target(ctx, *target)) break;
        out.push_back(*target);
    }
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

// A byte table read without a bound (the switch has no bounds check when its default cannot happen):
// small values up to padding, another table or the function's end.
usize unbounded_index_bytes(const JumpTableContext& ctx, u64 va) {
    usize n = 0;
    for (; n < 256; ++n) {
        const u64 at = va + n;
        if (n > 0 && ctx.table_starts && ctx.table_starts->contains(at)) break;
        if (at >= ctx.fn_limit && va >= ctx.fn_start && va < ctx.fn_limit) break;
        const auto b = ctx.image->read<u8>(at);
        if (!b || *b >= 0x80) break;
    }
    return n;
}

// The byte table of a two-level dispatch: the instruction before `before[from]` that loads the jump's
// index register from `byte ptr [idx + table]`. Returns (position, byte table address, the byte table's
// index register).
struct IndexLoad {
    usize position = 0;
    u64 table_va = 0;
    std::string index;
};

std::optional<IndexLoad> index_load(const JumpTableContext& ctx, std::span<const Instruction> before, usize from, const std::string& reg,
                                    std::optional<std::string> image_base_reg) {
    const usize stop = from > kLookBack ? from - kLookBack : 0;
    for (usize k = from; k-- > stop;) {
        const Instruction& ins = before[k];
        if (ins.flow != x86::Flow::none) return std::nullopt;
        if (ins.operands.empty() || ins.operands[0].kind != OperandKind::reg || family(ins.operands[0].reg) != reg) continue;
        if (ins.mnemonic != "movzx" || ins.operands.size() != 2 || ins.operands[1].kind != OperandKind::mem || ins.operands[1].size_bits != 8)
            return std::nullopt;  // the index is written some other way: a one-level table
        const auto& mem = ins.operands[1].mem;
        if (!mem.has_disp) return std::nullopt;
        IndexLoad load;
        load.position = k;
        if (image_base_reg) {
            // x64 MSVC: [base + idx + rva] with base holding __ImageBase.
            if (family(mem.base) == *image_base_reg && !mem.index.empty() && mem.scale <= 1) load.index = family(mem.index);
            else if (family(mem.index) == *image_base_reg && !mem.base.empty() && mem.scale <= 1) load.index = family(mem.base);
            else return std::nullopt;
            load.table_va = ctx.image->image_base() + static_cast<u64>(mem.disp);
        } else {
            // x86: [idx + table].
            if (!mem.base.empty() && mem.index.empty()) load.index = family(mem.base);
            else if (mem.base.empty() && !mem.index.empty() && mem.scale <= 1) load.index = family(mem.index);
            else return std::nullopt;
            load.table_va = static_cast<u64>(static_cast<u32>(mem.disp));
        }
        if (!ctx.image->contains(load.table_va)) return std::nullopt;
        return load;
    }
    return std::nullopt;
}

// Fills the entries of `t` (whose table_va and encoding are set): through a byte table, from the bound,
// or while valid. `index` is the register the table is indexed with; `from` the position in `before`
// where the search for its bound starts.
bool fill_entries(const JumpTableContext& ctx, JumpTable& t, std::span<const Instruction> before, usize from, const std::string& index,
                  std::optional<std::string> image_base_reg) {
    if (auto load = index_load(ctx, before, from, index, image_base_reg)) {
        std::optional<u64> values = bound_of(before, load->position, load->index);
        const bool bounded = values.has_value();
        if (!values)
            if (const usize n = unbounded_index_bytes(ctx, load->table_va)) values = n;
        if (values && *values <= 256) {
            std::vector<u8> bytes;
            for (u64 i = 0; i < *values; ++i) {
                auto b = ctx.image->read<u8>(load->table_va + i);
                if (!b) return false;
                bytes.push_back(*b);
            }
            const usize entries = static_cast<usize>(*std::ranges::max_element(bytes)) + 1;
            if (auto targets = read_counted(ctx, t, entries)) {
                t.targets = std::move(*targets);
                t.index_va = load->table_va;
                t.index_entries = bytes.size();
                t.bounded = bounded;
                return true;
            }
        }
    }
    if (auto values = bound_of(before, from, index)) {
        if (auto targets = read_counted(ctx, t, static_cast<usize>(*values))) {
            t.targets = std::move(*targets);
            t.bounded = true;
            return true;
        }
    }
    t.targets = read_while_valid(ctx, t);
    return !t.targets.empty();
}

// x86 (and absolute x64): jmp dword ptr [idx*4 + table]
std::optional<JumpTable> read_memory_table(const JumpTableContext& ctx, std::span<const Instruction> before, const Instruction& jump) {
    const Operand& op = jump.operands[0];
    if (op.kind != OperandKind::mem || !op.mem.base.empty() || op.mem.index.empty() || !op.mem.has_disp) return std::nullopt;
    const unsigned entry = op.size_bits == 64 ? 8 : 4;
    if (op.mem.scale != entry) return std::nullopt;
    JumpTable t;
    t.jump_va = jump.address;
    t.table_va = jump.memory_target ? *jump.memory_target : static_cast<u64>(static_cast<u32>(op.mem.disp));
    t.entry_size = entry;
    t.encoding = TableEncoding::absolute;
    if (!fill_entries(ctx, t, before, before.size(), family(op.mem.index), std::nullopt)) return std::nullopt;
    return t;
}

// x64: the target is computed in a register.
//   clang:  lea B, [rip+T]; movsxd R, dword ptr [B+I*4]; add R, B; jmp R          (entries: T + int32)
//   MSVC:   lea B, [rip+__ImageBase]; mov R, dword ptr [B+I*4+T_rva]; add R, B; jmp R   (entries: RVAs)
std::optional<JumpTable> read_register_table(const JumpTableContext& ctx, std::span<const Instruction> before, const Instruction& jump) {
    if (ctx.image->arch() != Arch::x64) return std::nullopt;
    const usize window = std::min(before.size(), kLookBack);
    usize load_pos = before.size();
    for (usize k = 0; k < window; ++k) {
        const usize pos = before.size() - 1 - k;
        const Instruction& ins = before[pos];
        if (ins.mnemonic != "movsxd" && ins.mnemonic != "mov") continue;
        const Operand* mem = memory_operand(ins);
        if (mem && mem->mem.scale == 4 && !mem->mem.base.empty() && !mem->mem.index.empty()) {
            load_pos = pos;
            break;
        }
    }
    if (load_pos == before.size()) return std::nullopt;
    const Instruction& load = before[load_pos];
    const Operand* mem = memory_operand(load);
    std::optional<u64> base_value;
    for (usize k = 0; k < load_pos; ++k) {
        const Instruction& ins = before[k];
        if (ins.mnemonic == "lea" && ins.operands.size() == 2 && ins.operands[0].kind == OperandKind::reg &&
            family(ins.operands[0].reg) == family(mem->mem.base) && ins.memory_target)
            base_value = ins.memory_target;
    }
    if (!base_value) return std::nullopt;
    JumpTable t;
    t.jump_va = jump.address;
    t.load_va = load.address;
    t.entry_size = 4;
    std::optional<std::string> image_base_reg;
    if (*base_value == ctx.image->image_base() && mem->mem.has_disp) {
        t.encoding = TableEncoding::rva;
        t.table_va = ctx.image->image_base() + static_cast<u64>(mem->mem.disp);
        image_base_reg = family(mem->mem.base);
    } else if (!mem->mem.has_disp || mem->mem.disp == 0) {
        t.encoding = TableEncoding::relative;
        t.table_va = *base_value;
    } else {
        return std::nullopt;
    }
    if (!fill_entries(ctx, t, before, load_pos, family(mem->mem.index), image_base_reg)) return std::nullopt;
    return t;
}

} // namespace

std::optional<JumpTable> read_jump_table(const JumpTableContext& ctx, std::span<const x86::Instruction> before, const x86::Instruction& jump) {
    if (!ctx.image || jump.flow != x86::Flow::indirect_jump || jump.operands.empty()) return std::nullopt;
    if (jump.operands[0].kind == OperandKind::mem) return read_memory_table(ctx, before, jump);
    if (jump.operands[0].kind == OperandKind::reg) return read_register_table(ctx, before, jump);
    return std::nullopt;
}

} // namespace decomp
