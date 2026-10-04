#pragma once

// How long functions take: a model of observed session durations by function size, and the ETA of a
// run's queue (the Run monitor's queue column and the status bar).

#include "analysis/symbols.hpp"
#include "core/result.hpp"
#include "events/run_state.hpp"
#include "viewmodel/common.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace decomp::vm {

// Session durations by size bucket (common.hpp: log2 buckets of the function's size). Only sessions
// that ended on their own terms count (is_complete_outcome()); stopped, aborted, skipped, interrupted
// and failed sessions were cut short and would make functions look faster than they are.
class DurationModel {
public:
    enum class Source : u8 {
        bucket,  // the median of this size bucket's sessions
        nearby,  // the nearest bucket with sessions (larger on ties), scaled by sqrt(size ratio)
        none,    // no history at all: default_seconds()
    };
    struct Estimate {
        double seconds = 0;
        Source source = Source::none;
        int bucket = 0;      // the bucket the estimate comes from
        usize samples = 0;   // sessions behind it
    };

    void add(u64 bytes, double seconds);
    // Finished sessions of a run (live or replayed from its event log) with their wall-clock duration
    // (session_finished - session_started); sizes from `symbols` (sessions of functions without a
    // known size are skipped). O(sessions).
    void add_run(const events::RunStateData& state, const SymbolDb& symbols);

    usize samples() const;
    usize samples_in(int bucket) const;
    std::optional<double> median(int bucket) const;
    Estimate estimate(u64 bytes) const;

    // The estimate without history: 60 s * (1 + log2(1 + bytes / 16)). About 2 minutes for a 16-byte
    // function, 3.3 minutes for 64 bytes, 7 minutes for 1 KiB and 11 minutes for 16 KiB.
    static double default_seconds(u64 bytes);

private:
    std::vector<std::vector<double>> buckets_;  // sorted durations per bucket
};

// Adds the sessions recorded in a past run's events.jsonl. Only session_started and session_finished
// lines are parsed (the others are skipped by a substring test): a 36 MB log with 2,000 sessions took
// about 130 ms in a Release build (tests/unit/viewmodel_eta_tests.cpp), mostly reading the file. Call
// it from a background job (for example for the last few runs).
Result<void> add_event_log(DurationModel& model, const std::filesystem::path& events_jsonl, const SymbolDb& symbols);

// The concurrency in effect: the latest acknowledged set_concurrency command, else the run's worker
// count (RunStateData::worker_count is the count the run started with).
int current_concurrency(const events::RunStateData& state);

struct QueueEta {
    struct Running {
        int worker = -1;
        std::string session;
        u64 va = 0;
        double elapsed = 0;    // seconds since the session started
        double remaining = 0;  // estimate - elapsed, but at least a tenth of the estimate
        DurationModel::Estimate estimate;
    };
    struct Item {
        u64 va = 0;
        std::string function;
        double start = 0, finish = 0;  // seconds from `now`, when a worker is expected to start and finish it
        DurationModel::Estimate estimate;
    };
    std::vector<Running> running;  // sessions in progress
    std::vector<Item> items;       // the queue, in dispatch order
    double finish = 0;             // seconds from `now` until the last queued function is done
    int workers = 0;
};

// Simulates the queue: every worker first finishes its running session, then takes the next queued
// function in dispatch order as soon as it is free (greedy list scheduling, the way the controller
// dispatches). `workers` < 1 uses current_concurrency(state). O(queue * log workers). Without a live
// run (no running sessions) it is the time to work through the queue from now.
QueueEta estimate_queue(const DurationModel& model, const events::RunStateData& state, const SymbolDb& symbols, TimePoint now,
                        int workers = 0);

} // namespace decomp::vm
