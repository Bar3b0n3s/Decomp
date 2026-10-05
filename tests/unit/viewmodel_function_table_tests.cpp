#include "analysis/difficulty.hpp"
#include "viewmodel/function_table.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>

using namespace decomp;
using namespace decomp::vm;
using project::FunctionStatus;

namespace {

std::vector<std::string> names_in(const std::vector<FunctionRow>& rows, const std::vector<u32>& order) {
    std::vector<std::string> out;
    for (u32 i : order) out.push_back(rows[i].display);
    return out;
}

const FunctionRow& row_of(const std::vector<FunctionRow>& rows, u64 va) {
    return *std::ranges::find(rows, va, &FunctionRow::va);
}

double elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

} // namespace

TEST_CASE("function table: rows with the stored state and the live overlay") {
    test::FixtureProject fx;
    fx.set("add", FunctionStatus::matched, 100, 3, 0.5);
    fx.set("sum_array", FunctionStatus::nonmatching, 60, 2, 1.0);
    fx.set("mix", FunctionStatus::refused, 0, 0, 0.1);
    const auto& symbols = fx.program.symbols();
    const auto infos = fx.project.function_infos();

    const auto rows = build_function_rows(symbols, *infos);
    CHECK(rows.size() == symbols.functions().size());
    CHECK(std::ranges::is_sorted(rows, {}, &FunctionRow::va));
    const FunctionRow& add = row_of(rows, fx.va("add"));
    CHECK(add.name == "?add@@YAHHH@Z");
    CHECK(add.display == "int __cdecl add(int, int)");
    CHECK(add.size == 15);
    CHECK(add.status == FunctionStatus::matched);
    CHECK(add.attempts == 3);
    CHECK(add.source == SymbolSource::pdb_public);
    CHECK_FALSE(add.callers);
    CHECK_FALSE(add.last_attempt);
    const FunctionRow& helper = row_of(rows, fx.va("helper"));
    CHECK(helper.is_static);
    CHECK(helper.display == "helper");

    test::EventScript run;
    run.add(events::SessionStarted{"s-sum", "?sum_array@@YAHPBHH@Z", "sum_array", fx.va("sum_array")}, 0);
    run.add(events::TurnFinished{"s-sum", 1, "tool_use", {10, 10, 0, 0}, 0.25, 100}, 0);
    run.add(events::CompileFinished{"s-sum", true, false, 10, 0}, 0);
    run.add(events::DiffComputed{"s-sum", 75, false, "75%"}, 0);
    run.add(events::SessionStarted{"s-add", "?add@@YAHHH@Z", "add", fx.va("add")}, 1);
    const auto live = build_function_rows(symbols, *infos, &run.data());
    const FunctionRow& sum = row_of(live, fx.va("sum_array"));
    CHECK(sum.status == FunctionStatus::in_progress);
    CHECK(sum.stored_status == FunctionStatus::nonmatching);
    CHECK(sum.best_match == 75);
    CHECK(sum.attempts == 3);
    CHECK(sum.cost_usd == doctest::Approx(1.25));
    CHECK(sum.session == "s-sum");
    CHECK(row_of(live, fx.va("add")).status == FunctionStatus::matched);  // a running session does not unmatch it
}

TEST_CASE("function table: analysis columns and last attempts") {
    test::FixtureProject fx;
    const auto& symbols = fx.program.symbols();
    auto rows = build_function_rows(symbols, *fx.project.function_infos());
    std::vector<u64> vas;
    for (const auto& r : rows) vas.push_back(r.va);
    apply_analysis(rows, analyze_functions(fx.program, vas));
    const FunctionRow& dispatch = row_of(rows, fx.va("dispatch"));
    REQUIRE(dispatch.blocks);
    CHECK(*dispatch.callees >= 3);
    CHECK(*dispatch.callers >= 1);
    CHECK(*dispatch.unknown_callees == 0);
    CHECK(dispatch.difficulty);
    CHECK(*row_of(rows, fx.va("sum_array")).loops >= 1);

    // attempts.jsonl: the time of the last line (only the end of the file is read).
    const Symbol& add = *symbols.at(fx.va("add"));
    CHECK_FALSE(last_attempt_time(fx.project, add));
    std::string big(100'000, 'x');
    REQUIRE(fx.project.record_attempt(add, Json{{"time", "2026-10-04T10:00:00.000Z"}, {"source", big}}));
    REQUIRE(fx.project.record_attempt(add, Json{{"time", "2026-10-04T11:30:00.500Z"}, {"source", big}}));
    CHECK(last_attempt_time(fx.project, add) == parse_iso8601("2026-10-04T11:30:00.500Z"));
    fx.set("add", FunctionStatus::nonmatching, 50, 2, 0.1);
    rows = build_function_rows(symbols, *fx.project.function_infos());
    fill_last_attempts(rows, fx.project, symbols);
    CHECK(row_of(rows, fx.va("add")).last_attempt == parse_iso8601("2026-10-04T11:30:00.500Z"));
    CHECK_FALSE(row_of(rows, fx.va("dispatch")).last_attempt);

    // Cost per function: a last attempt with a 100 KB source, then a typical one of 2 KB.
    for (usize source : {usize{100'000}, usize{2'000}}) {
        REQUIRE(fx.project.record_attempt(add, Json{{"time", "2026-10-04T12:00:00Z"}, {"source", std::string(source, 'y')}}));
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 200; ++i) REQUIRE(last_attempt_time(fx.project, add));
        MESSAGE("last_attempt_time: " << elapsed_ms(start) * 1000 / 200 << " us with a " << source / 1000 << " KB source");
    }
    // A record written by hand (spaces after the colons) is parsed as JSON.
    REQUIRE(fs::append_text(fx.project.function_dir(add) / "attempts.jsonl", "{\"source\": \"x\", \"time\": \"2026-10-05T08:00:00Z\"}\n"));
    CHECK(last_attempt_time(fx.project, add) == parse_iso8601("2026-10-05T08:00:00Z"));
}

TEST_CASE("function table: filters") {
    test::FixtureProject fx;
    fx.set("add", FunctionStatus::matched, 100);
    fx.set("sum_array", FunctionStatus::nonmatching, 60);
    fx.set("mix", FunctionStatus::refused);
    auto rows = build_function_rows(fx.program.symbols(), *fx.project.function_infos());
    auto run = [&](const FunctionFilter& f, std::vector<SortKey> sort = {}) { return filter_and_sort(rows, f, sort).value(); };

    CHECK(run({}).size() == rows.size());
    FunctionFilter matched;
    matched.statuses = {FunctionStatus::matched, FunctionStatus::refused};
    CHECK(names_in(rows, run(matched)) == std::vector<std::string>{"int __cdecl add(int, int)", "double __cdecl mix(double, double)"});
    FunctionFilter refused;
    refused.refused = true;
    CHECK(run(refused).size() == 1);
    FunctionFilter sized;
    sized.min_size = 15;
    sized.max_size = 30;
    for (u32 i : run(sized)) CHECK((rows[i].size >= 15 && rows[i].size <= 30));
    CHECK(run(sized).size() == 6);  // Player::Hit 30, add 15, scale 17, mix 23, exported_api 21, other_value 30

    FunctionFilter text;
    text.name = "PLAYER::";  // plain text: a substring, ignoring case, in the readable name
    CHECK(run(text).size() == 2);
    text.name = "add@@";  // in the decorated name
    CHECK(names_in(rows, run(text)) == std::vector<std::string>{"int __cdecl add(int, int)"});
    text.name = "^(add|mix)$";
    CHECK(run(text).empty());  // anchored against the full names
    text.name = "\\b(add|mix)\\(";
    CHECK(run(text).size() == 2);
    text.name = "[unclosed";
    CHECK_FALSE(filter_and_sort(rows, text, {}));

    FunctionFilter unknown;
    unknown.unknown_callees = true;
    CHECK(run(unknown).empty());  // not analyzed yet
    rows[0].unknown_callees = 2;
    CHECK(run(unknown).size() == 1);

    int polls = 0;
    CHECK_FALSE(filter_and_sort(rows, {}, {}, [&] { return ++polls > 0; }));
}

TEST_CASE("function table: multi-column sort, missing values last, deterministic ties") {
    std::vector<FunctionRow> rows(5);
    const char* names[] = {"beta", "Alpha", "gamma", "alpha", "Delta"};
    const u32 sizes[] = {10, 30, 10, 20, 30};
    for (usize i = 0; i < rows.size(); ++i) {
        rows[i].va = 0x1000 + i * 0x10;
        rows[i].display = names[i];
        rows[i].name = names[i];
        rows[i].size = sizes[i];
    }
    rows[0].status = FunctionStatus::unstarted;
    rows[1].status = FunctionStatus::matched;
    rows[2].status = FunctionStatus::nonmatching;
    rows[3].status = FunctionStatus::library;
    rows[4].status = FunctionStatus::matched;
    rows[2].callers = 5;
    rows[4].callers = 1;
    auto order = [&](std::vector<SortKey> keys) { return names_in(rows, filter_and_sort(rows, {}, keys).value()); };

    CHECK(order({}) == std::vector<std::string>{"beta", "Alpha", "gamma", "alpha", "Delta"});  // by address
    CHECK(order({{Column::display, false}}) == std::vector<std::string>{"Alpha", "alpha", "beta", "Delta", "gamma"});  // case ignored, ties by address
    CHECK(order({{Column::size, true}, {Column::display, false}}) == std::vector<std::string>{"Alpha", "Delta", "alpha", "beta", "gamma"});
    CHECK(order({{Column::status, false}}) == std::vector<std::string>{"Alpha", "Delta", "alpha", "gamma", "beta"});  // dashboard order
    CHECK(order({{Column::callers, false}}) == std::vector<std::string>{"Delta", "gamma", "beta", "Alpha", "alpha"});
    CHECK(order({{Column::callers, true}}) == std::vector<std::string>{"gamma", "Delta", "beta", "Alpha", "alpha"});  // missing still last
    CHECK(order({{Column::address, true}}) == std::vector<std::string>{"Delta", "alpha", "gamma", "Alpha", "beta"});

    CHECK(to_string(Column::unknown_callees) == "unknown_callees");
    CHECK(column_from_string("best_match") == Column::best_match);
    CHECK_FALSE(column_from_string("nope"));
}

TEST_CASE("function table: 100,000 rows") {
    SymbolDb symbols;
    std::map<u64, project::FunctionInfo> infos;
    u32 state = 99;
    for (u64 i = 0; i < 100'000; ++i) {
        state = state * 1664525u + 1013904223u;
        const u64 va = 0x10000000 + i * 64;
        const std::string name = std::format("?function_{}@Namespace{}@@QAEXH@Z", state % 100'000, i % 97);
        symbols.add(Symbol{va, name, std::format("void Namespace{}::function_{}(int)", i % 97, state % 100'000), "", SymbolKind::function,
                           4 + (state >> 16) % 4000, SymbolSource::pdb_public, false, {}});
        if (i % 3 == 0)
            infos[va] = project::FunctionInfo{i % 6 == 0 ? FunctionStatus::matched : FunctionStatus::nonmatching, static_cast<double>(state % 100),
                                              static_cast<int>(state % 7), (state % 1000) / 100.0};
    }
    auto start = std::chrono::steady_clock::now();
    const auto rows = build_function_rows(symbols, infos);
    const double build = elapsed_ms(start);
    REQUIRE(rows.size() == 100'000);

    FunctionFilter by_status;
    by_status.statuses = {FunctionStatus::nonmatching, FunctionStatus::unstarted};
    start = std::chrono::steady_clock::now();
    const auto status_only = filter_and_sort(rows, by_status, {}).value();
    const double status_ms = elapsed_ms(start);
    CHECK(status_only.size() > 50'000);

    FunctionFilter substring;
    substring.name = "namespace42::";
    start = std::chrono::steady_clock::now();
    const auto sub = filter_and_sort(rows, substring, {}).value();
    const double substring_ms = elapsed_ms(start);
    CHECK_FALSE(sub.empty());

    FunctionFilter regex;
    regex.name = "function_1[0-9]+\\(";
    start = std::chrono::steady_clock::now();
    const auto re = filter_and_sort(rows, regex, {}).value();
    const double regex_ms = elapsed_ms(start);
    CHECK_FALSE(re.empty());

    const std::vector<SortKey> by_size = {{Column::size, true}};
    start = std::chrono::steady_clock::now();
    const auto sized = filter_and_sort(rows, {}, by_size).value();
    const double size_ms = elapsed_ms(start);
    for (usize i = 1; i < sized.size(); ++i) REQUIRE(rows[sized[i - 1]].size >= rows[sized[i]].size);

    const std::vector<SortKey> by_name = {{Column::display, false}};
    start = std::chrono::steady_clock::now();
    const auto named = filter_and_sort(rows, {}, by_name).value();
    const double name_ms = elapsed_ms(start);
    CHECK(named.size() == rows.size());

    const std::vector<SortKey> two = {{Column::status, false}, {Column::best_match, true}};
    start = std::chrono::steady_clock::now();
    const auto multi = filter_and_sort(rows, {}, two).value();
    const double multi_ms = elapsed_ms(start);
    CHECK(multi.size() == rows.size());

    MESSAGE("100,000 rows: build " << build << " ms; filter by status " << status_ms << " ms, substring " << substring_ms << " ms, regex "
                                   << regex_ms << " ms; sort by size " << size_ms << " ms, by name " << name_ms << " ms, by status and best "
                                   << multi_ms << " ms");
#ifdef NDEBUG
    // Generous bounds (10x or more what a CI machine needs), so only a real regression fails.
    CHECK(build < 2000);
    CHECK(status_ms < 1000);
    CHECK(substring_ms < 2000);
    CHECK(name_ms < 3000);
    CHECK(multi_ms < 3000);
#endif
}
