#include "viewmodel/eta.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>

namespace decomp::vm {

void DurationModel::add(u64 bytes, double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) return;
    const auto b = static_cast<usize>(size_bucket(bytes));
    if (buckets_.size() <= b) buckets_.resize(b + 1);
    auto& list = buckets_[b];
    list.insert(std::ranges::upper_bound(list, seconds), seconds);
}

void DurationModel::add_run(const events::RunStateData& state, const SymbolDb& symbols) {
    for (const auto& [id, s] : state.sessions) {
        if (!s->finished || !is_complete_outcome(s->outcome)) continue;
        if (s->started == TimePoint{} || s->ended == TimePoint{}) continue;
        const Symbol* sym = symbols.at(s->va);
        if (!sym || sym->size == 0) continue;
        add(sym->size, seconds_between(s->started, s->ended));
    }
}

usize DurationModel::samples() const {
    usize n = 0;
    for (const auto& b : buckets_) n += b.size();
    return n;
}

usize DurationModel::samples_in(int bucket) const {
    return bucket >= 0 && static_cast<usize>(bucket) < buckets_.size() ? buckets_[static_cast<usize>(bucket)].size() : 0;
}

std::optional<double> DurationModel::median(int bucket) const {
    if (samples_in(bucket) == 0) return std::nullopt;
    const auto& list = buckets_[static_cast<usize>(bucket)];
    const usize n = list.size();
    return n % 2 ? list[n / 2] : (list[n / 2 - 1] + list[n / 2]) / 2;
}

DurationModel::Estimate DurationModel::estimate(u64 bytes) const {
    const int b = size_bucket(bytes);
    if (auto m = median(b)) return {*m, Source::bucket, b, samples_in(b)};
    for (int d = 1; d < 64; ++d) {
        for (int nb : {b + d, b - d}) {
            if (auto m = median(nb)) return {*m * std::pow(2.0, (b - nb) / 2.0), Source::nearby, nb, samples_in(nb)};
        }
    }
    return {default_seconds(bytes), Source::none, b, 0};
}

double DurationModel::default_seconds(u64 bytes) { return 60.0 * (1.0 + std::log2(1.0 + static_cast<double>(bytes) / 16.0)); }

Result<void> add_event_log(DurationModel& model, const std::filesystem::path& events_jsonl, const SymbolDb& symbols) {
    TRY_ASSIGN(auto text, fs::read_text(events_jsonl));
    events::RunState state;
    std::string_view rest = text;
    while (!rest.empty()) {
        const usize nl = rest.find('\n');
        const std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        if (line.find("\"session_started\"") == std::string_view::npos && line.find("\"session_finished\"") == std::string_view::npos)
            continue;
        auto json = parse_json(line);
        if (!json) continue;
        auto event = events::event_from_json(*json);
        if (!event) continue;
        if (std::holds_alternative<events::SessionStarted>(event->payload) || std::holds_alternative<events::SessionFinished>(event->payload))
            state.apply(*event);
    }
    model.add_run(state.data(), symbols);
    return {};
}

int current_concurrency(const events::RunStateData& state) {
    for (auto it = state.controls.rbegin(); it != state.controls.rend(); ++it) {
        if (it->control.command != "set_concurrency" || !it->control.target.empty()) continue;
        if (auto n = parse_u64(it->control.detail); n && *n > 0 && *n < 1000) return static_cast<int>(*n);
    }
    return state.worker_count;
}

QueueEta estimate_queue(const DurationModel& model, const events::RunStateData& state, const SymbolDb& symbols, TimePoint now, int workers) {
    QueueEta q;
    q.workers = std::max(1, workers >= 1 ? workers : current_concurrency(state));
    auto size_of = [&](u64 va) -> u64 {
        const Symbol* s = symbols.at(va);
        return s ? s->size : 0;
    };
    std::vector<double> busy_until;
    for (const auto& [id, s] : state.sessions) {
        if (s->finished) continue;
        QueueEta::Running r;
        r.worker = s->worker;
        r.session = s->id;
        r.va = s->va;
        r.estimate = model.estimate(size_of(s->va));
        r.elapsed = std::max(0.0, seconds_between(s->started, now));
        r.remaining = std::max(r.estimate.seconds - r.elapsed, 0.1 * r.estimate.seconds);
        busy_until.push_back(r.remaining);
        q.finish = std::max(q.finish, r.remaining);
        q.running.push_back(std::move(r));
    }
    // With more running sessions than workers (concurrency was lowered), the first to finish keep
    // working and the others retire.
    std::ranges::sort(busy_until);
    std::priority_queue<double, std::vector<double>, std::greater<>> free_at;
    for (int w = 0; w < q.workers; ++w) free_at.push(static_cast<usize>(w) < busy_until.size() ? busy_until[static_cast<usize>(w)] : 0.0);
    for (const auto& entry : *state.queue) {
        QueueEta::Item item;
        item.va = entry.va;
        item.function = entry.function;
        item.estimate = model.estimate(size_of(entry.va));
        item.start = free_at.top();
        free_at.pop();
        item.finish = item.start + item.estimate.seconds;
        free_at.push(item.finish);
        q.finish = std::max(q.finish, item.finish);
        q.items.push_back(std::move(item));
    }
    return q;
}

} // namespace decomp::vm
