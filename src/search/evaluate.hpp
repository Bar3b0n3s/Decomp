#pragma once

// What every search compiles and scores (docs/matching.md#searching): a configuration (a toolchain and
// its flags) applied to probes (translation units and the target functions they define), each probe
// compiled once and its functions diffed against the target. Flag search tries configurations, compiler
// identification tries toolchains, the permuter tries sources; all of them rank candidates by Score.

#include "analysis/program.hpp"
#include "matching/match.hpp"

#include <chrono>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace decomp::search {

// A toolchain and the flags its candidates are compiled with (after the toolchain's own).
struct Configuration {
    matching::Toolchain toolchain;
    std::vector<std::string> flags;
    std::string label() const;  // "clang-cl-x64 /O2 /GS-"
};

// Source to compile and the target functions it defines.
struct Probe {
    std::string source;
    std::string file_name = "candidate.cpp";  // its extension picks the language
    std::vector<u64> functions;
};

// The weight of the differences between a candidate and the target: an inserted or deleted instruction
// costs most, a different opcode less, a different operand or encoding least; a candidate equivalent in
// every instruction but not in its bytes costs 1, a byte-exact one 0.
u32 distance_of(const matching::FunctionDiff& diff);
// What a function that did not compile, or is not in the object, costs.
inline constexpr u32 kMissingDistance = 1'000'000;

// One function's result for one candidate.
struct FunctionScore {
    u64 va = 0;
    bool found = false;  // compiled and found in the object
    bool byte_exact = false;
    double match_percent = 0;
    u32 distance = kMissingDistance;
    std::string error;  // why it was not found
};
FunctionScore score_of(u64 va, const matching::FunctionDiff& diff);

// A candidate's score over its functions: more byte-exact functions is better, then less distance.
struct Score {
    usize functions = 0;
    usize exact = 0;
    u64 distance = 0;
    double match_percent = 0;  // the functions' mean

    bool complete() const { return functions > 0 && exact == functions; }
    bool better_than(const Score& other) const;
    bool operator==(const Score&) const = default;
    std::string text() const;  // "3/4 byte-exact, distance 12, 97.5%"
};
Score total(std::span<const FunctionScore> functions);
Json to_json(const Score& score);
Score score_from_json(const Json& j);

// A configuration's result on all probes, functions in probe order.
struct Evaluation {
    std::vector<FunctionScore> functions;
    Score score;
    std::string compile_error;  // the first failed compile's diagnostics, when one failed
    std::chrono::milliseconds duration{0};
    bool cancelled = false;
};
// Compiles each probe with `configuration` (`setup` gives the include directories, the work and cache
// directories and cancellation; its toolchain and flags are replaced) and diffs its functions.
Evaluation evaluate(const Program& program, const matching::MatchSetup& setup, const Configuration& configuration,
                    std::span<const Probe> probes);

// Runs job(0) .. job(count - 1) on up to `threads` threads (at least one), starting no new job once
// `cancelled` says so. Compiles still wait for one of the process's compile slots
// (matching::max_parallel_compiles()).
void parallel_for(usize count, int threads, const std::function<void(usize)>& job, const std::function<bool()>& cancelled = {});

} // namespace decomp::search
