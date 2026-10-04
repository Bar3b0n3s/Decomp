#include "core/fs.hpp"
#include "matching/health.hpp"
#include "project/progress.hpp"
#include "project/setup.hpp"
#include "run/selection.hpp"
#include "llvm_fixture.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;

namespace {

struct Fixture {
    fs::TempDir dir = fs::TempDir::create("decomp-selection").value();
    project::Project project = project::Project::init(dir.path() / "p", test::fixture("x86/basic.exe"), std::nullopt, "clang-cl-x86").value();
    Program program = project.open_program().value();
    u64 va(const char* name) const { return *program.resolve(name); }
};

} // namespace

TEST_CASE("progress counts functions and bytes per status, like decomp status") {
    Fixture fx;
    const u64 add = fx.va("add"), dispatch = fx.va("dispatch");
    REQUIRE(fx.project.update_function(add, {project::FunctionStatus::matched, 100, 2, 0.5}));
    REQUIRE(fx.project.update_function(dispatch, {project::FunctionStatus::nonmatching, 87.5, 4, 1.25}));
    const auto p = project::compute_progress(fx.program.symbols(), fx.project);
    CHECK(p.functions == fx.program.symbols().functions().size());
    CHECK(p.matched_functions == 1);
    CHECK(p.matched_bytes == fx.program.symbols().at(add)->size);
    CHECK(p.spend_usd == doctest::Approx(1.75));
    CHECK(p.buckets.at(project::FunctionStatus::nonmatching).functions == 1);
    CHECK(p.buckets.at(project::FunctionStatus::unstarted).functions == p.functions - 2);
    CHECK(p.percent_functions() > 0);
    const Json j = project::to_json(p);
    CHECK(j["buckets"]["matched"]["functions"] == 1);
}

TEST_CASE("selection: defaults skip finished functions; filters and explicit names") {
    Fixture fx;
    const u64 add = fx.va("add");
    REQUIRE(fx.project.update_function(add, {project::FunctionStatus::matched, 100, 1, 0}));
    auto all = run::select_functions(fx.program, &fx.project, {}).value();
    CHECK(std::ranges::find(all, add) == all.end());
    CHECK(std::ranges::is_sorted(all));
    // Everything but the matched add() and the ExitProcess import thunk the linker made.
    const u64 exit_thunk = fx.va("_ExitProcess@4");  // "ExitProcess" names the import's IAT slot
    CHECK(fx.program.thunk_destination(exit_thunk));
    CHECK(std::ranges::find(all, exit_thunk) == all.end());
    CHECK(all.size() == fx.program.symbols().functions().size() - 2);
    // Named explicitly, a thunk is still selected.
    run::Selection thunk_by_name;
    thunk_by_name.functions = {"_ExitProcess@4"};
    CHECK(run::select_functions(fx.program, &fx.project, thunk_by_name).value() == std::vector<u64>{exit_thunk});

    run::Selection everything;
    everything.include_finished = true;
    CHECK(run::select_functions(fx.program, &fx.project, everything).value().size() == all.size() + 1);

    run::Selection only_matched;
    only_matched.statuses = {project::FunctionStatus::matched};
    CHECK(run::select_functions(fx.program, &fx.project, only_matched).value() == std::vector<u64>{add});

    run::Selection by_name;
    by_name.filter = "^(dispatch|sum_array)$|Player::";
    auto filtered = run::select_functions(fx.program, &fx.project, by_name).value();
    CHECK(filtered.size() == 4);  // dispatch, sum_array, Player::Hit, Player::Score

    run::Selection explicit_names;
    explicit_names.functions = {"add", "0x4010f0"};  // explicit names ignore the status filter
    CHECK(run::select_functions(fx.program, &fx.project, explicit_names).value() == std::vector<u64>{add, 0x4010f0});

    run::Selection bad;
    bad.functions = {"nope"};
    CHECK_FALSE(run::select_functions(fx.program, &fx.project, bad));
    run::Selection bad_re;
    bad_re.filter = "(";
    CHECK_FALSE(run::select_functions(fx.program, &fx.project, bad_re));
}

TEST_CASE("match setup comes from the project plus extra flags") {
    Fixture fx;
    fx.project.config().flags = {"/O2"};
    auto setup = project::make_match_setup(&fx.project, "", {"/Gy"}).value();
    CHECK(setup.toolchain.name == "clang-cl-x86");
    CHECK(setup.flags == std::vector<std::string>{"/O2", "/Gy"});
    CHECK(setup.work_dir == fx.project.build_dir());
    CHECK_FALSE(project::make_match_setup(&fx.project, "no-such-toolchain"));
    CHECK_FALSE(project::make_match_setup(nullptr, ""));
}

TEST_CASE("toolchain health check compiles a probe") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl not found; skipping");
        return;
    }
    matching::Toolchain t;
    t.name = "probe";
    t.kind = matching::ToolchainKind::clang_cl;
    t.compiler = tools->clang_cl;
    t.flags = {"--target=i686-pc-windows-msvc", "/Zl"};
    auto r = matching::check_toolchain(t).value();
    CHECK(r.ok);
    CHECK(r.arch == Arch::x86);
    CHECK(r.functions == 1);
    // clang-cl answers --version with "clang version <x.y.z> ...".
    CHECK(r.version.find("clang version") != std::string::npos);
    CHECK(matching::to_json(r)["version"] == r.version);
    t.compiler = "/nonexistent/clang-cl";
    auto missing = matching::check_toolchain(t);
    CHECK((!missing || !missing->ok));
    CHECK(matching::detect_version(t).empty());
}
