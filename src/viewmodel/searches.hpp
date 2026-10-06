#pragma once

// What the Search view shows (docs/ui.md#search): the project's searches (.decomp/search/, written by
// `decomp search` and by the view), a search's result read into rows by its kind (a flag search's groups,
// a permutation's edits, an identification's ranking), and its log of candidates with the best so far.
// Pure: reading files is the caller's.

#include "core/json.hpp"
#include "search/evaluate.hpp"
#include "search/runs.hpp"
#include "viewmodel/common.hpp"

#include <span>
#include <string>
#include <vector>

namespace decomp::vm {

struct SearchRunRow {
    std::string id, kind, status, target;
    std::string started;  // UTC, ISO 8601
    usize candidates = 0;
    std::string best;     // the best score's text; "-" without one
    std::string best_label;
    bool complete = false;  // the best has every function byte-exact
    double seconds = 0;
};
SearchRunRow search_run_row(const search::RunRecord& run);

struct FlagGroupRow {
    std::string name;
    std::vector<std::string> alternatives;  // "none" for no flags
    usize start = 0, chosen = 0;
    std::vector<usize> equivalent;  // the alternatives as good as the chosen one (the chosen one too)

    bool decided() const { return equivalent.size() <= 1; }
    std::string chosen_text() const;
    std::string also_text() const;  // the other equally good alternatives, comma-separated
    bool changed() const { return chosen != start; }
};
struct FlagSearchView {
    std::vector<std::string> flags, base;
    std::vector<FlagGroupRow> groups;
    search::Score score, start_score;
    usize candidates = 0, space = 0;
    bool exhaustive = false, cancelled = false;
};
FlagSearchView read_flag_search(const Json& result);

struct PermuteView {
    std::vector<std::string> steps;
    search::Score score, start_score;
    usize candidates = 0;
    bool cancelled = false;
    std::string error;

    bool improved() const { return score.better_than(start_score); }
};
PermuteView read_permute(const Json& result);

struct ToolchainRow {
    std::string toolchain, kind, flags, error;
    search::Score score;
};
struct IdentifyView {
    std::vector<ToolchainRow> ranking;  // best first
    usize candidates = 0;
    bool decided = false, cancelled = false;
};
IdentifyView read_identify(const Json& result);

struct SearchLogRow {
    usize index = 0;
    double seconds = 0;
    std::string label, score;
    bool best = false;      // the best so far when it came
    bool complete = false;  // every function byte-exact
};
// The log's rows; with `improvements`, only the candidates that were the best when they came.
std::vector<SearchLogRow> search_log_rows(std::span<const search::LogEntry> entries, bool improvements);
// The best match so far (percent) after each candidate: x the candidate's index, y the percent.
Series best_so_far(std::span<const search::LogEntry> entries);

} // namespace decomp::vm
