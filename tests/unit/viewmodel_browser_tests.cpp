#include "viewmodel/browser.hpp"
#include "viewmodel/function_table.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;
using namespace decomp::vm;
using project::FunctionStatus;

TEST_CASE("browser: the column layout survives JSON, unknown columns are dropped and new ones appended") {
    const auto defaults = default_column_layout();
    REQUIRE(defaults.size() == browser_columns().size());
    CHECK(defaults.front().column == Column::address);
    CHECK_FALSE(std::ranges::find(defaults, Column::name, &ColumnState::column)->visible);  // decorated names are hidden at first

    std::vector<ColumnState> layout = {{Column::best_match, true}, {Column::display, true}, {Column::address, false}, {Column::size, false}};
    const Json saved = column_layout_to_json(layout);
    const auto loaded = column_layout_from_json(saved);
    REQUIRE(loaded.size() == defaults.size());
    CHECK(loaded[0].column == Column::best_match);
    CHECK(loaded[1].column == Column::display);
    CHECK(loaded[2].column == Column::address);
    CHECK(loaded[2].visible);  // the address column cannot be hidden
    CHECK(loaded[3].column == Column::size);
    CHECK_FALSE(loaded[3].visible);
    CHECK(loaded[4].column == Column::name);  // then the columns the saved layout lacks, in default order
    CHECK_FALSE(loaded[4].visible);

    Json odd = Json::array({Json{{"column", "nope"}}, Json{{"column", "size"}, {"visible", false}}, Json{{"column", "size"}}, Json(3)});
    const auto partial = column_layout_from_json(odd);
    CHECK(partial.size() == defaults.size());
    CHECK(partial[0].column == Column::size);
    CHECK(std::ranges::count(partial, Column::size, &ColumnState::column) == 1);
    CHECK(column_layout_from_json(Json("garbage")).size() == defaults.size());
    CHECK(column_layout_from_json(Json::array()).front().column == Column::address);
}

TEST_CASE("browser: filters and sort keys survive JSON") {
    FunctionFilter f;
    f.statuses = {FunctionStatus::nonmatching, FunctionStatus::gave_up};
    f.min_size = 16;
    f.max_size = 4096;
    f.name = "Player::";
    f.unknown_callees = true;
    f.refused = true;
    f.min_best = 50;
    f.best_below = 60;
    const FunctionFilter back = filter_from_json(filter_to_json(f));
    CHECK(back.statuses == f.statuses);
    CHECK(back.min_size == f.min_size);
    CHECK(back.max_size == f.max_size);
    CHECK(back.name == f.name);
    CHECK(back.unknown_callees);
    CHECK(back.refused);
    CHECK(back.min_best == 50);
    CHECK(back.best_below == 60);
    CHECK(filter_to_json({}) == Json::object());
    CHECK(filter_is_empty(filter_from_json(Json::object())));
    CHECK(filter_is_empty(filter_from_json(Json("not an object"))));
    // Invalid values keep their defaults.
    const FunctionFilter bad = filter_from_json(Json{{"statuses", Json::array({"matched", "bogus", 7, "matched"})}, {"min_size", -4}, {"name", 5}});
    CHECK(bad.statuses == std::vector<FunctionStatus>{FunctionStatus::matched});
    CHECK_FALSE(bad.min_size);
    CHECK(bad.name.empty());

    const std::vector<SortKey> keys = {{Column::status, false}, {Column::best_match, true}};
    const auto sort = sort_keys_from_json(sort_keys_to_json(keys));
    REQUIRE(sort.size() == 2);
    CHECK(sort[1].column == Column::best_match);
    CHECK(sort[1].descending);
    // Unknown and repeated columns are dropped.
    CHECK(sort_keys_from_json(Json::array({Json{{"column", "x"}}, Json{{"column", "size"}}, Json{{"column", "size"}, {"descending", true}}})).size() == 1);
    CHECK(sort_keys_from_json(Json::object()).empty());
}

TEST_CASE("browser: navigation anchors carry a filter") {
    FunctionFilter f;
    f.statuses = {FunctionStatus::nonmatching};
    f.min_best = 50;
    f.best_below = 60;
    const std::string anchor = browser_anchor(f);
    CHECK(anchor == "filter:status=nonmatching;best=50-60");
    auto back = browser_filter_from_anchor(anchor);
    REQUIRE(back);
    CHECK(back->statuses == f.statuses);
    CHECK(back->min_best == 50);
    CHECK(back->best_below == 60);

    FunctionFilter g;
    g.statuses = {FunctionStatus::matched, FunctionStatus::library};
    g.max_size = 32;
    g.refused = true;
    g.unknown_callees = true;
    g.min_best = 90;  // the last bin is open-ended
    g.name = "a;b=c";  // the name goes last, so it may hold separators
    back = browser_filter_from_anchor(browser_anchor(g));
    REQUIRE(back);
    CHECK(back->statuses == g.statuses);
    CHECK_FALSE(back->min_size);
    CHECK(back->max_size == 32);
    CHECK(back->refused);
    CHECK(back->unknown_callees);
    CHECK(back->min_best == 90);
    CHECK_FALSE(back->best_below);
    CHECK(back->name == "a;b=c");

    CHECK_FALSE(browser_filter_from_anchor("seq:120"));
    CHECK_FALSE(browser_filter_from_anchor(""));
    CHECK(filter_is_empty(*browser_filter_from_anchor("filter:")));
    CHECK(browser_filter_from_anchor("filter:status=bogus,matched")->statuses == std::vector<FunctionStatus>{FunctionStatus::matched});

    CHECK(describe_filter({}) == "no filter");
    CHECK(describe_filter(f) == "status nonmatching, best 50-60%");
    CHECK(describe_filter(g).find("at most 32 bytes") != std::string::npos);
}

TEST_CASE("browser: the best-match range filter is half-open, like the Dashboard's bins") {
    std::vector<FunctionRow> rows(4);
    const double bests[] = {49.9, 50, 59.99, 60};
    for (usize i = 0; i < rows.size(); ++i) {
        rows[i].va = 0x1000 + i;
        rows[i].best_match = bests[i];
        rows[i].status = FunctionStatus::nonmatching;
    }
    FunctionFilter f;
    f.min_best = 50;
    f.best_below = 60;
    const auto order = filter_and_sort(rows, f, {}).value();
    CHECK(order == std::vector<u32>{1, 2});
    f.best_below.reset();
    CHECK(filter_and_sort(rows, f, {}).value().size() == 3);
}

TEST_CASE("browser: the live overlay digest follows what the overlay shows") {
    test::EventScript run;
    const u64 empty = live_overlay_digest(run.data());
    run.add(events::SessionStarted{"s1", "f", "f", 0x401000}, 0);
    const u64 started = live_overlay_digest(run.data());
    CHECK(started != empty);
    run.add(events::TurnStarted{"s1", 1}, 0);
    CHECK(live_overlay_digest(run.data()) == started);  // a turn changes nothing the table shows
    run.add(events::CompileFinished{"s1", true, false, 10, 0}, 0);
    const u64 compiled = live_overlay_digest(run.data());
    CHECK(compiled != started);
    run.add(events::DiffComputed{"s1", 75, false, "75%"}, 0);
    const u64 diffed = live_overlay_digest(run.data());
    CHECK(diffed != compiled);
    run.add(events::TurnFinished{"s1", 1, "tool_use", {10, 10, 0, 0}, 0.25, 100}, 0);
    const u64 spent = live_overlay_digest(run.data());
    CHECK(spent != diffed);
    run.add(events::SessionFinished{"s1", "matched", "", 100, 1, 0.25}, 0);
    CHECK(live_overlay_digest(run.data()) != spent);
}
