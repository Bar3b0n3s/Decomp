#pragma once

// The few run figures the chrome shows (top bar, status bar), derived from a snapshot. This is the only
// place the shell reads RunStateData fields, so changes to RunState stay local.

#include "core/types.hpp"
#include "events/run_state.hpp"

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

struct RunSummary {
    RunPhase phase = RunPhase::none;
    std::string status;  // RunStateData::status as recorded ("running", "completed", ...)
    std::string run_id;
    int workers_active = 0;  // workers with a session
    int workers_total = 0;
    int planned = 0, finished = 0, matched = 0;
    double cost_usd = 0;
    double cache_hit_rate = 0;  // 0..1
    usize errors = 0;
    std::string last_activity;  // the newest activity line
};

RunSummary summarize(const events::RunStateData* snapshot);
// "Idle", "Running", "Paused", "Stopping", "Completed", "Stopped", "Aborted", "Error".
std::string label(const RunSummary& run);

} // namespace decomp::gui
