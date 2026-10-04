#pragma once

// Runs the agent on many functions with N workers under supervision. Every command is thread-safe and
// acknowledged with a `control` event; the run lives in its directory (RunStore) and event log, so a
// stopped, budget-limited or interrupted run can be resumed.
//
// Locking: commands change state under the controller's lock and publish events after releasing it;
// bus handlers never call back into the controller.

#include "agent/approvals.hpp"
#include "agent/cost.hpp"
#include "agent/loop.hpp"
#include "agent/rate_gate.hpp"
#include "agent/runner.hpp"
#include "analysis/program.hpp"
#include "events/bus.hpp"
#include "events/run_state.hpp"
#include "matching/match.hpp"
#include "project/project.hpp"
#include "run/queue.hpp"
#include "run/store.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace decomp::run {

// Everything one session needs (built by the controller for each dispatched function).
struct SessionRequest {
    std::shared_ptr<const Program> program;  // the symbol generation current at dispatch
    project::Project* project = nullptr;
    u64 va = 0;
    std::string session_id;
    std::filesystem::path transcript;
    int worker = -1;
    agent::AgentRunConfig config;  // session id, transport, rate gate, run ledger, approvals, first-message hook
    matching::MatchSetup setup;
    agent::LoopControl* control = nullptr;
};

using SessionFn = std::function<agent::FunctionRunResult(const SessionRequest&, events::EventBus&)>;
using TransportFactory = std::function<Result<std::shared_ptr<agent::HttpTransport>>(const Symbol&)>;

struct RunDeps {
    std::function<std::shared_ptr<const Program>()> program = {};  // the current symbol generation
    project::Project* project = nullptr;
    matching::MatchSetup setup = {};
    TransportFactory transport = {};  // per session; null: the agent config's transport (or the default HTTPS one)
    SessionFn run_session = {};       // null: agent::run_function
};

struct RunOptions {
    int workers = 4;
    agent::AgentRunConfig agent;  // model, effort, per-function limits, client settings
    double run_budget_usd = 0;    // 0 = unlimited
    std::map<std::string, agent::ApprovalPolicy, std::less<>> policies;
    // Until the first session hears back from the API (or this long), only one session runs, so the
    // others start with the shared prompt prefix already cached. 0 = no stagger.
    std::chrono::milliseconds stagger_timeout{30'000};
    std::string project_name;
    bool replay = false;      // scripted responses (recorded in run.json)
    std::string replay_dir;   // where the scripts are (recorded, so a resume can find them)
    Json selection;           // how the functions were chosen (recorded in run.json)
};

// The bus must outlive the controller.
class RunController {
public:
    RunController(RunDeps deps, events::EventBus& bus);
    ~RunController();  // aborts a live run and joins its workers
    RunController(const RunController&) = delete;
    RunController& operator=(const RunController&) = delete;

    // Starts a new run over `items` (dispatch order) in `store`.
    Result<void> start(RunStore store, std::vector<QueueItem> items, RunOptions options);
    // Resumes a stopped, budget-limited or interrupted run. Finished functions stay finished;
    // interrupted, stopped, aborted and failed ones start over with a fresh conversation (the brief
    // carries their attempts, notes and best source). Event numbering continues the log.
    Result<void> resume(RunStore store, RunOptions options);

    // ---- commands (any thread) ----
    void pause();
    void unpause();
    void pause_worker(int worker);
    void unpause_worker(int worker);
    void stop();   // sessions end after their current turn; pending functions stay pending
    void abort();  // requests and compiles in flight are cancelled
    bool skip(u64 va);
    bool requeue(u64 va);
    usize enqueue(std::vector<QueueItem> items);
    bool remove(u64 va);
    bool move(u64 va, usize index);
    bool pin(u64 va, bool pinned);
    void set_concurrency(int workers);
    void set_run_budget(double usd);
    void set_limits(const agent::LoopLimits& limits);  // running and later sessions
    std::optional<u64> inject(const std::string& session, std::string text);
    bool retract(const std::string& session, u64 id);
    bool decide(u64 approval, bool approve, std::string reason = {});
    void set_policy(const std::string& action, agent::ApprovalPolicy policy);

    // ---- state ----
    void wait();  // until every worker has exited and run_finished was published
    bool wait_for(std::chrono::milliseconds timeout);
    bool finished() const;
    // starting, running, paused, stopping, aborting; then completed, stopped, aborted, budget_exhausted
    std::string status() const;
    std::vector<QueueItem> queue() const;
    std::vector<std::string> running_sessions() const;
    int concurrency() const;
    std::string run_id() const;
    std::filesystem::path run_dir() const;
    std::vector<agent::PendingApproval> pending_approvals() const;
    std::shared_ptr<agent::RateGate> rate_gate() const { return gate_; }
    std::shared_ptr<agent::SpendLedger> ledger() const;
    std::shared_ptr<agent::ApprovalGate> approvals() const { return approvals_; }

    static constexpr int kMaxWorkers = 64;

private:
    struct Running {
        u64 va = 0;
        std::string session;
        std::shared_ptr<agent::LoopControl> control;
    };
    struct Dispatch {
        enum class Kind { exit, wait, run } kind = Kind::exit;
        bool retired = false;  // exit: a lower concurrency retired this worker
        std::string reason;    // wait
        QueueItem item;      // run
        std::string session;
        std::shared_ptr<agent::LoopControl> control;
    };

    void begin_locked(RunStore store, RunOptions options);
    void spawn_locked(int worker);
    void worker_main(int worker);
    // Waits until the worker can run something, has a new reason to wait, or must exit (`last`: it
    // was the last worker alive and finishes the run).
    Dispatch next_dispatch(int worker, const std::string& announced, bool& last);
    std::optional<std::string> blocked_locked(int worker);
    bool should_exit_locked(int worker) const;
    void run_item(int worker, Dispatch& d);
    void finish_run();
    void open_stagger();
    void notify_queue();  // publishes queue_updated (pending items in dispatch order)
    void control_event(std::string command, std::string target = {}, std::string detail = {});
    Json run_json_locked() const;
    std::string live_status_locked() const;
    void persist(bool force);
    void write_summary(bool force);

    RunDeps deps_;
    bool needs_program_ = false;  // the default session function runs the agent on the program
    events::EventBus& bus_;
    std::shared_ptr<agent::RateGate> gate_ = std::make_shared<agent::RateGate>();
    std::shared_ptr<agent::ApprovalGate> approvals_;
    events::RunStateStore summary_state_;  // folds the run's events for summary.json

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable finished_cv_;
    std::optional<RunStore> store_;
    RunOptions options_;
    WorkQueue queue_;
    std::shared_ptr<agent::SpendLedger> ledger_;
    std::optional<agent::LoopLimits> limits_;
    std::map<int, Running> running_;  // by worker
    bool started_ = false, paused_ = false, stopping_ = false, aborting_ = false;
    bool finishing_ = false, finished_ = false;
    std::set<int> paused_workers_;
    int concurrency_ = 1;
    bool stagger_open_ = true;
    std::chrono::steady_clock::time_point stagger_deadline_{};
    usize dispatched_ = 0;
    std::set<int> alive_;  // workers whose loop has not exited
    std::map<int, std::jthread> threads_;
    std::vector<std::jthread> retired_;
    std::string final_status_;
    std::string created_;

    std::mutex persist_mutex_;      // orders run.json and summary.json writes
    std::mutex queue_event_mutex_;  // orders queue_updated events
    std::chrono::steady_clock::time_point last_persist_{}, last_summary_{};
    std::mutex join_mutex_;
};

} // namespace decomp::run
