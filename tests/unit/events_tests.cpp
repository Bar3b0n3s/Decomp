#include "events/progress.hpp"
#include "core/fs.hpp"
#include "events/bus.hpp"
#include "events/run_state.hpp"

#include <doctest/doctest.h>

#include <cstdio>
#include <thread>

using namespace decomp;
using namespace decomp::events;

namespace {

void scripted_run(EventBus& bus) {
    bus.publish(RunStarted{"proj", "claude-opus-5-5", "high", 1, {"add", "sum_array"}});
    bus.publish(SessionStarted{"s1", "?add@@YAHHH@Z", "add", 0x401060}, 0);
    bus.publish(TurnStarted{"s1", 1}, 0);
    bus.publish(StreamDelta{"s1", "text", "Let me compile a first attempt."}, 0);
    bus.publish(TurnFinished{"s1", 1, "tool_use", {1000, 200, 3000, 0}, 0.05, 1500}, 0);
    bus.publish(ToolCallStarted{"s1", "toolu_1", "compile_and_diff", 1, Json{{"source", "int add(int a, int b);"}}}, 0);
    bus.publish(CompileFinished{"s1", true, false, 120, 0}, 0);
    bus.publish(DiffComputed{"s1", 68.8, false, "match 68.8%"}, 0);
    bus.publish(ToolCallFinished{"s1", "toolu_1", "compile_and_diff", false, "match 68.8%", 140}, 0);
    bus.publish(TurnStarted{"s1", 2}, 0);
    bus.publish(Retry{"s1", 1, "overloaded_error", 2000}, 0);
    bus.publish(TurnFinished{"s1", 2, "tool_use", {500, 100, 0, 3000}, 0.02, 900}, 0);
    bus.publish(ToolCallStarted{"s1", "toolu_2", "submit_result", 2, Json{{"outcome", "matched"}}}, 0);
    bus.publish(DiffComputed{"s1", 100.0, true, "MATCHING"}, 0);
    bus.publish(ToolCallFinished{"s1", "toolu_2", "submit_result", false, "verified byte-exact", 130}, 0);
    bus.publish(FileWritten{"src/functions/add_401060.cpp", "matched source"}, 0);
    bus.publish(SessionFinished{"s1", "matched", "", 100.0, 2, 0.07}, 0);
    bus.publish(SessionStarted{"s2", "?sum_array@@YAHPBHH@Z", "sum_array", 0x401080}, 0);
    bus.publish(Refusal{"s2", "cyber", ""}, 0);
    bus.publish(SessionFinished{"s2", "refused", "declined", 0, 1, 0.01}, 0);
    bus.publish(RunFinished{"completed"});
}

} // namespace

TEST_CASE("event JSON round trip for every payload type") {
    EventBus bus("run-1");
    std::vector<Event> seen;
    bus.subscribe([&](const Event& e) { seen.push_back(e); });
    scripted_run(bus);
    REQUIRE(seen.size() == 21);
    for (const auto& e : seen) {
        CAPTURE(type_name(e.payload));
        auto back = event_from_json(to_json(e)).value();
        CHECK(to_json(back) == to_json(e));
    }
    CHECK(seen[0].seq == 1);
    CHECK(seen.back().seq == 21);
    CHECK(seen[1].worker == 0);
    CHECK_FALSE(event_from_json(Json{{"type", "bogus"}}));
}

TEST_CASE("RunState folds a run into sessions, workers and totals") {
    EventBus bus("run-2");
    RunState state;
    bus.subscribe([&](const Event& e) { state.apply(e); });
    scripted_run(bus);
    const auto& d = state.data();
    CHECK(d.status == "completed");
    CHECK(d.model == "claude-opus-5-5");
    CHECK(d.matched == 1);
    CHECK(d.finished == 2);
    CHECK(d.refusals == 1);
    CHECK(d.retries == 1);
    CHECK(d.tool_calls == 2);
    CHECK(d.usage.input == 1500);
    CHECK(d.usage.cache_read == 3000);
    CHECK(d.cost_usd == doctest::Approx(0.07));
    CHECK(d.cache_hit_rate() == doctest::Approx(3000.0 / 7500.0));
    const auto& s1 = *d.sessions.at("s1");
    CHECK(s1.outcome == "matched");
    CHECK(s1.matched);
    CHECK(s1.best_match == 100.0);
    CHECK(s1.scores == std::vector<double>{68.8, 100.0});
    CHECK(s1.compiles == 1);
    CHECK(d.sessions.at("s2")->refusal_category == "cyber");
    CHECK(d.workers.at(0).session.empty());
    CHECK(d.files_written.size() == 1);
    CHECK(d.activity_total >= 6);
}

TEST_CASE("event log replays into the same state") {
    auto dir = fs::TempDir::create("decomp-events").value();
    auto path = dir.path() / "runs" / "r" / "events.jsonl";
    EventBus bus("run-3");
    auto log = JsonlEventLog::open(path).value();
    RunState live;
    bus.subscribe([&](const Event& e) {
        live.apply(e);
        log->write(e);
    });
    scripted_run(bus);
    auto events = read_event_log(path).value();
    CHECK(events.size() == 20);  // stream deltas are not logged
    auto replayed = RunState::replay(events);
    const auto& a = live.data();
    const auto& b = replayed.data();
    CHECK(b.matched == a.matched);
    CHECK(b.finished == a.finished);
    CHECK(b.cost_usd == doctest::Approx(a.cost_usd));
    CHECK(b.usage.total() == a.usage.total());
    CHECK(b.sessions.at("s1")->scores == a.sessions.at("s1")->scores);
    CHECK(b.sessions.at("s1")->outcome == a.sessions.at("s1")->outcome);
}

TEST_CASE("event bus is safe across threads and keeps sequence numbers unique") {
    EventBus bus("run-4");
    std::mutex m;
    std::vector<u64> seqs;
    bus.subscribe([&](const Event& e) {
        std::lock_guard lock(m);
        seqs.push_back(e.seq);
    });
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < 250; ++i) bus.publish(LogLine{"info", std::to_string(i)}, t);
        });
    for (auto& t : threads) t.join();
    std::ranges::sort(seqs);
    CHECK(seqs.size() == 1000);
    CHECK(std::ranges::adjacent_find(seqs) == seqs.end());
}

TEST_CASE("progress block shows the run header, workers and recent activity") {
    EventBus bus("run-5");
    RunState state;
    bus.subscribe([&](const Event& e) { state.apply(e); });
    bus.publish(RunStarted{"proj", "claude-opus-5-5", "high", 1, {"add"}});
    bus.publish(SessionStarted{"s1", "?add@@YAHHH@Z", "add", 0x401060}, 0);
    bus.publish(TurnStarted{"s1", 3}, 0);
    auto block = events::ProgressRenderer::render_block(state.data(), std::chrono::system_clock::now());
    CHECK(block.find("model claude-opus-5-5 (high)") != std::string::npos);
    CHECK(block.find("worker 0  add") != std::string::npos);
    CHECK(block.find("waiting for model") != std::string::npos);
    CHECK(block.find("session started") != std::string::npos);

    // Plain mode prints activity lines as they happen.
    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp);
    events::ProgressRenderer plain(false, tmp);
    EventBus bus2("run-6");
    plain.attach(bus2);
    scripted_run(bus2);
    std::rewind(tmp);
    std::string out;
    char buf[4096];
    while (auto n = std::fread(buf, 1, sizeof(buf), tmp)) out.append(buf, n);
    std::fclose(tmp);
    CHECK(out.find("run started") != std::string::npos);
    CHECK(out.find("add: matched") != std::string::npos);
    CHECK(out.find("declined (category: cyber)") != std::string::npos);
}
