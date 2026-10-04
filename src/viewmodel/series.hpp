#pragma once

// Chart series derived from a run snapshot (Run monitor, Agent session, Cost and usage). x values are
// seconds relative to an origin the caller passes (typically RunStateData::started, so the axis reads
// "time into the run"); x = 0 is the origin. All functions are pure and linear in what they read; the
// per-minute ones read at most RunState::kMinutes (1440) entries, so they are cheap enough per frame.

#include "events/run_state.hpp"
#include "viewmodel/common.hpp"

#include <string>
#include <vector>

namespace decomp::vm {

// One entry per minute from the first to the last minute with activity (minutes without activity in
// between are present with zero counts), x = the minute's start - origin.
struct ThroughputSeries {
    std::vector<double> x;
    std::vector<double> turns, compiles, retries, rate_limited;  // per minute (rate_limited: retries after a 429)
    std::vector<double> output_tokens_per_second;                // the minute's output tokens / 60
    std::vector<double> mean_ttft_ms;    // mean time to first token of the minute's turns; NaN without turns
    std::vector<double> cost_usd;        // spend in the minute
    std::vector<double> cache_hit_rate;  // cache reads / all prompt tokens of the minute, 0..1; NaN without prompt tokens
};
ThroughputSeries throughput(const events::RunStateData& state, TimePoint origin);

// Cumulative spend at the end of each minute with activity (x = the minute's end - origin), ending at
// state.cost_usd. Minutes older than the snapshot keeps (RunState::kMinutes) are folded into the first
// value.
Series spend_over_time(const events::RunStateData& state, TimePoint origin);

// Cache-hit rate (0..1) per minute with prompt tokens (x = the minute's start - origin); `cumulative`:
// the rate over all minutes so far instead of the minute's own.
Series cache_hit_rate_over_time(const events::RunStateData& state, TimePoint origin, bool cumulative = false);

// Score per attempt of one session: x = attempt number (1-based), y = match percent of each diff.
Series score_per_attempt(const events::SessionState& session);
// The best score so far after each attempt (a non-decreasing step line over the same x).
Series best_score_per_attempt(const events::SessionState& session);

// The worker timeline (Gantt chart): every worker's phase spans that overlap a time window, clipped to
// it. Phase and function names are interned so segments stay small; the view maps phase indices to
// colors.
struct WorkerTimeline {
    struct Segment {
        double start = 0, end = 0;  // seconds relative to the origin, inside the window
        u32 phase = 0;              // index into `phases`
        u32 function = 0;           // index into `functions` (0 = none)
        bool open = false;          // the span is still going (its end is `now`, clipped)
    };
    struct Row {
        int worker = -1;
        std::vector<Segment> segments;  // in time order
    };
    double start = 0, end = 0;           // the window, relative to the origin
    std::vector<std::string> phases;     // distinct phase names, in order of first appearance
    std::vector<std::string> functions;  // distinct function names; functions[0] is ""
    std::vector<Row> rows;               // one per worker, by worker id
};
// Spans still open end at `now`. O(spans); a worker keeps at most RunState::kSpans spans.
WorkerTimeline worker_timeline(const events::RunStateData& state, TimePoint origin, TimePoint window_start, TimePoint window_end,
                               TimePoint now);

} // namespace decomp::vm
