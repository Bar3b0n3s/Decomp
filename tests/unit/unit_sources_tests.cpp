// Unit sources (matching/unit_source.hpp, project/units.hpp): matched functions composed into one source
// per translation unit, which still compiles to the target's bytes for every function in it.

#include "analysis/program.hpp"
#include "core/fs.hpp"
#include "llvm_fixture.hpp"
#include "matching/unit_source.hpp"
#include "project/project.hpp"
#include "project/units.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>

using namespace decomp;
using namespace decomp::matching;

namespace {

std::vector<std::string> names(std::initializer_list<const char*> list) { return {list.begin(), list.end()}; }

const char* const kAddSource = "#define NOINLINE __declspec(noinline)\n"
                               "extern int g_counter;\n"
                               "\n"
                               "NOINLINE int add(int a, int b) { return a + b + g_counter; }\n";

} // namespace

TEST_CASE("composing functions into a unit source: a shared prelude, then the functions in address order") {
    UnitSource unit;
    REQUIRE(compose_function(unit, 0x401060, names({"add"}), kAddSource));
    // Composed out of order; the same macro and declaration are not repeated.
    REQUIRE(compose_function(unit, 0x401000, names({"first"}),
                             "#define NOINLINE __declspec(noinline)\n// counts calls\nextern int g_counter;\nint add(int a, int b);\n"
                             "#pragma optimize(\"\", off)\nint first(void) { return add(1, 2); }\n#pragma optimize(\"\", on)\n"));
    const std::string text = unit.render();
    CHECK(text == "#define NOINLINE __declspec(noinline)\n"
                  "extern int g_counter;\n"
                  "int add(int a, int b);\n"
                  "\n"
                  "// FUNCTION: 0x00401000\n"
                  "#pragma optimize(\"\", off)\n"
                  "int first(void) { return add(1, 2); }\n"
                  "#pragma optimize(\"\", on)\n"
                  "\n"
                  "// FUNCTION: 0x00401060\n"
                  "NOINLINE int add(int a, int b) { return a + b + g_counter; }\n");
    // What render() writes, parse() reads back.
    const UnitSource again = UnitSource::parse(text);
    CHECK(again.render() == text);
    REQUIRE(again.functions.size() == 2);
    CHECK(again.functions[0].name == "first");
    CHECK(again.find(0x401060)->name == "add");

    // A source without the function's definition at its top level is refused.
    UnitSource copy = again;
    CHECK_FALSE(compose_function(copy, 0x401100, names({"Player::Hit"}), "struct Player { void Hit(int d) { hp -= d; } int hp; };\n"));
    // Composing a function again replaces its entry.
    REQUIRE(compose_function(copy, 0x401060, names({"add"}), "extern int g_counter;\nint add(int a, int b) { return a - b; }\n"));
    REQUIRE(copy.functions.size() == 2);
    CHECK(copy.find(0x401060)->text == "int add(int a, int b) { return a - b; }");
}

TEST_CASE("a function's entry takes over a definition another function's source brought along") {
    // dispatch's source defines the static helper it calls (that is how it compiles to the same code).
    const std::string dispatch = "static int s_calls;\n"
                                 "static int helper(int x) { ++s_calls; return x * 3; }\n"
                                 "int dispatch(int v) { return helper(v) + 1; }\n";
    UnitSource unit;
    REQUIRE(compose_function(unit, 0x4010f0, names({"dispatch"}), dispatch));
    CHECK(unit.render().find("static int helper(int x) { ++s_calls; return x * 3; }\n\n// FUNCTION: 0x004010f0") != std::string::npos);
    // helper is matched later (it comes after dispatch): the prelude keeps a declaration of it.
    REQUIRE(compose_function(unit, 0x401160, names({"helper"}), dispatch));
    const std::string text = unit.render();
    CHECK(text == "static int s_calls;\n"
                  "static int helper(int x);\n"
                  "\n"
                  "// FUNCTION: 0x004010f0\n"
                  "int dispatch(int v) { return helper(v) + 1; }\n"
                  "\n"
                  "// FUNCTION: 0x00401160\n"
                  "static int helper(int x) { ++s_calls; return x * 3; }\n");
    // Another source that declares helper without `static` would conflict with the unit's: the
    // declaration is left out. Conditional directives always come along.
    REQUIRE(compose_function(unit, 0x401200, names({"later"}),
                             "int helper(int x);\n#ifndef LIMIT\n#define LIMIT 8\n#endif\nint later(int v) { return helper(v) % LIMIT; }\n"));
    const std::string after = unit.render();
    CHECK(after.find("\nint helper(int x);\n") == std::string::npos);
    CHECK(after.find("#ifndef LIMIT\n#define LIMIT 8\n#endif\n") != std::string::npos);
}

TEST_CASE("a function composed from a whole translation unit stays declared for what came after it") {
    // weigh's source is the whole file: entry, after weigh there, calls it. In the unit source entry is in the
    // prelude, before weigh's entry: a declaration made from weigh's head goes before it.
    const std::string file = "struct P { int hp; };\n"
                             "static int weigh(const P* p) { return p->hp * 4; }\n"
                             "int unrelated(int x) { return x; }\n"
                             "// the entry point\n"
                             "extern \"C\" int entry() { P p{3}; return weigh(&p); }\n";
    UnitSource unit;
    REQUIRE(compose_function(unit, 0x401020, names({"weigh"}), file));
    CHECK(unit.render() == "struct P { int hp; };\n"
                           "int unrelated(int x) { return x; }\n"
                           "\n"
                           "static int weigh(const P* p);\n"
                           "// the entry point\n"
                           "extern \"C\" int entry() { P p{3}; return weigh(&p); }\n"
                           "\n"
                           "// FUNCTION: 0x00401020\n"
                           "static int weigh(const P* p) { return p->hp * 4; }\n");
    // A member function's class declares it already; nothing is added.
    UnitSource member;
    REQUIRE(compose_function(member, 0x401000, names({"P::Get"}),
                             "struct P { int hp; int Get(); };\nint P::Get() { return hp; }\nint use(P* p) { return p->Get(); }\n"));
    CHECK(member.render() == "struct P { int hp; int Get(); };\nint use(P* p) { return p->Get(); }\n\n// FUNCTION: 0x00401000\nint P::Get() { return hp; }\n");
}

TEST_CASE("matched functions move into their units' sources and verify byte-exact there") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    const std::string noinline = "#define NOINLINE __declspec(noinline)\n";
    const std::string player = "struct Player {\n    int hp;\n    float speed;\n    NOINLINE void Hit(int dmg);\n    NOINLINE int Score() const;\n};\n";
    // dispatch and helper come from one source: a static function gets clang's register convention
    // only with its callers in the same translation unit.
    const std::string dispatch = noinline +
                                 "extern int g_counter;\nextern int g_table[8];\nstatic int s_calls = 0;\nint other_value(int x);\n"
                                 "int add(int a, int b);\nint sum_array(const int* p, int n);\n\n"
                                 "NOINLINE static int helper(int x) {\n    ++s_calls;\n    return x * 3 + 1;\n}\n\n"
                                 "NOINLINE int dispatch(int op, int v) {\n    switch (op) {\n    case 0: return add(v, 1);\n"
                                 "    case 1: return helper(v);\n    case 2: return other_value(v);\n"
                                 "    case 3: return sum_array(g_table, v & 7);\n    case 4: return v - g_counter;\n"
                                 "    case 5: return g_table[v & 7] + helper(v + 1);\n    default: return -1;\n    }\n}\n";
    // Each function's own verified translation unit, as an agent session leaves it.
    const std::vector<std::pair<const char*, std::string>> sources = {
        {"Player::Hit", noinline + player + "\nvoid Player::Hit(int dmg) {\n    hp -= dmg;\n    if (hp < 0) hp = 0;\n    speed *= 0.5f;\n}\n"},
        {"Player::Score", noinline + player + "\nint Player::Score() const { return hp * 10 + static_cast<int>(speed); }\n"},
        {"add", kAddSource},
        {"read_counter", noinline + "extern int g_counter;\n\nNOINLINE int read_counter() { return g_counter; }\n"},
        {"sum_array", noinline + "NOINLINE int sum_array(const int* p, int n) {\n    int s = 0;\n    for (int i = 0; i < n; ++i) s += p[i];\n"
                                 "    return s;\n}\n"},
        {"dispatch", dispatch},
        {"helper", dispatch},
        {"message", noinline + "NOINLINE const char* message() { return \"hello world\"; }\n"},
        {"scale", noinline + "NOINLINE float scale(float x) { return x * 1.5f + 0.25f; }\n"},
        {"mix", noinline + "NOINLINE double mix(double a, double b) { return a * 0.75 + b * 0.25; }\n"},
        {"exported_api", "int dispatch(int op, int v);\n\nextern \"C\" __declspec(dllexport) int exported_api(int x) { return dispatch(x & 3, x) + 7; }\n"},
        {"entry", noinline + "extern \"C\" __declspec(dllimport) void __stdcall ExitProcess(unsigned int code);\n"
                             "extern int g_counter;\nextern int g_table[8];\n" + player +
                      "int add(int a, int b);\nint read_counter();\nint sum_array(const int* p, int n);\nint dispatch(int op, int v);\n"
                      "const char* message();\nfloat scale(float x);\ndouble mix(double a, double b);\nextern \"C\" int exported_api(int x);\n\n"
                      "extern \"C\" void entry() {\n    Player p{100, 2.0f};\n    p.Hit(5);\n"
                      "    int total = add(1, 2) + read_counter() + sum_array(g_table, 8) + dispatch(g_counter, 4) + message()[0] +\n"
                      "                p.Score() + static_cast<int>(scale(2.0f) + mix(1.0, 3.0)) + exported_api(5);\n"
                      "    ExitProcess(static_cast<unsigned>(total));\n}\n"},
        {"other_value", noinline + "extern int g_table[8];\nstatic const char kName[] = \"other\";\n\n"
                                   "NOINLINE int other_value(int x) { return g_table[x & 7] * 2 + kName[x & 3]; }\n"},
    };
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        const std::string a(arch);
        auto dir = fs::TempDir::create("decomp-emit").value();
        // Built with the installed clang-cl, which the sources are compiled with too: another version than
        // the committed fixture's can compile a function differently.
        const auto exe = test::build_fixture_program(a == "x86" ? Arch::x86 : Arch::x64, *tools, dir.path() / "target");
        REQUIRE(exe);
        auto p = project::Project::init(dir.path() / "p", *exe, std::nullopt, "clang-cl-" + a).value();
        p.config().flags = test::fixture_flags();
        REQUIRE(p.save_config());
        const Program program = p.open_program().value();
        std::vector<u64> matched;
        for (const auto& [name, source] : sources) {
            CAPTURE(name);
            const Symbol* fn = program.symbols().find(name);
            REQUIRE(fn);
            REQUIRE(p.write_matched_source(*fn, source));
            matched.push_back(fn->va);
        }
        REQUIRE(p.modify_functions(matched, [](u64, project::FunctionInfo& info) { info.status = project::FunctionStatus::matched; }));
        const auto setup = test::clang_setup(program.arch(), tools->clang_cl, dir.path() / "work", dir.path() / "cache");

        const auto report = project::emit_unit_sources(p, program, setup, project::ChangeOrigin{SymbolSource::user, "", "emit"}).value();
        REQUIRE(report.units.size() == 2);
        for (const auto& u : report.units) {
            CAPTURE(u.name);
            for (const auto& [va, why] : u.kept) MESSAGE(std::format("{:#x}: {}", va, why));
            CHECK(u.kept.empty());
        }
        CHECK(report.units[0].emitted.size() == 12);
        CHECK(report.units[1].emitted.size() == 1);
        // The functions' own files are gone; every function verifies from its unit's source.
        for (const auto& [name, source] : sources) CHECK_FALSE(std::filesystem::exists(p.matched_source_path(*program.symbols().find(name))));
        const auto verified = project::verify_unit_sources(p, program, setup).value();
        REQUIRE(verified.size() == 2);
        CHECK(verified[0].unit.source == "src/basic.cpp");
        CHECK(verified[0].verification.functions.size() == 12);
        for (const auto& v : verified) CHECK(v.verification.all_byte_exact());
        const UnitSource basic = UnitSource::parse(fs::read_text(dir.path() / "p" / "src" / "basic.cpp").value());
        REQUIRE(basic.functions.size() == 12);
        CHECK(std::ranges::is_sorted(basic.functions, {}, &UnitSource::Function::va));
        for (u64 va : matched) CHECK(project::has_matched_source(p, *program.symbols().at(va), project::load_units(p).value()));

        // Reverting the unit source's write takes the functions out of it: with their own files gone
        // too, they are no longer matched.
        const auto changes = p.changes();
        auto unit_write = std::ranges::find_if(changes, [](const Json& c) { return c.value("path", "") == "src/other.cpp"; });
        REQUIRE(unit_write != changes.end());
        CHECK(unit_write->value("unit", "") == "other.obj");
        REQUIRE(p.revert_change(*unit_write, project::ChangeOrigin{SymbolSource::user, "", "revert"}));
        CHECK_FALSE(std::filesystem::exists(dir.path() / "p" / "src" / "other.cpp"));
        CHECK(p.function_info(program.symbols().find("other_value")->va).status == project::FunctionStatus::nonmatching);
        CHECK(p.function_info(program.symbols().find("add")->va).status == project::FunctionStatus::matched);
    }
}

TEST_CASE("composing a whole translation unit: its prelude once, declarations a compiler takes") {
    // As compose_unit_source() does: the translation unit is the prelude, and each function's definition
    // becomes its entry, leaving a declaration for the functions before it.
    const std::string tu = "// helpers\n"
                           "#if defined(_M_IX86)\n"
                           "__declspec(naked) void helper(void) { __asm { ret } }\n"
                           "#endif\n"
                           "int first(int x) { return x + 1; }\n";
    UnitSource unit = UnitSource::parse(tu);
    REQUIRE(compose_function(unit, 0x401000, names({"helper"}), tu, false));
    REQUIRE(compose_function(unit, 0x401010, names({"first"}), tu, false));
    const std::string text = unit.render();
    // Nothing of the source joins again.
    CHECK(std::ranges::count(text, '#') == 2);
    CHECK(text.find("// helpers") == text.rfind("// helpers"));
    // MSVC takes __declspec(naked) only on a definition.
    CHECK(text.find("#if defined(_M_IX86)\nvoid helper(void);\n#endif\n") != std::string::npos);
    CHECK(text.find("__declspec(naked) void helper(void) {") != std::string::npos);
    CHECK(text.find("int first(int x);") != std::string::npos);
    REQUIRE(unit.functions.size() == 2);
    CHECK(unit.functions[0].name == "helper");
}

TEST_CASE("matches join a guessed unit's source only once it has one") {
    auto dir = fs::TempDir::create("decomp-matching-unit").value();
    auto p = project::Project::init(dir.path() / "p", test::fixture("x86/basic.exe"), std::nullopt, "clang-cl-x86").value();
    const Program program = p.open_program().value();
    const Symbol& add = *program.symbols().find("add");
    auto units = project::load_units(p).value();
    // The fixture's PDB names add's unit.
    const Unit* unit = project::matching_unit(p, units, add);
    REQUIRE(unit);
    CHECK(unit->name == "basic.obj");
    CHECK(unit->origin == UnitOrigin::pdb);
    CHECK(unit->source == "src/basic.cpp");
    // The same unit as a guess: the function's matches go to its own file until the unit's source exists.
    for (auto& u : units) u.origin = UnitOrigin::analysis;
    CHECK_FALSE(project::matching_unit(p, units, add));
    REQUIRE(fs::write_text(p.root() / "src" / "basic.cpp", "extern int g_counter;\n"));
    unit = project::matching_unit(p, units, add);
    REQUIRE(unit);
    CHECK(unit->name == "basic.obj");
    // Not a code unit, or no source path: no unit source.
    for (auto& u : units) u.source.clear();
    CHECK_FALSE(project::matching_unit(p, units, add));
}
