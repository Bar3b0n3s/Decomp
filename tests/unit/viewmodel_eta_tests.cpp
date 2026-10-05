#include "events/bus.hpp"
#include "viewmodel/eta.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

using namespace decomp;
using namespace decomp::vm;

TEST_CASE("eta: durations by size bucket, nearby buckets and the default") {
    DurationModel m;
    CHECK(m.samples() == 0);
    const auto none = m.estimate(64);
    CHECK(none.source == DurationModel::Source::none);
    CHECK(none.seconds == doctest::Approx(DurationModel::default_seconds(64)));
    CHECK(DurationModel::default_seconds(16) == doctest::Approx(120));
    CHECK(DurationModel::default_seconds(1024) == doctest::Approx(60 * (1 + std::log2(65.0))));

    m.add(100, 300);  // bucket 6
    m.add(120, 100);
    m.add(64, 200);
    m.add(70, -5);    // ignored
    CHECK(m.samples() == 3);
    CHECK(m.samples_in(6) == 3);
    CHECK(m.median(6) == doctest::Approx(200));
    const auto own = m.estimate(80);
    CHECK(own.source == DurationModel::Source::bucket);
    CHECK(own.seconds == doctest::Approx(200));
    CHECK(own.samples == 3);
    const auto bigger = m.estimate(256);  // bucket 8: two buckets up
    CHECK(bigger.source == DurationModel::Source::nearby);
    CHECK(bigger.bucket == 6);
    CHECK(bigger.seconds == doctest::Approx(400));  // sqrt(4) times
    m.add(16, 50);  // bucket 4
    m.add(16, 70);
    CHECK(m.median(4) == doctest::Approx(60));
    const auto tie = m.estimate(32);  // bucket 5: buckets 4 and 6 are both one away; the larger wins
    CHECK(tie.bucket == 6);
}

TEST_CASE("eta: sessions of a run, the queue simulation and the concurrency in effect") {
    test::FixtureProject fx;
    const auto& symbols = fx.program.symbols();
    test::EventScript run;
    run.add(events::RunStarted{"p", "model-a", "high", 2, {}, {}});
    // Finished sessions: add (bucket 3) took 100 s and matched; sum_array was stopped (does not count).
    run.add(events::SessionStarted{"s-add", "?add@@YAHHH@Z", "add", fx.va("add")}, 0);
    run.add(events::SessionStarted{"s-sum", "?sum_array@@YAHPBHH@Z", "sum_array", fx.va("sum_array")}, 1);
    run.at(100);
    run.add(events::SessionFinished{"s-add", "matched", "", 100, 3, 0.2}, 0);
    run.add(events::SessionFinished{"s-sum", "stopped", "", 10, 1, 0.1}, 1);
    DurationModel model;
    model.add_run(run.data(), symbols);
    CHECK(model.samples() == 1);
    CHECK(model.median(size_bucket(fx.size("add"))) == doctest::Approx(100));

    // Now: helper runs on worker 0 since t=100 (bucket 3: expect 100 s); three functions are queued.
    run.add(events::SessionStarted{"s-helper", "helper", "helper", fx.va("helper")}, 0);
    run.add(events::QueueUpdated{{{fx.va("read_counter"), "read_counter", false, 0},
                                  {fx.va("message"), "message", false, 0},
                                  {fx.va("mix"), "mix", false, 0}}});
    run.at(160);
    const QueueEta q = estimate_queue(model, run.data(), symbols, run.now, 2);
    CHECK(q.workers == 2);
    REQUIRE(q.running.size() == 1);
    CHECK(q.running[0].elapsed == doctest::Approx(60));
    CHECK(q.running[0].remaining == doctest::Approx(40));
    REQUIRE(q.items.size() == 3);
    // read_counter and message (6 bytes, bucket 2) and mix (23 bytes, bucket 4) borrow bucket 3's median.
    const double small = 100 / std::sqrt(2.0), large = 100 * std::sqrt(2.0);
    CHECK(q.items[0].start == doctest::Approx(0));  // worker 1 is free
    CHECK(q.items[0].finish == doctest::Approx(small));
    CHECK(q.items[1].start == doctest::Approx(40));  // worker 0 after helper
    CHECK(q.items[2].start == doctest::Approx(small));
    CHECK(q.items[2].finish == doctest::Approx(small + large));
    CHECK(q.finish == doctest::Approx(small + large));
    CHECK(q.items[2].estimate.source == DurationModel::Source::nearby);

    // One worker: everything in sequence after the running session.
    const QueueEta one = estimate_queue(model, run.data(), symbols, run.now, 1);
    CHECK(one.finish == doctest::Approx(40 + small + small + large));
    CHECK(one.beyond_head == 0);
    // A session running longer than its estimate is given a tenth of the estimate.
    const QueueEta late = estimate_queue(model, run.data(), symbols, run.now + std::chrono::seconds(500), 1);
    CHECK(late.running[0].remaining == doctest::Approx(10));

    // A large queue arrives as its head: the 100 functions beyond it take the listed ones' mean,
    // spread over both workers from when each is free (after mix and after message).
    {
        test::EventScript big = run;
        big.add(events::QueueUpdated{{{fx.va("read_counter"), "read_counter", false, 0},
                                      {fx.va("message"), "message", false, 0},
                                      {fx.va("mix"), "mix", false, 0}},
                                     103});
        const QueueEta head = estimate_queue(model, big.data(), symbols, run.now, 2);
        CHECK(head.items.size() == 3);
        CHECK(head.beyond_head == 100);
        const double mean = (2 * small + large) / 3;
        CHECK(head.finish == doctest::Approx((40 + small + small + large + 100 * mean) / 2));
    }

    CHECK(current_concurrency(run.data()) == 2);
    run.add(events::Control{"set_concurrency", "", "5"});
    run.add(events::Control{"pause", "worker 1", ""});
    CHECK(current_concurrency(run.data()) == 5);
    CHECK(estimate_queue(model, run.data(), symbols, run.now).workers == 5);
}

TEST_CASE("eta: a past run's event log") {
    test::FixtureProject fx;
    auto dir = fs::TempDir::create("decomp-vm-eta").value();
    const auto path = dir.path() / "events.jsonl";
    {
        events::EventBus bus("run-eta");
        auto log = events::JsonlEventLog::open(path).value();
        bus.subscribe([&](const events::Event& e) { log->write(e); });
        bus.publish(events::RunStarted{"p", "m", "high", 1, {}, {}});
        bus.publish(events::SessionStarted{"s1", "?add@@YAHHH@Z", "add", fx.va("add")}, 0);
        bus.publish(events::LogLine{"warn", "session_started is mentioned here"}, 0);
        bus.publish(events::SessionFinished{"s1", "gave_up", "", 50, 4, 0.1}, 0);
    }
    DurationModel model;
    REQUIRE(add_event_log(model, path, fx.program.symbols()));
    CHECK(model.samples() == 1);
    CHECK(model.samples_in(size_bucket(fx.size("add"))) == 1);
    CHECK_FALSE(add_event_log(model, dir.path() / "missing.jsonl", fx.program.symbols()));

    // Speed on a large log: 2,000 sessions among 30 MB of other events.
    const events::TimePoint base = from_unix_ms(1'791'108'000'000);
    std::string big;
    const std::string filler = dump_compact(events::to_json(
        events::Event{7, base, "r", 0, events::ToolCallFinished{"s", "toolu_1", "compile_and_diff", false, std::string(180, 'x'), 120}}));
    for (int i = 0; i < 2000; ++i) {
        events::Event started{0, base + std::chrono::seconds(i), "r", 0, events::SessionStarted{std::format("s{}", i), "f", "f", fx.va("add")}};
        events::Event finished{0, base + std::chrono::seconds(i + 30), "r", 0,
                               events::SessionFinished{std::format("s{}", i), "matched", "", 100, 3, 0.1}};
        big += dump_compact(events::to_json(started)) + "\n";
        for (int k = 0; k < 50; ++k) big += filler + "\n";
        big += dump_compact(events::to_json(finished)) + "\n";
    }
    REQUIRE(fs::write_text(dir.path() / "big.jsonl", big));
    DurationModel large;
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(add_event_log(large, dir.path() / "big.jsonl", fx.program.symbols()));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("add_event_log: " << big.size() / (1 << 20) << " MB in " << ms << " ms");
    CHECK(large.samples() == 2000);
    CHECK(large.median(size_bucket(fx.size("add"))) == doctest::Approx(30));
}
