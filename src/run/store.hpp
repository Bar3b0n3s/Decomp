#pragma once

// A run's directory, <project>/.decomp/runs/<id>/:
//   run.json      settings, status and the queue (rewritten as the run goes)
//   summary.json  per-function results so far
//   events.jsonl  every event (the durable record; replays into the run's state)
//   sessions/     one transcript per session
//   run.lock      held while a process runs it. A run whose run.json says it is running but whose lock
//                 is free was interrupted (the process died) and can be resumed.

#include "core/file_lock.hpp"
#include "core/json.hpp"
#include "core/result.hpp"
#include "events/run_state.hpp"
#include "project/project.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace decomp::run {

// Statuses of a run that has not ended.
bool is_live_status(std::string_view status);

struct RunInfo {
    std::string id;
    std::filesystem::path dir;
    // From run.json; "interrupted" when it says live but no process holds the run's lock.
    std::string status;
    bool live = false;  // a process holds the lock
    std::string created, updated;
    std::string model, effort;
    int workers = 0;
    usize functions = 0, matched = 0;
    usize done = 0;  // worked on for good (is_final_outcome) or skipped; a stopped function is not done
    double spent_usd = 0;
    bool replay = false;
    Json run;  // the whole run.json
};

class RunStore {
public:
    // A new run directory (an existing one is an error); takes the run's lock.
    static Result<RunStore> create(const std::filesystem::path& runs_dir, const std::string& run_id);
    // An existing run, to resume it: takes the run's lock (fails while another process runs it).
    static Result<RunStore> open(const std::filesystem::path& dir);

    const std::string& id() const { return id_; }
    const std::filesystem::path& dir() const { return dir_; }
    std::filesystem::path events_path() const { return dir_ / "events.jsonl"; }
    std::filesystem::path sessions_dir() const { return dir_ / "sessions"; }

    Result<Json> read_run() const;
    Result<void> write_run(const Json& run);
    Result<void> write_summary(const Json& summary);
    // The run is over: other processes may open it.
    void release() { lock_.reset(); }

private:
    std::string id_;
    std::filesystem::path dir_;
    std::optional<FileLock> lock_;
};

// The runs under `runs_dir`, newest first; directories without a readable run.json are skipped.
std::vector<RunInfo> list_runs(const std::filesystem::path& runs_dir);
Result<RunInfo> read_run_info(const std::filesystem::path& dir);
// A run by id, or by a unique prefix of its id.
Result<std::filesystem::path> find_run(const std::filesystem::path& runs_dir, std::string_view id);

// A resumed run keeps the settings it recorded in run.json (model, effort, workers, run budget,
// per-function limits, approval policies) unless the caller changes them afterwards.
void apply_recorded_settings(const Json& run, project::AgentSettings& settings);

// What a run did, from its (replayed or live) state: totals and, per function, the outcome of its
// latest session plus the totals of all its sessions. summary.json holds this; `decomp runs show`
// computes it again from events.jsonl.
Json run_summary(const events::RunStateData& state);

} // namespace decomp::run
