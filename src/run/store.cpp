#include "run/store.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "run/queue.hpp"

#include <algorithm>
#include <format>
#include <map>

namespace decomp::run {

namespace {

std::string iso(events::TimePoint t) {
    if (t == events::TimePoint{}) return {};
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(t));
}

Json usage_json(const events::TokenUsage& u) {
    return Json{{"input_tokens", u.input},
                {"output_tokens", u.output},
                {"cache_creation_input_tokens", u.cache_write},
                {"cache_read_input_tokens", u.cache_read}};
}

} // namespace

bool is_live_status(std::string_view status) {
    return status == "running" || status == "paused" || status == "stopping" || status == "aborting" || status == "starting";
}

Result<RunStore> RunStore::create(const std::filesystem::path& runs_dir, const std::string& run_id) {
    RunStore store;
    store.id_ = run_id;
    store.dir_ = runs_dir / fs::from_utf8(run_id);
    std::error_code ec;
    if (std::filesystem::exists(store.dir_ / "run.json", ec)) return make_error(ErrorCode::invalid_argument, "run {} already exists", run_id);
    TRY(fs::create_directories(store.sessions_dir()));
    TRY_ASSIGN(auto lock, FileLock::try_acquire(store.dir_ / "run.lock", FileLock::Mode::exclusive));
    if (!lock) return make_error(ErrorCode::invalid_argument, "run {} is in use by another process", run_id);
    store.lock_ = std::move(*lock);
    return store;
}

Result<RunStore> RunStore::open(const std::filesystem::path& dir) {
    RunStore store;
    store.dir_ = dir;
    store.id_ = fs::to_utf8(dir.filename());
    std::error_code ec;
    if (!std::filesystem::exists(dir / "run.json", ec)) return make_error(ErrorCode::not_found, "no run in {}", fs::to_utf8(dir));
    TRY_ASSIGN(auto lock, FileLock::try_acquire(dir / "run.lock", FileLock::Mode::exclusive));
    if (!lock) return make_error(ErrorCode::invalid_argument, "run {} is still running in another process", store.id_);
    store.lock_ = std::move(*lock);
    TRY(fs::create_directories(store.sessions_dir()));
    return store;
}

Result<Json> RunStore::read_run() const {
    TRY_ASSIGN(auto text, fs::read_text(dir_ / "run.json"));
    TRY_ASSIGN(auto j, parse_json(text));
    if (!j.is_object()) return make_error(ErrorCode::parse, "{}: run.json is not an object", fs::to_utf8(dir_));
    return j;
}

Result<void> RunStore::write_run(const Json& run) { return fs::write_text(dir_ / "run.json", dump_pretty(run) + "\n"); }

Result<void> RunStore::write_summary(const Json& summary) { return fs::write_text(dir_ / "summary.json", dump_pretty(summary) + "\n"); }

Result<RunInfo> read_run_info(const std::filesystem::path& dir) {
    TRY_ASSIGN(auto text, fs::read_text(dir / "run.json"));
    TRY_ASSIGN(auto run, parse_json(text));
    if (!run.is_object()) return make_error(ErrorCode::parse, "{}: run.json is not an object", fs::to_utf8(dir));
    RunInfo info;
    info.dir = dir;
    info.id = json_string_or(run, "id", fs::to_utf8(dir.filename()));
    info.status = json_string_or(run, "status", "unknown");
    info.created = json_string_or(run, "created", "");
    info.updated = json_string_or(run, "updated", "");
    info.model = json_string_or(run, "model", "");
    info.effort = json_string_or(run, "effort", "");
    info.workers = static_cast<int>(json_int_or(run, "workers", 0));
    info.spent_usd = json_number_or(run, "spent_usd", 0);
    info.replay = json_bool_or(run, "replay", false);
    if (auto q = run.find("queue"); q != run.end() && q->is_array()) {
        info.functions = q->size();
        for (const auto& item : *q) {
            const std::string state = json_string_or(item, "state", "");
            // Done: worked on for good (a stopped or failed function runs again when the run resumes).
            if (state == "skipped" || (state == "done" && is_final_outcome(json_string_or(item, "outcome", "")))) ++info.done;
            if (json_string_or(item, "outcome", "") == "matched") ++info.matched;
        }
    }
    // A live status is only true while some process holds the run's lock.
    auto lock = FileLock::try_acquire(dir / "run.lock", FileLock::Mode::exclusive);
    info.live = lock && !lock->has_value();
    if (is_live_status(info.status) && !info.live) info.status = "interrupted";
    info.run = std::move(run);
    return info;
}

std::vector<RunInfo> list_runs(const std::filesystem::path& runs_dir) {
    std::vector<RunInfo> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(runs_dir, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(runs_dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        if (auto info = read_run_info(entry.path())) out.push_back(std::move(*info));
    }
    // Run ids start with their creation time, so they sort chronologically.
    std::ranges::sort(out, [](const RunInfo& a, const RunInfo& b) { return a.id > b.id; });
    return out;
}

Result<std::filesystem::path> find_run(const std::filesystem::path& runs_dir, std::string_view id) {
    std::error_code ec;
    const auto exact = runs_dir / fs::from_utf8(std::string(id));
    if (!id.empty() && std::filesystem::exists(exact / "run.json", ec)) return exact;
    std::vector<std::filesystem::path> matches;
    if (std::filesystem::is_directory(runs_dir, ec))
        for (const auto& entry : std::filesystem::directory_iterator(runs_dir, ec))
            if (entry.is_directory(ec) && fs::to_utf8(entry.path().filename()).starts_with(id) &&
                std::filesystem::exists(entry.path() / "run.json", ec))
                matches.push_back(entry.path());
    if (matches.empty()) return make_error(ErrorCode::not_found, "no run '{}' in {}", id, fs::to_utf8(runs_dir));
    if (matches.size() > 1) return make_error(ErrorCode::invalid_argument, "'{}' matches {} runs; give more of the id", id, matches.size());
    return matches.front();
}

void apply_recorded_settings(const Json& run, project::AgentSettings& settings) {
    if (!run.is_object()) return;
    settings.model = json_string_or(run, "model", settings.model);
    settings.effort = json_string_or(run, "effort", settings.effort);
    settings.workers = static_cast<int>(json_int_or(run, "workers", settings.workers));
    settings.max_usd_per_run = json_number_or(run, "run_budget_usd", settings.max_usd_per_run);
    if (auto l = run.find("limits"); l != run.end() && l->is_object()) {
        settings.max_turns = static_cast<int>(json_int_or(*l, "max_turns", settings.max_turns));
        settings.max_usd_per_function = json_number_or(*l, "max_usd", settings.max_usd_per_function);
        settings.max_tokens_per_function = json_int_or(*l, "max_tokens", settings.max_tokens_per_function);
        settings.max_minutes_per_function = static_cast<int>(json_int_or(*l, "max_seconds", settings.max_minutes_per_function * 60LL) / 60);
    }
    if (auto p = run.find("policies"); p != run.end() && p->is_object())
        for (auto it = p->begin(); it != p->end(); ++it)
            if (it->is_string()) settings.approvals[it.key()] = it->get<std::string>();
}

Json run_summary(const events::RunStateData& state) {
    struct PerFunction {
        const events::SessionState* latest = nullptr;
        int sessions = 0, turns = 0;
        double cost_usd = 0, best_match = 0;
        bool matched = false;
        events::TokenUsage usage;
    };
    std::map<u64, PerFunction> functions;
    for (const auto& [id, s] : state.sessions) {
        auto& f = functions[s->va];
        ++f.sessions;
        f.turns += s->turn;
        f.cost_usd += s->cost_usd;
        f.usage += s->usage;
        f.best_match = std::max(f.best_match, s->best_match);
        f.matched = f.matched || s->matched || s->outcome == "matched";
        // The latest session decides the outcome (started later; ids break ties).
        if (!f.latest || s->started > f.latest->started || (s->started == f.latest->started && s->id > f.latest->id)) f.latest = s.get();
    }
    Json list = Json::array();
    usize matched = 0;
    for (const auto& [va, f] : functions) {
        const auto* s = f.latest;
        if (f.matched) ++matched;
        list.push_back(Json{{"va", va},
                            {"function", s->function},
                            {"display", s->display},
                            {"session", s->id},
                            {"outcome", s->finished ? s->outcome : "running"},
                            {"detail", s->detail},
                            {"matched", f.matched},
                            {"best_match", f.best_match},
                            {"sessions", f.sessions},
                            {"turns", f.turns},
                            {"cost_usd", f.cost_usd},
                            {"usage", usage_json(f.usage)}});
    }
    Json j = {{"run", state.run_id},
              {"project", state.project},
              {"status", state.status},
              {"model", state.model},
              {"effort", state.effort},
              {"workers", state.worker_count},
              {"started", iso(state.started)},
              {"functions_planned", state.planned_count()},
              {"functions_worked", functions.size()},
              {"functions_matched", matched},
              {"sessions", state.sessions.size()},
              {"cost_usd", state.cost_usd},
              {"usage", usage_json(state.usage)},
              {"functions", std::move(list)}};
    if (state.ended != events::TimePoint{}) j["finished"] = iso(state.ended);
    return j;
}

} // namespace decomp::run
