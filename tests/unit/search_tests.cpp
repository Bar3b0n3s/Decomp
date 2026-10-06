// The search core (search/evaluate.hpp, search/runs.hpp, search/probes.hpp): candidates compiled and
// scored against the target, in parallel, and search runs as a project keeps them.

#include "analysis/program.hpp"
#include "core/fs.hpp"
#include "llvm_fixture.hpp"
#include "search/evaluate.hpp"
#include "search/probes.hpp"
#include "search/runs.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <set>

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
    LogEntry first;
    first.index = 0;
    first.label = "/Od";
    first.score = Score{1, 0, 40, 70};
    writer->log(first);
    LogEntry second;
    second.index = 1;
    second.label = "/O2";
    second.score = Score{1, 1, 0, 100};
    second.best = true;
    writer->log(second);
    REQUIRE(writer->write_file("note.txt", "hello"));
    REQUIRE(writer->finish(RunStatus::done, Json{{"flags", Json::array({"/O2"})}}));

    auto run = load_run(writer->dir()).value();
    CHECK(run.kind == SearchKind::flags);
    CHECK(run.target == "add");
    CHECK(run.functions == std::vector<u64>{0x401000});
    CHECK(run.status == RunStatus::done);
    CHECK(run.candidates == 2);
    REQUIRE(run.best);
    CHECK(run.best->exact == 1);
    CHECK(run.best_label == "/O2");
    CHECK(run.settings == settings);
    CHECK(run.result["flags"][0] == "/O2");
    const auto log = load_log(writer->dir());
    REQUIRE(log.size() == 2);
    CHECK(log[0].label == "/Od");
    CHECK(log[1].best);
    CHECK(log[1].score == second.score);
    CHECK(fs::read_text(writer->dir() / "note.txt").value() == "hello");

    // Newest first; something that is not a run is skipped.
    auto other = RunWriter::create(tmp.path(), SearchKind::permute, "sum_array", {}, Json::object()).value();
    REQUIRE(other->finish(RunStatus::cancelled, Json::object()));
    REQUIRE(fs::create_directories(tmp.path() / "zzz-not-a-run"));
    listed = list_runs(tmp.path());
    REQUIRE(listed.size() == 2);
    CHECK(std::set<std::string>{listed[0].id, listed[1].id} == std::set<std::string>{run.id, other->record().id});
    CHECK(listed[0].id > listed[1].id);
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
