#pragma once

// Function discovery for images without debug information (docs/architecture.md#function-discovery):
// which bytes of the code sections are functions, where each one starts and ends, and which are padding.
//
// From what is known (the entry point, exports, x64 unwind data, symbols) it follows calls to new
// functions and traces each function's flow through the bytes up to the next known start: jumps,
// switch tables, returns, and calls that never return. Code that the flow only reaches by a jump past
// alignment padding is a separate function (a tail call); code addresses stored in data, relocations
// and instructions start functions too; and the code left over between functions is tried last. The
// passes repeat until nothing changes, because each new start or each function found never to return
// changes how its neighbours and callers are traced.

#include "analysis/symbols.hpp"
#include "arch/x86/decoder.hpp"
#include "formats/pe.hpp"

#include <string_view>
#include <vector>

namespace decomp {

// How a function's start was found, strongest first.
enum class FunctionEvidence : u8 { symbol, entry, export_table, unwind, call, tail_jump, address, gap };
std::string_view to_string(FunctionEvidence evidence);

struct DiscoveredFunction {
    u64 start = 0;
    u64 end = 0;  // exclusive: the code, and the switch tables placed after it
    FunctionEvidence evidence = FunctionEvidence::call;
    bool noreturn = false;  // no path returns to the caller
    u64 import_slot = 0;    // an import thunk (`jmp [slot]`): the IAT slot it jumps through
};

struct DiscoveryOptions {
    bool address_taken = true;  // code addresses held in data, relocations and instructions
    bool gaps = true;           // code nothing references, between other functions
};

struct DiscoveryResult {
    std::vector<DiscoveredFunction> functions;  // by start
    usize passes = 0;
    usize instructions = 0;  // distinct instructions decoded
};

// `known` holds the symbols the image (or the project) already has. Its functions are kept; those with
// a size keep it, the others are traced like any other start.
DiscoveryResult discover_functions(const pe::Image& image, const x86::Decoder& decoder, const SymbolDb& known,
                                   const DiscoveryOptions& options = {});

// The number of alignment-padding bytes at `va` (int3, nop and zero fill, and the filler instructions
// compilers align with, such as `lea esi, [esi+0]`), stopping at `limit`. Zeros count when there are at
// least four of them or they reach `limit`.
u64 padding_length(const BinaryImage& image, const x86::Decoder& decoder, u64 va, u64 limit);

} // namespace decomp
