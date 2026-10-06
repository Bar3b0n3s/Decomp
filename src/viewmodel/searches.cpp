#include "viewmodel/searches.hpp"

#include "core/strings.hpp"

#include <algorithm>

namespace decomp::vm {

namespace {

std::vector<std::string> strings_of(const Json& j, std::string_view key) {
    std::vector<std::string> out;
    if (auto it = j.find(key); it != j.end() && it->is_array())
        for (const auto& s : *it)
            if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

search::Score score_of(const Json& j, std::string_view key) {
    auto it = j.find(key);
    return it == j.end() ? search::Score{} : search::score_from_json(*it);
}

} // namespace

SearchRunRow search_run_row(const search::RunRecord& run) {
    SearchRunRow r;
    r.id = run.id;
    r.kind = std::string(search::to_string(run.kind));
    r.status = std::string(search::to_string(run.status));
    r.target = run.target;
    r.started = run.started;
    r.candidates = run.candidates;
    r.best = run.best ? run.best->text() : "-";
    r.best_label = run.best_label;
    r.complete = run.best && run.best->complete();
    r.seconds = static_cast<double>(run.duration_ms) / 1000.0;
    return r;
}

std::string FlagGroupRow::chosen_text() const { return chosen < alternatives.size() ? alternatives[chosen] : std::string(); }

std::string FlagGroupRow::also_text() const {
    std::vector<std::string> also;
    for (usize a : equivalent)
        if (a != chosen && a < alternatives.size()) also.push_back(alternatives[a]);
    return join(also, ", ");
}

FlagSearchView read_flag_search(const Json& result) {
    FlagSearchView v;
    if (!result.is_object()) return v;
    v.flags = strings_of(result, "flags");
    v.base = strings_of(result, "base");
    if (auto groups = result.find("groups"); groups != result.end() && groups->is_array())
        for (const auto& g : *groups) {
            FlagGroupRow row;
            row.name = g.value("name", std::string());
            row.alternatives = strings_of(g, "alternatives");
            row.start = g.value("start", usize{0});
            row.chosen = g.value("chosen", usize{0});
            if (auto e = g.find("equivalent"); e != g.end() && e->is_array())
                for (const auto& a : *e)
                    if (a.is_number_unsigned()) row.equivalent.push_back(a.get<usize>());
            v.groups.push_back(std::move(row));
        }
    v.score = score_of(result, "score");
    v.start_score = score_of(result, "start_score");
    v.candidates = result.value("candidates", usize{0});
    v.space = result.value("space", usize{0});
    v.exhaustive = result.value("exhaustive", false);
    v.cancelled = result.value("cancelled", false);
    return v;
}

PermuteView read_permute(const Json& result) {
    PermuteView v;
    if (!result.is_object()) return v;
    v.steps = strings_of(result, "steps");
    v.score = score_of(result, "score");
    v.start_score = score_of(result, "start_score");
    v.candidates = result.value("candidates", usize{0});
    v.cancelled = result.value("cancelled", false);
    v.error = result.value("error", std::string());
    return v;
}

IdentifyView read_identify(const Json& result) {
    IdentifyView v;
    if (!result.is_object()) return v;
    if (auto ranking = result.find("ranking"); ranking != result.end() && ranking->is_array())
        for (const auto& t : *ranking) {
            ToolchainRow row;
            row.toolchain = t.value("toolchain", std::string());
            row.kind = t.value("kind", std::string());
            row.flags = join(strings_of(t, "flags"), " ");
            row.error = t.value("error", std::string());
            row.score = score_of(t, "score");
            v.ranking.push_back(std::move(row));
        }
    v.candidates = result.value("candidates", usize{0});
    v.decided = result.value("decided", false);
    v.cancelled = result.value("cancelled", false);
    return v;
}

std::vector<SearchLogRow> search_log_rows(std::span<const search::LogEntry> entries, bool improvements) {
    std::vector<SearchLogRow> out;
    for (const auto& e : entries) {
        if (improvements && !e.best) continue;
        SearchLogRow r;
        r.index = e.index;
        r.seconds = static_cast<double>(e.ms) / 1000.0;
        r.label = e.label;
        r.score = e.score.text();
        r.best = e.best;
        r.complete = e.score.complete();
        out.push_back(std::move(r));
    }
    return out;
}

Series best_so_far(std::span<const search::LogEntry> entries) {
    Series s;
    std::optional<search::Score> best;
    for (const auto& e : entries) {
        if (!best || e.score.better_than(*best)) best = e.score;
        s.push(static_cast<double>(e.index), best->match_percent);
    }
    return s;
}

} // namespace decomp::vm
