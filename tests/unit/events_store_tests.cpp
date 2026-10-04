#include "events/bus.hpp"
#include "events/run_state.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace decomp;
using namespace decomp::events;
using namespace std::chrono_literals;

namespace {

Event ev(u64 seq, Payload p, int worker = -1, int seconds = 0) {
    Event e;
    e.seq = seq;
    // 1'790'000'040 is a whole number of minutes since the epoch.
    e.time = std::chrono::system_clock::time_point(std::chrono::seconds(1'790'000'040 + seconds));
    e.run = "r1";
    e.worker = worker;
    e.payload = std::move(p);
    return e;
}

} // namespace

TEST_CASE("bus: concurrent publishers are delivered one at a time in seq order") {
    EventBus bus("r");
    std::vector<u64> seen;
    std::atomic<int> inside{0};
    bool overlapped = false;
    bus.subscribe([&](const Event& e) {
        if (inside.fetch_add(1) != 0) overlapped = true;
        seen.push_back(e.seq);
        inside.fetch_sub(1);
    });
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) bus.publish(LogLine{"info", "x"});
        });
    for (auto& t : threads) t.join();
    CHECK_FALSE(overlapped);
    REQUIRE(seen.size() == 1600);
    for (usize i = 0; i < seen.size(); ++i) CHECK(seen[i] == i + 1);
    CHECK(bus.last_seq() == 1600);
}

TEST_CASE("bus: publishes from a handler follow the current event; unsubscribe waits") {
    EventBus bus("r");
    std::vector<std::string> order;
    bus.subscribe([&](const Event& e) {
        if (const auto* l = std::get_if<LogLine>(&e.payload)) {
            order.push_back("a:" + l->message);
            if (l->message == "first") bus.publish(LogLine{"info", "nested"});
        }
    });
    bus.subscribe([&](const Event& e) {
        if (const auto* l = std::get_if<LogLine>(&e.payload)) order.push_back("b:" + l->message);
    });
    auto first = bus.publish(LogLine{"info", "first"});
    CHECK(first.seq == 1);
    CHECK(order == std::vector<std::string>{"a:first", "b:first", "a:nested", "b:nested"});

    std::atomic<bool> in_handler{false}, finished{false};
    int slow = bus.subscribe([&](const Event& e) {
        if (const auto* l = std::get_if<LogLine>(&e.payload); l && l->message == "slow") {
            in_handler = true;
            std::this_thread::sleep_for(150ms);
            finished = true;
        }
    });
    std::thread publisher([&] { bus.publish(LogLine{"info", "slow"}); });
    while (!in_handler) std::this_thread::yield();
    bus.unsubscribe(slow);
    CHECK(finished);  // unsubscribe returned only after the delivery ended
    publisher.join();

    bus.set_next_seq(100);
    CHECK(bus.publish(LogLine{"info", "after"}).seq == 100);
}

TEST_CASE("every event type survives a JSON round trip") {
    std::vector<Payload> payloads = {
        RunStarted{"proj", "claude-opus-5-5", "high", 4, {"add", "dispatch"}, {0x401060, 0x4010f0}, Json{{"workers", 4}}},
        RunFinished{"completed"},
        RunResumed{{0x401060}},
        SessionStarted{"s1", "?add@@YAHHH@Z", "int add(int, int)", 0x401060, "sessions/add_401060.jsonl"},
        SessionFinished{"s1", "matched", "ok", 100, 3, 0.25},
        TurnStarted{"s1", 2},
        TurnFinished{"s1", 2, "tool_use", TokenUsage{10, 20, 30, 40}, 0.01, 900, "claude-opus-5-5", 120, true},
        StreamDelta{"s1", "thinking", "hmm"},
        ToolCallStarted{"s1", "t1", "compile_and_diff", 2, Json{{"source", "int x;"}}},
        ToolCallFinished{"s1", "t1", "compile_and_diff", false, "compile: ok", 35},
        CompileStarted{"s1", "clang-cl-x86", "/O2 /Gy"},
        CompileFinished{"s1", true, false, 30, 0, 0, "clang-cl /c x.cpp", "warning: x", "clang-cl-x86"},
        DiffComputed{"s1", 87.5, false, "match 87.5%", 3, 7, 0, 1, 0, 0, 0},
        Retry{"s1", 1, "rate limited", 2000, 429, 2000},
        Refusal{"s1", "cyber", "no"},
        Guidance{"s1", "try unsigned", 7},
        StatusChanged{"add", 0x401060, "matched", "in_progress", 100},
        FileWritten{"src/functions/add_401060.cpp", "matched source", 96, std::string(40, 'a'), "s1", "policy"},
        LogLine{"warn", "careful", "s1"},
        WorkerPhaseChanged{"waiting for approval", "s1", "add"},
        RateLimitUpdated{50, 49, 100000, 90000, 20000, 19000, "2026-10-04T12:00:00Z", 1500},
        BudgetChanged{"run", 25.0, 0, 0, 0},
        SymbolChanged{0x401060, "?add@@YAHHH@Z", "my_add", "function", 15, "user", ""},
        ApprovalRequested{3, "write_source", "s1", "add", 0x401060, "src/functions/add_401060.cpp", "byte-exact"},
        ApprovalDecided{3, "approved", "user", "looks good"},
        QueueUpdated{{{0x401060, "add", true, 1.5}, {0x4010f0, "dispatch", false, 4.0}}},
        Control{"pause", "worker 2", ""},
    };
    CHECK(payloads.size() == std::variant_size_v<Payload>);
    u64 seq = 1;
    for (auto& p : payloads) {
        Event e = ev(seq++, p, 1);
        const Json j = to_json(e);
        CAPTURE(j.dump());
        auto back = event_from_json(j);
        REQUIRE(back);
        CHECK(to_json(*back) == j);
        CHECK(type_name(back->payload) == type_name(p));
    }
}

TEST_CASE("a log written by the first slice (no new fields) still replays") {
    const char* lines[] = {
        R"json({"data":{"effort":"high","functions":["int __cdecl add(int, int)"],"model":"claude-opus-5-5","project":"p","workers":1},"run":"r","seq":1,"time":1791079713000,"type":"run_started"})json",
        R"json({"data":{"display":"int __cdecl add(int, int)","function":"?add@@YAHHH@Z","session":"r-401060","va":4198496},"run":"r","seq":2,"time":1791079713001,"type":"session_started","worker":0})json",
        R"json({"data":{"session":"r-401060","turn":1},"run":"r","seq":3,"time":1791079713002,"type":"turn_started","worker":0})json",
        R"json({"data":{"cost_usd":0.02,"latency_ms":900,"session":"r-401060","stop_reason":"tool_use","turn":1,"usage":{"cache_read":0,"cache_write":6000,"input":12,"output":400}},"run":"r","seq":4,"time":1791079714000,"type":"turn_finished","worker":0})json",
        R"json({"data":{"cached":false,"duration_ms":30,"errors":0,"ok":true,"session":"r-401060"},"run":"r","seq":5,"time":1791079714100,"type":"compile_finished","worker":0})json",
        R"json({"data":{"byte_exact":true,"match_percent":100.0,"session":"r-401060","summary":"match 100%"},"run":"r","seq":6,"time":1791079714200,"type":"diff_computed","worker":0})json",
        R"json({"data":{"function":"int __cdecl add(int, int)","status":"matched","va":4198496},"run":"r","seq":7,"time":1791079714300,"type":"status_changed","worker":0})json",
        R"json({"data":{"best_match":100.0,"cost_usd":0.02,"detail":"","outcome":"matched","session":"r-401060","turns":1},"run":"r","seq":8,"time":1791079714400,"type":"session_finished","worker":0})json",
        R"json({"data":{"status":"completed"},"run":"r","seq":9,"time":1791079714500,"type":"run_finished"})json",
    };
    RunState state;
    for (const char* line : lines) {
        auto j = parse_json(line).value();
        auto e = event_from_json(j);
        REQUIRE(e);
        state.apply(*e);
    }
    const auto& d = state.data();
    CHECK(d.status == "completed");
    CHECK(d.matched == 1);
    REQUIRE(d.session("r-401060"));
    CHECK(d.session("r-401060")->compiles == 1);
    CHECK(d.session("r-401060")->outcome == "matched");
    CHECK(d.recent_compiles.size() == 1);
    CHECK(d.recent_compiles.front()->exit_code == -1);  // absent in old logs
    CHECK(d.usage.cache_write == 6000);
}

TEST_CASE("reducer: workers, queue, approvals, rate limits, budgets, logs, compiles, resume") {
    RunState st;
    u64 seq = 1;
    auto apply = [&](Payload p, int worker = -1, int sec = 0) { st.apply(ev(seq++, std::move(p), worker, sec)); };
    apply(RunStarted{"p", "m", "high", 2, {"add", "dispatch"}, {0x401060, 0x4010f0}, Json{{"run_budget_usd", 10}}});
    apply(QueueUpdated{{{0x401060, "add", false, 1}, {0x4010f0, "dispatch", true, 3}}});
    CHECK(st.data().queue->size() == 2);
    CHECK(st.data().planned_vas->size() == 2);
    apply(WorkerPhaseChanged{"waiting for slot", "", ""}, 1, 1);
    apply(SessionStarted{"s1", "add", "add", 0x401060, "sessions/add.jsonl"}, 0, 2);
    apply(TurnStarted{"s1", 1}, 0, 3);
    apply(WorkerPhaseChanged{"thinking", "s1", ""}, 0, 4);
    apply(StreamDelta{"s1", "thinking", std::string(20000, 't')}, 0, 4);
    apply(WorkerPhaseChanged{"writing", "s1", ""}, 0, 5);
    apply(StreamDelta{"s1", "text", "hello"}, 0, 5);
    {
        const auto* s = st.data().session("s1");
        CHECK(s->live_thinking.size() == RunState::kLiveText);
        CHECK(s->live_text == "hello");
        CHECK(s->transcript == "sessions/add.jsonl");
        CHECK(s->phase == "writing");
        CHECK(st.data().workers.at(0).phase == "writing");
    }
    apply(TurnFinished{"s1", 1, "tool_use", TokenUsage{10, 100, 0, 0}, 0.5, 1000, "claude-opus-5-5", 250, true}, 0, 30);
    apply(CompileStarted{"s1", "clang-cl-x86", "/O2"}, 0, 62);
    apply(CompileFinished{"s1", false, false, 40, 2, 1, "clang-cl /c", "error: x"}, 0, 62);
    apply(TurnStarted{"s1", 2}, 0, 63);
    CHECK(st.data().session("s1")->live_thinking.empty());
    apply(ApprovalRequested{1, "write_source", "s1", "add", 0x401060, "src/functions/add.cpp", "byte-exact"}, 0, 64);
    apply(WorkerPhaseChanged{"waiting for approval", "s1", "add"}, 0, 64);
    CHECK(st.data().approvals_pending == 1);
    apply(ApprovalDecided{1, "approved", "user", ""}, -1, 65);
    CHECK(st.data().approvals_pending == 0);
    CHECK(st.data().approvals.at(1).verdict == "approved");
    apply(RateLimitUpdated{50, 0, -1, -1, -1, -1, "soon", 3000}, -1, 66);
    CHECK(st.data().rate_limit.known);
    CHECK(st.data().rate_limit.last.backoff_ms == 3000);
    apply(BudgetChanged{"run", 25, 0, 0, 0});
    CHECK(st.data().budget.run.usd == 25);
    apply(LogLine{"warn", "careful", "s1"}, 0, 67);
    CHECK(st.data().log_tail.size() == 1);
    CHECK(st.data().errors.back() == "careful");
    apply(Control{"pause", "worker 1", ""}, -1, 68);
    CHECK(st.data().controls.size() == 1);
    apply(SymbolChanged{0x401060, "add", "my_add", "function", 15, "user", ""});
    CHECK(st.data().symbol_changes.size() == 1);

    const auto& w0 = st.data().workers.at(0);
    CHECK(w0.phase == "waiting for approval");
    CHECK(w0.function == "add");
    REQUIRE(w0.spans.size() >= 5);  // starting, waiting for model, thinking, writing, running tools, compiling, ...
    for (usize i = 0; i + 1 < w0.spans.size(); ++i) CHECK(w0.spans[i].end == w0.spans[i + 1].start);
    CHECK(st.data().workers.at(1).phase == "waiting for slot");
    CHECK(st.data().recent_compiles.back()->exit_code == 1);
    CHECK(st.data().fallback_turns == 1);
    CHECK(st.data().session("s1")->model == "claude-opus-5-5");
    // Minute buckets: one turn in the first minute, one compile in the second.
    REQUIRE(st.data().minutes.size() == 2);
    const auto& first_minute = st.data().minutes.begin()->second;
    const auto& second_minute = std::next(st.data().minutes.begin())->second;
    CHECK(first_minute.turns == 1);
    CHECK(first_minute.ttft_count == 1);
    CHECK(first_minute.ttft_sum_ms == 250);
    CHECK(first_minute.output_tokens == 100);
    CHECK(first_minute.compiles == 0);
    CHECK(second_minute.turns == 0);
    CHECK(second_minute.compiles == 1);

    // A resume closes sessions cut off by the interruption.
    apply(RunResumed{{0x401060}}, -1, 70);
    CHECK(st.data().session("s1")->finished);
    CHECK(st.data().session("s1")->outcome == "interrupted");
    CHECK(st.data().interrupted.size() == 1);
    // A duplicate session_finished does not count twice.
    apply(SessionFinished{"s1", "stopped", "", 50, 2, 0.5}, 0, 71);
    CHECK(st.data().finished == 0);
}

TEST_CASE("reducer: a large queue arrives as its head and a total") {
    RunState st;
    u64 seq = 1;
    auto apply = [&](Payload p, int worker = -1) { st.apply(ev(seq++, std::move(p), worker)); };
    // An older log: no total, so the items are the whole queue.
    apply(QueueUpdated{{{0x10, "a", false, 1}, {0x20, "b", false, 1}}});
    CHECK(st.data().queue_total == 2);
    // A head of three out of a thousand pending functions.
    apply(QueueUpdated{{{0x10, "a", false, 1}, {0x20, "b", false, 1}, {0x30, "c", false, 1}}, 1000});
    CHECK(st.data().queue->size() == 3);
    CHECK(st.data().queue_total == 1000);
    apply(SessionStarted{"s1", "a", "a", 0x10, ""}, 0);
    CHECK(st.data().queue->size() == 2);
    CHECK(st.data().queue_total == 999);
    // A head taken just before a worker started one of its functions: the running one is not pending.
    apply(SessionStarted{"s2", "b", "b", 0x20, ""}, 1);
    CHECK(st.data().queue_total == 998);
    apply(QueueUpdated{{{0x20, "b", false, 1}, {0x30, "c", false, 1}, {0x40, "d", false, 1}}, 999});
    REQUIRE(st.data().queue->size() == 2);
    CHECK(st.data().queue->front().va == 0x30);
    CHECK(st.data().queue_total == 998);
    // Taken before "c" started and published after its session already finished: still stale. A
    // requeued "a" (its one session known) is pending again.
    apply(SessionStarted{"s3", "c", "c", 0x30, ""}, 2);
    apply(SessionFinished{"s3", "matched", "", 100, 1, 0.1}, 2);
    apply(QueueUpdated{{{0x10, "a", false, 1, 1}, {0x30, "c", false, 1, 0}, {0x40, "d", false, 1, 0}}, 3});
    REQUIRE(st.data().queue->size() == 2);
    CHECK(st.data().queue->front().va == 0x10);
    CHECK(st.data().queue->back().va == 0x40);
    CHECK(st.data().queue_total == 2);
    apply(QueueUpdated{{}, 0});
    CHECK(st.data().queue_total == 0);
    // The total survives the log.
    const Event e = ev(9, QueueUpdated{{{0x30, "c", true, 2}}, 77});
    auto back = event_from_json(to_json(e));
    REQUIRE(back);
    CHECK(std::get<QueueUpdated>(back->payload).total == 77);
}

TEST_CASE("snapshots are immutable and share unchanged sessions") {
    EventBus bus("r");
    RunStateStore store;
    store.attach(bus);
    bus.publish(SessionStarted{"a", "fa", "fa", 1}, 0);
    bus.publish(SessionStarted{"b", "fb", "fb", 2}, 1);
    bus.publish(TurnStarted{"a", 1}, 0);
    auto snap1 = store.snapshot();
    CHECK(store.snapshot() == snap1);  // cached until something changes
    bus.publish(TurnStarted{"a", 2}, 0);
    bus.publish(StreamDelta{"a", "text", "new text"}, 0);
    auto snap2 = store.snapshot();
    REQUIRE(snap2 != snap1);
    CHECK(snap1->session("a")->turn == 1);  // the old snapshot did not change
    CHECK(snap1->session("a")->live_text.empty());
    CHECK(snap2->session("a")->turn == 2);
    CHECK(snap2->session("a")->live_text == "new text");
    CHECK(snap1->sessions.at("b") == snap2->sessions.at("b"));  // unchanged session shared
    CHECK(snap1->sessions.at("a") != snap2->sessions.at("a"));
    // Writers keep working while readers hold snapshots.
    std::thread writer([&] {
        for (int i = 0; i < 2000; ++i) bus.publish(StreamDelta{"a", "text", "x"}, 0);
    });
    for (int i = 0; i < 200; ++i) {
        auto s = store.snapshot();
        CHECK(s->session("a") != nullptr);
    }
    writer.join();
    CHECK(store.snapshot()->session("a")->live_text.size() == std::string("new text").size() + 2000);
    store.detach();
}

TEST_CASE("reducer: errors are kept by kind; rate limits keep a history") {
    RunState st;
    u64 seq = 1;
    auto apply = [&](Payload p, int worker = 0) { st.apply(ev(seq++, std::move(p), worker)); };
    apply(SessionStarted{"s1", "add", "add", 0x401060});
    apply(Retry{"s1", 1, "HTTP 529 overloaded_error: busy", 2000, 529, 0});
    apply(ToolCallFinished{"s1", "t1", "read_memory", true, "0x9 is outside the image", 1});
    apply(CompileFinished{"s1", false, false, 30, 3, 2, "clang-cl", "x.cpp(1): error: ..."});   // the candidate's errors: normal
    apply(CompileFinished{"s1", false, false, 30, 0, -1, "clang-cl", "cannot start", "clang-cl-x86"});  // the compiler failed
    apply(LogLine{"warn", "slow disk", "s1"});
    apply(SessionFinished{"s1", "error", "HTTP 401 authentication_error: invalid x-api-key", 0, 1, 0});
    apply(SessionFinished{"s1", "error", "again", 0, 1, 0});  // a duplicate is not counted twice
    apply(RateLimitUpdated{50, 10, -1, -1, -1, -1, "", 0}, -1);
    apply(RateLimitUpdated{50, 9, -1, -1, -1, -1, "", 0}, -1);
    std::map<std::string, int> kinds;
    for (const auto& r : st.data().error_log) ++kinds[r.kind];
    CHECK(kinds == std::map<std::string, int>{{"api", 1}, {"compiler", 1}, {"log", 1}, {"session", 1}, {"tool", 1}});
    const auto& api = *std::ranges::find(st.data().error_log, std::string("api"), &ErrorRecord::kind);
    CHECK(api.status == 529);
    CHECK(api.delay_ms == 2000);
    CHECK(api.session == "s1");
    CHECK(api.worker == 0);
    REQUIRE(st.data().rate_history.size() == 2);
    CHECK(st.data().rate_history.back().snapshot.requests_remaining == 9);
}
