#include "gui/workspace.hpp"

#include "agent/replay_transport.hpp"
#include "agent/runner.hpp"
#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "project/setup.hpp"
#include "run/selection.hpp"

#include <algorithm>
#include <format>

namespace decomp::gui {

using namespace std::chrono_literals;

// The bus, its event log, the reducer the views read and the controller of a live run.
struct Workspace::LiveRun {
    explicit LiveRun(std::string run_id) : id(run_id), bus(std::move(run_id)) {}
    ~LiveRun() {
        if (log_sink) log::remove_sink(log_sink);
        controller.reset();  // aborts a run that is still going and joins its workers
        if (state) state->detach();
    }
    LiveRun(const LiveRun&) = delete;
    LiveRun& operator=(const LiveRun&) = delete;

    std::string id;
    std::filesystem::path dir;
    events::EventBus bus;
    std::unique_ptr<events::JsonlEventLog> log;
    std::shared_ptr<events::RunStateStore> state;
    std::unique_ptr<run::RunController> controller;
    std::optional<FileLock> active_lock;  // one live run per project, across processes
    int log_sink = 0;
};

namespace {

template <class T>
bool ready(const std::future<T>& f) {
    return f.valid() && f.wait_for(0s) == std::future_status::ready;
}

} // namespace

Workspace::Workspace(Options options) : options_(std::move(options)) {}

Workspace::~Workspace() {
    if (live_ && live_->controller) live_->controller->abort();
    live_.reset();
}

void Workspace::notify_ui() {
    // One wake per frame is enough: poll() re-arms it.
    if (options_.wake && !wake_pending_.exchange(true)) options_.wake();
}

void Workspace::poll() {
    wake_pending_ = false;
    if (ready(project_load_)) {
        auto loaded = project_load_.get();
        if (loaded) {
            project_.emplace(std::move(loaded->project));
            {
                std::lock_guard lock(program_mutex_);
                program_ = std::move(loaded->program);
            }
            derived_symbols_ = std::move(loaded->derived);
            target_status_ = std::move(loaded->status);
            project_state_.phase = ProjectPhase::open;
            project_state_.target = fs::to_utf8(project_->target_path().filename());
            runs_loaded_ = false;
        } else {
            project_state_.phase = ProjectPhase::failed;
            project_state_.error = loaded.error().message;
        }
    }
    if (ready(past_load_)) {
        auto loaded = past_load_.get();
        if (loaded) past_ = std::move(*loaded);
        else error_ = std::format("cannot open the run: {}", loaded.error().message);
    }
}

// ---- project ----------------------------------------------------------------------------------------

Result<void> Workspace::open_project(const std::filesystem::path& root) {
    if (run_live()) return make_error(ErrorCode::invalid_argument, "a run is in progress: stop it before opening another project");
    close_run();
    close_project();
    project_state_ = ProjectState{ProjectPhase::loading, root, {}, {}};
    project_load_ = std::async(std::launch::async, [root]() -> Result<LoadedProject> {
        TRY_ASSIGN(auto project, project::Project::load(root));
        // A changed target still opens, for display; runs are refused until it matches again.
        std::optional<std::filesystem::path> pdb;
        if (!project.config().pdb.empty()) pdb = project.root() / fs::from_utf8(project.config().pdb);
        TRY_ASSIGN(auto base, Program::open(project.target_path(), pdb));
        LoadedProject loaded;
        loaded.status = project.target_status(base);
        loaded.derived = base.symbols();
        SymbolDb merged = base.symbols();
        for (const auto& s : project.symbols()) merged.add(s);
        loaded.program = std::make_shared<const Program>(base.with_symbols(std::move(merged)));
        loaded.project = std::move(project);
        return loaded;
    });
    return {};
}

void Workspace::close_project() {
    if (run_live()) return;
    if (project_load_.valid()) project_load_.wait();
    project_load_ = {};
    close_run();
    project_.reset();
    {
        std::lock_guard lock(program_mutex_);
        program_.reset();
    }
    derived_symbols_.reset();
    target_status_.reset();
    project_state_ = {};
    runs_.clear();
    runs_loaded_ = false;
}

void Workspace::wait_loaded() {
    if (project_load_.valid()) project_load_.wait();
    if (past_load_.valid()) past_load_.wait();
    poll();
}

std::shared_ptr<const Program> Workspace::program() const {
    std::lock_guard lock(program_mutex_);
    return program_;
}

void Workspace::reload_symbols() {
    auto current = program();
    if (!project_ || !current || !derived_symbols_) return;
    // Symbols the project removed fall back to what the binary says, as when the project is opened.
    SymbolDb merged = *derived_symbols_;
    for (const auto& s : project_->symbols()) merged.add(s);
    auto next = std::make_shared<const Program>(current->with_symbols(std::move(merged)));
    std::lock_guard lock(program_mutex_);
    program_ = std::move(next);
}

// ---- runs ---------------------------------------------------------------------------------------------

Result<run::RunOptions> Workspace::options_for(project::AgentSettings settings,
                                               const std::map<std::string, agent::ApprovalPolicy, std::less<>>& extra) {
    if (settings.model.starts_with("claude-haiku"))
        return make_error(ErrorCode::invalid_argument,
                          "model '{}' does not support the adaptive thinking and effort settings the agent sends", settings.model);
    run::RunOptions o;
    o.workers = settings.workers;
    o.agent = agent::run_config_from(settings);
    o.run_budget_usd = settings.max_usd_per_run;
    TRY_ASSIGN(o.policies, agent::resolve_approval_policies(settings.approvals, {}, true));
    for (const auto& [action, policy] : extra) o.policies[action] = policy;
    o.stagger_timeout = options_.stagger;
    o.project_name = fs::to_utf8(project_->root().filename());
    if (!options_.replay_dir.empty()) {
        o.replay = true;
        o.replay_dir = fs::to_utf8(options_.replay_dir);
        o.agent.client.api_key = "replay";  // a scripted run never sends the real key anywhere
    } else if (!options_.session_override) {
        if (trim(o.agent.client.api_key).empty())
            return make_error(ErrorCode::invalid_argument,
                              "ANTHROPIC_API_KEY is not set. Export it before starting decomp-gui, or set a replay directory "
                              "(Settings > Developer) for a scripted run");
        o.agent.transport = std::shared_ptr<agent::HttpTransport>(agent::make_default_transport());  // shared, thread-safe
    }
    return o;
}

Result<void> Workspace::launch(run::RunStore store, std::vector<run::QueueItem> items, run::RunOptions options, bool resume,
                               std::shared_ptr<events::RunStateStore> state) {
    auto live = std::make_unique<LiveRun>(store.id());
    TRY_ASSIGN(auto lock, project_->try_lock_active_run());
    if (!lock) return make_error(ErrorCode::invalid_argument, "another run is active in this project (decomp run or decomp agent elsewhere)");
    live->active_lock = std::move(*lock);
    live->dir = store.dir();
    TRY_ASSIGN(live->log, events::JsonlEventLog::open(store.events_path()));
    live->bus.subscribe([log = live->log.get()](const events::Event& e) { log->write(e); });
    live->state = state ? std::move(state) : std::make_shared<events::RunStateStore>();
    live->state->attach(live->bus);
    live->bus.subscribe([this](const events::Event&) { notify_ui(); });
    // Warnings and errors logged while the run goes on become events, like in `decomp run`.
    live->log_sink = log::add_sink([bus = &live->bus](const log::Entry& e) {
        if (e.level >= log::Level::warn) bus->publish(events::LogLine{std::string(log::to_string(e.level)), e.message, e.session}, e.worker);
    });

    run::RunDeps deps;
    deps.program = [this] { return program(); };
    deps.project = &*project_;
    TRY_ASSIGN(deps.setup, project::make_match_setup(&*project_, ""));
    if (!options.replay_dir.empty()) {
        const auto dir = fs::from_utf8(options.replay_dir);
        deps.transport = [dir](const Symbol& fn) -> Result<std::shared_ptr<agent::HttpTransport>> {
            auto script = agent::find_replay_script(dir, fn);
            if (!script) return make_error(ErrorCode::not_found, "no replay script for {} in {}", fn.name, fs::to_utf8(dir));
            TRY_ASSIGN(auto replay, agent::ReplayTransport::load(*script));
            return std::shared_ptr<agent::HttpTransport>(std::move(replay));
        };
    }
    deps.run_session = options_.session_override;
    live->controller = std::make_unique<run::RunController>(std::move(deps), live->bus);
    if (resume) {
        TRY(live->controller->resume(std::move(store), std::move(options)));
    } else {
        TRY(live->controller->start(std::move(store), std::move(items), std::move(options)));
    }
    live_ = std::move(live);
    runs_loaded_ = false;
    return {};
}

Result<std::string> Workspace::start_run(const RunRequest& request) {
    if (!project_) return make_error(ErrorCode::invalid_argument, "open a project first");
    if (run_live()) return make_error(ErrorCode::invalid_argument, "a run is already in progress");
    if (target_status_ && !target_status_->sha1_ok)
        return make_error(ErrorCode::invalid_argument,
                          "the target binary has changed (SHA-1 {} instead of {}): runs are blocked until it matches decomp.json",
                          target_status_->actual_sha1, target_status_->expected_sha1);
    project::AgentSettings settings = project_->config().agent;
    if (request.workers > 0) settings.workers = request.workers;
    if (request.run_budget_usd) settings.max_usd_per_run = *request.run_budget_usd;
    if (request.limits) {
        settings.max_turns = request.limits->max_turns;
        settings.max_usd_per_function = request.limits->max_cost_usd;
        settings.max_tokens_per_function = request.limits->max_total_tokens;
        settings.max_minutes_per_function = static_cast<int>(request.limits->max_wall.count() / 60);
    }
    TRY_ASSIGN(auto options, options_for(std::move(settings), request.policies));

    const auto current = program();
    std::vector<u64> vas = request.functions;
    const bool chosen = !vas.empty();
    if (!chosen) {
        TRY_ASSIGN(auto selected, run::select_functions(*current, &*project_, {}));
        vas = std::move(selected);
    }
    if (vas.empty()) return make_error(ErrorCode::invalid_argument, "nothing to run: every function is matched or set aside");
    std::vector<run::QueueItem> items;
    for (u64 va : vas) {
        const Symbol* s = current->symbols().at(va);
        run::QueueItem item;
        item.va = va;
        item.name = s ? s->name : std::format("sub_{:x}", va);
        item.display = s && !s->display.empty() ? s->display : item.name;
        item.difficulty = s ? run::estimate_difficulty(*s) : 0;
        items.push_back(std::move(item));
    }
    if (!chosen) std::ranges::stable_sort(items, {}, &run::QueueItem::difficulty);  // easy functions first
    options.selection = chosen ? Json{{"functions", vas}, {"from", "gui"}} : Json{{"all", true}, {"from", "gui"}};
    close_run();
    TRY_ASSIGN(auto store, run::RunStore::create(project_->runs_dir(), events::new_run_id()));
    const std::string id = store.id();
    TRY(launch(std::move(store), std::move(items), std::move(options), false, nullptr));
    return id;
}

Result<void> Workspace::resume_run(const std::string& run_id) {
    if (!project_) return make_error(ErrorCode::invalid_argument, "open a project first");
    if (run_live()) return make_error(ErrorCode::invalid_argument, "a run is already in progress");
    TRY_ASSIGN(auto dir, run::find_run(project_->runs_dir(), run_id));
    TRY_ASSIGN(auto store, run::RunStore::open(dir));
    TRY_ASSIGN(Json recorded, store.read_run());
    project::AgentSettings settings = project_->config().agent;
    run::apply_recorded_settings(recorded, settings);
    // A scripted run resumes with its scripts when no other replay directory is set.
    const auto saved_replay = options_.replay_dir;
    if (options_.replay_dir.empty() && json_bool_or(recorded, "replay", false))
        options_.replay_dir = fs::from_utf8(json_string_or(recorded, "replay_dir", ""));
    auto options = options_for(std::move(settings), {});
    options_.replay_dir = saved_replay;
    if (!options) return std::unexpected(options.error());
    options->selection = recorded.value("selection", Json::object());
    // The views show the whole run: the log so far, then the live events.
    auto state = std::make_shared<events::RunStateStore>();
    TRY_ASSIGN(auto past, events::read_event_log(store.events_path()));
    for (const auto& e : past) state->apply(e);
    close_run();
    return launch(std::move(store), {}, std::move(*options), true, std::move(state));
}

Result<void> Workspace::open_run(const std::string& run_id) {
    if (!project_) return make_error(ErrorCode::invalid_argument, "open a project first");
    if (run_live()) return make_error(ErrorCode::invalid_argument, "a run is in progress: stop it before opening another");
    TRY_ASSIGN(auto dir, run::find_run(project_->runs_dir(), run_id));
    close_run();
    past_load_ = std::async(std::launch::async, [dir]() -> Result<PastRun> {
        TRY_ASSIGN(auto events, events::read_event_log(dir / "events.jsonl"));
        PastRun past;
        past.id = fs::to_utf8(dir.filename());
        past.dir = dir;
        past.store = std::make_shared<events::RunStateStore>();
        for (const auto& e : events) past.store->apply(e);
        return past;
    });
    return {};
}

void Workspace::close_run() {
    if (run_live()) return;
    live_.reset();
    if (past_load_.valid()) past_load_.wait();
    past_load_ = {};
    past_.reset();
}

bool Workspace::run_live() const { return live_ && live_->controller && !live_->controller->finished(); }

run::RunController* Workspace::controller() { return live_ ? live_->controller.get() : nullptr; }

bool Workspace::run_read_only() const { return past_.has_value(); }

std::string Workspace::run_id() const {
    if (live_) return live_->id;
    if (past_) return past_->id;
    return {};
}

std::filesystem::path Workspace::run_dir() const {
    if (live_) return live_->dir;
    if (past_) return past_->dir;
    return {};
}

const std::vector<run::RunInfo>& Workspace::runs(bool refresh) {
    if (project_ && (refresh || !runs_loaded_)) {
        runs_ = run::list_runs(project_->runs_dir());
        runs_loaded_ = true;
    }
    return runs_;
}

std::shared_ptr<const events::RunStateData> Workspace::snapshot() {
    if (live_ && live_->state) return live_->state->snapshot();
    if (past_ && past_->store) return past_->store->snapshot();
    return nullptr;
}

void Workspace::end_run(bool abort) {
    if (!run_live()) return;
    if (abort) live_->controller->abort();
    else live_->controller->stop();
}

AppServices Workspace::services() {
    AppServices s;
    s.snapshot = [this] { return snapshot(); };
    s.commands = std::make_shared<WorkspaceCommands>(*this);
    s.project = [this] {
        return ProjectInfo{project_state_.root, project_state_.target, project_state_.phase == ProjectPhase::open};
    };
    s.post_empty_event = options_.wake;
    s.workspace = this;
    return s;
}

// ---- commands --------------------------------------------------------------------------------------

bool WorkspaceCommands::available() const {
    return workspace_.project_state().phase == ProjectPhase::open && !workspace_.run_read_only();
}

void WorkspaceCommands::start() {
    if (auto r = workspace_.start_run({}); !r) workspace_.report_error(std::format("cannot start the run: {}", r.error().message));
}

void WorkspaceCommands::start_functions(std::vector<u64> functions) {
    RunRequest request;
    request.functions = std::move(functions);
    if (auto r = workspace_.start_run(request); !r) workspace_.report_error(std::format("cannot start the run: {}", r.error().message));
}

void WorkspaceCommands::pause() {
    if (auto* c = controller()) c->pause();
}
void WorkspaceCommands::resume() {
    if (auto* c = controller()) c->unpause();
}
void WorkspaceCommands::stop() {
    if (auto* c = controller()) c->stop();
}
void WorkspaceCommands::abort() {
    if (auto* c = controller()) c->abort();
}
void WorkspaceCommands::pause_worker(int worker) {
    if (auto* c = controller()) c->pause_worker(worker);
}
void WorkspaceCommands::resume_worker(int worker) {
    if (auto* c = controller()) c->unpause_worker(worker);
}
void WorkspaceCommands::set_concurrency(int workers) {
    if (auto* c = controller()) c->set_concurrency(workers);
}
void WorkspaceCommands::set_run_budget(double usd) {
    if (auto* c = controller()) c->set_run_budget(usd);
}
void WorkspaceCommands::set_limits(const agent::LoopLimits& limits) {
    if (auto* c = controller()) c->set_limits(limits);
}
bool WorkspaceCommands::skip(u64 va) {
    auto* c = controller();
    return c && c->skip(va);
}
bool WorkspaceCommands::requeue(u64 va) {
    auto* c = controller();
    return c && c->requeue(va);
}
usize WorkspaceCommands::enqueue(std::vector<u64> functions) {
    auto* c = controller();
    auto program = workspace_.program();
    if (!c || !program) return 0;
    std::vector<run::QueueItem> items;
    for (u64 va : functions) {
        const Symbol* s = program->symbols().at(va);
        run::QueueItem item;
        item.va = va;
        item.name = s ? s->name : std::format("sub_{:x}", va);
        item.display = s && !s->display.empty() ? s->display : item.name;
        item.difficulty = s ? run::estimate_difficulty(*s) : 0;
        items.push_back(std::move(item));
    }
    return c->enqueue(std::move(items));
}
bool WorkspaceCommands::remove(u64 va) {
    auto* c = controller();
    return c && c->remove(va);
}
bool WorkspaceCommands::move(u64 va, usize index) {
    auto* c = controller();
    return c && c->move(va, index);
}
bool WorkspaceCommands::pin(u64 va, bool pinned) {
    auto* c = controller();
    return c && c->pin(va, pinned);
}
u64 WorkspaceCommands::inject(std::string_view session, std::string_view text) {
    auto* c = controller();
    return c ? c->inject(std::string(session), std::string(text)).value_or(0) : 0;
}
bool WorkspaceCommands::retract(std::string_view session, u64 id) {
    auto* c = controller();
    return c && c->retract(std::string(session), id);
}
bool WorkspaceCommands::decide(u64 approval, bool approve, std::string_view reason) {
    auto* c = controller();
    return c && c->decide(approval, approve, std::string(reason));
}
void WorkspaceCommands::set_policy(std::string_view action, agent::ApprovalPolicy policy) {
    if (auto* c = controller()) c->set_policy(std::string(action), policy);
}
std::vector<agent::PendingApproval> WorkspaceCommands::pending_approvals() const {
    auto* c = controller();
    return c ? c->pending_approvals() : std::vector<agent::PendingApproval>{};
}
int WorkspaceCommands::concurrency() const {
    auto* c = controller();
    return c ? c->concurrency() : 0;
}
double WorkspaceCommands::run_budget() const {
    auto* c = controller();
    return c ? c->ledger()->limit() : 0;
}

} // namespace decomp::gui
