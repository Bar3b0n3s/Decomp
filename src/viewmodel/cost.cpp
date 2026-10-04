#include "viewmodel/cost.hpp"

#include "agent/cost.hpp"
#include "run/selection.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <set>

namespace decomp::vm {

using project::FunctionStatus;

double CostSlice::cache_hit_rate() const {
    const long long prompt = usage.input + usage.cache_write + usage.cache_read;
    return prompt ? static_cast<double>(usage.cache_read) / static_cast<double>(prompt) : 0.0;
}

namespace {

void add_run(CostSlice& slice, const RunRecord& r) {
    slice.cost_usd += r.cost_usd;
    slice.worked += r.worked();
    slice.matched += r.matched;
    slice.turns += r.turns();
    slice.usage += r.usage;
    ++slice.runs;
}

std::string model_key(const RunRecord& r) {
    if (r.model.empty()) return r.effort.empty() ? std::string("(unknown)") : "(unknown) " + r.effort;
    return r.effort.empty() ? r.model : r.model + " " + r.effort;
}

// "no price known for model 'X': ..." (the runner's warning) -> X.
std::optional<std::string> unpriced_model_in(std::string_view message) {
    constexpr std::string_view kPrefix = "no price known for model '";
    const usize at = message.find(kPrefix);
    if (at == std::string_view::npos) return std::nullopt;
    const usize start = at + kPrefix.size();
    const usize end = message.find('\'', start);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string(message.substr(start, end - start));
}

} // namespace

CostReport cost_report(const std::vector<RunRecord>& runs, const SymbolDb& symbols, const std::map<u64, project::FunctionInfo>& infos,
                       const events::RunStateData* live, std::chrono::minutes utc_offset) {
    CostReport report;
    report.total.key = "all";
    std::map<i64, CostSlice> days;
    std::map<std::string, CostSlice> models;
    for (const auto& r : runs) {
        add_run(report.total, r);
        CostSlice& run = report.by_run.emplace_back();
        run.key = r.id;
        run.time = r.started;
        add_run(run, r);
        const i64 day = day_number(r.started, utc_offset);
        CostSlice& d = days[day];
        if (d.key.empty()) {
            d.key = day_label(day);
            d.time = TimePoint(std::chrono::sys_days(std::chrono::days(day))) - utc_offset;
        }
        add_run(d, r);
        CostSlice& m = models[model_key(r)];
        m.key = model_key(r);
        add_run(m, r);
    }
    std::ranges::stable_sort(report.by_run, [](const CostSlice& a, const CostSlice& b) { return a.time < b.time; });
    for (auto& [day, slice] : days) report.by_day.push_back(std::move(slice));
    for (auto& [key, slice] : models) report.by_model.push_back(std::move(slice));
    std::ranges::stable_sort(report.by_model, [](const CostSlice& a, const CostSlice& b) { return a.cost_usd > b.cost_usd; });

    std::map<u64, const events::SessionState*> running;
    if (live) running = live_sessions(*live);
    std::map<u64, FunctionSpend> spend;
    auto entry = [&](u64 va) -> FunctionSpend& {
        FunctionSpend& f = spend[va];
        if (f.va == 0) {
            f.va = va;
            if (const Symbol* s = symbols.at(va)) {
                f.name = s->display.empty() ? s->name : s->display;
                f.size = s->size;
            } else {
                f.name = std::format("sub_{:x}", va);
            }
        }
        return f;
    };
    for (const auto& [va, info] : infos) {
        if (info.cost_usd <= 0) continue;
        FunctionSpend& f = entry(va);
        f.stored_usd = info.cost_usd;
        f.status = info.status;
        f.attempts = info.attempts;
    }
    for (const auto& [va, session] : running) {
        if (session->cost_usd <= 0) continue;
        FunctionSpend& f = entry(va);
        f.live_usd = session->cost_usd;
        if (auto it = infos.find(va); it != infos.end()) {
            f.status = it->second.status;
            f.attempts = it->second.attempts;
        }
        if (f.status != FunctionStatus::matched) f.status = FunctionStatus::in_progress;
        f.attempts += session->compiles;
    }
    for (auto& [va, f] : spend) {
        report.function_spend_usd += f.total_usd();
        report.by_function.push_back(std::move(f));
    }
    std::ranges::stable_sort(report.by_function, [](const FunctionSpend& a, const FunctionSpend& b) { return a.total_usd() > b.total_usd(); });

    if (live) {
        report.fallback_turns = live->fallback_turns;
        std::set<std::string> unpriced;
        for (const auto& [id, s] : live->sessions)
            if (!s->model.empty() && !agent::price_for(s->model).known) unpriced.insert(s->model);
        for (const auto& line : live->log_tail)
            if (auto model = unpriced_model_in(line->message)) unpriced.insert(*model);
        report.unpriced_models.assign(unpriced.begin(), unpriced.end());
    }
    return report;
}

CostProjection project_remaining_cost(const SymbolDb& symbols, const std::map<u64, project::FunctionInfo>& infos) {
    CostProjection p;
    std::map<int, std::vector<double>> observed;
    std::map<int, usize> remaining;
    for (const auto& [va, s] : symbols) {
        if (s.kind != SymbolKind::function) continue;
        auto it = infos.find(va);
        const project::FunctionInfo info = it == infos.end() ? project::FunctionInfo{} : it->second;
        const bool done = info.status == FunctionStatus::matched || info.status == FunctionStatus::gave_up || info.status == FunctionStatus::refused;
        if (done && info.cost_usd > 0 && s.size > 0) observed[size_bucket(s.size)].push_back(info.cost_usd);
        if (!run::runnable_by_default(info.status)) continue;
        if (s.size == 0) {
            ++p.unsized;
            continue;
        }
        ++remaining[size_bucket(s.size)];
        ++p.remaining;
    }
    p.has_history = !observed.empty();
    auto mean = [](const std::vector<double>& v) {
        double sum = 0;
        for (double x : v) sum += x;
        return sum / static_cast<double>(v.size());
    };
    std::set<int> buckets;
    for (const auto& [b, v] : observed) buckets.insert(b);
    for (const auto& [b, n] : remaining) buckets.insert(b);
    for (int b : buckets) {
        CostProjection::Bucket bucket;
        bucket.bucket = b;
        if (auto it = observed.find(b); it != observed.end()) {
            bucket.observed = it->second.size();
            bucket.mean_usd = mean(it->second);
        } else if (p.has_history) {
            // The nearest bucket with observations (the larger one on ties), scaled like the ETA model.
            for (int d = 1; d < 64 && !bucket.from_nearby; ++d)
                for (int nb : {b + d, b - d})
                    if (auto near = observed.find(nb); near != observed.end() && !bucket.from_nearby) {
                        bucket.mean_usd = mean(near->second) * std::pow(2.0, (b - nb) / 2.0);
                        bucket.from_nearby = true;
                    }
        }
        if (auto it = remaining.find(b); it != remaining.end()) bucket.remaining = it->second;
        bucket.projected_usd = static_cast<double>(bucket.remaining) * bucket.mean_usd;
        p.projected_usd += bucket.projected_usd;
        p.buckets.push_back(bucket);
    }
    return p;
}

} // namespace decomp::vm
