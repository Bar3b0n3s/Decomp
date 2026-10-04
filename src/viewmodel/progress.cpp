#include "viewmodel/progress.hpp"

#include <algorithm>
#include <set>

namespace decomp::vm {

using project::FunctionInfo;
using project::FunctionStatus;

const StatusSegment& DashboardProgress::segment(FunctionStatus status) const {
    for (const auto& s : segments)
        if (s.status == status) return s;
    return segments.back();
}

namespace {

usize best_bin(double best) { return static_cast<usize>(std::clamp(static_cast<int>(best / 10.0), 0, 9)); }

} // namespace

DashboardProgress dashboard_progress(const SymbolDb& symbols, const project::Project& project, const events::RunStateData* live) {
    DashboardProgress p;
    std::shared_ptr<const std::map<u64, FunctionInfo>> infos;
    // The stored numbers and the per-function states must describe the same version of symbols.txt.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const u64 version = project.version();
        infos = project.function_infos();
        p.stored = project::compute_progress(symbols, project);
        if (project.version() == version) break;
    }

    std::map<FunctionStatus, project::Progress::Bucket> buckets = p.stored.buckets;
    for (const auto& [va, info] : *infos) {
        if (info.status != FunctionStatus::nonmatching) continue;
        const Symbol* s = symbols.at(va);
        if (s && s->kind == SymbolKind::function) ++p.best_match_bins[best_bin(info.best_match)];
    }
    if (live) {
        for (const auto& [va, session] : live_sessions(*live)) {
            const Symbol* s = symbols.at(va);
            if (!s || s->kind != SymbolKind::function) continue;
            auto it = infos->find(va);
            const FunctionInfo info = it == infos->end() ? FunctionInfo{} : it->second;
            if (info.status == FunctionStatus::matched || info.status == FunctionStatus::in_progress) continue;
            auto& from = buckets[info.status];
            if (from.functions == 0) continue;  // symbols.txt changed under us; the next computation catches up
            --from.functions;
            from.bytes -= std::min<u64>(from.bytes, s->size);
            auto& to = buckets[FunctionStatus::in_progress];
            ++to.functions;
            to.bytes += s->size;
            if (info.status == FunctionStatus::nonmatching) {
                auto& bin = p.best_match_bins[best_bin(info.best_match)];
                if (bin > 0) --bin;
            }
            ++p.running;
        }
    }
    for (usize i = 0; i < kStatusOrder.size(); ++i) {
        auto& seg = p.segments[i];
        seg.status = kStatusOrder[i];
        if (auto it = buckets.find(seg.status); it != buckets.end()) {
            seg.functions = it->second.functions;
            seg.bytes = it->second.bytes;
        }
        seg.function_share = p.stored.functions ? static_cast<double>(seg.functions) / static_cast<double>(p.stored.functions) : 0.0;
        seg.byte_share = p.stored.code_bytes ? static_cast<double>(seg.bytes) / static_cast<double>(p.stored.code_bytes) : 0.0;
    }
    return p;
}

ProgressHistory progress_history(const std::vector<RunRecord>& runs, const SymbolDb& symbols, std::chrono::minutes utc_offset) {
    ProgressHistory h;
    std::vector<const RunRecord*> ordered;
    for (const auto& r : runs) ordered.push_back(&r);
    std::ranges::stable_sort(ordered, [](const RunRecord* a, const RunRecord* b) { return a->started < b->started; });

    auto size_of = [&](u64 va) -> u64 {
        const Symbol* s = symbols.at(va);
        return s ? s->size : 0;
    };
    std::set<u64> ever;  // functions matched so far
    usize total_functions = 0;
    u64 total_bytes = 0;
    std::map<i64, ProgressPoint> days;
    std::map<i64, std::set<u64>> matched_on_day;
    for (const RunRecord* r : ordered) {
        ProgressPoint point;
        point.time = r->started;
        point.label = r->id;
        point.cost_usd = r->cost_usd;
        const i64 day = day_number(r->started, utc_offset);
        auto& d = days[day];
        if (d.label.empty()) {
            d.label = day_label(day);
            d.time = TimePoint(std::chrono::sys_days(std::chrono::days(day))) - utc_offset;
        }
        d.cost_usd += r->cost_usd;
        for (const auto& f : r->functions) {
            if (!f.matched) continue;
            const u64 bytes = size_of(f.va);
            ++point.functions;
            point.bytes += bytes;
            const bool first = ever.insert(f.va).second;
            if (first) {
                ++point.new_functions;
                point.new_bytes += bytes;
                ++d.new_functions;
                d.new_bytes += bytes;
                ++total_functions;
                total_bytes += bytes;
            }
            if (matched_on_day[day].insert(f.va).second) {
                ++d.functions;
                d.bytes += bytes;
            }
        }
        point.total_functions = total_functions;
        point.total_bytes = total_bytes;
        h.run_functions.push(to_unix_seconds(point.time), static_cast<double>(point.total_functions));
        h.run_bytes.push(to_unix_seconds(point.time), static_cast<double>(point.total_bytes));
        h.runs.push_back(std::move(point));
    }
    usize day_functions = 0;
    u64 day_bytes = 0;
    for (auto& [day, d] : days) {
        day_functions += d.new_functions;
        day_bytes += d.new_bytes;
        d.total_functions = day_functions;
        d.total_bytes = day_bytes;
        h.day_functions.push(to_unix_seconds(d.time), static_cast<double>(d.total_functions));
        h.day_bytes.push(to_unix_seconds(d.time), static_cast<double>(d.total_bytes));
        h.days.push_back(std::move(d));
    }
    return h;
}

} // namespace decomp::vm
