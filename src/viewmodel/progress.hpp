#pragma once

// The Dashboard's progress numbers: progress by code bytes and by functions, the status buckets, the
// best-match distribution of non-matching functions, and progress over time from past runs.

#include "analysis/symbols.hpp"
#include "events/run_state.hpp"
#include "project/progress.hpp"
#include "project/project.hpp"
#include "viewmodel/common.hpp"
#include "viewmodel/run_history.hpp"

#include <array>
#include <chrono>
#include <vector>

namespace decomp::vm {

// Segment order of the progress bars: done (matched, then library and skipped as separate segments),
// work in flight and tried, then untouched.
inline constexpr std::array<project::FunctionStatus, 8> kStatusOrder = {
    project::FunctionStatus::matched,
    project::FunctionStatus::library,
    project::FunctionStatus::skipped,
    project::FunctionStatus::nonmatching,
    project::FunctionStatus::in_progress,
    project::FunctionStatus::gave_up,
    project::FunctionStatus::refused,
    project::FunctionStatus::unstarted,
};

struct StatusSegment {
    project::FunctionStatus status = project::FunctionStatus::unstarted;
    usize functions = 0;
    u64 bytes = 0;                  // functions of unknown size count 0 bytes
    double function_share = 0;      // of all functions, 0..1
    double byte_share = 0;          // of all code bytes, 0..1
};

struct DashboardProgress {
    // project::compute_progress(): exactly what `decomp status` prints (in_progress is never stored).
    project::Progress stored;
    // Every status in kStatusOrder with its count and bytes after the live overlay: a function with an
    // unfinished session counts as in_progress (unless it is matched). Without a live run these equal
    // stored.buckets.
    std::array<StatusSegment, kStatusOrder.size()> segments{};
    usize running = 0;  // functions the overlay moved to in_progress
    // Non-matching functions (after the overlay) by best match: bin i holds [10i, 10i + 10) percent,
    // the last bin [90, 100]. The bins add up to the nonmatching segment's count.
    std::array<usize, 10> best_match_bins{};

    const StatusSegment& segment(project::FunctionStatus status) const;
};

// The stored numbers and the per-function states come from the same version of symbols.txt (it is
// read again if another writer changes it meanwhile). Cost: compute_progress() (one locked lookup per
// function), one pass over the stored function states and one lookup per live session: about 12 ms
// for 100,000 functions in a Release build (tests/unit/viewmodel_progress_tests.cpp). Run it as a
// background job for large targets.
DashboardProgress dashboard_progress(const SymbolDb& symbols, const project::Project& project,
                                     const events::RunStateData* live = nullptr);

// Progress over time, from what each run matched (run summaries). A function counts as new in the
// first run (or day) that matched it; later matches of the same function (after a reset) count in the
// per-run totals but not as new. Matches made outside runs (by hand) do not appear.
struct ProgressPoint {
    TimePoint time{};           // run start, or the start of the day (shifted by the UTC offset)
    std::string label;          // run id, or "2026-10-04"
    usize functions = 0;        // functions matched in this run or day
    u64 bytes = 0;              // their sizes (from the symbol database)
    usize new_functions = 0;    // of those, matched for the first time
    u64 new_bytes = 0;
    usize total_functions = 0;  // distinct functions matched up to and including this point
    u64 total_bytes = 0;
    double cost_usd = 0;        // spend of the run, or of the runs started that day
};

struct ProgressHistory {
    std::vector<ProgressPoint> runs;  // one per run, oldest first
    std::vector<ProgressPoint> days;  // one per day with a run, oldest first
    // Cumulative series ready to plot: x in seconds since the Unix epoch (ImPlot's time axis), y the
    // distinct functions or bytes matched so far.
    Series run_functions, run_bytes, day_functions, day_bytes;
};

// `runs` from load_run_records() (taken in start-time order whatever their order). Days start at
// midnight in the time zone `utc_offset` east of UTC. O(functions in all runs * log).
ProgressHistory progress_history(const std::vector<RunRecord>& runs, const SymbolDb& symbols, std::chrono::minutes utc_offset = {});

} // namespace decomp::vm
