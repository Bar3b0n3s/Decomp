#include "analysis/annotate.hpp"
#include "events/bus.hpp"
#include "llvm_fixture.hpp"
#include "viewmodel/difficulty.hpp"
#include "viewmodel/eta.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

using namespace decomp;
using namespace decomp::vm;

TEST_CASE("difficulty: features of the fixture's functions agree with the annotated listing") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        auto p = Program::open(test::fixture(std::string(arch) + "/basic.exe")).value();
        for (const char* name : {"add", "sum_array", "dispatch", "entry", "Player::Hit"}) {
            CAPTURE(name);
            const u64 va = *p.resolve(name);
            const FunctionFeatures f = function_features(p, va).value();
            const AnnotatedFunction a = annotate_function(p, va).value();
            CHECK(f.va == va);
            CHECK(f.bytes == a.end - a.start);
            CHECK(f.instructions == a.instruction_count);
            CHECK(f.blocks == a.block_count);
            CHECK(f.loops == a.loop_count);
        }
        const FunctionFeatures add = function_features(p, *p.resolve("add")).value();
        CHECK(add.calls == 0);
        CHECK(add.callees == 0);
        CHECK(add.unknown_callees == 0);
        const FunctionFeatures sum = function_features(p, *p.resolve("sum_array")).value();
        CHECK(sum.loops >= 1);
        CHECK(sum.max_loop_depth >= 1);
        const FunctionFeatures dispatch = function_features(p, *p.resolve("dispatch")).value();
        CHECK(dispatch.jump_tables == 1);
        CHECK(dispatch.callees >= 3);  // add, helper, other_value, sum_array (calls and tail jumps)
        CHECK(dispatch.unknown_callees == 0);
        const FunctionFeatures entry = function_features(p, *p.resolve("entry")).value();
        CHECK(entry.calls >= 8);
        CHECK(entry.callees >= 8);  // the fixture's functions and ExitProcess through its import
        CHECK(entry.unknown_callees == 0);

        CHECK(difficulty(add) < difficulty(sum));
        CHECK(difficulty(add) < difficulty(entry));
        CHECK(difficulty(add) < difficulty(dispatch));
        MESSAGE(std::string(arch) << " difficulty: add " << difficulty(add) << ", sum_array " << difficulty(sum) << ", dispatch "
                                  << difficulty(dispatch) << ", entry " << difficulty(entry));
        CHECK(difficulty_label(difficulty(add)) == "easy");
        CHECK_FALSE(function_features(p, *p.resolve("g_counter")));

        // Unnamed callees count as unknown.
        auto unnamed = p.with_symbols(p.symbols());
        Symbol renamed = *unnamed.symbols().at(*p.resolve("helper"));
        unnamed.symbols().remove(renamed.va);
        renamed.name = std::format("sub_{:x}", renamed.va);
        renamed.display.clear();
        renamed.pdb_name.clear();
        renamed.source = SymbolSource::analysis;
        unnamed.symbols().add(renamed);
        const FunctionFeatures blind = function_features(unnamed, *p.resolve("dispatch")).value();
        CHECK(blind.unknown_callees == 1);
        CHECK(blind.callees == dispatch.callees - 1);
        CHECK(difficulty(blind) > difficulty(dispatch));
    }
}

TEST_CASE("difficulty: the formula") {
    FunctionFeatures f;
    f.bytes = 15;
    f.blocks = 1;
    CHECK(difficulty(f) == doctest::Approx(4.5));
    f.loops = 20;  // capped at 8
    f.max_loop_depth = 9;  // capped at 4
    f.unknown_callees = 3;
    f.callees = 40;  // capped at 20
    f.jump_tables = 1;
    CHECK(difficulty(f) == doctest::Approx(4.5 + 8 + 2 + 5 + 3 + 0.5));
    CHECK(difficulty_label(7.9) == "easy");
    CHECK(difficulty_label(8) == "medium");
    CHECK(difficulty_label(14) == "hard");
    CHECK(difficulty_label(20) == "very hard");
}

TEST_CASE("analyze_functions: every function, callers, cancellation and progress") {
    auto p = Program::open(test::fixture("x86/basic.exe")).value();
    std::vector<u64> vas;
    for (const auto* f : p.symbols().functions()) vas.push_back(f->va);
    usize reported = 0;
    const FunctionAnalysis all = analyze_functions(p, vas, {}, [&](usize done, usize total) {
        CHECK(done <= total);
        reported = done;
    });
    CHECK_FALSE(all.cancelled);
    CHECK(reported == vas.size());
    CHECK(all.functions.size() + all.failed.size() == vas.size());
    REQUIRE(all.find(*p.resolve("add")));
    // add is called by dispatch and entry (Program::callers_of agrees for direct calls).
    CHECK(all.find(*p.resolve("add"))->callers == p.callers_of(*p.resolve("add")).size());
    CHECK(all.find(*p.resolve("helper"))->callers >= 1);
    CHECK(all.find(*p.resolve("entry"))->callers == 0);
    CHECK(std::ranges::is_sorted(all.functions, {}, &FunctionFeatures::va));
    CHECK_FALSE(all.find(0x12345));

    int polls = 0;
    const FunctionAnalysis stopped = analyze_functions(p, vas, [&] { return ++polls > 0; });
    CHECK(stopped.cancelled);
    CHECK(stopped.functions.empty());

    // Cost per function.
    const auto start = std::chrono::steady_clock::now();
    usize instructions = 0;
    for (int i = 0; i < 200; ++i)
        for (const auto& f : analyze_functions(p, vas).functions) instructions += f.instructions;
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("analyze_functions: " << us / (200.0 * static_cast<double>(vas.size())) << " us per function, "
                                  << us / static_cast<double>(instructions) * 100 << " us per 100 instructions");
}

TEST_CASE("analyze_functions: a generated program with a very large function") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    // 300 small functions with loops, and one function of thousands of instructions calling them.
    auto dir = fs::TempDir::create("decomp-vm-large").value();
    std::string src = "#define NOINLINE __declspec(noinline)\nvolatile int g_sink;\n";
    for (int i = 0; i < 300; ++i)
        src += std::format("NOINLINE int f{0}(int x) {{ int s = 0; for (int i = 0; i < x; ++i) s += (i * {1}) ^ (s >> 3); return s + {0}; }}\n", i,
                           i + 3);
    src += "NOINLINE int big(int x) {\n  int s = 0;\n";
    for (int i = 0; i < 1500; ++i) src += std::format("  if (x & {}) s += f{}(x + {}); else g_sink = s;\n", 1 << (i % 30), i % 300, i);
    src += "  return s;\n}\nextern \"C\" void entry() { g_sink = big(g_sink); }\n";
    const auto source = dir.path() / "large.cpp";
    REQUIRE(fs::write_text(source, src));
    const auto exe = test::build_program(Arch::x86, *tools, dir.path() / "out", {source}, "large");
    REQUIRE(exe);
    auto p = Program::open(*exe).value();
    std::vector<u64> vas;
    for (const auto* f : p.symbols().functions()) vas.push_back(f->va);
    auto start = std::chrono::steady_clock::now();
    const FunctionAnalysis analysis = analyze_functions(p, vas);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    usize instructions = 0;
    for (const auto& f : analysis.functions) instructions += f.instructions;
    const FunctionFeatures* big = analysis.find(*p.resolve("big"));
    REQUIRE(big);
    CHECK(big->instructions > 5000);
    CHECK(big->callees >= 250);
    CHECK(analysis.find(*p.resolve("f7"))->callers >= 1);
    CHECK(difficulty_label(difficulty(*big)) == "very hard");
    start = std::chrono::steady_clock::now();
    CHECK_FALSE(p.callers_of(*p.resolve("f7")).empty());  // builds the cross-reference index
    const double xref_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("analyze_functions: " << analysis.functions.size() << " functions, " << instructions << " instructions (the largest "
                                  << big->instructions << ") in " << ms << " ms: " << ms * 1000 / static_cast<double>(instructions) * 100
                                  << " us per 100 instructions; cross-reference index " << xref_ms << " ms");
}

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
