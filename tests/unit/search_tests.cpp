// The search core (search/evaluate.hpp, search/runs.hpp, search/probes.hpp): candidates compiled and
// scored against the target, in parallel, and search runs as a project keeps them.

#include "analysis/program.hpp"
#include "core/fs.hpp"
#include "llvm_fixture.hpp"
#include "search/evaluate.hpp"
#include "search/flags.hpp"
#include "search/probes.hpp"
#include "search/runs.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <thread>

using namespace decomp;
using namespace decomp::search;

TEST_CASE("search scores: byte-exact functions first, then less distance") {
    matching::FunctionDiff exact;
    exact.byte_exact = true;
    exact.exact = true;
    exact.match_percent = 100;
    CHECK(distance_of(exact) == 0);
    matching::FunctionDiff near;
    near.exact = true;
    near.match_percent = 100;
    CHECK(distance_of(near) == 1);  // equivalent instructions, other bytes
    matching::FunctionDiff far;
    far.inserted = 2;
    far.opcode = 1;
    far.operand = 3;
    far.encoding = 1;
    far.match_percent = 60;
    CHECK(distance_of(far) == 20 + 6 + 9 + 1);

    const std::vector<FunctionScore> a = {score_of(1, exact), score_of(2, far)};
    const std::vector<FunctionScore> b = {score_of(1, near), score_of(2, near)};
    const Score sa = total(a), sb = total(b);
    CHECK(sa.exact == 1);
    CHECK(sa.distance == 36);
    CHECK(sa.match_percent == doctest::Approx(80));
    CHECK(sb.exact == 0);
    // More byte-exact functions wins over a smaller distance.
    CHECK(sa.better_than(sb));
    CHECK_FALSE(sb.better_than(sa));
    CHECK_FALSE(sa.better_than(sa));
    CHECK(total(std::vector<FunctionScore>{score_of(1, exact)}).complete());
    CHECK_FALSE(sa.complete());
    CHECK(score_from_json(to_json(sa)) == sa);
    CHECK(sa.text() == "1/2 byte-exact, distance 36, 80.0%");
}

TEST_CASE("search: parallel_for runs every job once, and stops starting jobs when cancelled") {
    std::vector<std::atomic<int>> runs(100);
    parallel_for(runs.size(), 4, [&](usize i) { runs[i].fetch_add(1); });
    CHECK(std::ranges::all_of(runs, [](const std::atomic<int>& r) { return r.load() == 1; }));
    std::atomic<int> done{0};
    std::atomic<bool> stop{false};
    parallel_for(1000, 3, [&](usize) {
        if (done.fetch_add(1) + 1 >= 10) stop = true;
    }, [&] { return stop.load(); });
    CHECK(done.load() >= 10);
    CHECK(done.load() < 1000);
    // No jobs, or more threads than jobs.
    parallel_for(0, 8, [&](usize) { CHECK(false); });
    std::atomic<int> one{0};
    parallel_for(1, 8, [&](usize) { one.fetch_add(1); });
    CHECK(one.load() == 1);
}

TEST_CASE("search runs: written as they go, listed and read back") {
    auto tmp = fs::TempDir::create("decomp-search-runs").value();
    Json settings{{"groups", Json::array({"/Od|/O2"})}};
    auto writer = RunWriter::create(tmp.path(), SearchKind::flags, "add", {0x401000}, settings).value();
    CHECK(writer->record().id.find("-flags-") != std::string::npos);
    // A running run is listed already.
    auto listed = list_runs(tmp.path());
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].status == RunStatus::running);
    std::vector<LogEntry> seen;
    CandidateLog log(writer.get(), [&](const LogEntry& e) { seen.push_back(e); });
    CHECK(log.add("/Od", Score{1, 0, 40, 70}).best);
    CHECK_FALSE(log.add("/O1", Score{1, 0, 50, 60}).best);
    CHECK(log.add("/O2", Score{1, 1, 0, 100}).best);
    CHECK(log.count() == 3);
    CHECK(log.best()->exact == 1);
    REQUIRE(seen.size() == 3);
    CHECK(seen[2].index == 2);
    REQUIRE(writer->write_file("note.txt", "hello"));
    REQUIRE(writer->finish(RunStatus::done, Json{{"flags", Json::array({"/O2"})}}));

    auto run = load_run(writer->dir()).value();
    CHECK(run.kind == SearchKind::flags);
    CHECK(run.target == "add");
    CHECK(run.functions == std::vector<u64>{0x401000});
    CHECK(run.status == RunStatus::done);
    CHECK(run.candidates == 3);
    REQUIRE(run.best);
    CHECK(run.best->exact == 1);
    CHECK(run.best_label == "/O2");
    CHECK(run.settings == settings);
    CHECK(run.result["flags"][0] == "/O2");
    const auto logged = load_log(writer->dir());
    REQUIRE(logged.size() == 3);
    CHECK(logged[0].label == "/Od");
    CHECK(logged[0].best);
    CHECK_FALSE(logged[1].best);
    CHECK(logged[2].best);
    CHECK(logged[2].index == 2);
    CHECK(logged[2].score == Score{1, 1, 0, 100});
    CHECK(fs::read_text(writer->dir() / "note.txt").value() == "hello");

    // Newest first (by the millisecond they started); something that is not a run is skipped.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto other = RunWriter::create(tmp.path(), SearchKind::permute, "sum_array", {}, Json::object()).value();
    REQUIRE(other->finish(RunStatus::cancelled, Json::object()));
    REQUIRE(fs::create_directories(tmp.path() / "zzz-not-a-run"));
    listed = list_runs(tmp.path());
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].id == other->record().id);
    CHECK(listed[1].id == run.id);
}

TEST_CASE("search: a configuration evaluated on the fixture's source") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl not found; skipping");
        return;
    }
    auto tmp = fs::TempDir::create("decomp-search-evaluate").value();
    // Built with the installed LLVM, which compiles the candidates too.
    const auto exe = test::build_fixture_program(Arch::x86, *tools, tmp.path() / "target");
    REQUIRE(exe);
    auto program = Program::open(*exe).value();
    auto probe = file_probe(program, test::fixture("src/basic.cpp")).value();
    CHECK(probe.file_name == "basic.cpp");
    // The functions basic.cpp defines, and nothing other.cpp does.
    CHECK(std::ranges::find(probe.functions, program.resolve("?add@@YAHHH@Z").value()) != probe.functions.end());
    CHECK(std::ranges::find(probe.functions, program.resolve("?Hit@Player@@QAEXH@Z").value()) != probe.functions.end());
    CHECK(std::ranges::find(probe.functions, program.resolve("?other_value@@YAHH@Z").value()) == probe.functions.end());
    CHECK(std::ranges::is_sorted(probe.functions));

    const auto setup = test::clang_setup(Arch::x86, tools->clang_cl, tmp.path() / "work", tmp.path() / "cache");
    Configuration fixture{setup.toolchain, test::fixture_flags()};
    CHECK(fixture.label() == "clang-cl-x86 /O2 /Gy /GS- /GR- /EHs-c-");
    const std::vector<Probe> probes = {probe};
    const auto good = evaluate(program, setup, fixture, probes);
    CHECK(good.compile_error.empty());
    CHECK(good.score.complete());
    CHECK(good.score.functions == probe.functions.size());
    Configuration unoptimized{setup.toolchain, {"/Od", "/GS-", "/GR-", "/EHs-c-"}};
    const auto bad = evaluate(program, setup, unoptimized, probes);
    CHECK(bad.score.exact < bad.score.functions);
    CHECK(good.score.better_than(bad.score));
    // A configuration that does not compile scores every function as missing.
    Configuration broken{setup.toolchain, {"/O2", "/DNOT_A_TYPE=+"}};
    std::vector<Probe> broken_probe = {Probe{"NOT_A_TYPE f() { return 0; }\n", "broken.cpp", {probe.functions[0]}}};
    const auto failed = evaluate(program, setup, broken, broken_probe);
    CHECK_FALSE(failed.compile_error.empty());
    REQUIRE(failed.functions.size() == 1);
    CHECK_FALSE(failed.functions[0].found);
    CHECK(failed.functions[0].distance == kMissingDistance);
}

TEST_CASE("flag groups: parsed, presets, and the flags a choice makes") {
    auto g = parse_flag_group("/Od | /O1 | /O2").value();
    CHECK(g.name.empty());
    CHECK(g.alternatives == std::vector<std::vector<std::string>>{{"/Od"}, {"/O1"}, {"/O2"}});
    g = parse_flag_group("  frame pointers: none |/Oy- ").value();
    CHECK(g.name == "frame pointers");
    CHECK(g.alternatives == std::vector<std::vector<std::string>>{{}, {"/Oy-"}});
    CHECK(to_string(g) == "frame pointers: none | /Oy-");
    g = parse_flag_group("/fp:fast | | -O1 -fno-inline").value();
    CHECK(g.name.empty());
    CHECK(g.alternatives == std::vector<std::vector<std::string>>{{"/fp:fast"}, {}, {"-O1", "-fno-inline"}});
    CHECK(to_string(g) == "/fp:fast | none | -O1 -fno-inline");
    CHECK_FALSE(parse_flag_group("/O2"));
    CHECK_FALSE(parse_flag_group("/O2 | /O2"));

    const std::vector<FlagGroup> groups = {parse_flag_group("opt: /Od | /O1 | /O2").value(), parse_flag_group("none | /Oy-").value(),
                                           parse_flag_group("none | /GS-").value()};
    const std::vector<std::string> start = {"/O2", "/Gy", "/GS-", "/GR-", "-Oy-"};
    CHECK(base_flags(start, groups, true) == std::vector<std::string>{"/Gy", "/GR-"});
    const auto choice = choice_of(start, groups, true);
    CHECK(choice == std::vector<usize>{2, 1, 1});
    CHECK(flags_of(base_flags(start, groups, true), groups, choice) == std::vector<std::string>{"/Gy", "/GR-", "/O2", "/Oy-", "/GS-"});
    CHECK(choice_label(groups, choice) == "/O2 /Oy- /GS-");
    CHECK(choice_label(groups, std::vector<usize>{0, 0, 0}) == "/Od");
    // GCC-style flags are not MSVC-style: "-Oy-" is not "/Oy-" there.
    CHECK(choice_of(start, groups, false) == std::vector<usize>{2, 0, 1});
    // The last one given wins; a group none of whose flags is given takes its empty alternative, else
    // its first.
    CHECK(choice_of(std::vector<std::string>{"/O2", "/Od"}, groups, true) == std::vector<usize>{0, 0, 0});
    CHECK(choice_of(std::vector<std::string>{}, groups, true) == std::vector<usize>{0, 0, 0});

    auto msvc86 = preset_groups("common", matching::ToolchainKind::msvc, Arch::x86).value();
    REQUIRE(msvc86.size() >= 4);
    CHECK(msvc86[0].name == "optimization");
    CHECK(std::ranges::any_of(msvc86, [](const FlagGroup& f) { return f.name == "frame pointers"; }));
    auto msvc64 = preset_groups("common", matching::ToolchainKind::clang_cl, Arch::x64).value();
    CHECK_FALSE(std::ranges::any_of(msvc64, [](const FlagGroup& f) { return f.name == "frame pointers"; }));
    CHECK(preset_groups("full", matching::ToolchainKind::msvc, Arch::x64).value().size() > msvc64.size());
    auto gcc = preset_groups("common", matching::ToolchainKind::gcc, Arch::x64).value();
    CHECK(gcc[0].alternatives[3] == std::vector<std::string>{"-O2"});
    CHECK(preset_groups("none", matching::ToolchainKind::gcc, Arch::x64).value().empty());
    CHECK_FALSE(preset_groups("most", matching::ToolchainKind::gcc, Arch::x64));
}

TEST_CASE("flag search recovers the fixture's flags from a candidate set") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl not found; skipping");
        return;
    }
    auto tmp = fs::TempDir::create("decomp-search-flags").value();
    const auto exe = test::build_fixture_program(Arch::x86, *tools, tmp.path() / "target");
    REQUIRE(exe);
    auto program = Program::open(*exe).value();
    const std::vector<Probe> probes = {file_probe(program, test::fixture("src/basic.cpp")).value()};
    const auto setup = test::clang_setup(Arch::x86, tools->clang_cl, tmp.path() / "work", tmp.path() / "cache");

    FlagSearchOptions options;
    // Starting from unoptimized code with security checks.
    options.start = {"/Od", "/Gy", "/GR-", "/EHs-c-"};
    std::vector<std::string> labels;
    CandidateLog log(nullptr, [&](const LogEntry& e) { labels.push_back(e.label); });
    options.log = &log;

    SUBCASE("every combination of a small set") {
        options.groups = {parse_flag_group("optimization: /Od | /O1 | /O2").value(), parse_flag_group("security checks: none | /GS-").value()};
        const auto r = search_flags(program, setup, probes, options);
        CHECK(r.exhaustive);
        CHECK(r.space == 6);
        CHECK(r.candidates == 6);
        CHECK(log.count() == 6);
        CHECK(labels.front() == "/Od");  // the start first
        CHECK(r.start_choice == std::vector<usize>{0, 0});
        CHECK(r.choice == std::vector<usize>{2, 1});
        CHECK(r.flags == std::vector<std::string>{"/Gy", "/GR-", "/EHs-c-", "/O2", "/GS-"});
        CHECK(r.score.complete());
        CHECK_FALSE(r.start_score.complete());
        CHECK(r.equivalent == std::vector<std::vector<usize>>{{2}, {1}});
        const auto j = to_json(r, options.groups);
        CHECK(j["groups"][0]["chosen"] == 2);
        CHECK(j["groups"][1]["alternatives"][1] == "/GS-");
    }
    SUBCASE("local search over the common groups") {
        options.groups = preset_groups("common", matching::ToolchainKind::clang_cl, Arch::x86).value();
        // /Ox makes the same code as /O2 here: leave it out to see the level decided.
        options.groups[0] = parse_flag_group("optimization: /Od | /O1 | /O2").value();
        options.exhaustive_limit = 16;
        const auto r = search_flags(program, setup, probes, options);
        CHECK_FALSE(r.exhaustive);
        CHECK(r.score.complete());
        CHECK(r.candidates < r.space);
        // The fixture's flags, with every other group at the compiler's default.
        auto sorted = [](std::vector<std::string> v) {
            std::ranges::sort(v);
            return v;
        };
        CHECK(sorted(r.flags) == sorted(test::fixture_flags()));
        CHECK(r.equivalent[0] == std::vector<usize>{2});  // the optimization level is decided
        CHECK(choice_label(options.groups, r.choice) == "/O2 /GS-");
    }
}
