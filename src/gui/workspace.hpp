#pragma once

// What decomp-gui has open: a project (loaded in the background), its program, and the run the views
// show. A run is either live (a RunController over the project, docs/ui.md#run-control) or past (its
// event log replayed through the same reducer, read-only, docs/ui.md#replay-of-past-runs). No ImGui
// here: the shell reaches the workspace through AppServices (services()).
//
// Threading: everything is called on the UI thread, except the wake callback (any thread) and what the
// run's workers call through the controller.

#include "agent/approvals.hpp"
#include "agent/loop.hpp"
#include "analysis/program.hpp"
#include "events/bus.hpp"
#include "events/run_state.hpp"
#include "gui/services.hpp"
#include "project/project.hpp"
#include "run/controller.hpp"
#include "run/store.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace decomp::gui {

enum class ProjectPhase { none, loading, open, failed };

struct ProjectState {
    ProjectPhase phase = ProjectPhase::none;
    std::filesystem::path root;
    std::string error;   // failed
    std::string target;  // the target binary's file name, once open
};

// How to start a run from the GUI.
struct RunRequest {
    std::vector<u64> functions;  // in this order; empty: every function the default selection takes
    int workers = 0;             // 0: the project's agent.workers
    std::optional<double> run_budget_usd;
    std::optional<agent::LoopLimits> limits;
    // On top of decomp.json's agent.approvals ("ask" works here: the GUI answers).
    std::map<std::string, agent::ApprovalPolicy, std::less<>> policies;
};

class Workspace {
public:
    struct Options {
        std::function<void()> wake = {};  // wakes the UI loop; called from any thread, at most once per frame
        // Developer setting: scripted API responses per function (tests/replay/run/), no key needed.
        std::filesystem::path replay_dir = {};
        std::chrono::milliseconds stagger{30'000};
        // Tests: run sessions with this instead of the agent.
        run::SessionFn session_override = {};
    };

    explicit Workspace(Options options);
    ~Workspace();  // aborts a live run and waits for its workers
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    // Completes background work (project and past-run loading); call once per frame.
    void poll();

    // ---- project ----
    // Starts loading a project in the background. Refused while a run is live.
    Result<void> open_project(const std::filesystem::path& root);
    void close_project();
    // Blocks until a background load finishes (tests and command-line options).
    void wait_loaded();
    const ProjectState& project_state() const { return project_state_; }
    project::Project* project() { return project_ ? &*project_ : nullptr; }
    const project::Project* project() const { return project_ ? &*project_ : nullptr; }
    std::shared_ptr<const Program> program() const;
    const std::optional<project::TargetStatus>& target_status() const { return target_status_; }
    // Symbols changed (set_symbol): later sessions see a new program generation.
    void reload_symbols();
    void set_replay_dir(std::filesystem::path dir) { options_.replay_dir = std::move(dir); }
    const std::filesystem::path& replay_dir() const { return options_.replay_dir; }

    // ---- runs ----
    Result<std::string> start_run(const RunRequest& request);  // the new run's id
    Result<void> resume_run(const std::string& run_id);
    // Loads a past run in the background (read-only); a live run must be finished first.
    Result<void> open_run(const std::string& run_id);
    void close_run();  // a finished live run or a past run
    bool run_live() const;       // a controller whose run has not finished
    bool run_read_only() const;  // a past run is shown
    bool run_loading() const { return past_load_.valid(); }
    std::string run_id() const;
    std::filesystem::path run_dir() const;
    // Changes whenever another run is shown (started, resumed, opened or closed).
    u64 run_serial() const { return run_serial_; }
    // What the shown run had done before this workspace attached to it: the folded log of a resumed or
    // reopened run (so its history is not announced again as news); null for a run started here, and
    // while a past run is still loading.
    std::shared_ptr<const events::RunStateData> run_history() const { return run_history_; }
    run::RunController* controller();  // the live run's (also after it finished); null otherwise
    // The project's runs, newest first (cached; refresh re-reads the runs directory).
    const std::vector<run::RunInfo>& runs(bool refresh = false);

    // The snapshot to render this frame (null: no run).
    std::shared_ptr<const events::RunStateData> snapshot();

    // Ends a live run: stop (sessions finish their turn) or abort; returns at once.
    void end_run(bool abort);

    // What App needs: snapshot, commands, project info, wake; plus a pointer back to this workspace.
    AppServices services();

    // Errors of commands that return nothing (the shell shows them as notifications).
    void report_error(std::string message) { error_ = std::move(message); }
    std::string take_error() { return std::exchange(error_, {}); }

private:
    struct LiveRun;
    struct LoadedProject {
        project::Project project;
        std::shared_ptr<const Program> program;
        SymbolDb derived;  // the binary's own symbols, before symbols.txt
        std::optional<project::TargetStatus> status;
    };
    struct PastRun {
        std::string id;
        std::filesystem::path dir;
        std::shared_ptr<events::RunStateStore> store;
    };

    Result<void> launch(run::RunStore store, std::vector<run::QueueItem> items, run::RunOptions options, bool resume,
                        std::shared_ptr<events::RunStateStore> state);
    Result<run::RunOptions> options_for(project::AgentSettings settings, const std::map<std::string, agent::ApprovalPolicy, std::less<>>& extra);
    void notify_ui();

    Options options_;
    std::atomic<bool> wake_pending_{false};

    ProjectState project_state_;
    std::optional<project::Project> project_;
    mutable std::mutex program_mutex_;  // the workers read the current generation
    std::shared_ptr<const Program> program_;
    std::optional<SymbolDb> derived_symbols_;
    std::optional<project::TargetStatus> target_status_;
    std::future<Result<LoadedProject>> project_load_;

    std::unique_ptr<LiveRun> live_;
    std::optional<PastRun> past_;
    std::future<Result<PastRun>> past_load_;
    std::vector<run::RunInfo> runs_;
    bool runs_loaded_ = false;
    u64 run_serial_ = 0;
    std::shared_ptr<const events::RunStateData> run_history_;
    std::string error_;
};

// RunCommands over a Workspace: the shell's run controls, the views' queue edits, steering and approvals.
class WorkspaceCommands : public RunCommands {
public:
    explicit WorkspaceCommands(Workspace& workspace) : workspace_(workspace) {}

    bool available() const override;
    bool live() const override { return workspace_.run_live(); }
    void start() override;
    void start_functions(std::vector<u64> functions) override;
    void pause() override;
    void resume() override;
    void stop() override;
    void abort() override;
    void pause_worker(int worker) override;
    void resume_worker(int worker) override;
    void set_concurrency(int workers) override;
    void set_run_budget(double usd) override;
    void set_limits(const agent::LoopLimits& limits) override;
    bool skip(u64 va) override;
    bool requeue(u64 va) override;
    usize enqueue(std::vector<u64> functions) override;
    bool remove(u64 va) override;
    bool move(u64 va, usize index) override;
    bool pin(u64 va, bool pinned) override;
    u64 inject(std::string_view session, std::string_view text) override;
    bool retract(std::string_view session, u64 id) override;
    bool decide(u64 approval, bool approve, std::string_view reason) override;
    void set_policy(std::string_view action, agent::ApprovalPolicy policy) override;
    std::vector<agent::PendingApproval> pending_approvals() const override;
    int concurrency() const override;
    double run_budget() const override;

private:
    run::RunController* controller() const { return workspace_.controller(); }
    Workspace& workspace_;
};

} // namespace decomp::gui
