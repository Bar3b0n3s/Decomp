#pragma once

// A search run from start to end (docs/matching.md#searching), the same for the CLI's `decomp search`
// and the GUI's Search view: the probes made from what was asked, the search of its kind, and the run
// kept under .decomp/search/ (run.json, log.jsonl, and for a permutation its start and best sources).

#include "analysis/program.hpp"
#include "core/json.hpp"
#include "core/result.hpp"
#include "matching/match.hpp"
#include "search/evaluate.hpp"
#include "search/flags.hpp"
#include "search/identify.hpp"
#include "search/permute.hpp"
#include "search/runs.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace decomp::project {
class Project;
}

namespace decomp::search {

// What a search compiles. Functions' verified sources (best attempts with `attempt`; every function of
// the file with `whole`), units' sources, files (whose target functions are `functions` when given,
// else every one they define), or every verified source.
struct ProbeSpec {
    std::vector<u64> functions;
    std::vector<std::string> units;
    std::vector<std::filesystem::path> sources;
    bool verified = false, attempt = false, whole = false;
};

struct Probes {
    std::vector<Probe> probes;
    std::string target;  // what they are: "Player::Hit", "basic.cpp", "every verified source"
    std::vector<u64> functions;
};
Result<Probes> make_probes(const project::Project* project, const Program& program, const ProbeSpec& spec);

struct SearchRequest {
    SearchKind kind = SearchKind::flags;
    ProbeSpec probes;
    // Flags (and identification's per-toolchain search).
    std::vector<std::string> groups;  // "name: a | b" texts, added to the preset's or replacing its group of that name
    std::string preset;               // empty: flags "common" ("none" with groups), identify "basic"
    usize limit = 0;                  // 0: the kind's default (flags 1000, permute 500, identify 64 per toolchain)
    usize exhaustive_limit = 256;
    int restarts = 2;
    u64 seed = 1;
    // Permutation.
    usize batch = 0;
    int seconds = 0;
    // Identification: toolchain names; empty: every registered one for the target's architecture.
    std::vector<std::string> toolchains;
    int threads = 0;
    bool record = true;  // keep the run under the project's .decomp/search/
};

struct SearchOutcome {
    Probes probes;
    std::optional<RunRecord> run;  // when recorded
    std::filesystem::path run_dir;
    Json result = Json::object();  // what run.json keeps as the result
    std::vector<FlagGroup> groups; // a flag search's
    std::variant<std::monostate, FlagSearchResult, PermuteResult, IdentifyResult> detail;
    Score start_score, score;
    usize candidates = 0;
    bool cancelled = false;
    std::string error;  // a permutation that found nothing to edit

    // "12/12 byte-exact with /O2 /GS-", "gcc-x64 first: 5/5 byte-exact with -O2", ...
    std::string headline() const;
};

struct SearchCallbacks {
    std::function<void(const Probes&)> started;      // the probes are made: the search begins
    std::function<void(const LogEntry&)> candidate;  // every candidate, as it is logged
    std::function<bool()> cancelled;                 // polled; the candidates being compiled finish first
};

// Runs a search. Fails before searching when the probes, the groups or the toolchains cannot be made.
Result<SearchOutcome> run_search(const project::Project* project, const Program& program, const matching::MatchSetup& setup,
                                 const SearchRequest& request, const SearchCallbacks& callbacks = {});

} // namespace decomp::search
