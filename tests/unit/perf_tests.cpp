// Performance checks (docs/ui.md#testing-strategy): a large synthetic run must keep folding events and
// taking snapshots cheap. The timings are printed; a check fails only when a measurement is five times
// over its budget, and only in optimized builds (Debug builds just run the code).

#include "events/bus.hpp"
#include "events/run_state.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <format>
#include <print>

using namespace decomp;
using namespace decomp::events;
using namespace std::chrono_literals;

namespace {

#if defined(NDEBUG)
constexpr bool kTimed = true;
#else
constexpr bool kTimed = false;
#endif

using Clock = std::chrono::steady_clock;

double micros(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

// Reports a measurement against its budget and fails only far above it.
void budget(const char* what, double measured_us, double budget_us) {
    std::println("perf: {:<52} {:>10.1f} us (budget {:.0f} us)", what, measured_us, budget_us);
    if (kTimed) CHECK_MESSAGE(measured_us <= 5 * budget_us, what);
}

} // namespace

TEST_CASE("performance: a run over 100,000 functions with 1,000 sessions folds and snapshots within budget") {
    constexpr int kFunctions = 100'000, kSessions = 1'000, kWorkers = 8;
    EventBus bus("perf");
    RunStateStore store;
    store.attach(bus);

    RunStarted started{"perf", "claude-opus-5-5", "high", kWorkers, {}, {}, Json::object()};
    started.functions.reserve(kFunctions);
    started.vas.reserve(kFunctions);
    for (int i = 0; i < kFunctions; ++i) {
        started.functions.push_back(std::format("?function_{}@@YAHH@Z", i));
        started.vas.push_back(0x401000 + static_cast<u64>(i) * 0x40);
    }
    bus.publish(std::move(started));
    QueueUpdated queue;
    for (int i = 0; i < 500; ++i) queue.items.push_back({0x401000 + static_cast<u64>(i) * 0x40, std::format("function_{}", i), false, 4, 0});
    queue.total = kFunctions;
    bus.publish(std::move(queue));

    // 1,000 sessions of 6 turns each, on 8 workers, with streaming, tools, compiles and diffs; a viewer
    // takes a snapshot every 50 events (a UI frame at a high event rate).
    usize events = 0, snapshots = 0;
    Clock::duration publishing{}, snapshotting{};
    std::shared_ptr<const RunStateData> last;
    auto publish = [&](Payload p, int worker) {
        const auto t0 = Clock::now();
        bus.publish(std::move(p), worker);
        publishing += Clock::now() - t0;
        if (++events % 50 == 0) {
            const auto t1 = Clock::now();
            last = store.snapshot();
            snapshotting += Clock::now() - t1;
            ++snapshots;
        }
    };
    for (int n = 0; n < kSessions; ++n) {
        const int worker = n % kWorkers;
        const u64 va = 0x401000 + static_cast<u64>(n) * 0x40;
        const std::string id = std::format("perf-{:x}", va);
        publish(SessionStarted{id, std::format("?function_{}@@YAHH@Z", n), std::format("function_{}", n), va, ""}, worker);
        for (int turn = 1; turn <= 6; ++turn) {
            publish(TurnStarted{id, turn}, worker);
            publish(WorkerPhaseChanged{"thinking", id, ""}, worker);
            for (int d = 0; d < 4; ++d) publish(StreamDelta{id, "thinking", std::string(64, 't')}, worker);
            publish(WorkerPhaseChanged{"writing", id, ""}, worker);
            for (int d = 0; d < 4; ++d) publish(StreamDelta{id, "text", std::string(64, 'x')}, worker);
            publish(TurnFinished{id, turn, "tool_use", TokenUsage{100, 900, 0, 20'000}, 0.05, 9000, "claude-opus-5-5", 800, false}, worker);
            publish(ToolCallStarted{id, std::format("t{}", turn), "compile_and_diff", turn, "{\"source\": \"...\"}"}, worker);
            publish(CompileStarted{id, "clang-cl-x86", "clang-cl /c candidate.cpp"}, worker);
            publish(CompileFinished{id, true, false, 120, 0, 0, "clang-cl /c candidate.cpp", ""}, worker);
            publish(DiffComputed{id, 50.0 + turn * 5, false, "match", turn}, worker);
            publish(ToolCallFinished{id, std::format("t{}", turn), "compile_and_diff", false, "match 80%", 130}, worker);
        }
        publish(SessionFinished{id, n % 3 ? "matched" : "gave_up", "", n % 3 ? 100.0 : 80.0, 6, 0.3}, worker);
    }
    REQUIRE(last);
    CHECK(store.snapshot()->finished == kSessions);
    CHECK(store.snapshot()->sessions.size() == kSessions);

    budget("publish one event (log-free bus, reducer)", micros(publishing) / static_cast<double>(events), 20);
    budget("snapshot after 50 events (1,000 sessions)", micros(snapshotting) / static_cast<double>(snapshots), 500);

    // A snapshot when nothing changed is the cached one.
    const auto t0 = Clock::now();
    for (int i = 0; i < 1000; ++i) last = store.snapshot();
    budget("snapshot without changes", micros(Clock::now() - t0) / 1000.0, 1);

    // Replaying the whole log (opening a past run).
    std::vector<Event> log;
    {
        EventBus replay_bus("perf");
        replay_bus.subscribe([&](const Event& e) { log.push_back(e); });
        RunStarted again{"perf", "claude-opus-5-5", "high", kWorkers, {}, {}, Json::object()};
        for (int i = 0; i < kFunctions; ++i) again.vas.push_back(0x401000 + static_cast<u64>(i) * 0x40);
        replay_bus.publish(std::move(again));
        for (int n = 0; n < kSessions; ++n) {
            const std::string id = std::format("perf-{}", n);
            replay_bus.publish(SessionStarted{id, "f", "f", 0x401000 + static_cast<u64>(n) * 0x40, ""}, n % kWorkers);
            for (int turn = 1; turn <= 6; ++turn) {
                replay_bus.publish(TurnStarted{id, turn}, n % kWorkers);
                replay_bus.publish(TurnFinished{id, turn, "tool_use", TokenUsage{100, 900, 0, 20'000}, 0.05, 9000}, n % kWorkers);
                replay_bus.publish(DiffComputed{id, 60, false, "match", turn}, n % kWorkers);
            }
            replay_bus.publish(SessionFinished{id, "matched", "", 100, 6, 0.3}, n % kWorkers);
        }
    }
    const auto t1 = Clock::now();
    const auto replayed = RunState::replay(log);
    budget("replay 20,000 events into a run state", micros(Clock::now() - t1), 100'000);
    CHECK(replayed.data().finished == kSessions);
}
