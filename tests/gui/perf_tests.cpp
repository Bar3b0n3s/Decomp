// Frame time with a large run (docs/ui.md#testing-strategy): every view rendering a snapshot of a run
// over 100,000 functions with 1,000 sessions, eight of them streaming. The times are printed; a check
// fails only at five times the budget, and only in optimized builds.

#include "events/run_state.hpp"
#include "harness.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <format>
#include <print>

using namespace decomp;
using namespace decomp::gui;
using namespace decomp::gui::test;

namespace {

#if defined(NDEBUG)
constexpr bool kTimed = true;
#else
constexpr bool kTimed = false;
#endif

std::shared_ptr<const events::RunStateData> huge_snapshot() {
    constexpr int kFunctions = 100'000, kSessions = 1'000, kWorkers = 8;
    events::RunState state;
    u64 seq = 0;
    const auto t0 = std::chrono::system_clock::time_point(std::chrono::seconds(1'791'100'000));
    auto add = [&](events::Payload p, int worker, int second) {
        events::Event e;
        e.seq = ++seq;
        e.time = t0 + std::chrono::seconds(second);
        e.run = "2026-10-04T10-00-00-huge";
        e.worker = worker;
        e.payload = std::move(p);
        state.apply(e);
    };
    events::RunStarted started{"huge", "claude-opus-5-5", "high", kWorkers, {}, {}, Json::object()};
    for (int i = 0; i < kFunctions; ++i) {
        started.functions.push_back(std::format("function_{}", i));
        started.vas.push_back(0x401000 + static_cast<u64>(i) * 0x40);
    }
    add(std::move(started), -1, 0);
    events::QueueUpdated queue;
    for (int i = kSessions; i < kSessions + 500; ++i) queue.items.push_back({0x401000 + static_cast<u64>(i) * 0x40, std::format("function_{}", i), false, 5, 0});
    queue.total = kFunctions - kSessions;
    add(std::move(queue), -1, 0);
    for (int n = 0; n < kSessions; ++n) {
        const int worker = n % kWorkers;
        const int second = n * 7;  // about two hours of activity
        const u64 va = 0x401000 + static_cast<u64>(n) * 0x40;
        const std::string id = std::format("huge-{:x}", va);
        add(events::SessionStarted{id, std::format("?function_{}@@YAHH@Z", n), std::format("int __cdecl function_{}(int)", n), va, ""}, worker, second);
        for (int turn = 1; turn <= 3; ++turn) {
            add(events::TurnStarted{id, turn}, worker, second + turn);
            add(events::WorkerPhaseChanged{"thinking", id, ""}, worker, second + turn);
            add(events::TurnFinished{id, turn, "tool_use", events::TokenUsage{100, 900, 0, 20'000}, 0.05, 9000, "claude-opus-5-5", 800, false}, worker,
                second + turn);
            add(events::ToolCallStarted{id, std::format("t{}", turn), "compile_and_diff", turn, Json("int f(int x) { ... }")}, worker, second + turn);
            add(events::CompileFinished{id, true, turn == 1, 120, 0, 0, "clang-cl /c candidate.cpp", ""}, worker, second + turn);
            add(events::DiffComputed{id, 60.0 + turn * 10, false, "rows differ", turn}, worker, second + turn);
            add(events::ToolCallFinished{id, std::format("t{}", turn), "compile_and_diff", false, "match 80%", 130}, worker, second + turn);
        }
        if (n < kSessions - kWorkers) {
            add(events::SessionFinished{id, n % 3 ? "matched" : "gave_up", "", n % 3 ? 100.0 : 80.0, 3, 0.15}, worker, second + 5);
            if (n % 3) add(events::StatusChanged{std::format("function_{}", n), va, "matched", "unstarted", 100}, worker, second + 5);
        } else {
            // Still streaming: a long thought and some text.
            add(events::StreamDelta{id, "thinking", std::string(8000, 't')}, worker, second + 6);
            add(events::StreamDelta{id, "text", std::string(2000, 'x')}, worker, second + 6);
        }
        if (n % 50 == 0) add(events::LogLine{"warn", std::format("slow compile for function_{}", n), id}, worker, second + 4);
    }
    return std::make_shared<const events::RunStateData>(state.data());
}

} // namespace

TEST_CASE("performance: every view draws a run over 100,000 functions with 1,000 sessions within budget") {
    const auto snapshot = huge_snapshot();
    REQUIRE(snapshot->sessions.size() == 1000);
    REQUIRE(snapshot->queue_total == 99'000);
    HeadlessContext gui;
    Settings settings;
    App app(make_services(snapshot), settings);
    constexpr double kBudgetMs = 16.0;  // a 60 Hz frame for the whole UI
    for (const auto& view : all_view_ids()) {
        CAPTURE(view);
        REQUIRE(app.focus_view(view));
        gui.frames(3, [&] { app.frame(); });  // settle: layout, first-use caches
        const auto start = std::chrono::steady_clock::now();
        constexpr int kFrames = 10;
        gui.frames(kFrames, [&] { app.frame(); });
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kFrames;
        std::println("perf: frame with {:<20} {:>8.2f} ms (budget {:.0f} ms)", view, ms, kBudgetMs);
        if (kTimed) CHECK_MESSAGE(ms <= 5 * kBudgetMs, view);
        CHECK(app.view_visible(view));
    }
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}
