#include "search/runs.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "project/project.hpp"

#include <algorithm>
#include <format>
#include <random>
#include <tuple>

namespace decomp::search {

std::string_view to_string(SearchKind kind) {
    switch (kind) {
    case SearchKind::flags: return "flags";
    case SearchKind::permute: return "permute";
    case SearchKind::identify: return "identify";
    }
    return "flags";
}

std::optional<SearchKind> search_kind_from_string(std::string_view s) {
    for (SearchKind k : {SearchKind::flags, SearchKind::permute, SearchKind::identify})
        if (to_string(k) == s) return k;
    return std::nullopt;
}

std::string_view to_string(RunStatus status) {
    switch (status) {
    case RunStatus::running: return "running";
    case RunStatus::done: return "done";
    case RunStatus::cancelled: return "cancelled";
    case RunStatus::failed: return "failed";
    }
    return "running";
}

namespace {

std::optional<RunStatus> run_status_from_string(std::string_view s) {
    for (RunStatus r : {RunStatus::running, RunStatus::done, RunStatus::cancelled, RunStatus::failed})
        if (to_string(r) == s) return r;
    return std::nullopt;
}

} // namespace

Json to_json(const LogEntry& entry) {
    Json j{{"index", entry.index}, {"ms", entry.ms}, {"label", entry.label}, {"score", to_json(entry.score)}};
    if (entry.best) j["best"] = true;
    return j;
}

LogEntry log_entry_from_json(const Json& j) {
    LogEntry e;
    if (!j.is_object()) return e;
    e.index = j.value("index", usize{0});
    e.ms = j.value("ms", i64{0});
    e.label = j.value("label", std::string());
    if (auto s = j.find("score"); s != j.end()) e.score = score_from_json(*s);
    e.best = j.value("best", false);
    return e;
}

Json to_json(const RunRecord& run) {
    Json functions = Json::array();
    for (u64 va : run.functions) functions.push_back(va);
    Json j{{"id", run.id},
           {"kind", std::string(to_string(run.kind))},
           {"started", run.started},
           {"target", run.target},
           {"functions", std::move(functions)},
           {"settings", run.settings},
           {"status", std::string(to_string(run.status))},
           {"candidates", run.candidates},
           {"result", run.result},
           {"duration_ms", run.duration_ms}};
    if (!run.error.empty()) j["error"] = run.error;
    if (run.best) {
        j["best"] = to_json(*run.best);
        j["best_label"] = run.best_label;
    }
    return j;
}

Result<RunRecord> run_from_json(const Json& j) {
    if (!j.is_object()) return make_error(ErrorCode::parse, "a search run is a JSON object");
    RunRecord r;
    r.id = j.value("id", std::string());
    const auto kind = search_kind_from_string(j.value("kind", std::string()));
    if (!kind) return make_error(ErrorCode::parse, "unknown search kind '{}'", j.value("kind", std::string()));
    r.kind = *kind;
    r.started = j.value("started", std::string());
    r.target = j.value("target", std::string());
    if (auto f = j.find("functions"); f != j.end() && f->is_array())
        for (const auto& va : *f)
            if (va.is_number_unsigned()) r.functions.push_back(va.get<u64>());
    if (auto s = j.find("settings"); s != j.end()) r.settings = *s;
    r.status = run_status_from_string(j.value("status", std::string())).value_or(RunStatus::failed);
    r.error = j.value("error", std::string());
    r.candidates = j.value("candidates", usize{0});
    if (auto b = j.find("best"); b != j.end() && b->is_object()) r.best = score_from_json(*b);
    r.best_label = j.value("best_label", std::string());
    if (auto res = j.find("result"); res != j.end()) r.result = *res;
    r.duration_ms = j.value("duration_ms", i64{0});
    return r;
}

std::filesystem::path search_dir(const project::Project& project) { return project.root() / ".decomp" / "search"; }

Result<std::unique_ptr<RunWriter>> RunWriter::create(const std::filesystem::path& dir, SearchKind kind, std::string target,
                                                     std::vector<u64> functions, Json settings) {
    std::unique_ptr<RunWriter> w(new RunWriter());
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    std::random_device rd;
    w->record_.id = std::format("{:%Y-%m-%dT%H-%M-%S}-{}-{:04x}", std::chrono::floor<std::chrono::seconds>(now), to_string(kind), rd() & 0xFFFF);
    w->record_.kind = kind;
    w->record_.started = std::format("{:%FT%TZ}", now);  // to the millisecond
    w->record_.target = std::move(target);
    w->record_.functions = std::move(functions);
    w->record_.settings = std::move(settings);
    w->dir_ = dir / fs::from_utf8(w->record_.id);
    w->start_ = std::chrono::steady_clock::now();
    TRY(fs::create_directories(w->dir_));
    TRY(w->save());
    return w;
}

i64 RunWriter::elapsed_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
}

void RunWriter::log(const LogEntry& entry) {
    std::lock_guard lock(mutex_);
    ++record_.candidates;
    if (!record_.best || entry.score.better_than(*record_.best)) {
        record_.best = entry.score;
        record_.best_label = entry.label;
    }
    (void)fs::append_text(dir_ / "log.jsonl", to_json(entry).dump() + "\n");
}

Result<void> RunWriter::write_file(const std::string& name, std::string_view text) const { return fs::write_text(dir_ / fs::from_utf8(name), text); }

Result<void> RunWriter::finish(RunStatus status, Json result, std::string error) {
    std::lock_guard lock(mutex_);
    record_.status = status;
    record_.result = std::move(result);
    record_.error = std::move(error);
    record_.duration_ms = elapsed_ms();
    return save();
}

Result<void> RunWriter::save() const { return fs::write_text(dir_ / "run.json", to_json(record_).dump(2) + "\n"); }

CandidateLog::CandidateLog(RunWriter* run, std::function<void(const LogEntry&)> on_entry)
    : run_(run), on_entry_(std::move(on_entry)), start_(std::chrono::steady_clock::now()) {}

LogEntry CandidateLog::add(std::string label, const Score& score) {
    std::lock_guard lock(mutex_);
    LogEntry e;
    e.index = count_++;
    e.ms = run_ ? run_->elapsed_ms() : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
    e.label = std::move(label);
    e.score = score;
    e.best = !best_ || score.better_than(*best_);
    if (e.best) best_ = score;
    if (run_) run_->log(e);
    if (on_entry_) on_entry_(e);
    return e;
}

usize CandidateLog::count() const {
    std::lock_guard lock(mutex_);
    return count_;
}

std::optional<Score> CandidateLog::best() const {
    std::lock_guard lock(mutex_);
    return best_;
}

std::vector<RunRecord> list_runs(const std::filesystem::path& dir) {
    std::vector<RunRecord> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        if (auto r = load_run(entry.path())) out.push_back(std::move(*r));
    }
    std::ranges::sort(out, [](const RunRecord& a, const RunRecord& b) { return std::tie(a.started, a.id) > std::tie(b.started, b.id); });
    return out;
}

Result<RunRecord> load_run(const std::filesystem::path& run_dir) {
    TRY_ASSIGN(auto text, fs::read_text(run_dir / "run.json"));
    Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded()) return make_error(ErrorCode::parse, "{} is not JSON", fs::to_utf8(run_dir / "run.json"));
    return run_from_json(j);
}

std::vector<LogEntry> load_log(const std::filesystem::path& run_dir) {
    std::vector<LogEntry> out;
    auto text = fs::read_text(run_dir / "log.jsonl");
    if (!text) return out;
    for (const auto& line : split_lines(*text)) {
        if (trim(line).empty()) continue;
        Json j = Json::parse(line, nullptr, false);
        if (!j.is_discarded()) out.push_back(log_entry_from_json(j));
    }
    return out;
}

} // namespace decomp::search
