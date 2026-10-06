// Compiler identification (search/identify.hpp): toolchains ranked by how close their best
// configuration compiles the probes to the target, for programs built by different toolchains.

#include "analysis/program.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "llvm_fixture.hpp"
#include "search/identify.hpp"
#include "search/probes.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

using namespace decomp;
using namespace decomp::search;

namespace {

matching::Toolchain toolchain(std::string name, matching::ToolchainKind kind, std::string compiler, std::vector<std::string> flags = {}) {
    matching::Toolchain t;
    t.name = std::move(name);
    t.kind = kind;
    t.compiler = std::move(compiler);
    t.flags = std::move(flags);
    return t;
}

} // namespace

TEST_CASE("identify: the architecture a toolchain compiles for") {
    using K = matching::ToolchainKind;
    CHECK(toolchain_arch(toolchain("clang", K::clang_cl, "clang-cl", {"--target=i686-pc-windows-msvc"})) == Arch::x86);
    CHECK(toolchain_arch(toolchain("clang", K::clang, "clang", {"-target", "x86_64-w64-mingw32"})) == Arch::x64);
    CHECK(toolchain_arch(toolchain("gcc", K::gcc, "/usr/bin/x86_64-w64-mingw32-gcc")) == Arch::x64);
    CHECK(toolchain_arch(toolchain("gcc", K::gcc, "i686-w64-mingw32-gcc")) == Arch::x86);
    CHECK(toolchain_arch(toolchain("gcc", K::gcc, "gcc", {"-O2", "-m32"})) == Arch::x86);
    CHECK(toolchain_arch(toolchain("vs", K::msvc, "C:/VS/VC/Tools/MSVC/14.44/bin/Hostx64/x86/cl.exe")) == Arch::x86);
    CHECK(toolchain_arch(toolchain("vs", K::msvc, "C:/VS/VC/Tools/MSVC/14.44/bin/Hostx64/x64/cl.exe")) == Arch::x64);
    CHECK(toolchain_arch(toolchain("vc6", K::msvc, "C:/VS6/VC98/Bin/cl.exe")) == std::nullopt);
    CHECK(toolchain_arch(toolchain("msvc-x64", K::msvc, "cl.exe")) == Arch::x64);
    CHECK(toolchain_arch(toolchain("msvc6-win32", K::msvc, "cl.exe")) == Arch::x86);
    CHECK(toolchain_arch(toolchain("msvc", K::msvc, "cl.exe")) == std::nullopt);
}

TEST_CASE("identify: the toolchain that built a program ranks first") {
    auto tools = test::find_llvm();
    auto gcc = test::find_mingw_gcc();
    if (!tools || !gcc) {
        MESSAGE("clang-cl, lld-link or x86_64-w64-mingw32-gcc not found; skipping");
        return;
    }
    auto tmp = fs::TempDir::create("decomp-identify").value();
    const auto source = test::fixture("src/ident.c");
    const auto by_clang = test::build_program(Arch::x64, *tools, tmp.path() / "clang", {source}, "ident");
    const auto by_gcc = test::build_gcc_program(*gcc, *tools, tmp.path() / "gcc", {source}, "ident");
    REQUIRE(by_clang);
    REQUIRE(by_gcc);

    const auto setup = test::clang_setup(Arch::x64, tools->clang_cl, tmp.path() / "work", tmp.path() / "cache");
    const std::vector<matching::Toolchain> candidates = {
        toolchain("missing-gcc", matching::ToolchainKind::gcc, fs::to_utf8(tmp.path() / "no" / "gcc")),
        toolchain("mingw-gcc", matching::ToolchainKind::gcc, *gcc),
        setup.toolchain,
    };
    for (const auto& [exe, expected] : {std::pair{*by_clang, setup.toolchain.name}, std::pair{*by_gcc, std::string("mingw-gcc")}}) {
        CAPTURE(expected);
        auto program = Program::open(exe).value();
        const std::vector<Probe> probes = {file_probe(program, source).value()};
        CHECK(probes[0].functions.size() == 5);  // add, sum_array, classify, mix, entry
        IdentifyOptions options;
        options.start = setup.flags;  // MSVC-style: kept for clang-cl, not for GCC
        CandidateLog log;
        options.log = &log;
        const auto r = identify(program, setup, probes, candidates, options);
        REQUIRE(r.ranking.size() == 3);
        CHECK(r.ranking[0].toolchain == expected);
        CHECK(r.ranking[0].score.complete());
        CHECK(r.decided());
        CHECK_FALSE(r.cancelled);
        // The compiler that is not there ranks last, with why.
        CHECK(r.ranking[2].toolchain == "missing-gcc");
        CHECK_FALSE(r.ranking[2].error.empty());
        CHECK(log.count() == r.candidates);
        const auto j = to_json(r);
        CHECK(j["ranking"][0]["toolchain"] == expected);
        CHECK(j["decided"] == true);
        MESSAGE(expected << ": " << join(r.ranking[0].flags, " ") << " (" << r.ranking[0].score.text() << "), then " << r.ranking[1].toolchain
                         << " (" << r.ranking[1].score.text() << ")");
    }
}
