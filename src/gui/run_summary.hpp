#pragma once

// The few run figures the chrome shows (top bar, status bar), derived from a snapshot. This is the only
// place the shell reads RunStateData fields, so changes to RunState stay local.

#include "core/types.hpp"
#include "events/run_state.hpp"

#include <chrono>
#include <string>
#include <string_view>

namespace decomp::gui {

enum class RunPhase {
    none,      // no run loaded
    running,
    paused,
    stopping,
    finished,  // completed, stopped, aborted or error (RunSummary::status says which)
};

// The API light in the top bar.
enum class ApiHealth {
    unknown,    // no request answered yet
    ok,         // answering, no recent retries
    degraded,   // retries in the last minutes, or held back by the rate limit
    failing,    // authentication or permission errors
};

struct RunSummary {
    RunPhase phase = RunPhase::none;
    std::string status;  // RunStateData::status as recorded ("running", "completed", ...)
    std::string run_id;
    int workers_active = 0;  // workers with a session
    int workers_total = 0;
    int planned = 0, finished = 0, matched = 0;
    usize queued = 0;  // pending functions
    double cost_usd = 0;
    double budget_usd = 0;      // the run budget; 0 = none
    double cache_hit_rate = 0;  // 0..1
    usize errors = 0;
    std::string last_activity;  // the newest activity line
    std::string last_error;

    ApiHealth api = ApiHealth::unknown;
    long long last_ttft_ms = 0;  // mean time to first token in the latest minute with turns
    int recent_retries = 0;      // in the last five minutes
    bool rate_known = false;     // rate-limit headers seen
    long long requests_remaining = -1, requests_limit = -1;
    long long input_remaining = -1, input_limit = -1;
    long long output_remaining = -1, output_limit = -1;
    long long backoff_left_ms = 0;  // requests held back for this long from now
};

RunSummary summarize(const events::RunStateData* snapshot,
                     std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
// "Idle", "Running", "Paused", "Stopping", "Completed", "Stopped", "Aborted", "Error".
std::string label(const RunSummary& run);

} // namespace decomp::gui
