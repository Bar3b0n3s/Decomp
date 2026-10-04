#include "run/controller.hpp"

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <format>

namespace decomp::run {

using namespace std::chrono_literals;

namespace {

std::string now_iso() { return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())); }

// Outcomes after which a resumed run does not work on the function again.
bool final_outcome(std::string_view outcome) {
    return outcome == "matched" || outcome == "gave_up" || outcome == "refused" || outcome == "budget_exhausted" ||
           outcome == "max_turns" || outcome == "no_result" || outcome == "skipped";
}

Json limits_json(const agent::LoopLimits& l) {
    return Json{{"max_turns", l.max_turns}, {"max_usd", l.max_cost_usd}, {"max_tokens", l.max_total_tokens}, {"max_seconds", l.max_wall.count()}};
}

Json policies_json(const std::map<std::string, agent::ApprovalPolicy, std::less<>>& policies) {
    Json j = Json::object();
    for (const auto& [action, policy] : policies) j[action] = std::string(agent::to_string(policy));
    return j;
}

std::string display_of(const QueueItem& item) { return item.display.empty() ? item.name : item.display; }

// sessions/<safe>.jsonl for a function's first session in the run, sessions/<safe>.<n>.jsonl later.
std::string transcript_file(const Symbol& fn, int session_number) {
    const std::string safe = project::safe_function_name(fn);
    return session_number <= 1 ? safe + ".jsonl" : std::format("{}.{}.jsonl", safe, session_number);
}

} // namespace

RunController::RunController(RunDeps deps, events::EventBus& bus) : deps_(std::move(deps)), bus_(bus) {
    approvals_ = std::make_shared<agent::ApprovalGate>(&bus_);
    // The shared rate gate's view of the account's limits, for the top bar's gauge and the Run monitor.
    gate_->set_listener([this](const agent::RateLimitSnapshot& s) {
        bus_.publish(events::RateLimitUpdated{s.requests_limit, s.requests_remaining, s.input_limit, s.input_remaining, s.output_limit,
                                              s.output_remaining, s.reset, s.backoff_ms});
    });
    needs_program_ = !deps_.run_session;
    if (!deps_.run_session)
        deps_.run_session = [](const SessionRequest& r, events::EventBus& b) {
            return agent::run_function(*r.program, r.project, r.setup, r.va, r.config, b, r.transcript, r.control, r.worker);
        };
}

RunController::~RunController() {
    bool live = false;
    {
        std::lock_guard lock(mutex_);
        live = started_ && !finished_;
    }
    if (live) abort();
    wait();
    gate_->set_listener(nullptr);
    summary_state_.detach();
}

// ---- starting ----------------------------------------------------------------------------------------

void RunController::begin_locked(RunStore store, RunOptions options) {
    store_ = std::move(store);
    options_ = std::move(options);
    concurrency_ = std::clamp(options_.workers, 1, kMaxWorkers);
    options_.workers = concurrency_;
    for (const auto& [action, policy] : options_.policies) approvals_->set_policy(action, policy);
    stagger_open_ = options_.stagger_timeout.count() <= 0;
    stagger_deadline_ = std::chrono::steady_clock::now() + options_.stagger_timeout;
    started_ = true;
}

Result<void> RunController::start(RunStore store, std::vector<QueueItem> items, RunOptions options) {
    if (items.empty()) return make_error(ErrorCode::invalid_argument, "no functions to run");
    {
        std::lock_guard lock(mutex_);
        if (started_) return make_error(ErrorCode::invalid_argument, "this controller has already run");
    }
    std::vector<std::string> names;
    std::vector<u64> vas;
    for (auto& item : items) {
        item.state = ItemState::pending;
        item.sessions = 0;
        item.outcome.clear();
        item.worker = -1;
        names.push_back(display_of(item));
        vas.push_back(item.va);
    }
    summary_state_.attach(bus_);
    events::RunStarted started;
    double budget = 0;
    {
        std::lock_guard lock(mutex_);
        queue_ = WorkQueue(std::move(items));
        ledger_ = std::make_shared<agent::SpendLedger>(options.run_budget_usd);
        created_ = now_iso();
        begin_locked(std::move(store), std::move(options));
        Json config = run_json_locked();
        config.erase("queue");
        started = events::RunStarted{options_.project_name, options_.agent.conversation.model, options_.agent.conversation.effort,
                                     concurrency_, std::move(names), std::move(vas), std::move(config)};
        budget = ledger_->limit();
    }
    persist(true);
    bus_.publish(std::move(started));
    if (budget > 0) bus_.publish(events::BudgetChanged{"run", budget});
    notify_queue();
    std::lock_guard lock(mutex_);
    for (int w = 0; w < concurrency_; ++w) spawn_locked(w);
    return {};
}

Result<void> RunController::resume(RunStore store, RunOptions options) {
    {
        std::lock_guard lock(mutex_);
        if (started_) return make_error(ErrorCode::invalid_argument, "this controller has already run");
    }
    TRY_ASSIGN(Json run, store.read_run());
    std::vector<QueueItem> items;
    if (auto q = run.find("queue"); q != run.end() && q->is_array())
        for (const auto& j : *q) {
            TRY_ASSIGN(auto item, QueueItem::from_json(j));
            items.push_back(std::move(item));
        }
    if (items.empty()) return make_error(ErrorCode::parse, "run {} has no functions in run.json", store.id());

    // The event log is the record of what happened (run.json may lag behind it).
    std::vector<events::Event> past;
    std::error_code ec;
    if (std::filesystem::exists(store.events_path(), ec)) {
        TRY_ASSIGN(auto read, events::read_event_log(store.events_path()));
        past = std::move(read);
    }
    for (const auto& e : past) summary_state_.apply(e);  // the summary covers the whole run
    const auto replayed = summary_state_.snapshot();
    struct Latest {
        const events::SessionState* session = nullptr;
        int count = 0;
    };
    std::map<u64, Latest> latest;
    for (const auto& [id, s] : replayed->sessions) {
        auto& l = latest[s->va];
        ++l.count;
        if (!l.session || s->started > l.session->started || (s->started == l.session->started && id > l.session->id)) l.session = s.get();
    }
    std::vector<u64> interrupted;
    for (auto& item : items) {
        item.worker = -1;
        if (auto it = latest.find(item.va); it != latest.end()) {
            item.sessions = std::max(item.sessions, it->second.count);
            if (!it->second.session->finished) {
                interrupted.push_back(item.va);
                item.outcome = "interrupted";
                item.state = ItemState::pending;
                continue;
            }
            item.outcome = it->second.session->outcome;
        }
        if (item.state == ItemState::skipped || item.outcome == "skipped") {
            item.state = ItemState::skipped;
            continue;
        }
        item.state = final_outcome(item.outcome) ? ItemState::done : ItemState::pending;
        // A function matched since (by hand, or by another run) needs no session.
        if (item.state == ItemState::pending && deps_.project &&
            deps_.project->function_info(item.va).status == project::FunctionStatus::matched) {
            item.state = ItemState::done;
            item.outcome = "matched";
        }
    }
    bus_.set_next_seq(replayed->last_seq + 1);
    summary_state_.attach(bus_);
    double budget = 0;
    {
        std::lock_guard lock(mutex_);
        queue_ = WorkQueue(std::move(items));
        ledger_ = std::make_shared<agent::SpendLedger>(options.run_budget_usd, replayed->cost_usd);
        created_ = json_string_or(run, "created", now_iso());
        begin_locked(std::move(store), std::move(options));
        budget = ledger_->limit();
    }
    persist(true);
    bus_.publish(events::RunResumed{interrupted});
    if (budget > 0) bus_.publish(events::BudgetChanged{"run", budget});
    notify_queue();
    std::lock_guard lock(mutex_);
    for (int w = 0; w < concurrency_; ++w) spawn_locked(w);
    return {};
}

// ---- workers -----------------------------------------------------------------------------------------

void RunController::spawn_locked(int worker) {
    if (alive_.contains(worker)) return;
    if (auto it = threads_.find(worker); it != threads_.end()) {
        retired_.push_back(std::move(it->second));  // its loop has exited; joined later
        threads_.erase(it);
    }
    alive_.insert(worker);
    threads_.emplace(worker, std::jthread([this, worker] { worker_main(worker); }));
}

void RunController::worker_main(int worker) {
    std::string announced;
    bool last = false;
    while (true) {
        Dispatch d = next_dispatch(worker, announced, last);
        if (d.kind == Dispatch::Kind::exit) {
            if (d.retired) bus_.publish(events::WorkerPhaseChanged{"retired", "", ""}, worker);
            break;
        }
        if (d.kind == Dispatch::Kind::wait) {
            announced = d.reason;
            bus_.publish(events::WorkerPhaseChanged{d.reason, "", ""}, worker);
            continue;
        }
        announced.clear();
        run_item(worker, d);
    }
    if (last) finish_run();
}

bool RunController::should_exit_locked(int worker) const {
    if (aborting_ || stopping_) return true;
    if (worker >= concurrency_) return true;  // retired by a lower concurrency
    if (running_.empty()) {
        if (queue_.pending_count() == 0) return true;
        if (ledger_->exhausted()) return true;  // nothing can be dispatched until the budget is raised
    }
    return false;
}

std::optional<std::string> RunController::blocked_locked(int worker) {
    if (paused_ || paused_workers_.contains(worker)) return "paused";
    if (ledger_->exhausted()) return "run budget exhausted";
    if (queue_.pending_count() == 0) return "idle";
    if (!stagger_open_) {
        if (std::chrono::steady_clock::now() >= stagger_deadline_) stagger_open_ = true;
        else if (dispatched_ > 0) return "waiting for the first session";
    }
    return std::nullopt;
}

RunController::Dispatch RunController::next_dispatch(int worker, const std::string& announced, bool& last) {
    std::unique_lock lock(mutex_);
    while (true) {
        if (should_exit_locked(worker)) {
            // Decided under the lock, so a command arriving now sees the run finishing.
            Dispatch d;
            d.retired = worker >= concurrency_ && !stopping_ && !aborting_;
            alive_.erase(worker);
            if (alive_.empty() && !finishing_) {
                finishing_ = true;
                last = true;
            }
            return d;
        }
        if (auto reason = blocked_locked(worker)) {
            if (*reason != announced) {
                Dispatch d;
                d.kind = Dispatch::Kind::wait;
                d.reason = std::move(*reason);
                return d;
            }
            cv_.wait_for(lock, 250ms);  // also notices the stagger timeout
            continue;
        }
        Dispatch d;
        d.kind = Dispatch::Kind::run;
        d.item = *queue_.next(worker);
        d.session = d.item.sessions <= 1 ? std::format("{}-{:x}", store_->id(), d.item.va)
                                         : std::format("{}-{:x}-{}", store_->id(), d.item.va, d.item.sessions);
        d.control = std::make_shared<agent::LoopControl>();
        if (limits_) d.control->set_limits(*limits_);
        running_[worker] = Running{d.item.va, d.session, d.control};
        ++dispatched_;
        return d;
    }
}

void RunController::run_item(int worker, Dispatch& d) {
    const u64 va = d.item.va;
    std::shared_ptr<const Program> program = deps_.program ? deps_.program() : nullptr;
    const Symbol* found = program ? program->symbols().at(va) : nullptr;
    Symbol fallback;
    if (!found) {
        fallback.va = va;
        fallback.name = d.item.name.empty() ? std::format("sub_{:x}", va) : d.item.name;
        fallback.kind = SymbolKind::function;
    }
    const Symbol& sym = found ? *found : fallback;
    const std::string display = display_of(d.item);

    SessionRequest request;
    request.program = program;
    request.project = deps_.project;
    request.va = va;
    request.session_id = d.session;
    request.worker = worker;
    request.setup = deps_.setup;
    request.control = d.control.get();
    {
        std::lock_guard lock(mutex_);
        request.transcript = store_->sessions_dir() / fs::from_utf8(transcript_file(sym, d.item.sessions));
        request.config = options_.agent;
    }
    request.config.session_id = d.session;
    request.config.client.gate = gate_;
    request.config.loop.ledger = ledger_;
    request.config.approvals = approvals_;
    request.config.on_first_message = [this] { open_stagger(); };

    std::optional<std::string> failure;
    if (!program && needs_program_) failure = "no program is loaded";
    else if (deps_.transport) {
        auto transport = deps_.transport(sym);
        if (transport) request.config.transport = std::move(*transport);
        else failure = transport.error().message;
    }

    agent::FunctionRunResult result;
    if (failure) {
        // The session could not start: report it the way a session would.
        bus_.publish(events::SessionStarted{d.session, sym.name, display, va, ""}, worker);
        bus_.publish(events::LogLine{"error", std::format("{}: {}", display, *failure), d.session}, worker);
        bus_.publish(events::SessionFinished{d.session, "error", *failure, 0, 0, 0}, worker);
        result.outcome = "error";
        result.detail = *failure;
    } else {
        try {
            result = deps_.run_session(request, bus_);
        } catch (const std::exception& e) {
            result.outcome = "error";
            result.detail = std::format("unexpected error: {}", e.what());
        } catch (...) {
            result.outcome = "error";
            result.detail = "unexpected error";
        }
        if (result.outcome == "error" && result.detail.starts_with("unexpected error"))
            bus_.publish(events::SessionFinished{d.session, "error", result.detail, result.best_match, result.turns, result.cost_usd}, worker);
    }
    {
        std::lock_guard lock(mutex_);
        queue_.finish(va, result.outcome);
        running_.erase(worker);
        stagger_open_ = true;  // a finished session has warmed the cache too
    }
    cv_.notify_all();
    persist(false);
    write_summary(false);
}

void RunController::finish_run() {
    std::string status;
    {
        std::lock_guard lock(mutex_);
        if (aborting_) status = "aborted";
        else if (stopping_) status = "stopped";
        else if (queue_.pending_count() > 0 && ledger_->exhausted()) status = "budget_exhausted";
        else status = "completed";
        final_status_ = status;
    }
    bus_.publish(events::RunFinished{status});
    persist(true);
    write_summary(true);
    {
        std::lock_guard lock(mutex_);
        if (store_) store_->release();
        finished_ = true;
    }
    finished_cv_.notify_all();
}

void RunController::open_stagger() {
    {
        std::lock_guard lock(mutex_);
        if (stagger_open_) return;
        stagger_open_ = true;
    }
    cv_.notify_all();
}

// ---- persistence and events ----------------------------------------------------------------------------

std::string RunController::live_status_locked() const {
    if (!final_status_.empty()) return final_status_;
    if (!started_) return "starting";
    if (aborting_) return "aborting";
    if (stopping_) return "stopping";
    if (paused_) return "paused";
    return "running";
}

Json RunController::run_json_locked() const {
    Json queue = Json::array();
    for (const auto& item : queue_.items()) queue.push_back(item.to_json());
    usize matched = 0;
    for (const auto& item : queue_.items()) matched += item.outcome == "matched";
    return Json{{"version", 1},
                {"id", store_->id()},
                {"status", live_status_locked()},
                {"created", created_},
                {"updated", now_iso()},
                {"project", options_.project_name},
                {"model", options_.agent.conversation.model},
                {"effort", options_.agent.conversation.effort},
                {"workers", concurrency_},
                {"run_budget_usd", ledger_->limit()},
                {"spent_usd", ledger_->spent()},
                {"limits", limits_json(limits_.value_or(options_.agent.loop.limits))},
                {"policies", policies_json(approvals_->policies())},
                {"replay", options_.replay},
                {"replay_dir", options_.replay_dir},
                {"selection", options_.selection},
                {"counts",
                 {{"pending", queue_.count(ItemState::pending)},
                  {"running", queue_.count(ItemState::running)},
                  {"done", queue_.count(ItemState::done)},
                  {"skipped", queue_.count(ItemState::skipped)},
                  {"matched", matched}}},
                {"queue", std::move(queue)}};
}

void RunController::persist(bool force) {
    std::lock_guard order(persist_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - last_persist_ < 250ms) return;  // a later (or the final) write catches up
    Json run;
    {
        std::lock_guard lock(mutex_);
        if (!store_) return;
        run = run_json_locked();
    }
    last_persist_ = now;
    if (auto written = store_->write_run(run); !written) log::warn("cannot write run.json: {}", written.error().message);
}

void RunController::write_summary(bool force) {
    std::lock_guard order(persist_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - last_summary_ < 2s) return;
    {
        std::lock_guard lock(mutex_);
        if (!store_) return;
    }
    last_summary_ = now;
    if (auto written = store_->write_summary(run_summary(*summary_state_.snapshot())); !written)
        log::warn("cannot write summary.json: {}", written.error().message);
}

void RunController::notify_queue() {
    std::lock_guard order(queue_event_mutex_);
    std::vector<events::QueueEntry> entries;
    {
        std::lock_guard lock(mutex_);
        for (const QueueItem* item : queue_.pending()) entries.push_back(events::QueueEntry{item->va, display_of(*item), item->pinned, item->difficulty});
    }
    bus_.publish(events::QueueUpdated{std::move(entries)});
}

void RunController::control_event(std::string command, std::string target, std::string detail) {
    bus_.publish(events::Control{std::move(command), std::move(target), std::move(detail)});
}

// ---- commands ----------------------------------------------------------------------------------------

void RunController::pause() {
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_ || paused_) return;
        paused_ = true;
        for (auto& [w, r] : running_) r.control->request_pause();
    }
    control_event("pause");
    persist(true);
}

void RunController::unpause() {
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_ || !paused_) return;
        paused_ = false;
        for (auto& [w, r] : running_)
            if (!paused_workers_.contains(w)) r.control->resume();
    }
    cv_.notify_all();
    control_event("resume");
    persist(true);
}

void RunController::pause_worker(int worker) {
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_ || !paused_workers_.insert(worker).second) return;
        if (auto it = running_.find(worker); it != running_.end()) it->second.control->request_pause();
    }
    control_event("pause", std::format("worker {}", worker));
}

void RunController::unpause_worker(int worker) {
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_ || paused_workers_.erase(worker) == 0) return;
        if (!paused_)
            if (auto it = running_.find(worker); it != running_.end()) it->second.control->resume();
    }
    cv_.notify_all();
    control_event("resume", std::format("worker {}", worker));
}

void RunController::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_ || stopping_ || aborting_) return;
        stopping_ = true;
        for (auto& [w, r] : running_) r.control->request_stop(agent::StopReason::user);
    }
    cv_.notify_all();
    control_event("stop");
    persist(true);
}

void RunController::abort() {
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_ || aborting_) return;
        aborting_ = true;
        for (auto& [w, r] : running_) r.control->request_abort(agent::StopReason::user);
    }
    approvals_->cancel_all("the run was aborted");
    cv_.notify_all();
    control_event("abort");
    persist(true);
}

bool RunController::skip(u64 va) {
    bool was_pending = false, was_running = false;
    std::string display;
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_) return false;
        const QueueItem* item = queue_.find(va);
        if (!item) return false;
        display = display_of(*item);
        if (item->state == ItemState::pending) was_pending = queue_.skip(va);
        else if (item->state == ItemState::running)
            for (auto& [w, r] : running_)
                if (r.va == va) {
                    r.control->request_stop(agent::StopReason::skip);
                    was_running = true;
                }
    }
    if (!was_pending && !was_running) return false;
    control_event("skip", display);
    if (was_pending) notify_queue();
    persist(true);
    return true;
}

bool RunController::requeue(u64 va) {
    std::string display;
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_ || !queue_.requeue(va)) return false;
        display = display_of(*queue_.find(va));
    }
    cv_.notify_all();
    control_event("requeue", display);
    notify_queue();
    persist(true);
    return true;
}

usize RunController::enqueue(std::vector<QueueItem> items) {
    usize added = 0;
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_) return 0;
        added = queue_.enqueue(std::move(items));
    }
    if (added == 0) return 0;
    cv_.notify_all();
    control_event("enqueue", "", std::format("{} function(s)", added));
    notify_queue();
    persist(true);
    return added;
}

bool RunController::remove(u64 va) {
    std::string display;
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_) return false;
        const QueueItem* item = queue_.find(va);
        if (!item) return false;
        display = display_of(*item);
        if (!queue_.remove(va)) return false;
    }
    cv_.notify_all();  // an emptied queue lets idle workers finish
    control_event("remove", display);
    notify_queue();
    persist(true);
    return true;
}

bool RunController::move(u64 va, usize index) {
    std::string display;
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_ || !queue_.move(va, index)) return false;
        display = display_of(*queue_.find(va));
    }
    control_event("move", display, std::to_string(index));
    notify_queue();
    persist(true);
    return true;
}

bool RunController::pin(u64 va, bool pinned) {
    std::string display;
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_ || !queue_.pin(va, pinned)) return false;
        display = display_of(*queue_.find(va));
    }
    control_event(pinned ? "pin" : "unpin", display);
    notify_queue();
    persist(true);
    return true;
}

void RunController::set_concurrency(int workers) {
    workers = std::clamp(workers, 1, kMaxWorkers);
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_ || workers == concurrency_) return;
        concurrency_ = workers;
        for (int w = 0; w < workers; ++w) spawn_locked(w);  // workers above the new count retire when idle
    }
    cv_.notify_all();
    control_event("set_concurrency", "", std::to_string(workers));
    persist(true);
}

void RunController::set_run_budget(double usd) {
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_) return;
        ledger_->set_limit(usd);
    }
    cv_.notify_all();
    bus_.publish(events::BudgetChanged{"run", std::max(usd, 0.0)});
    control_event("set_run_budget", "", std::format("{:.2f}", std::max(usd, 0.0)));
    persist(true);
}

void RunController::set_limits(const agent::LoopLimits& limits) {
    {
        std::lock_guard lock(mutex_);
        if (!started_ || finishing_) return;
        limits_ = limits;
        for (auto& [w, r] : running_) r.control->set_limits(limits);
    }
    bus_.publish(events::BudgetChanged{"function", limits.max_cost_usd, limits.max_total_tokens, limits.max_turns,
                                       static_cast<int>(limits.max_wall.count() / 60)});
    control_event("set_limits", "",
                  std::format("{} turns, ${:.2f}, {} tokens, {} s", limits.max_turns, limits.max_cost_usd, limits.max_total_tokens,
                              limits.max_wall.count()));
    persist(true);
}

std::optional<u64> RunController::inject(const std::string& session, std::string text) {
    if (trim(text).empty()) return std::nullopt;
    std::optional<u64> id;
    {
        std::lock_guard lock(mutex_);
        for (auto& [w, r] : running_)
            if (r.session == session) id = r.control->inject(text);
    }
    if (id) control_event("inject", session, std::move(text));
    return id;
}

bool RunController::retract(const std::string& session, u64 id) {
    bool retracted = false;
    {
        std::lock_guard lock(mutex_);
        for (auto& [w, r] : running_)
            if (r.session == session) retracted = r.control->retract(id);
    }
    if (retracted) control_event("retract", session, std::to_string(id));
    return retracted;
}

bool RunController::decide(u64 approval, bool approve, std::string reason) {
    if (!approvals_->decide(approval, approve, reason, "user")) return false;
    control_event(approve ? "approve" : "deny", std::to_string(approval), std::move(reason));
    return true;
}

void RunController::set_policy(const std::string& action, agent::ApprovalPolicy policy) {
    approvals_->set_policy(action, policy);
    control_event("set_policy", action, std::string(agent::to_string(policy)));
    persist(true);
}

// ---- state ---------------------------------------------------------------------------------------------

void RunController::wait() {
    {
        std::unique_lock lock(mutex_);
        finished_cv_.wait(lock, [&] { return !started_ || finished_; });
    }
    std::lock_guard join(join_mutex_);
    std::map<int, std::jthread> threads;
    std::vector<std::jthread> retired;
    {
        std::lock_guard lock(mutex_);
        threads.swap(threads_);
        retired.swap(retired_);
    }
    for (auto& [w, t] : threads)
        if (t.joinable()) t.join();
    for (auto& t : retired)
        if (t.joinable()) t.join();
}

bool RunController::wait_for(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return finished_cv_.wait_for(lock, timeout, [&] { return !started_ || finished_; });
}

bool RunController::finished() const {
    std::lock_guard lock(mutex_);
    return finished_;
}

std::string RunController::status() const {
    std::lock_guard lock(mutex_);
    return live_status_locked();
}

std::vector<QueueItem> RunController::queue() const {
    std::lock_guard lock(mutex_);
    return queue_.items();
}

std::vector<std::string> RunController::running_sessions() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [w, r] : running_) out.push_back(r.session);
    return out;
}

int RunController::concurrency() const {
    std::lock_guard lock(mutex_);
    return concurrency_;
}

std::string RunController::run_id() const {
    std::lock_guard lock(mutex_);
    return store_ ? store_->id() : std::string();
}

std::filesystem::path RunController::run_dir() const {
    std::lock_guard lock(mutex_);
    return store_ ? store_->dir() : std::filesystem::path();
}

std::vector<agent::PendingApproval> RunController::pending_approvals() const { return approvals_->pending(); }

std::shared_ptr<agent::SpendLedger> RunController::ledger() const {
    std::lock_guard lock(mutex_);
    return ledger_;
}

} // namespace decomp::run
