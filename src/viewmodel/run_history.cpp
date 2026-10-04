#include "viewmodel/run_history.hpp"

#include "core/fs.hpp"
#include "run/store.hpp"

#include <algorithm>

namespace decomp::vm {

namespace {

events::TokenUsage usage_from(const Json& j) {
    if (!j.is_object()) return {};
    return {json_int_or(j, "input_tokens", 0), json_int_or(j, "output_tokens", 0), json_int_or(j, "cache_creation_input_tokens", 0),
            json_int_or(j, "cache_read_input_tokens", 0)};
}

TimePoint time_field(const Json& j, const char* key) {
    if (auto t = parse_iso8601(json_string_or(j, key, ""))) return *t;
    return {};
}

} // namespace

long long RunRecord::turns() const {
    long long n = 0;
    for (const auto& f : functions) n += f.turns;
    return n;
}

RunRecord run_record_from_summary(const Json& summary) {
    RunRecord r;
    if (!summary.is_object()) return r;
    r.id = json_string_or(summary, "run", "");
    r.status = json_string_or(summary, "status", "");
    r.model = json_string_or(summary, "model", "");
    r.effort = json_string_or(summary, "effort", "");
    r.started = time_field(summary, "started");
    if (r.started == TimePoint{})
        if (auto t = run_id_time(r.id)) r.started = *t;
    r.finished = time_field(summary, "finished");
    r.cost_usd = json_number_or(summary, "cost_usd", 0);
    r.replay = json_bool_or(summary, "replay", false);
    bool has_usage = summary.contains("usage");
    r.usage = usage_from(summary.value("usage", Json::object()));
    if (auto list = summary.find("functions"); list != summary.end() && list->is_array()) {
        for (const auto& j : *list) {
            if (!j.is_object()) continue;
            RunFunctionRecord f;
            f.va = j.contains("va") && j["va"].is_number() ? j["va"].get<u64>() : 0;
            f.function = json_string_or(j, "function", "");
            f.display = json_string_or(j, "display", "");
            f.outcome = json_string_or(j, "outcome", "");
            f.matched = json_bool_or(j, "matched", f.outcome == "matched");
            f.best_match = json_number_or(j, "best_match", 0);
            f.turns = static_cast<int>(json_int_or(j, "turns", 0));
            f.sessions = static_cast<int>(json_int_or(j, "sessions", 1));
            f.cost_usd = json_number_or(j, "cost_usd", 0);
            f.usage = usage_from(j.value("usage", Json::object()));
            if (f.matched) ++r.matched;
            if (!has_usage) r.usage += f.usage;
            r.functions.push_back(std::move(f));
        }
    }
    std::ranges::sort(r.functions, {}, &RunFunctionRecord::va);
    return r;
}

RunRecord run_record_from_state(const events::RunStateData& state) {
    RunRecord r = run_record_from_summary(run::run_summary(state));
    if (r.started == TimePoint{}) r.started = state.started;
    if (r.finished == TimePoint{} && !run::is_live_status(state.status)) r.finished = state.ended;
    return r;
}

std::vector<RunRecord> load_run_records(const std::filesystem::path& runs_dir) {
    std::vector<RunRecord> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(runs_dir, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(runs_dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        auto text = fs::read_text(entry.path() / "summary.json");
        if (!text) continue;
        auto json = parse_json(*text);
        if (!json || !json->is_object()) continue;
        RunRecord r = run_record_from_summary(*json);
        r.dir = entry.path();
        if (r.id.empty()) r.id = fs::to_utf8(entry.path().filename());
        if (r.started == TimePoint{})
            if (auto t = run_id_time(r.id)) r.started = *t;
        if (std::filesystem::exists(entry.path() / "run.json", ec))
            if (auto info = run::read_run_info(entry.path())) r.status = info->status;
        out.push_back(std::move(r));
    }
    std::ranges::sort(out, [](const RunRecord& a, const RunRecord& b) { return a.started != b.started ? a.started < b.started : a.id < b.id; });
    return out;
}

void merge_live_run(std::vector<RunRecord>& records, const events::RunStateData& live) {
    if (live.run_id.empty()) return;
    RunRecord record = run_record_from_state(live);
    auto it = std::ranges::find(records, live.run_id, &RunRecord::id);
    if (it != records.end()) {
        record.dir = it->dir;
        *it = std::move(record);
        return;
    }
    auto pos = std::ranges::upper_bound(records, record.started, {}, &RunRecord::started);
    records.insert(pos, std::move(record));
}

} // namespace decomp::vm
