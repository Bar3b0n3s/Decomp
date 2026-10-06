#pragma once

// Search runs as a project keeps them (docs/project-format.md#search-runs): .decomp/search/<id>/ holds
// run.json (what was searched, how, and the outcome) and log.jsonl (a line per candidate evaluated, as
// it was). The CLI and the GUI write them the same way, and the GUI's Search view reads them, a running
// one's log as it grows.

#include "core/json.hpp"
#include "core/result.hpp"
#include "search/evaluate.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace decomp::project {
class Project;
}

namespace decomp::search {

enum class SearchKind : u8 { flags, permute, identify };
std::string_view to_string(SearchKind kind);
std::optional<SearchKind> search_kind_from_string(std::string_view s);

// A candidate evaluated during a run.
struct LogEntry {
    usize index = 0;     // in the order the candidates were made
    i64 ms = 0;          // since the run started
    std::string label;   // the flags, the mutations, the toolchain
    Score score;
    bool best = false;   // the run's best when it was evaluated
};
Json to_json(const LogEntry& entry);
LogEntry log_entry_from_json(const Json& j);

enum class RunStatus : u8 { running, done, cancelled, failed };
std::string_view to_string(RunStatus status);

struct RunRecord {
    std::string id;
    SearchKind kind = SearchKind::flags;
    std::string started;  // UTC, ISO 8601
    std::string target;   // what was searched: functions, a unit
    std::vector<u64> functions;
    Json settings = Json::object();  // the kind's parameters
    RunStatus status = RunStatus::running;
    std::string error;
    usize candidates = 0;  // evaluated
    std::optional<Score> best;
    std::string best_label;
    Json result = Json::object();  // the kind's result
    i64 duration_ms = 0;
};
Json to_json(const RunRecord& run);
Result<RunRecord> run_from_json(const Json& j);

// .decomp/search/ of a project.
std::filesystem::path search_dir(const project::Project& project);

// Writes a run as it goes: run.json when it starts and when it ends, and a log line per candidate
// (from any thread).
class RunWriter {
public:
    // A new run directory under `dir` (.decomp/search/ of a project).
    static Result<std::unique_ptr<RunWriter>> create(const std::filesystem::path& dir, SearchKind kind, std::string target,
                                                     std::vector<u64> functions, Json settings);

    const RunRecord& record() const { return record_; }
    const std::filesystem::path& dir() const { return dir_; }
    // Milliseconds since the run started.
    i64 elapsed_ms() const;
    // Appends a candidate to log.jsonl, and keeps the best.
    void log(const LogEntry& entry);
    // Writes another file of the run (a best source, a result table).
    Result<void> write_file(const std::string& name, std::string_view text) const;
    // Ends the run: its status, result and (when set) error, written to run.json.
    Result<void> finish(RunStatus status, Json result, std::string error = {});

private:
    RunWriter() = default;
    Result<void> save() const;
    std::filesystem::path dir_;
    RunRecord record_;
    std::chrono::steady_clock::time_point start_;
    mutable std::mutex mutex_;
};

// The runs of a project, newest first.
std::vector<RunRecord> list_runs(const std::filesystem::path& dir);
Result<RunRecord> load_run(const std::filesystem::path& run_dir);
// The candidates of a run, in the order they were logged.
std::vector<LogEntry> load_log(const std::filesystem::path& run_dir);

} // namespace decomp::search
