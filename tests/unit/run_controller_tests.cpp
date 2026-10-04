// The run controller's scheduling and commands, with a fake session function (no API, no compiler).

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "events/bus.hpp"
#include "events/run_state.hpp"
#include "run/controller.hpp"
#include "run/store.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

using namespace decomp;
using namespace decomp::run;
using namespace std::chrono_literals;

namespace {

// Pretends to be a session: a few short "turns" that honor pause, stop, abort and the run ledger, each
// costing `cost_per_turn`; optionally held until released; optionally asking for approval.
struct FakeSessions {
    std::mutex mutex;
    std::condition_variable cv;
    int active = 0, max_active = 0, started = 0;
    std::vector<u64> dispatch_order;
    std::vector<int> workers;  // per dispatch
    std::vector<std::string> session_ids, transcripts;
    std::set<u64> held;      // these wait until released
    std::set<u64> released;
    bool hold_all = false;
    int turns = 2;
    std::chrono::milliseconds turn_time{1};
    double cost_per_turn = 0;
    std::set<u64> ask_approval;
    // Every other function (by index: addresses are 0x10 apart) matches.
    std::function<std::string(u64)> outcome = [](u64 va) { return (va / 0x10) % 2 == 0 ? std::string("matched") : std::string("gave_up"); };

    void release(u64 va) {
        std::lock_guard lock(mutex);
        released.insert(va);
        cv.notify_all();
    }
    void release_all() {
        std::lock_guard lock(mutex);
        hold_all = false;
        held.clear();
        cv.notify_all();
    }
    void hold(std::set<u64> vas) {
        std::lock_guard lock(mutex);
        held = std::move(vas);
    }
    bool wait_until(const std::function<bool()>& ready, std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, ready);
    }

    SessionFn fn() {
        return [this](const SessionRequest& r, events::EventBus& bus) { return run(r, bus); };
    }

    agent::FunctionRunResult run(const SessionRequest& r, events::EventBus& bus) {
        bus.publish(events::SessionStarted{r.session_id, std::format("f_{:x}", r.va), std::format("f_{:x}", r.va), r.va,
                                           fs::to_utf8(r.transcript.filename())},
                    r.worker);
        if (r.config.on_first_message) r.config.on_first_message();
        {
            std::lock_guard lock(mutex);
            ++active;
            ++started;
            max_active = std::max(max_active, active);
            dispatch_order.push_back(r.va);
            workers.push_back(r.worker);
            session_ids.push_back(r.session_id);
            transcripts.push_back(fs::to_utf8(r.transcript.filename()));
            cv.notify_all();
        }
        agent::FunctionRunResult result;
        auto stopped = [&]() -> bool {
            auto* c = r.control;
            if (!c->stop_requested() && !c->abort_requested()) return false;
            const auto reason = c->stop_reason();
            result.outcome = reason == agent::StopReason::skip ? "skipped" : c->abort_requested() ? "aborted" : "stopped";
            return true;
        };
        // Held sessions wait for the test (a stop or abort also ends the wait).
        {
            std::unique_lock lock(mutex);
            while ((hold_all || held.contains(r.va)) && !released.contains(r.va) && !r.control->stop_requested() &&
                   !r.control->abort_requested())
                cv.wait_for(lock, 2ms);
        }
        for (int turn = 1; result.outcome.empty(); ++turn) {
            if (stopped()) break;
            if (r.control->is_paused() && !r.control->wait_while_paused()) continue;  // stopped while paused
            bus.publish(events::TurnStarted{r.session_id, turn}, r.worker);
            std::this_thread::sleep_for(turn_time);
            result.turns = turn;
            result.cost_usd += cost_per_turn;
            if (r.config.loop.ledger) r.config.loop.ledger->add(cost_per_turn);
            bus.publish(events::TurnFinished{r.session_id, turn, "tool_use", events::TokenUsage{100, 10, 0, 0}, cost_per_turn, 1}, r.worker);
            const auto limits = r.control->limits().value_or(r.config.loop.limits);
            if (turn >= std::min(turns, limits.max_turns)) {
                result.outcome = turn < turns ? "max_turns" : outcome(r.va);
                break;
            }
            if (r.config.loop.ledger && r.config.loop.ledger->exhausted()) result.outcome = "run_budget_exhausted";
        }
        if (result.outcome == "matched" && ask_approval.contains(r.va) && r.config.approvals) {
            auto d = r.config.approvals->request(agent::ApprovalRequest{std::string(agent::kWriteSourceAction), r.session_id,
                                                                        std::format("f_{:x}", r.va), r.va, "src/f.cpp", "byte-exact", "int f;", ""},
                                                 [&] { return r.control->abort_requested(); }, r.worker);
            if (!d.approved()) result.outcome = "gave_up";
        }
        result.matched = result.outcome == "matched";
        result.best_match = result.matched ? 100 : 50;
        {
            std::lock_guard lock(mutex);
            --active;
            cv.notify_all();
        }
        bus.publish(events::SessionFinished{r.session_id, result.outcome, "", result.best_match, result.turns, result.cost_usd}, r.worker);
        return result;
    }
};

std::vector<QueueItem> make_items(int n, u64 base = 0x1000) {
    std::vector<QueueItem> items;
    for (int i = 0; i < n; ++i) {
        const u64 va = base + static_cast<u64>(i) * 0x10;
        items.push_back(QueueItem{.va = va, .name = std::format("f_{:x}", va), .display = std::format("f_{:x}", va)});
    }
    return items;
}

// A run in a temporary directory: the bus, its event log, a recorder and the controller.
struct Harness {
    fs::TempDir dir = fs::TempDir::create("decomp-run").value();
    std::string id;
    events::EventBus bus;
    std::unique_ptr<events::JsonlEventLog> log;
    events::RunStateStore state;
    std::mutex events_mutex;
    std::vector<events::Event> events;
    FakeSessions sessions;
    std::unique_ptr<RunController> controller;

    explicit Harness(std::string run_id = "2026-10-04T12-00-00-test") : id(run_id), bus(run_id) { attach(); }

    void attach() {
        log = events::JsonlEventLog::open(dir.path() / "runs" / id / "events.jsonl").value();
        bus.subscribe([this](const events::Event& e) {
            if (log) log->write(e);
        });
        bus.subscribe([this](const events::Event& e) {
            std::lock_guard lock(events_mutex);
            events.push_back(e);
        });
        state.attach(bus);
        controller = std::make_unique<RunController>(RunDeps{.run_session = sessions.fn()}, bus);
    }

    RunOptions options(int workers) {
        RunOptions o;
        o.workers = workers;
        o.stagger_timeout = 0ms;
        o.project_name = "test";
        return o;
    }

    Result<void> start(int n, int workers, RunOptions o) {
        auto store = RunStore::create(dir.path() / "runs", id);
        if (!store) return std::unexpected(store.error());
        o.workers = workers;
        return controller->start(std::move(*store), make_items(n), std::move(o));
    }

    std::map<std::string, int> counts() {
        std::lock_guard lock(events_mutex);
        std::map<std::string, int> out;
        for (const auto& e : events) ++out[std::string(events::type_name(e.payload))];
        return out;
    }
    std::vector<events::Control> controls() {
        std::lock_guard lock(events_mutex);
        std::vector<events::Control> out;
        for (const auto& e : events)
            if (const auto* c = std::get_if<events::Control>(&e.payload)) out.push_back(*c);
        return out;
    }
    Json run_json() { return parse_json(fs::read_text(dir.path() / "runs" / id / "run.json").value()).value(); }
    // Closes the harness's handle on events.jsonl (Windows cannot replace a file that is open).
    void close_log() { log.reset(); }
    std::filesystem::path run_dir() const { return dir.path() / "runs" / id; }
};

std::map<u64, int> dispatch_counts(FakeSessions& s) {
    std::lock_guard lock(s.mutex);
    std::map<u64, int> out;
    for (u64 va : s.dispatch_order) ++out[va];
    return out;
}

} // namespace

TEST_CASE("run controller: 200 functions on 4 workers") {
    Harness h;
    REQUIRE(h.start(200, 4, h.options(4)));
    h.controller->wait();
    CHECK(h.controller->status() == "completed");
    const auto per_va = dispatch_counts(h.sessions);
    CHECK(per_va.size() == 200);
    for (const auto& [va, n] : per_va) CHECK(n == 1);
    CHECK(h.sessions.max_active <= 4);
    CHECK(h.sessions.max_active >= 2);
    std::set<int> workers(h.sessions.workers.begin(), h.sessions.workers.end());
    CHECK(*workers.rbegin() <= 3);
    auto counts = h.counts();
    CHECK(counts["run_started"] == 1);
    CHECK(counts["run_finished"] == 1);
    CHECK(counts["session_started"] == 200);
    CHECK(counts["session_finished"] == 200);

    const auto snap = h.state.snapshot();
    CHECK(snap->status == "completed");
    CHECK(snap->finished == 200);
    CHECK(snap->matched == 100);
    CHECK(snap->queue->empty());  // every dispatched function left the queue

    const Json run = h.run_json();
    CHECK(run["status"] == "completed");
    CHECK(run["counts"]["done"] == 200);
    CHECK(run["counts"]["matched"] == 100);
    CHECK(run["queue"].size() == 200);
    // summary.json matches what a replay of the log gives.
    const Json summary = parse_json(fs::read_text(h.run_dir() / "summary.json").value()).value();
    CHECK(summary["functions"].size() == 200);
    CHECK(summary["functions_matched"] == 100);
    const auto replayed = events::RunState::replay(events::read_event_log(h.run_dir() / "events.jsonl").value());
    CHECK(run_summary(replayed.data()) == summary);
    // Session ids and transcripts: "<run>-<va>" and "<safe>.jsonl" for a function's first session.
    CHECK(h.sessions.session_ids.front().starts_with(h.id + "-"));
    CHECK(h.sessions.transcripts.front().ends_with(".jsonl"));
    CHECK(h.sessions.transcripts.front().find('.') == h.sessions.transcripts.front().rfind('.'));
    // The run is over: its lock is free and the listing says so.
    auto info = read_run_info(h.run_dir());
    REQUIRE(info);
    CHECK(info->status == "completed");
    CHECK_FALSE(info->live);
    CHECK(info->functions == 200);
    CHECK(info->matched == 100);
}

TEST_CASE("run controller: the first session runs alone until the API answers (stagger)") {
    Harness h;
    RunOptions o = h.options(4);
    o.stagger_timeout = 10s;
    // The fake session reports its first message only after being released.
    h.controller = std::make_unique<RunController>(
        RunDeps{.run_session =
                    [&](const SessionRequest& r, events::EventBus& bus) {
                        SessionRequest delayed = r;
                        auto first = r.config.on_first_message;
                        delayed.config.on_first_message = nullptr;
                        {
                            std::unique_lock lock(h.sessions.mutex);
                            if (r.va == 0x1000)
                                h.sessions.cv.wait_for(lock, 10s, [&] { return h.sessions.released.contains(0x1000); });
                        }
                        if (first) first();
                        return h.sessions.run(delayed, bus);
                    }},
        h.bus);
    REQUIRE(h.start(8, 4, o));
    std::this_thread::sleep_for(100ms);
    {
        std::lock_guard lock(h.sessions.mutex);
        CHECK(h.sessions.started == 0);  // the first one waits for its first message; nobody else started
    }
    h.sessions.release(0x1000);
    h.controller->wait();
    CHECK(h.sessions.started == 8);
    CHECK(h.sessions.dispatch_order.front() == 0x1000);
}

TEST_CASE("run controller: pause, per-worker pause and resume") {
    Harness h;
    h.sessions.hold_all = true;
    REQUIRE(h.start(10, 2, h.options(2)));
    REQUIRE(h.sessions.wait_until([&] { return h.sessions.started == 2; }));
    h.controller->pause();
    CHECK(h.controller->status() == "paused");
    h.sessions.release_all();
    std::this_thread::sleep_for(100ms);
    {
        std::lock_guard lock(h.sessions.mutex);
        CHECK(h.sessions.started == 2);  // nothing new starts...
        CHECK(h.sessions.active == 2);   // ...and the running sessions wait between turns
    }
    int on_worker1 = 0;
    {
        std::lock_guard lock(h.sessions.mutex);
        on_worker1 = h.sessions.workers[0] == 1 ? 0 : 1;  // which of the two dispatches went to worker 1
    }
    h.controller->pause_worker(1);
    h.controller->unpause();
    CHECK(h.controller->status() == "running");
    REQUIRE(h.sessions.wait_until([&] { return h.sessions.started >= 5; }));
    {
        std::lock_guard lock(h.sessions.mutex);
        for (usize i = 2; i < h.sessions.workers.size(); ++i) CHECK(h.sessions.workers[i] == 0);  // worker 1 stays paused
        CHECK(h.sessions.active >= 1);  // worker 1's session is still waiting
    }
    h.controller->unpause_worker(1);
    h.controller->wait();
    CHECK(h.sessions.started == 10);
    CHECK(on_worker1 >= 0);
    auto controls = h.controls();
    REQUIRE(controls.size() == 4);
    CHECK(controls[0].command == "pause");
    CHECK(controls[0].target.empty());
    CHECK(controls[1].command == "pause");
    CHECK(controls[1].target == "worker 1");
    CHECK(controls[2].command == "resume");
    CHECK(controls[3].target == "worker 1");
    CHECK(h.state.snapshot()->status == "completed");
}

TEST_CASE("run controller: stop leaves pending work for a resume, which continues the event numbering") {
    Harness h;
    h.sessions.hold({0x1050, 0x1060});  // the 6th and 7th functions keep both workers busy until the stop
    REQUIRE(h.start(12, 2, h.options(2)));
    REQUIRE(h.sessions.wait_until([&] { return h.sessions.active == 2 && std::ranges::count(h.sessions.dispatch_order, 0x1060u) == 1; }));
    h.controller->stop();
    h.controller->wait();
    CHECK(h.controller->status() == "stopped");
    const Json run = h.run_json();
    CHECK(run["status"] == "stopped");
    const int pending = run["counts"]["pending"].get<int>();
    CHECK(pending > 0);
    const int first_started = h.sessions.started;
    const u64 last_seq = h.bus.last_seq();
    auto info = read_run_info(h.run_dir());
    REQUIRE(info);
    CHECK(info->status == "stopped");

    // A new process resumes it.
    h.controller.reset();
    events::EventBus bus2(h.id);
    auto log2 = events::JsonlEventLog::open(h.run_dir() / "events.jsonl").value();
    bus2.subscribe([&](const events::Event& e) { log2->write(e); });
    std::vector<events::Event> seen;
    bus2.subscribe([&](const events::Event& e) { seen.push_back(e); });
    FakeSessions again;
    auto controller = std::make_unique<RunController>(RunDeps{.run_session = again.fn()}, bus2);
    auto store = RunStore::open(h.run_dir());
    REQUIRE(store);
    REQUIRE(controller->resume(std::move(*store), h.options(2)));
    controller->wait();
    CHECK(controller->status() == "completed");
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.front().seq == last_seq + 1);
    CHECK(std::holds_alternative<events::RunResumed>(seen.front().payload));
    // The stopped sessions start over as their functions' second sessions; finished ones do not run again.
    const auto per_va = dispatch_counts(again);
    CHECK(per_va.contains(0x1050));
    CHECK(per_va.contains(0x1060));
    CHECK(static_cast<int>(per_va.size()) == pending + 2);
    for (const auto& id : again.session_ids)
        if (id.find("-1050") != std::string::npos || id.find("-1060") != std::string::npos) CHECK(id.ends_with("-2"));
    CHECK(first_started + again.started == 14);
    const Json final_run = parse_json(fs::read_text(h.run_dir() / "run.json").value()).value();
    CHECK(final_run["counts"]["done"] == 12);
    // The whole run replays: every function finished once more than it was interrupted.
    const auto replayed = events::RunState::replay(events::read_event_log(h.run_dir() / "events.jsonl").value());
    CHECK(replayed.data().status == "completed");
    CHECK(replayed.data().finished == 14);
}

TEST_CASE("run controller: abort cancels running sessions") {
    Harness h;
    h.sessions.hold_all = true;
    REQUIRE(h.start(6, 3, h.options(3)));
    REQUIRE(h.sessions.wait_until([&] { return h.sessions.started == 3; }));
    h.controller->abort();
    h.controller->wait();
    CHECK(h.controller->status() == "aborted");
    CHECK(h.sessions.started == 3);
    const auto snap = h.state.snapshot();
    int aborted = 0;
    for (const auto& [id, s] : snap->sessions) aborted += s->outcome == "aborted";
    CHECK(aborted == 3);
    CHECK(snap->status == "aborted");
}

TEST_CASE("run controller: skip, requeue, remove") {
    Harness h;
    h.sessions.hold({0x1000, 0x1040});
    REQUIRE(h.start(6, 1, h.options(1)));
    REQUIRE(h.sessions.wait_until([&] { return h.sessions.started == 1; }));
    CHECK(h.controller->skip(0x1020));   // pending: never dispatched
    CHECK(h.controller->remove(0x1030));  // pending: gone
    CHECK_FALSE(h.controller->remove(0x1000));  // running
    CHECK(h.controller->skip(0x1000));   // running: its session ends as skipped
    REQUIRE(h.sessions.wait_until([&] { return std::ranges::count(h.sessions.dispatch_order, 0x1040u) == 1; }));
    CHECK(h.controller->requeue(0x1010));  // done: runs again
    CHECK(h.controller->requeue(0x1020));  // skipped: runs after all
    h.sessions.release(0x1040);
    h.controller->wait();
    const auto per_va = dispatch_counts(h.sessions);
    CHECK(per_va.at(0x1000) == 1);
    CHECK(per_va.at(0x1010) == 2);
    CHECK(per_va.at(0x1020) == 1);
    CHECK_FALSE(per_va.contains(0x1030));
    bool second = false;
    for (usize i = 0; i < h.sessions.session_ids.size(); ++i)
        if (h.sessions.session_ids[i] == h.id + "-1010-2") second = h.sessions.transcripts[i] == "f_1010_1010.2.jsonl";
    CHECK(second);
    const auto snap = h.state.snapshot();
    CHECK(snap->session(h.id + "-1000")->outcome == "skipped");
    const Json run = h.run_json();
    CHECK(run["queue"].size() == 5);
    for (const auto& item : run["queue"])
        if (item["va"] == 0x1000) CHECK(item["state"] == "skipped");
}

TEST_CASE("run controller: pins and moves change the dispatch order") {
    Harness h;
    h.sessions.hold({0x1000});
    REQUIRE(h.start(6, 1, h.options(1)));
    REQUIRE(h.sessions.wait_until([&] { return h.sessions.started == 1; }));
    CHECK(h.controller->pin(0x1050, true));
    CHECK(h.controller->move(0x1030, 1));  // right behind the pinned one
    {
        const auto snap = h.state.snapshot();
        REQUIRE(snap->queue->size() == 5);
        CHECK((*snap->queue)[0].va == 0x1050);
        CHECK((*snap->queue)[0].pinned);
        CHECK((*snap->queue)[1].va == 0x1030);
    }
    h.sessions.release(0x1000);
    h.controller->wait();
    CHECK(h.sessions.dispatch_order == std::vector<u64>{0x1000, 0x1050, 0x1030, 0x1010, 0x1020, 0x1040});
}

TEST_CASE("run controller: concurrency goes up and down while running") {
    Harness h;
    h.sessions.hold_all = true;
    REQUIRE(h.start(40, 1, h.options(1)));
    REQUIRE(h.sessions.wait_until([&] { return h.sessions.started == 1; }));
    h.controller->set_concurrency(4);
    REQUIRE(h.sessions.wait_until([&] { return h.sessions.active == 4; }));
    CHECK(h.controller->concurrency() == 4);
    h.controller->set_concurrency(2);
    h.sessions.release_all();
    {
        std::lock_guard lock(h.sessions.mutex);
        h.sessions.max_active = 0;  // measure from here on
    }
    // Retiring workers finish their sessions; then at most two run at a time.
    REQUIRE(h.sessions.wait_until([&] { return h.sessions.started >= 20; }));
    h.controller->wait();
    CHECK(h.sessions.started == 40);
    std::set<int> late_workers;
    {
        std::lock_guard lock(h.sessions.mutex);
        for (usize i = 30; i < h.sessions.workers.size(); ++i) late_workers.insert(h.sessions.workers[i]);
    }
    CHECK(*late_workers.rbegin() <= 1);
    bool retired = false;
    for (const auto& [w, worker] : h.state.snapshot()->workers)
        for (const auto& span : worker.spans) retired |= span.phase == "retired";
    CHECK(retired);
}

TEST_CASE("run controller: the run budget stops dispatching; a resume with more budget finishes") {
    Harness h;
    h.sessions.cost_per_turn = 0.5;  // $1 per session
    h.sessions.turns = 2;
    RunOptions o = h.options(2);
    o.run_budget_usd = 3.0;
    REQUIRE(h.start(10, 2, o));
    h.controller->wait();
    CHECK(h.controller->status() == "budget_exhausted");
    CHECK(h.sessions.started < 10);
    CHECK(h.sessions.started >= 3);
    CHECK(h.controller->ledger()->spent() >= 3.0);
    CHECK(h.state.snapshot()->budget.run.usd == 3.0);
    const Json run = h.run_json();
    CHECK(run["status"] == "budget_exhausted");
    CHECK(run["run_budget_usd"] == 3.0);

    h.controller.reset();
    events::EventBus bus2(h.id);
    FakeSessions more;
    more.cost_per_turn = 0.5;
    RunController resumed(RunDeps{.run_session = more.fn()}, bus2);
    RunOptions bigger = h.options(2);
    bigger.run_budget_usd = 100;
    REQUIRE(resumed.resume(RunStore::open(h.run_dir()).value(), bigger));
    resumed.wait();
    CHECK(resumed.status() == "completed");
    CHECK(resumed.ledger()->spent() >= 10.0);  // the first part's spend counts too
}

TEST_CASE("run controller: live limits, guidance and approvals go through the controller") {
    Harness h;
    h.sessions.turns = 1000;  // ends only through limits
    h.sessions.turn_time = 2ms;
    h.sessions.hold({0x1000});
    REQUIRE(h.start(2, 1, h.options(1)));
    REQUIRE(h.sessions.wait_until([&] { return h.sessions.started == 1; }));
    const std::string session = h.id + "-1000";
    CHECK(h.controller->running_sessions() == std::vector<std::string>{session});
    auto id = h.controller->inject(session, "Try a for loop.");
    REQUIRE(id);
    CHECK(h.controller->retract(session, *id));
    CHECK_FALSE(h.controller->inject("nobody", "hello"));
    h.controller->set_limits(agent::LoopLimits{.max_turns = 3});
    h.sessions.release(0x1000);
    h.controller->wait();
    const auto snap = h.state.snapshot();
    CHECK(snap->session(session)->outcome == "max_turns");
    CHECK(snap->session(h.id + "-1010")->outcome == "max_turns");  // later sessions get the new limits too
    CHECK(snap->budget.function.turns == 3);
    std::vector<std::string> commands;
    for (const auto& c : h.controls()) commands.push_back(c.command);
    CHECK(commands == std::vector<std::string>{"inject", "retract", "set_limits"});
}

TEST_CASE("run controller: approvals asked by sessions are decided through the controller") {
    Harness a("2026-10-04T12-00-01-approve");
    a.sessions.turns = 1;
    a.sessions.ask_approval = {0x1000, 0x1020};  // both match (even indexes); 0x1010 gives up
    RunOptions o = a.options(2);
    o.policies[std::string(agent::kWriteSourceAction)] = agent::ApprovalPolicy::ask;
    REQUIRE(a.start(3, 2, o));
    std::vector<agent::PendingApproval> pending;
    for (int i = 0; i < 2000 && pending.size() < 2; ++i) {
        pending = a.controller->pending_approvals();
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(pending.size() == 2);
    for (const auto& p : pending) CHECK(a.controller->decide(p.id, p.request.va == 0x1000, p.request.va == 0x1000 ? "" : "not like this"));
    CHECK_FALSE(a.controller->decide(pending[0].id, true));
    a.controller->wait();
    const auto s = a.state.snapshot();
    CHECK(s->session(a.id + "-1000")->outcome == "matched");
    CHECK(s->session(a.id + "-1020")->outcome == "gave_up");
    CHECK(s->approvals_pending == 0);
    CHECK(s->approvals.size() == 2);
    const Json run = a.run_json();
    CHECK(run["policies"]["write_source"] == "ask");
    std::vector<std::string> commands;
    for (const auto& c : a.controls()) commands.push_back(c.command);
    std::ranges::sort(commands);
    CHECK(commands == std::vector<std::string>{"approve", "deny"});
}

TEST_CASE("run controller: a function whose session cannot start fails alone") {
    Harness h;
    h.controller = std::make_unique<RunController>(
        RunDeps{.transport =
                    [](const Symbol& fn) -> Result<std::shared_ptr<agent::HttpTransport>> {
                        if (fn.va == 0x1010) return make_error(ErrorCode::not_found, "no replay script for {}", fn.name);
                        return std::shared_ptr<agent::HttpTransport>{};
                    },
                .run_session = h.sessions.fn()},
        h.bus);
    REQUIRE(h.start(3, 2, h.options(2)));
    h.controller->wait();
    CHECK(h.controller->status() == "completed");
    const auto snap = h.state.snapshot();
    const auto* failed = snap->session(h.id + "-1010");
    REQUIRE(failed);
    CHECK(failed->outcome == "error");
    CHECK(failed->detail.find("no replay script") != std::string::npos);
    CHECK(h.sessions.started == 2);
}

TEST_CASE("run controller: resuming an interrupted run uses the event log") {
    // A run that "crashed": the log records 3 finished sessions and 2 that never finished, and run.json
    // was last written at the start (everything pending, status running).
    Harness h;
    REQUIRE(h.start(8, 2, h.options(2)));
    h.controller->wait();
    h.controller.reset();
    h.close_log();
    auto all = events::read_event_log(h.run_dir() / "events.jsonl").value();
    // Keep the first 5 sessions; of those, only the first 3 finish.
    std::set<std::string> kept_sessions, finished_sessions;
    for (const auto& e : all)
        if (const auto* s = std::get_if<events::SessionStarted>(&e.payload); s && kept_sessions.size() < 5) kept_sessions.insert(s->session);
    for (const auto& e : all)
        if (const auto* f = std::get_if<events::SessionFinished>(&e.payload);
            f && kept_sessions.contains(f->session) && finished_sessions.size() < 3)
            finished_sessions.insert(f->session);
    std::vector<events::Event> kept;
    std::set<std::string> open;
    for (const auto& e : all) {
        if (std::holds_alternative<events::RunFinished>(e.payload) || std::holds_alternative<events::WorkerPhaseChanged>(e.payload)) continue;
        const std::string session = json_string_or(events::to_json(e)["data"], "session", "");
        if (!session.empty() && !kept_sessions.contains(session)) continue;
        if (std::holds_alternative<events::SessionStarted>(e.payload)) open.insert(session);
        if (const auto* f = std::get_if<events::SessionFinished>(&e.payload)) {
            if (!finished_sessions.contains(f->session)) continue;
            open.erase(f->session);
        }
        // A crash cuts the unfinished sessions' turns short as well.
        if (!session.empty() && !finished_sessions.contains(session) && std::holds_alternative<events::TurnFinished>(e.payload)) continue;
        kept.push_back(e);
    }
    REQUIRE(open.size() == 2);
    std::string text;
    for (const auto& e : kept) text += dump_compact(events::to_json(e)) + "\n";
    REQUIRE(fs::write_text(h.run_dir() / "events.jsonl", text));
    Json run = h.run_json();
    run["status"] = "running";
    for (auto& item : run["queue"]) {
        item["state"] = "pending";
        item.erase("outcome");
        item["sessions"] = 0;
    }
    REQUIRE(fs::write_text(h.run_dir() / "run.json", dump_pretty(run)));

    auto info = read_run_info(h.run_dir());
    REQUIRE(info);
    CHECK(info->status == "interrupted");  // says running, but nobody holds the lock
    auto listed = list_runs(h.dir.path() / "runs");
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].status == "interrupted");
    CHECK(find_run(h.dir.path() / "runs", "2026-10-04T12")->filename() == h.id);

    events::EventBus bus2(h.id);
    std::vector<events::Event> seen;
    bus2.subscribe([&](const events::Event& e) { seen.push_back(e); });
    FakeSessions again;
    RunController controller(RunDeps{.run_session = again.fn()}, bus2);
    auto store = RunStore::open(h.run_dir());
    REQUIRE(store);
    CHECK_FALSE(RunStore::open(h.run_dir()));  // one process at a time
    REQUIRE(controller.resume(std::move(*store), h.options(2)));
    controller.wait();
    REQUIRE(std::holds_alternative<events::RunResumed>(seen.front().payload));
    CHECK(std::get<events::RunResumed>(seen.front().payload).interrupted.size() == 2);
    CHECK(seen.front().seq == kept.back().seq + 1);
    // 3 finished ones are left alone; the 2 interrupted and the 3 never started run.
    CHECK(again.started == 5);
    int restarts = 0;
    for (const auto& id : again.session_ids) restarts += id.ends_with("-2");
    CHECK(restarts == 2);
}
