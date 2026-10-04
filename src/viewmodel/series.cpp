#include "viewmodel/series.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>

namespace decomp::vm {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

TimePoint minute_start(i64 minute) { return TimePoint(std::chrono::minutes(minute)); }

double prompt_tokens(const events::MinuteStats& m) {
    return static_cast<double>(m.input_tokens + m.cache_write_tokens + m.cache_read_tokens);
}

} // namespace

ThroughputSeries throughput(const events::RunStateData& state, TimePoint origin) {
    ThroughputSeries t;
    if (state.minutes.empty()) return t;
    const i64 first = state.minutes.begin()->first, last = state.minutes.rbegin()->first;
    for (i64 m = first; m <= last; ++m) {
        auto it = state.minutes.find(m);
        const events::MinuteStats stats = it == state.minutes.end() ? events::MinuteStats{} : it->second;
        t.x.push_back(seconds_between(origin, minute_start(m)));
        t.turns.push_back(stats.turns);
        t.compiles.push_back(stats.compiles);
        t.retries.push_back(stats.retries);
        t.rate_limited.push_back(stats.rate_limited);
        t.output_tokens_per_second.push_back(static_cast<double>(stats.output_tokens) / 60.0);
        t.mean_ttft_ms.push_back(stats.ttft_count ? static_cast<double>(stats.ttft_sum_ms) / stats.ttft_count : kNaN);
        t.cost_usd.push_back(stats.cost_usd);
        const double prompt = prompt_tokens(stats);
        t.cache_hit_rate.push_back(prompt > 0 ? static_cast<double>(stats.cache_read_tokens) / prompt : kNaN);
    }
    return t;
}

Series spend_over_time(const events::RunStateData& state, TimePoint origin) {
    Series s;
    double in_minutes = 0;
    for (const auto& [m, stats] : state.minutes) in_minutes += stats.cost_usd;
    double total = std::max(0.0, state.cost_usd - in_minutes);  // spend of minutes no longer kept
    for (const auto& [m, stats] : state.minutes) {
        total += stats.cost_usd;
        s.push(seconds_between(origin, minute_start(m + 1)), total);
    }
    return s;
}

Series cache_hit_rate_over_time(const events::RunStateData& state, TimePoint origin, bool cumulative) {
    Series s;
    double read = 0, prompt = 0;
    for (const auto& [m, stats] : state.minutes) {
        const double minute_prompt = prompt_tokens(stats);
        if (minute_prompt <= 0) continue;
        read += static_cast<double>(stats.cache_read_tokens);
        prompt += minute_prompt;
        const double rate = cumulative ? read / prompt : static_cast<double>(stats.cache_read_tokens) / minute_prompt;
        s.push(seconds_between(origin, minute_start(m)), rate);
    }
    return s;
}

Series score_per_attempt(const events::SessionState& session) {
    Series s;
    for (usize i = 0; i < session.scores.size(); ++i) s.push(static_cast<double>(i + 1), session.scores[i]);
    return s;
}

Series best_score_per_attempt(const events::SessionState& session) {
    Series s;
    double best = 0;
    for (usize i = 0; i < session.scores.size(); ++i) {
        best = std::max(best, session.scores[i]);
        s.push(static_cast<double>(i + 1), best);
    }
    return s;
}

WorkerTimeline worker_timeline(const events::RunStateData& state, TimePoint origin, TimePoint window_start, TimePoint window_end,
                               TimePoint now) {
    WorkerTimeline t;
    t.start = seconds_between(origin, window_start);
    t.end = seconds_between(origin, window_end);
    t.functions.emplace_back();
    std::map<std::string, u32, std::less<>> phase_index, function_index;
    function_index.emplace("", 0);
    auto intern = [](std::map<std::string, u32, std::less<>>& index, std::vector<std::string>& names, const std::string& name) {
        auto it = index.find(name);
        if (it != index.end()) return it->second;
        const auto id = static_cast<u32>(names.size());
        names.push_back(name);
        index.emplace(name, id);
        return id;
    };
    for (const auto& [id, worker] : state.workers) {
        WorkerTimeline::Row row;
        row.worker = id;
        for (const auto& span : worker.spans) {
            const bool open = span.end == TimePoint{};
            const TimePoint end = open ? now : span.end;
            if (end <= window_start || span.start >= window_end) continue;
            WorkerTimeline::Segment seg;
            seg.start = seconds_between(origin, std::max(span.start, window_start));
            seg.end = seconds_between(origin, std::min(end, window_end));
            seg.phase = intern(phase_index, t.phases, span.phase);
            seg.function = intern(function_index, t.functions, span.function);
            seg.open = open;
            row.segments.push_back(seg);
        }
        t.rows.push_back(std::move(row));
    }
    return t;
}

} // namespace decomp::vm
