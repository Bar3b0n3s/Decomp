#include "core/fs.hpp"
#include "project/progress.hpp"
#include "run/store.hpp"
#include "viewmodel/progress.hpp"
#include "viewmodel/run_history.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

#include <chrono>

using namespace decomp;
using namespace decomp::vm;
using project::FunctionStatus;

namespace {

const StatusSegment& seg(const DashboardProgress& p, FunctionStatus s) { return p.segment(s); }

RunRecord record(std::string id, TimePoint started, std::vector<std::pair<u64, bool>> functions, double cost) {
    RunRecord r;
    r.id = std::move(id);
    r.started = started;
    r.cost_usd = cost;
    for (auto [va, matched] : functions) {
        RunFunctionRecord f;
        f.va = va;
        f.matched = matched;
        f.outcome = matched ? "matched" : "gave_up";
        f.turns = 3;
        r.functions.push_back(f);
        r.matched += matched;
    }
    return r;
}

} // namespace

TEST_CASE("common: size buckets, ISO times, run ids, durations") {
    CHECK(size_bucket(0) == 0);
    CHECK(size_bucket(1) == 0);
    CHECK(size_bucket(2) == 1);
    CHECK(size_bucket(15) == 3);
    CHECK(size_bucket(16) == 4);
    CHECK(size_bucket(1000) == 9);
    CHECK(size_bucket_label(3) == "8-15 B");
    CHECK(size_bucket_label(10) == "1-2 KiB");
    const auto t = parse_iso8601("2026-10-04T13:00:05Z").value();
    CHECK(to_unix_ms(t) == 1'791'118'805'000);
    CHECK(to_unix_ms(parse_iso8601("2026-10-04T13:00:05.250Z").value()) == 1'791'118'805'250);
    CHECK(parse_iso8601("2026-10-04T15:00:05+02:00") == t);
    CHECK(parse_iso8601("2026-10-04T13:00:05") == t);
    CHECK_FALSE(parse_iso8601("2026-13-04T13:00:05Z"));
    CHECK_FALSE(parse_iso8601("yesterday"));
    CHECK(run_id_time("2026-10-04T13-00-05-1a2b") == t);
    CHECK(day_label(day_number(t)) == "2026-10-04");
    CHECK(day_label(day_number(t, std::chrono::hours(11))) == "2026-10-05");
    CHECK(format_duration(42) == "42s");
    CHECK(format_duration(185) == "3m 05s");
    CHECK(format_duration(3720) == "1h 02m");
    CHECK(format_duration(2 * 86400 + 3 * 3600) == "2d 03h");
    CHECK(format_usd(0.0042) == "$0.0042");
    CHECK(format_usd(12.345) == "$12.35");
    CHECK(is_complete_outcome("matched"));
    CHECK_FALSE(is_complete_outcome("interrupted"));
}

TEST_CASE("progress: the stored numbers are decomp status's, segments and best-match bins add up") {
    test::FixtureProject fx;
    fx.set("add", FunctionStatus::matched, 100, 2, 0.5);
    fx.set("sum_array", FunctionStatus::nonmatching, 85, 4, 1.0);
    fx.set("dispatch", FunctionStatus::nonmatching, 12, 1, 0.25);
    fx.set("helper", FunctionStatus::library);
    fx.set("message", FunctionStatus::skipped);
    fx.set("scale", FunctionStatus::gave_up, 40, 3, 0.75);
    fx.set("mix", FunctionStatus::refused);
    const auto& symbols = fx.program.symbols();

    const DashboardProgress p = dashboard_progress(symbols, fx.project);
    const project::Progress expected = project::compute_progress(symbols, fx.project);
    CHECK(project::to_json(p.stored) == project::to_json(expected));
    usize functions = 0;
    u64 bytes = 0;
    for (const auto& s : p.segments) {
        CAPTURE(project::to_string(s.status));
        auto it = expected.buckets.find(s.status);
        CHECK(s.functions == (it == expected.buckets.end() ? 0 : it->second.functions));
        CHECK(s.bytes == (it == expected.buckets.end() ? 0 : it->second.bytes));
        functions += s.functions;
        bytes += s.bytes;
    }
    CHECK(functions == expected.functions);
    CHECK(bytes == expected.code_bytes);
    CHECK(p.segments[0].status == FunctionStatus::matched);
    CHECK(p.segments[1].status == FunctionStatus::library);
    CHECK(p.segments[2].status == FunctionStatus::skipped);
    CHECK(seg(p, FunctionStatus::matched).byte_share == doctest::Approx(15.0 / static_cast<double>(expected.code_bytes)));
    CHECK(p.best_match_bins[8] == 1);
    CHECK(p.best_match_bins[1] == 1);
    CHECK(p.running == 0);

    // A live run: sum_array has a session (in progress), add's session cannot unmatch it, dispatch's ended.
    test::EventScript run;
    run.add(events::SessionStarted{"s-sum", "?sum_array@@YAHPBHH@Z", "sum_array", fx.va("sum_array")}, 0);
    run.add(events::SessionStarted{"s-add", "?add@@YAHHH@Z", "add", fx.va("add")}, 1);
    run.add(events::SessionStarted{"s-dispatch", "?dispatch@@YAHHH@Z", "dispatch", fx.va("dispatch")}, 2);
    run.add(events::SessionFinished{"s-dispatch", "gave_up", "stuck", 12, 3, 0.1}, 2);
    const DashboardProgress live = dashboard_progress(symbols, fx.project, &run.data());
    CHECK(live.running == 1);
    CHECK(seg(live, FunctionStatus::in_progress).functions == 1);
    CHECK(seg(live, FunctionStatus::in_progress).bytes == fx.size("sum_array"));
    CHECK(seg(live, FunctionStatus::nonmatching).functions == 1);
    CHECK(seg(live, FunctionStatus::matched).functions == 1);
    CHECK(live.best_match_bins[8] == 0);
    CHECK(live.best_match_bins[1] == 1);
    CHECK(project::to_json(live.stored) == project::to_json(expected));  // the stored numbers never change
}

TEST_CASE("progress over time: per run, per day, new versus repeated matches") {
    test::FixtureProject fx;
    const auto& symbols = fx.program.symbols();
    const TimePoint day1 = parse_iso8601("2026-10-03T09:00:00Z").value();
    const TimePoint day2 = parse_iso8601("2026-10-04T23:30:00Z").value();
    std::vector<RunRecord> runs = {
        record("b", day1 + std::chrono::hours(5), {{fx.va("add"), true}, {fx.va("dispatch"), true}, {fx.va("mix"), false}}, 2.0),
        record("a", day1, {{fx.va("add"), true}, {fx.va("sum_array"), true}}, 1.0),
        record("c", day2, {{fx.va("helper"), true}}, 0.5),
    };
    const ProgressHistory h = progress_history(runs, symbols);
    REQUIRE(h.runs.size() == 3);
    CHECK(h.runs[0].label == "a");  // ordered by start time
    CHECK(h.runs[0].functions == 2);
    CHECK(h.runs[0].new_functions == 2);
    CHECK(h.runs[0].total_functions == 2);
    CHECK(h.runs[0].total_bytes == fx.size("add") + fx.size("sum_array"));
    CHECK(h.runs[1].functions == 2);
    CHECK(h.runs[1].new_functions == 1);  // add was matched before
    CHECK(h.runs[1].total_functions == 3);
    CHECK(h.runs[2].total_functions == 4);
    CHECK(h.runs[2].cost_usd == doctest::Approx(0.5));
    REQUIRE(h.days.size() == 2);
    CHECK(h.days[0].label == "2026-10-03");
    CHECK(h.days[0].functions == 3);
    CHECK(h.days[0].new_functions == 3);
    CHECK(h.days[0].cost_usd == doctest::Approx(3.0));
    CHECK(h.days[1].total_functions == 4);
    CHECK(h.days[1].total_bytes == fx.size("add") + fx.size("sum_array") + fx.size("dispatch") + fx.size("helper"));
    CHECK(h.run_functions.y == std::vector<double>{2, 3, 4});
    CHECK(h.run_functions.x[0] == doctest::Approx(to_unix_seconds(day1)));
    CHECK(h.day_functions.y == std::vector<double>{3, 4});
    CHECK(h.day_functions.x[0] == doctest::Approx(to_unix_seconds(parse_iso8601("2026-10-03T00:00:00Z").value())));

    // Days in a time zone east of UTC: the 23:30 UTC run belongs to the next day.
    const ProgressHistory east = progress_history(runs, symbols, std::chrono::hours(2));
    REQUIRE(east.days.size() == 2);
    CHECK(east.days[1].label == "2026-10-05");
    CHECK(east.days[1].time == parse_iso8601("2026-10-04T22:00:00Z").value());
}

TEST_CASE("run records: both summary formats, the runs directory and the live run") {
    // `decomp run`'s summary, made from a scripted state.
    test::EventScript script;
    script.add(events::RunStarted{"proj", "model-a", "high", 2, {"add", "sum_array"}, {0x401060, 0x401080}});
    script.at(10);
    script.add(events::SessionStarted{"s1", "?add@@YAHHH@Z", "add", 0x401060}, 0);
    script.add(events::TurnFinished{"s1", 1, "tool_use", {100, 50, 10, 1000}, 0.25, 900}, 0);
    script.at(70);
    script.add(events::SessionFinished{"s1", "matched", "", 100, 1, 0.25}, 0);
    script.add(events::SessionStarted{"s2", "?sum_array@@YAHPBHH@Z", "sum_array", 0x401080}, 1);
    const RunRecord live = run_record_from_state(script.data());
    CHECK(live.id == script.run);
    CHECK(live.started == script.t0);
    CHECK(live.finished == TimePoint{});
    CHECK(live.matched == 1);
    REQUIRE(live.functions.size() == 2);
    CHECK(live.functions[0].va == 0x401060);
    CHECK(live.functions[1].outcome == "running");
    CHECK(live.usage.cache_read == 1000);
    CHECK(live.turns() == 1);

    // `decomp agent`'s summary: one function, usage only per function.
    const Json agent = parse_json(R"({"run":"2026-10-03T08-00-00-ffff","started":"2026-10-03T08:00:00Z","finished":"2026-10-03T08:05:00Z",
        "status":"completed","model":"model-a","effort":"high","replay":true,"cost_usd":0.5,
        "functions":[{"function":"?add@@YAHHH@Z","display":"add","va":4198496,"outcome":"gave_up","detail":"x","matched":false,
                      "best_match":80.0,"turns":7,"cost_usd":0.5,
                      "usage":{"input_tokens":1,"output_tokens":2,"cache_creation_input_tokens":3,"cache_read_input_tokens":4}}]})")
                           .value();
    const RunRecord a = run_record_from_summary(agent);
    CHECK(a.id == "2026-10-03T08-00-00-ffff");
    CHECK(a.replay);
    CHECK(a.matched == 0);
    CHECK(a.usage.total() == 10);
    CHECK(a.turns() == 7);
    CHECK(seconds_between(a.started, a.finished) == doctest::Approx(300));

    auto dir = fs::TempDir::create("decomp-vm-runs").value();
    const auto runs_dir = dir.path() / "runs";
    REQUIRE(fs::write_text(runs_dir / "2026-10-03T08-00-00-ffff" / "summary.json", dump_pretty(agent)));
    REQUIRE(fs::write_text(runs_dir / script.run / "summary.json", dump_pretty(run::run_summary(script.data()))));
    REQUIRE(fs::write_text(runs_dir / script.run / "run.json", dump_pretty(Json{{"id", script.run}, {"status", "running"}})));
    REQUIRE(fs::create_directories(runs_dir / "empty"));
    auto records = load_run_records(runs_dir);
    REQUIRE(records.size() == 2);
    CHECK(records[0].id == "2026-10-03T08-00-00-ffff");
    CHECK(records[0].status == "completed");
    CHECK(records[1].id == script.run);
    CHECK(records[1].status == "interrupted");  // run.json says running, but no process holds its lock
    CHECK(records[1].dir == runs_dir / script.run);

    script.at(100);
    script.add(events::SessionFinished{"s2", "gave_up", "", 50, 2, 0.5}, 1);
    merge_live_run(records, script.data());
    REQUIRE(records.size() == 2);
    CHECK(records[1].functions[1].outcome == "gave_up");
    CHECK(records[1].dir == runs_dir / script.run);
    CHECK(load_run_records(dir.path() / "missing").empty());
}

TEST_CASE("run records: fifty runs of 500 functions") {
    test::EventScript script;
    script.add(events::RunStarted{"proj", "model-a", "high", 4, {}, {}});
    for (int i = 0; i < 500; ++i) {
        const std::string s = std::format("s{}", i);
        script.add(events::SessionStarted{s, std::format("?f{}@@YAXXZ", i), std::format("f{}", i), 0x10000000 + static_cast<u64>(i) * 64}, i % 4);
        script.add(events::TurnFinished{s, 1, "tool_use", {1000, 200, 0, 5000}, 0.05, 900}, i % 4);
        script.add(events::SessionFinished{s, i % 3 == 0 ? "matched" : "gave_up", "", 100, 1, 0.05}, i % 4);
    }
    Json summary = run::run_summary(script.data());
    auto dir = fs::TempDir::create("decomp-vm-runs").value();
    usize bytes = 0;
    for (int r = 0; r < 50; ++r) {
        const std::string id = std::format("2026-09-{:02}T10-00-00-{:04x}", 1 + r % 28, r);
        summary["run"] = id;
        const std::string text = dump_pretty(summary);
        bytes += text.size();
        REQUIRE(fs::write_text(dir.path() / id / "summary.json", text));
        REQUIRE(fs::write_text(dir.path() / id / "run.json", dump_pretty(Json{{"id", id}, {"status", "completed"}})));
    }
    const auto start = std::chrono::steady_clock::now();
    const std::vector<RunRecord> records = load_run_records(dir.path());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("load_run_records: 50 runs of 500 functions (" << bytes / 1024 << " KB of summaries) in " << ms << " ms");
    REQUIRE(records.size() == 50);
    CHECK(records[0].functions.size() == 500);
    CHECK(records[0].matched == 167);
}

TEST_CASE("progress: 100,000 functions") {
    test::FixtureProject fx;
    SymbolDb symbols;
    for (u64 i = 0; i < 100'000; ++i)
        symbols.add(Symbol{0x10000000 + i * 32, std::format("f{}", i), std::format("f{}", i), "", SymbolKind::function, 32, SymbolSource::pdb, false, {}});
    const auto start = std::chrono::steady_clock::now();
    const DashboardProgress p = dashboard_progress(symbols, fx.project);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("dashboard_progress: 100,000 functions in " << ms << " ms");
    CHECK(p.stored.functions == 100'000);
    CHECK(seg(p, FunctionStatus::unstarted).functions == 100'000);
}
