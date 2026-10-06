#pragma once

// Compiler-flag search (docs/matching.md#flag-search): which flags, from groups of alternatives, make
// the probes' functions compile to the target's bytes. Small spaces are searched exhaustively; larger
// ones by local search from the starting flags (every single-group change at once, moving to the best
// one while that improves), then from seeded random starts.

#include "analysis/program.hpp"
#include "core/json.hpp"
#include "core/result.hpp"
#include "matching/match.hpp"
#include "search/evaluate.hpp"
#include "search/runs.hpp"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::search {

// Alternatives of which a configuration takes exactly one; an alternative is zero or more flags (zero:
// the compiler's default, "none" in text).
struct FlagGroup {
    std::string name;
    std::vector<std::vector<std::string>> alternatives;
};

// "/Od | /O1 | /O2" or "frame: none | /Oy-": alternatives separated by `|`, the flags of one by spaces,
// an optional name before a colon and a space. At least two alternatives.
Result<FlagGroup> parse_flag_group(std::string_view text);
std::string to_string(const FlagGroup& group);
// An alternative's flags as text ("none" when it has none).
std::string alternative_text(std::span<const std::string> flags);

// Named sets of groups for a toolchain kind and architecture: "basic" (the optimization level, frame
// pointers and security checks: what tells toolchains apart), "common" (also the flags that most often
// differ between builds) and "full" (also packing, signedness, hot patching and more).
std::vector<std::string> flag_presets();
Result<std::vector<FlagGroup>> preset_groups(std::string_view preset, matching::ToolchainKind kind, Arch arch);

// What a search keeps of `flags`: those in no alternative of `groups`. MSVC-style flags compare with
// "-" and "/" alike.
std::vector<std::string> base_flags(std::span<const std::string> flags, std::span<const FlagGroup> groups, bool msvc_style);
// The alternative of each group that `flags` selects: the one whose flags are all present, the last
// present winning; else the group's empty alternative, else its first.
std::vector<usize> choice_of(std::span<const std::string> flags, std::span<const FlagGroup> groups, bool msvc_style);
// `base` followed by the chosen alternative of every group.
std::vector<std::string> flags_of(std::span<const std::string> base, std::span<const FlagGroup> groups, std::span<const usize> choice);
// The chosen alternatives that have flags ("defaults" when none has).
std::string choice_label(std::span<const FlagGroup> groups, std::span<const usize> choice);

struct FlagSearchOptions {
    std::vector<FlagGroup> groups;
    std::vector<std::string> start;  // the flags the search starts from (usually the project's)
    usize exhaustive_limit = 256;    // try every combination when there are at most this many
    usize max_candidates = 1000;     // configurations compiled, at most
    int restarts = 2;                // local searches from random starts after the first
    u64 seed = 1;
    int threads = 0;                 // 0: as many as compiles may run at once
    std::function<bool()> cancelled;
    CandidateLog* log = nullptr;     // every configuration evaluated
};

struct FlagSearchResult {
    std::vector<std::string> base;
    std::vector<usize> start_choice, choice;
    std::vector<std::string> flags;  // the best configuration's: base + chosen alternatives
    // Per group, the alternatives that score the same as the chosen one with every other group as
    // chosen (one alternative: the group is decided).
    std::vector<std::vector<usize>> equivalent;
    Score start_score, score;
    usize candidates = 0;  // configurations compiled
    usize space = 0;       // configurations there are
    bool exhaustive = false;
    bool cancelled = false;
};

FlagSearchResult search_flags(const Program& program, const matching::MatchSetup& setup, std::span<const Probe> probes,
                              const FlagSearchOptions& options);

Json to_json(const FlagSearchResult& result, std::span<const FlagGroup> groups);

} // namespace decomp::search
