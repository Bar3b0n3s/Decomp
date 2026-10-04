#pragma once

// How hard a function looks before anyone has worked on it: features of its code (size, basic blocks,
// loops, calls, callees nobody has named) and a score built from them. The Run monitor shows the score
// with each queued function, the Function browser has it as a column, and runs can order by it.

#include "analysis/program.hpp"
#include "core/result.hpp"

#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace decomp::vm {

struct FunctionFeatures {
    u64 va = 0;
    u32 bytes = 0;           // the extent's size (the symbol size, or what recursive descent recovered)
    u32 instructions = 0;
    u32 blocks = 0;          // basic blocks (build_cfg, the count annotate_function reports)
    u32 loops = 0;           // loop headers
    u32 max_loop_depth = 0;
    u32 jump_tables = 0;     // switch dispatch tables
    u32 calls = 0;           // call instructions, direct and indirect (tail jumps are not counted)
    // Distinct targets of calls and of tail jumps that leave the function, with linker thunks followed:
    u32 callees = 0;          // ... that have a name (a symbol other than an analysis "sub_..." name, or an import)
    u32 unknown_callees = 0;  // ... that have none, plus indirect calls that do not go through an import
                              // slot (one per call site, or per memory slot such as a function pointer)
    u32 callers = 0;          // distinct functions calling or tail-jumping to it (analyze_functions() only)
};

// The features of one function (callers stays 0). Decodes the function once and builds its CFG: about
// a microsecond per instruction in a Release build (100 microseconds for a 100-instruction function).
Result<FunctionFeatures> function_features(const Program& program, u64 va);

struct FunctionAnalysis {
    std::vector<FunctionFeatures> functions;  // ascending by address
    std::vector<u64> failed;                  // functions that could not be decoded
    bool cancelled = false;                   // stopped early: `functions` holds what was done

    const FunctionFeatures* find(u64 va) const;  // binary search
};

// Features of every function in `vas` plus their callers among `vas` (pass every function symbol for
// complete caller counts). Callers are counted at the thunk's destination when a call goes through a
// linker thunk, which Program::callers_of() does not do. Decodes each function once, single-threaded:
// 1 to 1.5 microseconds per instruction in a Release build (a generated program of 38,000
// instructions, one function of 15,000, took about 50 ms in tests/unit/viewmodel_eta_tests.cpp), so a
// target of 100,000 functions and 10 million instructions takes 10 to 15 seconds; run it as a
// background job.
// `cancelled` is polled between functions; `progress(done, total)` is called every 256 functions.
FunctionAnalysis analyze_functions(const Program& program, std::span<const u64> vas, const std::function<bool()>& cancelled = {},
                                   const std::function<void(usize, usize)>& progress = {});

// The difficulty score. It extends run::estimate_difficulty (log2 of the size) with the other features:
//   log2(1 + bytes) + 0.5 * log2(1 + blocks) + min(loops, 8) + 0.5 * min(max_loop_depth, 4)
//   + 0.25 * min(callees, 20) + min(unknown_callees, 10) + 0.5 * min(jump_tables, 4)
// Unknown callees weigh most after size: the agent must work out their signatures. On the test
// fixtures: 4.5 for a 15-byte leaf function, 10 to 11.5 for a function of about 100 bytes with a loop
// or a switch, or for a 240-byte function making ten calls; a function of thousands of bytes with
// many blocks and calls scores above 20. Pure arithmetic.
double difficulty(const FunctionFeatures& features);
// "easy" below 8, "medium" below 14, "hard" below 20, then "very hard".
std::string_view difficulty_label(double score);

} // namespace decomp::vm
