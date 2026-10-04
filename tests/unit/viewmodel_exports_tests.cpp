#include "core/strings.hpp"
#include "formats/coff.hpp"
#include "viewmodel/exports.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

using namespace decomp;
using namespace decomp::vm;
using project::FunctionStatus;

TEST_CASE("exports: RFC 4180 quoting") {
    CHECK(csv_field("plain") == "plain");
    CHECK(csv_field("") == "");
    CHECK(csv_field("a,b") == "\"a,b\"");
    CHECK(csv_field("say \"hi\"") == "\"say \"\"hi\"\"\"");
    CHECK(csv_field("two\nlines") == "\"two\nlines\"");
    CHECK(csv_field("cr\r") == "\"cr\r\"");
    CHECK(csv_field("int __cdecl add(int, int)") == "\"int __cdecl add(int, int)\"");
    const std::vector<std::string> fields = {"a", "b,c", "\"d\""};
    CHECK(csv_record(fields) == "a,\"b,c\",\"\"\"d\"\"\"\r\n");
    CHECK(csv_record(std::vector<std::string>{}) == "\r\n");
}

TEST_CASE("exports: the progress report") {
    test::FixtureProject fx;
    fx.set("add", FunctionStatus::matched, 100, 2, 0.5);
    fx.set("sum_array", FunctionStatus::nonmatching, 91, 4, 1.25);
    ProgressReport report;
    report.project = "fixture";
    report.target = "basic.exe";
    report.generated = parse_iso8601("2026-10-04T12:00:00Z").value();
    report.progress = dashboard_progress(fx.program.symbols(), fx.project);
    RunRecord run;
    run.id = "2026-10-04T10-00-00-0001";
    run.started = parse_iso8601("2026-10-04T10:00:00Z").value();
    run.cost_usd = 1.75;
    run.functions.push_back(RunFunctionRecord{fx.va("add"), "?add@@YAHHH@Z", "add", "matched", true, 100, 3, 1, 0.5, {}});
    run.matched = 1;
    report.history = progress_history({run}, fx.program.symbols());

    const std::string md = progress_markdown(report);
    CHECK(md.starts_with("# Progress: fixture (basic.exe)\n\nGenerated 2026-10-04 12:00 UTC.\n"));
    CHECK(md.find("- Functions matched: 1 of 14 (7.1%)") != std::string::npos);
    CHECK(md.find("| matched | 1 | 15 |") != std::string::npos);
    CHECK(md.find("| 90-100% | 1 |") != std::string::npos);
    CHECK(md.find("## Progress by run") != std::string::npos);
    CHECK(md.find("| 2026-10-04T10-00-00-0001 | 2026-10-04T10:00:00Z | 1 | 1 | 1 | 15 | $1.75 |") != std::string::npos);
    CHECK(md.find("## Progress by day") != std::string::npos);

    const Json j = progress_json(report);
    CHECK(j["status"] == project::to_json(project::compute_progress(fx.program.symbols(), fx.project)));  // `decomp --json status`
    CHECK(j["segments"].size() == kStatusOrder.size());
    CHECK(j["segments"][0]["status"] == "matched");
    CHECK(j["best_match_bins"][9]["functions"] == 1);
    CHECK(j["runs"][0]["total_bytes"] == 15);
    CHECK(j["generated"] == "2026-10-04T12:00:00Z");

    ProgressReport bare;
    bare.project = "p";
    bare.progress = report.progress;
    CHECK(progress_markdown(bare).find("Progress by run") == std::string::npos);
}

TEST_CASE("exports: the cost report") {
    test::FixtureProject fx;
    fx.set("add", FunctionStatus::matched, 100, 2, 0.5);
    RunRecord a;
    a.id = "r1";
    a.started = parse_iso8601("2026-10-04T10:00:00Z").value();
    a.model = "model-a";
    a.effort = "high";
    a.cost_usd = 2.5;
    a.usage = {100, 200, 300, 400};
    a.functions.push_back(RunFunctionRecord{fx.va("add"), "", "add", "matched", true, 100, 3, 1, 2.5, {}});
    a.matched = 1;
    const CostReport report = cost_report({a}, fx.program.symbols(), *fx.project.function_infos());
    const std::string runs = cost_csv(report, CostTable::runs);
    CHECK(runs.starts_with("key,start,runs,cost_usd,functions_worked,functions_matched,success_rate,turns,usd_per_match,turns_per_match,"
                           "input_tokens,output_tokens,cache_write_tokens,cache_read_tokens,cache_hit_rate\r\n"));
    CHECK(runs.find("r1,2026-10-04T10:00:00Z,1,2.5,1,1,1,3,2.5,3,100,200,300,400,0.5\r\n") != std::string::npos);
    CHECK(cost_csv(report, CostTable::models).find("model-a high,") != std::string::npos);
    CHECK(cost_csv(report, CostTable::days).find("2026-10-04,") != std::string::npos);
    const std::string functions = cost_csv(report, CostTable::functions);
    CHECK(functions.find("0x401060,\"int __cdecl add(int, int)\",15,matched,0.5,0,0.5,2\r\n") != std::string::npos);

    const CostProjection projection = project_remaining_cost(fx.program.symbols(), *fx.project.function_infos());
    const Json j = cost_json(report, &projection);
    CHECK(j["total"]["cost_usd"] == 2.5);
    CHECK(j["by_run"][0]["usage"]["cache_read_input_tokens"] == 400);
    CHECK(j["by_function"][0]["va"] == fx.va("add"));
    CHECK(j["projection"]["has_history"] == true);
    CHECK(j["projection"]["remaining"] == projection.remaining);
    CHECK_FALSE(cost_json(report).contains("projection"));
}

TEST_CASE("exports: the function list follows the table's order") {
    test::FixtureProject fx;
    fx.set("add", FunctionStatus::matched, 100, 2, 0.5);
    auto rows = build_function_rows(fx.program.symbols(), *fx.project.function_infos());
    FunctionFilter filter;
    filter.max_size = 15;
    const std::vector<SortKey> sort = {{Column::size, true}};
    const auto order = filter_and_sort(rows, filter, sort).value();
    const std::string csv = function_list_csv(rows, order);
    const auto lines = split_lines(csv);
    REQUIRE(lines.size() == order.size() + 1);
    CHECK(lines[0] == "address,name,display,size,status,best_match,attempts,cost_usd,last_attempt,source,callers,callees,blocks,loops,"
                      "unknown_callees,difficulty");
    CHECK(lines[1] == "0x401060,?add@@YAHHH@Z,\"int __cdecl add(int, int)\",15,matched,100.0,2,0.5,,pdb_public,,,,,,");
    CHECK(csv.find("\r\n") != std::string::npos);

    const Json j = function_list_json(rows, order);
    REQUIRE(j.size() == order.size());
    CHECK(j[0]["va"] == fx.va("add"));
    CHECK(j[0]["callers"].is_null());
    CHECK(j[0]["status"] == "matched");
    rows[order[0]].callers = 2;
    rows[order[0]].difficulty = 4.5;
    CHECK(function_list_json(rows, order)[0]["callers"] == 2);
    CHECK(split_lines(function_list_csv(rows, order))[1].ends_with(",2,,,,,4.50"));
    const std::vector<u32> none;
    CHECK(function_list_json(rows, none).empty());
}

TEST_CASE("exports: a diff is exactly what decomp diff prints") {
    auto program = Program::open(test::fixture("x86/basic.exe")).value();
    auto obj = coff::Object::load(test::fixture("x86/mutated.obj")).value();
    const u64 va = *program.resolve("add");
    const auto diff = matching::diff_function(program, va, obj).value();
    // `decomp diff` defaults: no --compact, --context 3, no --bytes, no colors when not a terminal.
    matching::ReportOptions cli;
    cli.context = 3;
    CHECK(diff_text(diff) == matching::to_text(diff, cli));
    CHECK(diff_json(diff) == dump_pretty(matching::to_json(diff, cli)) + "\n");
    DiffExportOptions compact;
    compact.compact = true;
    compact.context = 1;
    compact.bytes = true;
    cli.compact = true;
    cli.context = 1;
    cli.bytes = true;
    CHECK(diff_text(diff, compact) == matching::to_text(diff, cli));
    CHECK(diff_text(diff, compact).find("\x1b[") == std::string::npos);
    CHECK(parse_json(diff_json(diff, compact)).value()["byte_exact"] == diff.byte_exact);
}
