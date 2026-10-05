#pragma once

// Switch dispatch through jump tables, for x86 and x64 code from MSVC and clang. A table is recognized
// from the indirect jump and the instructions that run before it. Its number of entries comes from the
// switch's bounds check (`cmp idx, N` then `ja default`) when there is one, otherwise from reading
// entries while they point into the function. MSVC places its tables in the code section right after
// the function, and may index them through a byte table (two-level dispatch):
//
//   cmp eax, N ; ja default ; movzx eax, byte ptr [eax+index] ; jmp dword ptr [eax*4+table]     (x86)
//   lea rdx, [__ImageBase] ; movzx eax, byte ptr [rdx+rax+index_rva]
//                          ; mov ecx, dword ptr [rdx+rax*4+table_rva] ; add rcx, rdx ; jmp rcx  (x64)

#include "analysis/symbols.hpp"
#include "arch/x86/decoder.hpp"
#include "formats/image.hpp"

#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace decomp {

enum class TableEncoding : u8 {
    absolute,  // entries are absolute addresses (x86: jmp [reg*4+table])
    relative,  // entries are int32 offsets from the table start (clang x64)
    rva,       // entries are 32-bit RVAs from the image base (MSVC x64)
};

struct JumpTable {
    u64 jump_va = 0;   // the indirect jump instruction
    u64 table_va = 0;  // first entry
    unsigned entry_size = 4;
    TableEncoding encoding = TableEncoding::absolute;
    u64 load_va = 0;   // x64: the instruction that loads the entry (its displacement is the table RVA for MSVC)
    std::vector<u64> targets;  // one per entry; an int3 target is a case that cannot happen (not code to follow)
    bool inside_code = false;  // table sits within the function's byte range (MSVC)
    // Two-level dispatch: the byte table that maps the switch value to an entry (0 when there is none).
    u64 index_va = 0;
    usize index_entries = 0;
    bool bounded = false;  // the entry count comes from the switch's bounds check

    // The table's bytes, and the index table's: [begin, end) ranges in address order.
    std::vector<std::pair<u64, u64>> data_ranges() const;
};

struct JumpTableContext {
    const BinaryImage* image = nullptr;
    const SymbolDb* symbols = nullptr;  // reading without a bound stops at the next named object
    u64 fn_start = 0;                   // targets must lie in [fn_start, fn_limit)
    u64 fn_limit = 0;
    const std::set<u64>* table_starts = nullptr;  // ... and at the start of another known table
};

// `before` holds the instructions that run before the jump, in order (the jump's block, preceded by the
// block that ends in the bounds check); the last one is the instruction right before the jump.
std::optional<JumpTable> read_jump_table(const JumpTableContext& ctx, std::span<const x86::Instruction> before, const x86::Instruction& jump);

} // namespace decomp
