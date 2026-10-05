// The difficulty model (analysis/difficulty.hpp): features of a function's code, the score built from
// them, and the analysis of every function that runs order their queue by.

#include "analysis/annotate.hpp"
#include "analysis/difficulty.hpp"
#include "core/fs.hpp"
#include "llvm_fixture.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

using namespace decomp;

TEST_CASE("difficulty: features of the fixture's functions agree with the annotated listing") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        auto p = Program::open(test::fixture(std::string(arch) + "/basic.exe")).value();
        for (const char* name : {"add", "sum_array", "dispatch", "entry", "Player::Hit"}) {
            CAPTURE(name);
            const u64 va = *p.resolve(name);
            const FunctionFeatures f = function_features(p, va).value();
            const AnnotatedFunction a = annotate_function(p, va).value();
            CHECK(f.va == va);
            CHECK(f.bytes == a.end - a.start);
            CHECK(f.instructions == a.instruction_count);
            CHECK(f.blocks == a.block_count);
            CHECK(f.loops == a.loop_count);
        }
        const FunctionFeatures add = function_features(p, *p.resolve("add")).value();
        CHECK(add.calls == 0);
        CHECK(add.callees == 0);
        CHECK(add.unknown_callees == 0);
        const FunctionFeatures sum = function_features(p, *p.resolve("sum_array")).value();
        CHECK(sum.loops >= 1);
        CHECK(sum.max_loop_depth >= 1);
        const FunctionFeatures dispatch = function_features(p, *p.resolve("dispatch")).value();
        CHECK(dispatch.jump_tables == 1);
        CHECK(dispatch.callees >= 3);  // add, helper, other_value, sum_array (calls and tail jumps)
        CHECK(dispatch.unknown_callees == 0);
        const FunctionFeatures entry = function_features(p, *p.resolve("entry")).value();
        CHECK(entry.calls >= 8);
        CHECK(entry.callees >= 8);  // the fixture's functions and ExitProcess through its import
        CHECK(entry.unknown_callees == 0);

        CHECK(difficulty(add) < difficulty(sum));
        CHECK(difficulty(add) < difficulty(entry));
        CHECK(difficulty(add) < difficulty(dispatch));
        MESSAGE(std::string(arch) << " difficulty: add " << difficulty(add) << ", sum_array " << difficulty(sum) << ", dispatch "
                                  << difficulty(dispatch) << ", entry " << difficulty(entry));
        CHECK(difficulty_label(difficulty(add)) == "easy");
        CHECK_FALSE(function_features(p, *p.resolve("g_counter")));

        // Unnamed callees count as unknown.
        auto unnamed = p.with_symbols(p.symbols());
        Symbol renamed = *unnamed.symbols().at(*p.resolve("helper"));
        unnamed.symbols().remove(renamed.va);
        renamed.name = std::format("sub_{:x}", renamed.va);
        renamed.display.clear();
        renamed.pdb_name.clear();
        renamed.source = SymbolSource::analysis;
        unnamed.symbols().add(renamed);
        const FunctionFeatures blind = function_features(unnamed, *p.resolve("dispatch")).value();
        CHECK(blind.unknown_callees == 1);
        CHECK(blind.callees == dispatch.callees - 1);
        CHECK(difficulty(blind) > difficulty(dispatch));
    }
}

TEST_CASE("difficulty: the formula") {
    FunctionFeatures f;
    f.bytes = 15;
    f.blocks = 1;
    CHECK(difficulty(f) == doctest::Approx(4.5));
    f.loops = 20;  // capped at 8
    f.max_loop_depth = 9;  // capped at 4
    f.unknown_callees = 3;
    f.callees = 40;  // capped at 20
    f.jump_tables = 1;
    CHECK(difficulty(f) == doctest::Approx(4.5 + 8 + 2 + 5 + 3 + 0.5));
    CHECK(difficulty_label(7.9) == "easy");
    CHECK(difficulty_label(8) == "medium");
    CHECK(difficulty_label(14) == "hard");
    CHECK(difficulty_label(20) == "very hard");
}

TEST_CASE("analyze_functions: every function, callers, cancellation and progress") {
    auto p = Program::open(test::fixture("x86/basic.exe")).value();
    std::vector<u64> vas;
    for (const auto* f : p.symbols().functions()) vas.push_back(f->va);
    usize reported = 0;
    const FunctionAnalysis all = analyze_functions(p, vas, {}, [&](usize done, usize total) {
        CHECK(done <= total);
        reported = done;
    });
    CHECK_FALSE(all.cancelled);
    CHECK(reported == vas.size());
    CHECK(all.functions.size() + all.failed.size() == vas.size());
    REQUIRE(all.find(*p.resolve("add")));
    // add is called by dispatch and entry (Program::callers_of agrees for direct calls).
    CHECK(all.find(*p.resolve("add"))->callers == p.callers_of(*p.resolve("add")).size());
    CHECK(all.find(*p.resolve("helper"))->callers >= 1);
    CHECK(all.find(*p.resolve("entry"))->callers == 0);
    CHECK(std::ranges::is_sorted(all.functions, {}, &FunctionFeatures::va));
    CHECK_FALSE(all.find(0x12345));

    int polls = 0;
    const FunctionAnalysis stopped = analyze_functions(p, vas, [&] { return ++polls > 0; });
    CHECK(stopped.cancelled);
    CHECK(stopped.functions.empty());

    // Cost per function.
    const auto start = std::chrono::steady_clock::now();
    usize instructions = 0;
    for (int i = 0; i < 200; ++i)
        for (const auto& f : analyze_functions(p, vas).functions) instructions += f.instructions;
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("analyze_functions: " << us / (200.0 * static_cast<double>(vas.size())) << " us per function, "
                                  << us / static_cast<double>(instructions) * 100 << " us per 100 instructions");
}

TEST_CASE("analyze_functions: a generated program with a very large function") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    // 300 small functions with loops, and one function of thousands of instructions calling them.
    auto dir = fs::TempDir::create("decomp-vm-large").value();
    std::string src = "#define NOINLINE __declspec(noinline)\nvolatile int g_sink;\n";
    for (int i = 0; i < 300; ++i)
        src += std::format("NOINLINE int f{0}(int x) {{ int s = 0; for (int i = 0; i < x; ++i) s += (i * {1}) ^ (s >> 3); return s + {0}; }}\n", i,
                           i + 3);
    src += "NOINLINE int big(int x) {\n  int s = 0;\n";
    for (int i = 0; i < 1500; ++i) src += std::format("  if (x & {}) s += f{}(x + {}); else g_sink = s;\n", 1 << (i % 30), i % 300, i);
    src += "  return s;\n}\nextern \"C\" void entry() { g_sink = big(g_sink); }\n";
    const auto source = dir.path() / "large.cpp";
    REQUIRE(fs::write_text(source, src));
    const auto exe = test::build_program(Arch::x86, *tools, dir.path() / "out", {source}, "large");
    REQUIRE(exe);
    auto p = Program::open(*exe).value();
    std::vector<u64> vas;
    for (const auto* f : p.symbols().functions()) vas.push_back(f->va);
    auto start = std::chrono::steady_clock::now();
    const FunctionAnalysis analysis = analyze_functions(p, vas);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    usize instructions = 0;
    for (const auto& f : analysis.functions) instructions += f.instructions;
    const FunctionFeatures* big = analysis.find(*p.resolve("big"));
    REQUIRE(big);
    CHECK(big->instructions > 5000);
    CHECK(big->callees >= 250);
    CHECK(analysis.find(*p.resolve("f7"))->callers >= 1);
    CHECK(difficulty_label(difficulty(*big)) == "very hard");
    start = std::chrono::steady_clock::now();
    CHECK_FALSE(p.callers_of(*p.resolve("f7")).empty());  // builds the cross-reference index
    const double xref_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("analyze_functions: " << analysis.functions.size() << " functions, " << instructions << " instructions (the largest "
                                  << big->instructions << ") in " << ms << " ms: " << ms * 1000 / static_cast<double>(instructions) * 100
                                  << " us per 100 instructions; cross-reference index " << xref_ms << " ms");
}
