#include "agent/cost.hpp"
#include "viewmodel/cost.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <tuple>

using namespace decomp;
using namespace decomp::vm;
using project::FunctionStatus;

namespace {

RunRecord run_of(std::string id, std::string started, std::string model, std::string effort, double cost,
                 std::vector<std::tuple<u64, bool, int>> functions) {
    RunRecord r;
    r.id = std::move(id);
    r.started = parse_iso8601(started).value();
    r.model = std::move(model);
    r.effort = std::move(effort);
    r.cost_usd = cost;
    for (auto [va, matched, turns] : functions) {
        RunFunctionRecord f;
        f.va = va;
        f.matched = matched;
        f.turns = turns;
        r.matched += matched;
        r.functions.push_back(f);
    }
    r.usage = {1000, 500, 200, 3000};
    return r;
}

} // namespace

TEST_CASE("cost: by run, day, model and effort, and per match") {
    test::FixtureProject fx;
    const std::vector<RunRecord> runs = {
        run_of("r1", "2026-10-03T09:00:00Z", "model-a", "high", 4.0, {{0x401060, true, 10}, {0x401080, false, 30}}),
        run_of("r2", "2026-10-03T15:00:00Z", "model-a", "high", 2.0, {{0x401080, true, 20}}),
        run_of("r3", "2026-10-04T01:00:00Z", "model-b", "medium", 1.0, {{0x4010f0, false, 5}, {0x401160, false, 5}}),
    };
    const CostReport report = cost_report(runs, fx.program.symbols(), *fx.project.function_infos());
    CHECK(report.total.cost_usd == doctest::Approx(7.0));
    CHECK(report.total.worked == 5);
    CHECK(report.total.matched == 2);
    CHECK(report.total.turns == 70);
    CHECK(report.total.runs == 3);
    CHECK(report.total.usd_per_match() == doctest::Approx(3.5));
    CHECK(report.total.turns_per_match() == doctest::Approx(35));
    CHECK(report.total.usage.cache_read == 9000);
    CHECK(report.total.cache_hit_rate() == doctest::Approx(3000.0 / 4200.0));
    REQUIRE(report.by_run.size() == 3);
    CHECK(report.by_run[0].key == "r1");
    CHECK(report.by_run[0].success_rate() == doctest::Approx(0.5));
    REQUIRE(report.by_day.size() == 2);
    CHECK(report.by_day[0].key == "2026-10-03");
    CHECK(report.by_day[0].cost_usd == doctest::Approx(6.0));
    CHECK(report.by_day[0].runs == 2);
    CHECK(report.by_day[1].key == "2026-10-04");
    // Nine hours east of UTC, r2 already belongs to the 4th.
    const CostReport east = cost_report(runs, fx.program.symbols(), *fx.project.function_infos(), nullptr, std::chrono::hours(9));
    CHECK(east.by_day[1].cost_usd == doctest::Approx(3.0));
    REQUIRE(report.by_model.size() == 2);
    CHECK(report.by_model[0].key == "model-a high");  // most spent first
    CHECK(report.by_model[0].worked == 3);
    CHECK(report.by_model[0].matched == 2);
    CHECK(report.by_model[0].success_rate() == doctest::Approx(2.0 / 3.0));
    CHECK(report.by_model[1].key == "model-b medium");
    CHECK(report.by_model[1].success_rate() == 0);
    CHECK(report.by_model[1].usd_per_match() == 0);
    CHECK(report.by_function.empty());
    CHECK(report.unpriced_models.empty());
}

TEST_CASE("cost: per function with the live run, fallback turns and unpriced models") {
    test::FixtureProject fx;
    fx.set("add", FunctionStatus::matched, 100, 2, 0.5);
    fx.set("sum_array", FunctionStatus::nonmatching, 50, 4, 2.0);
    test::EventScript run;
    run.add(events::SessionStarted{"s1", "?sum_array@@YAHPBHH@Z", "sum_array", fx.va("sum_array")}, 0);
    const std::string dated = std::string(agent::kDefaultPricingModel) + "-20270101";  // priced by its table row
    run.add(events::TurnFinished{"s1", 1, "tool_use", {10, 10, 0, 0}, 0.75, 100, dated, 10, true}, 0);
    run.add(events::CompileFinished{"s1", true, false, 10, 0}, 0);
    run.add(events::SessionStarted{"s2", "?dispatch@@YAHHH@Z", "dispatch", fx.va("dispatch")}, 1);
    run.add(events::TurnFinished{"s2", 1, "tool_use", {10, 10, 0, 0}, 0.25, 100, "mystery-model-1", 10}, 1);
    run.add(events::LogLine{"warn", std::format("no price known for model 'other-model': spend and the USD budget are estimated with {} prices",
                                                agent::kDefaultPricingModel)});
    const CostReport report = cost_report({}, fx.program.symbols(), *fx.project.function_infos(), &run.data());
    REQUIRE(report.by_function.size() == 3);
    CHECK(report.by_function[0].name == "int __cdecl sum_array(int const *, int)");
    CHECK(report.by_function[0].stored_usd == doctest::Approx(2.0));
    CHECK(report.by_function[0].live_usd == doctest::Approx(0.75));
    CHECK(report.by_function[0].status == FunctionStatus::in_progress);
    CHECK(report.by_function[0].attempts == 5);
    CHECK(report.by_function[1].va == fx.va("add"));
    CHECK(report.by_function[2].va == fx.va("dispatch"));
    CHECK(report.by_function[2].stored_usd == 0);
    CHECK(report.function_spend_usd == doctest::Approx(3.5));
    CHECK(report.fallback_turns == 1);
    CHECK(report.unpriced_models == std::vector<std::string>{"mystery-model-1", "other-model"});  // the dated id has a row
}

TEST_CASE("cost: projection for the functions still to do") {
    test::FixtureProject fx;
    // Observations: add (15 bytes, bucket 3) cost 1.0, helper (11 bytes, bucket 3) 3.0, entry (238, bucket 7) 8.0.
    fx.set("add", FunctionStatus::matched, 100, 2, 1.0);
    fx.set("helper", FunctionStatus::gave_up, 10, 2, 3.0);
    fx.set("entry", FunctionStatus::refused, 0, 1, 8.0);
    fx.set("sum_array", FunctionStatus::nonmatching, 50, 4, 2.0);  // not done: not an observation, still to do
    fx.set("message", FunctionStatus::skipped);
    const CostProjection p = project_remaining_cost(fx.program.symbols(), *fx.project.function_infos());
    CHECK(p.has_history);
    // Still to do: everything but add, entry (refused) and message (skipped); ExitProcess has no size.
    CHECK(p.remaining == 10);
    CHECK(p.unsized == 1);
    auto bucket = [&](int b) -> const CostProjection::Bucket& { return *std::ranges::find(p.buckets, b, &CostProjection::Bucket::bucket); };
    CHECK(bucket(3).observed == 2);
    CHECK(bucket(3).mean_usd == doctest::Approx(2.0));
    CHECK(bucket(3).remaining == 1);  // helper (gave up runs again by default)
    CHECK(bucket(2).from_nearby);     // read_counter: 6 bytes
    CHECK(bucket(2).mean_usd == doctest::Approx(2.0 / std::sqrt(2.0)));
    CHECK(bucket(6).remaining == 2);  // sum_array 107, dispatch 108
    double total = 0;
    for (const auto& b : p.buckets) total += b.projected_usd;
    CHECK(p.projected_usd == doctest::Approx(total));

    test::FixtureProject empty;
    const CostProjection none = project_remaining_cost(empty.program.symbols(), *empty.project.function_infos());
    CHECK_FALSE(none.has_history);
    CHECK(none.projected_usd == 0);
    CHECK(none.remaining == 13);
}

TEST_CASE("cost: 100,000 functions and 200 runs") {
    SymbolDb symbols;
    std::map<u64, project::FunctionInfo> infos;
    for (u64 i = 0; i < 100'000; ++i) {
        const u64 va = 0x10000000 + i * 64;
        symbols.add(Symbol{va, std::format("f{}", i), std::format("f{}", i), "", SymbolKind::function, static_cast<u32>(8 + i % 2000),
                           SymbolSource::pdb, false, {}});
        infos[va] = project::FunctionInfo{i % 3 == 0 ? FunctionStatus::matched : FunctionStatus::nonmatching, 50, 3, 0.01 * static_cast<double>(i % 100)};
    }
    std::vector<RunRecord> runs;
    for (int r = 0; r < 200; ++r) {
        std::vector<std::tuple<u64, bool, int>> functions;
        for (int f = 0; f < 500; ++f) functions.emplace_back(0x10000000 + static_cast<u64>(r * 500 + f) * 64, f % 2 == 0, 7);
        runs.push_back(run_of(std::format("r{}", r), std::format("2026-09-{:02}T10:00:00Z", 1 + r % 28), "model-a", "high", 5.0, functions));
    }
    auto start = std::chrono::steady_clock::now();
    const CostReport report = cost_report(runs, symbols, infos);
    const double report_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    start = std::chrono::steady_clock::now();
    const CostProjection projection = project_remaining_cost(symbols, infos);
    const double projection_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("cost_report: " << report_ms << " ms; project_remaining_cost: " << projection_ms << " ms");
    CHECK(report.by_run.size() == 200);
    CHECK(report.total.worked == 100'000);
    CHECK(projection.has_history);
}
