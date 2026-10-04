#pragma once

// Past runs as their summary.json files record them: the input of progress over time (progress.hpp)
// and spend by run, day and model (cost.hpp).

#include "core/json.hpp"
#include "events/run_state.hpp"
#include "viewmodel/common.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace decomp::vm {

// One function of a run: the outcome of its latest session and the totals of all its sessions.
struct RunFunctionRecord {
    u64 va = 0;
    std::string function, display, outcome;
    bool matched = false;
    double best_match = 0;  // percent
    int turns = 0;
    int sessions = 1;
    double cost_usd = 0;
    events::TokenUsage usage;
};

struct RunRecord {
    std::string id;
    std::filesystem::path dir;  // empty for a record made from a live state
    std::string status;         // completed, stopped, aborted, error, budget_exhausted, running, interrupted, ...
    std::string model, effort;  // as configured for the run (a fallback model may have served some turns)
    TimePoint started{};        // from summary.json, else from the run id
    TimePoint finished{};       // unset while the run goes on or when unknown
    double cost_usd = 0;
    events::TokenUsage usage;
    usize matched = 0;  // functions matched in this run
    bool replay = false;
    std::vector<RunFunctionRecord> functions;  // in address order

    usize worked() const { return functions.size(); }
    long long turns() const;
};

// Reads a summary.json in either form: `decomp run` and the GUI (run::run_summary) or `decomp agent`
// (one function, no top-level usage). Missing fields keep their defaults.
RunRecord run_record_from_summary(const Json& summary);

// A live (or replayed) run as a record, through run::run_summary, so it agrees with its summary.json.
RunRecord run_record_from_state(const events::RunStateData& state);

// Every run under `runs_dir` with a readable summary.json, oldest first (by start time, then id). The
// status comes from run.json where there is one (with run::read_run_info's "interrupted" detection:
// it tries each run's lock), else from summary.json. Parsing the summaries dominates: about 15 ms per
// MB in a Release build (50 runs of 500 functions, 11 MB of summary.json, took about 165 ms in
// tests/unit/viewmodel_progress_tests.cpp), so call it from a background job.
std::vector<RunRecord> load_run_records(const std::filesystem::path& runs_dir);

// Replaces the record of the live run (same id) with `live`, or adds it, keeping the order. The live
// state is newer than the summary.json the run writes every few seconds.
void merge_live_run(std::vector<RunRecord>& records, const events::RunStateData& live);

} // namespace decomp::vm
