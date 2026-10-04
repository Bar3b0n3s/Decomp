// Round trip with the installed LLVM: build the fixture program with clang-cl + lld-link, then check
// that compiling the same sources reproduces every function byte-for-byte. Building the target here
// (instead of using the committed fixture binaries) keeps the test independent of the LLVM version.
// Skipped when clang-cl or lld-link is not installed.
#include "analysis/program.hpp"
#include "core/fs.hpp"
#include "core/process.hpp"
#include "core/strings.hpp"
#include "matching/match.hpp"
#include "matching/toolchain.hpp"
#include "llvm_fixture.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

using namespace decomp;
using namespace decomp::matching;


TEST_CASE("integration: clang-cl round trip reproduces every function") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    auto tmp = fs::TempDir::create("decomp-roundtrip").value();
    for (Arch arch : {Arch::x86, Arch::x64}) {
        auto dir = tmp.path() / std::string(to_string(arch));
        REQUIRE(fs::create_directories(dir).has_value());
        auto exe = test::build_fixture_program(arch, *tools, dir);
        REQUIRE(exe);
        auto program = Program::open(*exe).value();
        REQUIRE(program.pdb_path());
        auto setup = test::clang_setup(arch, tools->clang_cl, dir / "work", dir / "cache");
        auto basic = fs::read_text(test::fixture("src/basic.cpp")).value();
        auto other = fs::read_text(test::fixture("src/other.cpp")).value();
        usize matched = 0;
        for (const char* fn : {"Player::Hit", "Player::Score", "add", "read_counter", "sum_array", "dispatch", "helper", "message",
                               "scale", "mix", "exported_api", "entry", "other_value"}) {
            CAPTURE(to_string(arch));
            CAPTURE(std::string(fn));
            auto va = program.resolve(fn);
            REQUIRE(va);
            auto r = compile_and_diff(program, setup, *va, std::string(fn) == "other_value" ? other : basic).value();
            REQUIRE_MESSAGE(r.compile.ok, r.compile.output);
            REQUIRE_MESSAGE(r.diff, r.diff_error);
            if (!r.diff->byte_exact) MESSAGE(to_text(*r.diff, {.compact = true}));
            CHECK(r.diff->byte_exact);
            matched += r.diff->byte_exact;
        }
        CHECK(matched == 13);
    }
}

TEST_CASE("integration: compile cache and compile errors") {
    auto clang_cl = find_clang_cl();
    if (!clang_cl) {
        MESSAGE("clang-cl not found; skipping");
        return;
    }
    auto tmp = fs::TempDir::create("decomp-cache").value();
    auto setup = test::clang_setup(Arch::x86, *clang_cl, tmp.path() / "work", tmp.path() / "cache");
    Compiler compiler(setup.toolchain, setup.work_dir, setup.cache_dir);
    CompileRequest req;
    req.source = "int twice(int x) { return x * 2; }\n";
    req.flags = setup.flags;
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
