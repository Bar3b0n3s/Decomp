#include "gui/run_summary.hpp"

#include <cctype>

namespace decomp::gui {

RunSummary summarize(const events::RunStateData* s) {
    RunSummary r;
    if (!s) return r;
    r.status = s->status;
    r.run_id = s->run_id;
    if (s->status.empty()) r.phase = RunPhase::none;
    else if (s->status == "running") r.phase = RunPhase::running;
    else if (s->status == "paused") r.phase = RunPhase::paused;
    else if (s->status == "stopping") r.phase = RunPhase::stopping;
    else r.phase = RunPhase::finished;
    r.workers_total = s->worker_count;
    for (const auto& [id, w] : s->workers)
        if (!w.session.empty()) ++r.workers_active;
    r.planned = static_cast<int>(s->planned.size());
    r.finished = s->finished;
    r.matched = s->matched;
    r.cost_usd = s->cost_usd;
    r.cache_hit_rate = s->cache_hit_rate();
    r.errors = s->errors.size();
    if (!s->activity.empty()) r.last_activity = s->activity.back();
    return r;
}

std::string label(const RunSummary& run) {
    switch (run.phase) {
    case RunPhase::none: return "Idle";
    case RunPhase::running: return "Running";
    case RunPhase::paused: return "Paused";
    case RunPhase::stopping: return "Stopping";
    case RunPhase::finished: break;
    }
    std::string s = run.status.empty() ? std::string("Finished") : run.status;
    s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

} // namespace decomp::gui
