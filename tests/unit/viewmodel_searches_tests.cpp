// The Search view's model (viewmodel/searches.hpp): searches as rows, results by kind, the log.

#include "viewmodel/searches.hpp"

#include <doctest/doctest.h>

using namespace decomp;
using namespace decomp::vm;

TEST_CASE("search view model: a search as a row") {
    search::RunRecord r;
    r.id = "2026-10-06T10-00-00-flags-1a2b";
    r.kind = search::SearchKind::flags;
    r.status = search::RunStatus::done;
    r.target = "Player::Hit";
    r.started = "2026-10-06T10:00:00.123Z";
    r.candidates = 41;
    r.best = search::Score{12, 12, 0, 100};
    r.best_label = "/O2 /GS-";
    r.duration_ms = 1500;
    const auto row = search_run_row(r);
    CHECK(row.kind == "flags");
    CHECK(row.status == "done");
    CHECK(row.best == "12/12 byte-exact, distance 0, 100.0%");
    CHECK(row.complete);
    CHECK(row.seconds == doctest::Approx(1.5));
    r.best.reset();
    CHECK(search_run_row(r).best == "-");
    CHECK_FALSE(search_run_row(r).complete);
}

TEST_CASE("search view model: results by kind") {
    const Json flags = Json::parse(R"({"flags": ["/Gy", "/O2", "/GS-"], "base": ["/Gy"],
        "groups": [{"name": "optimization", "alternatives": ["/Od", "/O1", "/O2", "/Ox"], "start": 0, "chosen": 2, "equivalent": [2, 3]},
                   {"name": "security checks", "alternatives": ["none", "/GS-"], "start": 1, "chosen": 1, "equivalent": [1]}],
        "score": {"functions": 2, "exact": 2, "distance": 0, "match_percent": 100},
        "start_score": {"functions": 2, "exact": 0, "distance": 90, "match_percent": 70},
        "candidates": 17, "space": 8, "exhaustive": true, "cancelled": false})");
    const auto f = read_flag_search(flags);
    CHECK(f.flags == std::vector<std::string>{"/Gy", "/O2", "/GS-"});
    REQUIRE(f.groups.size() == 2);
    CHECK(f.groups[0].chosen_text() == "/O2");
    CHECK(f.groups[0].also_text() == "/Ox");
    CHECK_FALSE(f.groups[0].decided());
    CHECK(f.groups[0].changed());
    CHECK(f.groups[1].decided());
    CHECK_FALSE(f.groups[1].changed());
    CHECK(f.score.complete());
    CHECK(f.start_score.exact == 0);
    CHECK(f.exhaustive);
    CHECK(f.space == 8);

    const Json permute = Json::parse(R"({"steps": ["move `a = 1;` down 1"], "score": {"functions": 1, "exact": 1, "distance": 0, "match_percent": 100},
        "start_score": {"functions": 1, "exact": 0, "distance": 30, "match_percent": 80}, "candidates": 9, "cancelled": false})");
    const auto p = read_permute(permute);
    CHECK(p.steps.size() == 1);
    CHECK(p.improved());
    CHECK(p.error.empty());

    const Json identify = Json::parse(R"({"ranking": [
        {"toolchain": "gcc-x64", "kind": "gcc", "flags": ["-O2"], "score": {"functions": 5, "exact": 5, "distance": 0, "match_percent": 100}, "candidates": 10},
        {"toolchain": "missing", "kind": "gcc", "flags": [], "score": {"functions": 5, "exact": 0, "distance": 5000000, "match_percent": 0}, "candidates": 1,
         "error": "cannot run gcc\nmore"}], "candidates": 11, "decided": true, "cancelled": false})");
    const auto i = read_identify(identify);
    REQUIRE(i.ranking.size() == 2);
    CHECK(i.ranking[0].flags == "-O2");
    CHECK(i.ranking[0].score.complete());
    CHECK(i.ranking[1].error == "cannot run gcc\nmore");
    CHECK(i.decided);
    // Anything else reads as empty.
    CHECK(read_flag_search(Json::array()).groups.empty());
    CHECK(read_identify(Json()).ranking.empty());
}

TEST_CASE("search view model: the log, and the best so far") {
    std::vector<search::LogEntry> log;
    auto add = [&](search::Score s, bool best) {
        search::LogEntry e;
        e.index = log.size();
        e.ms = static_cast<i64>(log.size()) * 100;
        e.label = std::format("candidate {}", log.size());
        e.score = s;
        e.best = best;
        log.push_back(e);
    };
    add(search::Score{2, 0, 50, 60}, true);
    add(search::Score{2, 0, 70, 50}, false);
    add(search::Score{2, 1, 20, 90}, true);
    add(search::Score{2, 2, 0, 100}, true);
    const auto all = search_log_rows(log, false);
    CHECK(all.size() == 4);
    CHECK(all[1].seconds == doctest::Approx(0.1));
    CHECK_FALSE(all[1].best);
    CHECK(all[3].complete);
    const auto improvements = search_log_rows(log, true);
    REQUIRE(improvements.size() == 3);
    CHECK(improvements[1].index == 2);
    const auto series = best_so_far(log);
    CHECK(series.x == std::vector<double>{0, 1, 2, 3});
    CHECK(series.y == std::vector<double>{60, 60, 90, 100});
}
