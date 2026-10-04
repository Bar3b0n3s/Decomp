#pragma once

#include "matching/diff.hpp"
#include "matching/toolchain.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace decomp::matching {

// Everything needed to compile candidates and compare them with a target program.
struct MatchSetup {
    Toolchain toolchain;
    std::vector<std::string> flags;
    std::vector<std::filesystem::path> include_dirs;
    std::filesystem::path work_dir;
    std::optional<std::filesystem::path> cache_dir;
    std::function<bool()> cancelled;  // polled while waiting for a compile slot and while compiling
    bool bypass_cache = false;
};

struct CandidateResult {
    CompileResult compile;
    std::optional<FunctionDiff> diff;  // set when the compile succeeded and the function was found
    std::string diff_error;            // why no diff was produced (compile failure, missing symbol)
};

// Compiles `source` and diffs the target function at `va` against it.
Result<CandidateResult> compile_and_diff(const Program& program, const MatchSetup& setup, u64 va, const std::string& source,
                                         const std::string& candidate_symbol = {});

} // namespace decomp::matching
