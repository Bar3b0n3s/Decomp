// Round trip with a real compiler: compiling the fixture sources must reproduce every function of the
// fixture image byte-for-byte. Skipped when clang-cl is not installed.
#include "analysis/program.hpp"
#include "core/fs.hpp"
#include "matching/match.hpp"
#include "matching/toolchain.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

using namespace decomp;
using namespace decomp::matching;

namespace {

std::optional<MatchSetup> clang_setup(Arch arch, const std::filesystem::path& work, const std::filesystem::path& cache) {
    auto clang_cl = find_clang_cl();
    if (!clang_cl) return std::nullopt;
    MatchSetup s;
    s.toolchain.name = "clang-cl-test";
    s.toolchain.kind = ToolchainKind::clang_cl;
    s.toolchain.compiler = *clang_cl;
    s.toolchain.flags = {arch == Arch::x86 ? "--target=i686-pc-windows-msvc" : "--target=x86_64-pc-windows-msvc", "/Zl", "/Brepro"};
    s.flags = {"/O2", "/Gy", "/GS-", "/GR-", "/EHs-c-"};  // same as build_fixtures.sh (minus debug info)
    s.work_dir = work;
    s.cache_dir = cache;
    return s;
}

} // namespace

TEST_CASE("integration: clang-cl round trip reproduces every fixture function") {
    auto tmp = fs::TempDir::create("decomp-roundtrip").value();
    for (Arch arch : {Arch::x86, Arch::x64}) {
        auto setup = clang_setup(arch, tmp.path() / "work", tmp.path() / "cache");
        if (!setup) {
            MESSAGE("clang-cl not found; skipping");
            return;
        }
        std::string dir = arch == Arch::x86 ? "x86" : "x64";
        auto program = Program::open(test::fixture(dir + "/basic.exe")).value();
        auto basic = fs::read_text(test::fixture("src/basic.cpp")).value();
        auto other = fs::read_text(test::fixture("src/other.cpp")).value();
        usize matched = 0;
        for (const char* fn : {"Player::Hit", "Player::Score", "add", "read_counter", "sum_array", "dispatch", "helper", "message",
                               "scale", "mix", "exported_api", "entry"}) {
            CAPTURE(dir);
            CAPTURE(fn);
            auto r = compile_and_diff(program, *setup, *program.resolve(fn), basic).value();
            REQUIRE_MESSAGE(r.compile.ok, r.compile.output);
            REQUIRE_MESSAGE(r.diff, r.diff_error);
            CHECK(r.diff->byte_exact);
            matched += r.diff->byte_exact;
        }
        auto o = compile_and_diff(program, *setup, *program.resolve("other_value"), other).value();
        REQUIRE(o.diff);
        CHECK(o.diff->byte_exact);
        CHECK(matched == 12);
    }
}

TEST_CASE("integration: compile cache and compile errors") {
    auto tmp = fs::TempDir::create("decomp-cache").value();
    auto setup = clang_setup(Arch::x86, tmp.path() / "work", tmp.path() / "cache");
    if (!setup) {
        MESSAGE("clang-cl not found; skipping");
        return;
    }
    Compiler compiler(setup->toolchain, setup->work_dir, setup->cache_dir);
    CompileRequest req;
    req.source = "int twice(int x) { return x * 2; }\n";
    req.flags = setup->flags;
    auto first = compiler.compile(req).value();
    REQUIRE(first.ok);
    CHECK_FALSE(first.cached);
    CHECK_FALSE(first.object_data.empty());
    auto second = compiler.compile(req).value();
    CHECK(second.cached);
    CHECK(second.object_data == first.object_data);

    req.source = "int broken(int x) { return x + missing_name; }\n";
    auto bad = compiler.compile(req).value();
    CHECK_FALSE(bad.ok);
    REQUIRE_FALSE(bad.diagnostics.empty());
    CHECK(bad.diagnostics[0].severity == "error");
    CHECK(bad.diagnostics[0].line == 1);
    CHECK(bad.diagnostics[0].message.find("missing_name") != std::string::npos);
}
