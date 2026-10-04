#pragma once

// The Cost and usage view: spend by run, by day, by model and effort, and by function; tokens by type;
// dollars and turns per match; a projection for the functions not done yet; and the flags for turns a
// fallback model served and for usage priced without a price-table row.
//
// Sources: run summaries (load_run_records(): one record per run, including `decomp agent` runs),
// the project's per-function spend (symbols.txt `cost=`, which includes every finished session) and
// the live run (its unfinished sessions are not in symbols.txt yet).

#include "analysis/symbols.hpp"
#include "events/run_state.hpp"
#include "project/project.hpp"
#include "viewmodel/common.hpp"
#include "viewmodel/run_history.hpp"

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace decomp::vm {

// Spend and results of a group of runs (one run, one day, one model and effort).
struct CostSlice {
    std::string key;    // run id, "2026-10-04", or "<model> <effort>"
    TimePoint time{};   // run start, day start (by_run and by_day)
    double cost_usd = 0;
    usize worked = 0;   // functions worked on (a function counts once per run)
    usize matched = 0;
    long long turns = 0;
    events::TokenUsage usage;
    usize runs = 0;

    double success_rate() const { return worked ? static_cast<double>(matched) / static_cast<double>(worked) : 0.0; }
    double usd_per_match() const { return matched ? cost_usd / static_cast<double>(matched) : 0.0; }
    double turns_per_match() const { return matched ? static_cast<double>(turns) / static_cast<double>(matched) : 0.0; }
    double cache_hit_rate() const;  // cache reads / all prompt tokens, 0..1
};

struct FunctionSpend {
    u64 va = 0;
    std::string name;            // readable
    u32 size = 0;
    project::FunctionStatus status = project::FunctionStatus::unstarted;
    double stored_usd = 0;       // symbols.txt (every finished session)
    double live_usd = 0;         // the running session's, not stored yet
    int attempts = 0;

    double total_usd() const { return stored_usd + live_usd; }
};

struct CostReport {
    CostSlice total;                 // every run (key "all")
    std::vector<CostSlice> by_run;   // oldest first
    std::vector<CostSlice> by_day;   // oldest first (days start at midnight `utc_offset` east of UTC)
    std::vector<CostSlice> by_model; // by model and effort, most spent first; success rate = matched / worked
    std::vector<FunctionSpend> by_function;  // functions with spend, most first
    double function_spend_usd = 0;   // sum of by_function (stored + live)
    // Turns a fallback model served (part of) in the live run: RunStateData::fallback_turns. Run
    // summaries do not record them, so past runs are not counted.
    int fallback_turns = 0;
    // Serving models of the live run's sessions with no price-table row (their usage was priced at the
    // configured model's rates), from each session's latest serving model and from the runner's "no
    // price known for model" warnings in the log tail.
    std::vector<std::string> unpriced_models;
};

// `runs` from load_run_records() (merge_live_run() makes the live run's record current). O((functions
// in all runs + functions in the project) * log): 200 runs of 500 functions each over a project of
// 100,000 functions took about 80 ms in a Release build (tests/unit/viewmodel_cost_tests.cpp), so run
// it as a background job for large projects.
CostReport cost_report(const std::vector<RunRecord>& runs, const SymbolDb& symbols, const std::map<u64, project::FunctionInfo>& infos,
                       const events::RunStateData* live = nullptr, std::chrono::minutes utc_offset = {});

// Projected spend for the functions still to do (those a run takes by default: not matched, refused,
// skipped or library) from what functions of the same size cost so far.
struct CostProjection {
    struct Bucket {
        int bucket = 0;           // size bucket (common.hpp)
        usize observed = 0;       // functions with recorded spend in this bucket
        double mean_usd = 0;      // their mean spend (what a function cost over all its sessions)
        usize remaining = 0;      // functions still to do in this bucket
        double projected_usd = 0; // remaining * the bucket's estimate
        bool from_nearby = false; // no observations here: the nearest bucket's mean scaled by sqrt(size ratio)
    };
    std::vector<Bucket> buckets;  // ascending; buckets with observations or remaining functions
    usize remaining = 0;          // functions still to do (with a known size)
    usize unsized = 0;            // still to do but of unknown size (not projected)
    double projected_usd = 0;
    bool has_history = false;     // false: no function has recorded spend, so nothing is projected
};
// An observation is a function that is done (matched, gave up or refused) with recorded spend: its
// total over all sessions (symbols.txt `cost=`). Functions still to do are projected at their bucket's
// mean, so one already partly worked on is projected as if it started over. O(functions * log): about
// 20 ms for 100,000 functions in a Release build (tests/unit/viewmodel_cost_tests.cpp).
CostProjection project_remaining_cost(const SymbolDb& symbols, const std::map<u64, project::FunctionInfo>& infos);

} // namespace decomp::vm
