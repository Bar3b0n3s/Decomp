#include "gui/run_summary.hpp"

#include <algorithm>
#include <cctype>

namespace decomp::gui {

RunSummary summarize(const events::RunStateData* s, std::chrono::system_clock::time_point now) {
    RunSummary r;
    if (!s) return r;
    r.status = s->status;
    r.run_id = s->run_id;
    if (s->status.empty()) r.phase = RunPhase::none;
    else if (s->status == "running") r.phase = RunPhase::running;
    else if (s->status == "paused") r.phase = RunPhase::paused;
    else if (s->status == "stopping" || s->status == "aborting") r.phase = RunPhase::stopping;
    else r.phase = RunPhase::finished;
    r.workers_total = s->worker_count;
    for (const auto& [id, w] : s->workers)
        if (!w.session.empty()) ++r.workers_active;
    r.planned = static_cast<int>(std::max(s->planned.size(), s->planned_vas.size()));
    r.finished = s->finished;
    r.matched = s->matched;
    r.queued = s->queue ? s->queue->size() : 0;
    r.cost_usd = s->cost_usd;
    r.budget_usd = s->budget.run.usd;
    r.cache_hit_rate = s->cache_hit_rate();
    r.errors = s->errors.size();
    if (!s->activity.empty()) r.last_activity = s->activity.back();
    if (!s->errors.empty()) r.last_error = s->errors.back();

    // API health from what the reducer kept: per-minute retries and turns, error lines, rate limits.
    const i64 this_minute = std::chrono::duration_cast<std::chrono::minutes>(now.time_since_epoch()).count();
    bool answered = false;
    for (auto it = s->minutes.rbegin(); it != s->minutes.rend(); ++it) {
        if (it->second.turns > 0) {
            answered = true;
            if (r.last_ttft_ms == 0 && it->second.ttft_count > 0) r.last_ttft_ms = it->second.ttft_sum_ms / it->second.ttft_count;
        }
        if (it->first >= this_minute - 4) r.recent_retries += it->second.retries;
    }
    if (s->rate_limit.known) {
        const auto& l = s->rate_limit.last;
        r.rate_known = true;
        r.requests_remaining = l.requests_remaining;
        r.requests_limit = l.requests_limit;
        r.input_remaining = l.input_tokens_remaining;
        r.input_limit = l.input_tokens_limit;
        r.output_remaining = l.output_tokens_remaining;
        r.output_limit = l.output_tokens_limit;
        const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - s->rate_limit.time).count();
        r.backoff_left_ms = std::max<long long>(0, l.backoff_ms - since);
    }
    bool auth_error = false;
    for (auto it = s->errors.rbegin(); it != s->errors.rend() && it - s->errors.rbegin() < 5; ++it)
        if (it->find("authentication_error") != std::string::npos || it->find("permission_error") != std::string::npos ||
            it->find("HTTP 401") != std::string::npos || it->find("HTTP 403") != std::string::npos)
            auth_error = true;
    if (auth_error) r.api = ApiHealth::failing;
    else if (r.recent_retries > 0 || r.backoff_left_ms > 0) r.api = ApiHealth::degraded;
    else if (answered || s->usage.total() > 0) r.api = ApiHealth::ok;
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
