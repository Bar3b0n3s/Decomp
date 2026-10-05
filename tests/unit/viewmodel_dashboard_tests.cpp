#include "viewmodel/dashboard.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

using namespace decomp;
using namespace decomp::vm;

TEST_CASE("dashboard: the target's identity") {
    test::FixtureProject fx;
    const project::TargetStatus status = fx.project.target_status(fx.program);
    const TargetIdentity t = target_identity(fx.program, &status);
    CHECK(t.path.ends_with("basic.exe"));
    CHECK(t.path.find("..") == std::string::npos);  // resolved
    CHECK(t.file_size == fx.program.image().data().size());
    CHECK(t.sha1.size() == 40);
    CHECK(t.sha1 == fx.project.config().target_sha1);
    CHECK(t.sha1_ok);
    CHECK(t.format == "PE32");
    CHECK(t.arch == "x86");
    CHECK_FALSE(t.dll);
    CHECK(t.image_base == 0x400000);
    CHECK(t.entry_point == 0x4011e0);
    CHECK(t.entry_name == "entry");
    CHECK(t.linker_version == "14.00");
    CHECK(t.rich.empty());  // lld-link writes no Rich header
    CHECK(t.pdb == PdbStatus::matched);
    CHECK(t.pdb_path.ends_with("basic.pdb"));
    CHECK(t.has_codeview);
    CHECK(t.codeview_path == "basic.pdb");
    CHECK(t.guid == "{D0294188-ED72-2A3D-4C4C-44205044422E}");
    CHECK(t.age == 1);

    // A binary that changed since init: the mismatch is reported (the Dashboard shows it in red).
    project::TargetStatus changed = status;
    changed.expected_sha1 = std::string(40, '0');
    changed.sha1_ok = false;
    const TargetIdentity m = target_identity(fx.program, &changed);
    CHECK_FALSE(m.sha1_ok);
    CHECK(m.expected_sha1 == std::string(40, '0'));
    // Without a status the SHA-1 is computed here.
    CHECK(target_identity(fx.program, nullptr).sha1 == t.sha1);

    CHECK(pdb_status_text(PdbStatus::matched).find("matching GUID and age") != std::string::npos);
    CHECK(pdb_status_text(PdbStatus::absent) == "absent");
}

TEST_CASE("dashboard: Rich header entries by role") {
    const std::vector<pe::RichEntry> entries = {{0x0001, 0, 30}, {0x0004, 8447, 1}, {0x000B, 8168, 12}, {0x0200, 1, 2}, {0x000A, 8168, 3}};
    const auto builds = rich_builds(entries);
    REQUIRE(builds.size() == 5);
    CHECK(builds[0].role == RichBuild::Role::compiler);
    CHECK(builds[0].description == "C++ compiler 12.00 (Visual C++ 6.0)");
    CHECK(builds[0].version == "12.00.8168");
    CHECK(builds[0].visual_studio == "Visual C++ 6.0");
    CHECK(builds[0].text == "C++ compiler 12.00.8168 (Visual C++ 6.0)");
    CHECK(builds[0].build == 8168);
    CHECK(builds[0].count == 12);
    CHECK(builds[1].description == "C compiler 12.00 (Visual C++ 6.0)");
    CHECK(builds[2].role == RichBuild::Role::linker);
    CHECK(builds[2].description == "linker 6.00 (Visual C++ 6.0)");
    CHECK(builds[3].role == RichBuild::Role::other);  // import entries
    CHECK(builds[3].description == "imported functions");
    CHECK(builds[3].version.empty());
    CHECK(builds[4].description == "product 0x0200");
    CHECK(builds[4].text == "product 0x0200 build 1");

    // Visual Studio 2015 and later: the image's linker version (14.xx) gives the minor version of the
    // tools that share the linker's build.
    const std::vector<pe::RichEntry> modern = {{0x0105, 36231, 40}, {0x0102, 36231, 1}, {0x0105, 30159, 3}};
    const auto v = rich_builds(modern, 14, 51);
    CHECK(v[0].version == "19.51.36231");
    CHECK(v[0].visual_studio == "Visual Studio 2026");
    CHECK(v[1].version == "19.29.30159");
    CHECK(v[1].visual_studio == "Visual Studio 2019 16.11");
    CHECK(v[2].version == "14.51.36231");
}

TEST_CASE("dashboard: spend of the shown run and of every run") {
    test::EventScript run;
    run.add(events::RunStarted{"/p", "claude-opus-5-5", "high", 2, {}}, -1);
    run.add(events::SessionStarted{"s1", "?add@@YAHHH@Z", "add", 0x401060}, 0);
    run.add(events::TurnFinished{"s1", 1, "tool_use", {1000, 200, 300, 2700}, 0.5, 100}, 0);
    run.add(events::SessionFinished{"s1", "matched", "", 100, 1, 0.5}, 0);
    const SpendSummary s = run_spend(run.data());
    CHECK(s.usd == doctest::Approx(0.5));
    CHECK(s.usage.cache_read == 2700);
    CHECK(s.matched == 1);
    CHECK(s.cache_hit_rate() == doctest::Approx(2700.0 / 4000.0));
    CHECK(s.usd_per_match() == doctest::Approx(0.5));

    std::vector<RunRecord> runs(2);
    runs[0].cost_usd = 1.0;
    runs[0].matched = 2;
    runs[0].usage = {100, 10, 0, 300};
    runs[1].cost_usd = 3.0;
    runs[1].usage = {100, 10, 100, 0};
    // All runs as cost.hpp counts them.
    const SpendSummary all = slice_spend(cost_report(runs, SymbolDb{}, {}).total);
    CHECK(all.usd == doctest::Approx(4.0));
    CHECK(all.runs == 2);
    CHECK(all.matched == 2);
    CHECK(all.usd_per_match() == doctest::Approx(2.0));
    CHECK(all.cache_hit_rate() == doctest::Approx(300.0 / 600.0));
    CHECK(SpendSummary{}.usd_per_match() == 0);
    CHECK(SpendSummary{}.cache_hit_rate() == 0);
}

TEST_CASE("dashboard: recent activity from the shown run and the other runs") {
    test::EventScript run;
    run.at(0);
    run.add(events::RunStarted{"/p", "m", "high", 2, {}}, -1);
    run.add(events::SessionStarted{"s-add", "?add@@YAHHH@Z", "int __cdecl add(int, int)", 0x401060}, 0);
    run.add(events::SessionStarted{"s-mix", "?mix@@YANNN@Z", "double __cdecl mix(double, double)", 0x4011a0}, 1);
    run.add(events::SessionStarted{"s-hit", "?Hit@Player@@QAEXH@Z", "Hit", 0x401000}, 1);
    run.at(10);
    run.add(events::SessionFinished{"s-add", "matched", "", 100, 2, 0.1}, 0);
    run.at(20);
    run.add(events::Refusal{"s-mix", "cyber", "declined"}, 1);
    run.add(events::SessionFinished{"s-mix", "refused", "", 0, 1, 0.1}, 1);
    run.at(30);
    run.add(events::ToolCallStarted{"s-hit", "t1", "compile_and_diff", 1, Json::object()}, 1);
    run.add(events::ToolCallFinished{"s-hit", "t1", "compile_and_diff", true, "compile failed", 10}, 1);
    run.at(40);
    run.add(events::SessionFinished{"s-hit", "stopped", "", 0, 1, 0.1}, 1);  // not notable

    std::vector<RunRecord> others(1);
    others[0].id = "2026-10-03T10-00-00-0000";
    others[0].started = run.t0 - std::chrono::hours(24);
    others[0].finished = run.t0 - std::chrono::hours(23);
    auto function = [](u64 va, std::string name, std::string outcome) {
        RunFunctionRecord f;
        f.va = va;
        f.function = f.display = std::move(name);
        f.matched = outcome == "matched";
        f.outcome = std::move(outcome);
        return f;
    };
    others[0].functions.push_back(function(0x401080, "sum_array", "gave_up"));
    others[0].functions.push_back(function(0x401070, "read_counter", "matched"));
    others[0].functions.push_back(function(0x4010f0, "dispatch", "max_turns"));

    const auto items = recent_activity(&run.data(), others);
    REQUIRE(items.size() == 5);
    CHECK(items[0].kind == ActivityKind::error);  // the tool error, newest
    CHECK(items[0].va == 0x401000);
    CHECK(items[0].session == "s-hit");
    CHECK(items[1].kind == ActivityKind::refused);
    CHECK(items[1].detail == "category: cyber");
    CHECK(items[2].kind == ActivityKind::matched);
    CHECK(items[2].function == "int __cdecl add(int, int)");
    CHECK(items[2].run == run.run);
    CHECK(items[3].run == others[0].id);  // the other run's outcomes, at its end
    CHECK(items[3].time == others[0].finished);
    CHECK(to_string(items[3].kind) == "gave up");
    CHECK(items[4].kind == ActivityKind::matched);
    CHECK(recent_activity(&run.data(), others, 2).size() == 2);
    // The shown run is not counted twice when its summary is among the records.
    std::vector<RunRecord> with_shown = others;
    with_shown.push_back(run_record_from_state(run.data()));
    CHECK(recent_activity(&run.data(), with_shown).size() == 5);
    CHECK(recent_activity(nullptr, others).size() == 2);
}
