#include "viewmodel/series.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

#include <cmath>

using namespace decomp;
using namespace decomp::vm;

TEST_CASE("series: throughput per minute, spend and cache-hit rate") {
    test::EventScript run;  // t0 is on a minute boundary
    run.add(events::RunStarted{"p", "m", "high", 2, {}, {}});
    run.at(5);
    run.add(events::SessionStarted{"s1", "f", "f", 0x1000}, 0);
    run.at(10);
    run.add(events::TurnFinished{"s1", 1, "tool_use", {1000, 600, 3000, 0}, 0.5, 2000, "m", 400}, 0);
    run.at(20);
    run.add(events::CompileFinished{"s1", true, false, 100, 0}, 0);
    run.at(50);
    run.add(events::TurnFinished{"s1", 2, "tool_use", {100, 1200, 0, 3900}, 0.25, 1000, "m", 200}, 0);
    run.at(130);  // minute 2 (minute 1 has nothing)
    run.add(events::Retry{"s1", 1, "HTTP 429 rate_limit_error", 1000, 429}, 0);
    run.add(events::Retry{"s1", 2, "overloaded", 1000, 529}, 0);
    run.add(events::TurnFinished{"s1", 3, "end_turn", {0, 60, 0, 4000}, 0.1, 1000, "m", 0}, 0);

    const ThroughputSeries t = throughput(run.data(), run.t0);
    REQUIRE(t.x.size() == 3);
    CHECK(t.x == std::vector<double>{0, 60, 120});
    CHECK(t.turns == std::vector<double>{2, 0, 1});
    CHECK(t.compiles == std::vector<double>{1, 0, 0});
    CHECK(t.retries == std::vector<double>{0, 0, 2});
    CHECK(t.rate_limited == std::vector<double>{0, 0, 1});
    CHECK(t.output_tokens_per_second[0] == doctest::Approx(1800.0 / 60));
    CHECK(t.mean_ttft_ms[0] == doctest::Approx(300));
    CHECK(std::isnan(t.mean_ttft_ms[1]));
    CHECK(std::isnan(t.mean_ttft_ms[2]));  // the only turn had no first token time
    CHECK(t.cost_usd[0] == doctest::Approx(0.75));
    CHECK(t.cache_hit_rate[0] == doctest::Approx(3900.0 / 8000.0));
    CHECK(std::isnan(t.cache_hit_rate[1]));
    CHECK(t.cache_hit_rate[2] == doctest::Approx(1.0));
    // A later origin shifts x.
    CHECK(throughput(run.data(), run.t0 + std::chrono::seconds(30)).x[0] == doctest::Approx(-30));
    CHECK(throughput(events::RunStateData{}, run.t0).x.empty());

    const Series spend = spend_over_time(run.data(), run.t0);
    CHECK(spend.x == std::vector<double>{60, 180});
    CHECK(spend.y[0] == doctest::Approx(0.75));
    CHECK(spend.y.back() == doctest::Approx(run.data().cost_usd));

    const Series per_minute = cache_hit_rate_over_time(run.data(), run.t0);
    CHECK(per_minute.x == std::vector<double>{0, 120});
    CHECK(per_minute.y[1] == doctest::Approx(1.0));
    const Series cumulative = cache_hit_rate_over_time(run.data(), run.t0, true);
    CHECK(cumulative.y[1] == doctest::Approx(7900.0 / 12000.0));
    CHECK(cumulative.y[1] == doctest::Approx(run.data().cache_hit_rate()));
}

TEST_CASE("series: score per attempt") {
    events::SessionState s;
    s.scores = {40, 75.5, 60, 100};
    const Series scores = score_per_attempt(s);
    CHECK(scores.x == std::vector<double>{1, 2, 3, 4});
    CHECK(scores.y == s.scores);
    CHECK(best_score_per_attempt(s).y == std::vector<double>{40, 75.5, 75.5, 100});
    CHECK(score_per_attempt(events::SessionState{}).empty());
}

TEST_CASE("series: the worker timeline clipped to a window") {
    test::EventScript run;
    run.add(events::RunStarted{"p", "m", "high", 2, {}, {}});
    run.add(events::SessionStarted{"s1", "?f@@YAXXZ", "f", 0x1000}, 0);  // worker 0: starting at 0
    run.at(10);
    run.add(events::TurnStarted{"s1", 1}, 0);  // waiting for model at 10
    run.at(30);
    run.add(events::ToolCallStarted{"s1", "t1", "compile_and_diff", 1, Json::object()}, 0);  // compiling at 30
    run.at(40);
    run.add(events::SessionStarted{"s2", "?g@@YAXXZ", "g", 0x2000}, 1);  // worker 1 from 40
    run.at(45);
    run.add(events::SessionFinished{"s1", "matched", "", 100, 1, 0.1}, 0);  // worker 0 idle from 45

    const auto t = worker_timeline(run.data(), run.t0, run.t0 + std::chrono::seconds(5), run.t0 + std::chrono::seconds(50),
                                   run.t0 + std::chrono::seconds(60));
    CHECK(t.start == doctest::Approx(5));
    CHECK(t.end == doctest::Approx(50));
    REQUIRE(t.rows.size() == 2);
    const auto& w0 = t.rows[0];
    CHECK(w0.worker == 0);
    REQUIRE(w0.segments.size() == 4);
    CHECK(w0.segments[0].start == doctest::Approx(5));  // clipped
    CHECK(w0.segments[0].end == doctest::Approx(10));
    CHECK(t.phases[w0.segments[0].phase] == "starting");
    CHECK(t.functions[w0.segments[0].function] == "f");
    CHECK(t.phases[w0.segments[1].phase] == "waiting for model");
    CHECK(t.phases[w0.segments[2].phase] == "compiling");
    CHECK(t.phases[w0.segments[3].phase] == "idle");
    CHECK(w0.segments[3].open);
    CHECK(w0.segments[3].end == doctest::Approx(50));  // clipped at the window's end
    CHECK(t.functions[w0.segments[3].function].empty());
    const auto& w1 = t.rows[1];
    REQUIRE(w1.segments.size() == 1);
    CHECK(w1.segments[0].start == doctest::Approx(40));
    CHECK(t.functions[w1.segments[0].function] == "g");
    CHECK(t.functions[0].empty());

    // A window before anything happened is empty.
    const auto before = worker_timeline(run.data(), run.t0, run.t0 - std::chrono::seconds(100), run.t0 - std::chrono::seconds(50), run.t0);
    for (const auto& row : before.rows) CHECK(row.segments.empty());
}
