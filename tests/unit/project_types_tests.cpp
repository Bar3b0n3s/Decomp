// Types in project headers (project/types.hpp): composing them, compiling them and reading their layouts
// back; and the program generations a run hands its sessions.

#include "core/fs.hpp"
#include "llvm_fixture.hpp"
#include "project/project.hpp"
#include "project/types.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <ostream>  // doctest prints std::string_view with operator<<, which MSVC declares without it
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace decomp;
using namespace decomp::project;

TEST_CASE("composing a type into a header: a new header, an added type, a replaced definition") {
    auto fresh = compose_type("", "Point", "struct Point { int x, y; };\n").value();
    CHECK(fresh.text == "#pragma once\n\nstruct Point { int x, y; };\n");
    CHECK_FALSE(fresh.replaced);

    const std::string header = "#pragma once\n\n// 2D\nstruct Point { int x, y; };\n\nstruct Rect;\n";
    auto added = compose_type(header, "Color", "enum Color { Red, Green };").value();
    CHECK(added.text == header.substr(0, header.size() - 1) + "\n\nenum Color { Red, Green };\n");
    CHECK_FALSE(added.replaced);

    // A definition takes the place of the old one (its leading comment stays), and of a forward declaration.
    auto replaced = compose_type(header, "Point", "struct Point { int x, y, z; };").value();
    CHECK(replaced.text == "#pragma once\n\n// 2D\nstruct Point { int x, y, z; };\n\nstruct Rect;\n");
    CHECK(replaced.replaced);
    auto defined = compose_type(header, "Rect", "struct Rect {\n    Point a, b;\n};").value();
    CHECK(defined.text == "#pragma once\n\n// 2D\nstruct Point { int x, y; };\n\nstruct Rect {\n    Point a, b;\n};\n");

    // #pragma pack may come with it; typedefs and aliases define types too.
    CHECK(compose_type("", "Packed", "#pragma pack(push, 1)\nstruct Packed { char c; int i; };\n#pragma pack(pop)"));
    CHECK(compose_type("", "Callback", "typedef void (*Callback)(int);"));
    CHECK(compose_type("", "Score", "using Score = long long;"));
    // Refused: a declaration of something else, data, functions, other directives.
    CHECK_FALSE(compose_type("", "Point", "struct Other { int a; };"));
    CHECK_FALSE(compose_type("", "Point", "struct Point { int x; };\nint g_points;"));
    CHECK_FALSE(compose_type("", "Point", "struct Point { int x; };\nint area() { return 0; }"));
    CHECK_FALSE(compose_type("", "Point", "#include <windows.h>\nstruct Point { int x; };"));
}

TEST_CASE("project header names") {
    for (const char* good : {"types.h", "game/player.hpp", "a-b_c.hh", "Engine/Math.HXX"}) {
        CAPTURE(good);
        CHECK(valid_header_name(good));
    }
    for (const char* bad : {"", "../types.h", "game/../types.h", "./types.h", "/types.h", "types", "types.cpp", "game\\\\types.h", "a b.h",
                            "C:/types.h", "game//types.h"}) {
        CAPTURE(bad);
        CHECK_FALSE(valid_header_name(bad));
    }
}

TEST_CASE("program generations: a new one when the project's symbols change, not on function states") {
    auto dir = fs::TempDir::create("decomp-generations").value();
    auto p = Project::init(dir.path() / "p", test::fixture("x86/basic.exe"), std::nullopt, "clang-cl-x86").value();
    auto generations = ProgramGenerations::open(p).value();
    const auto first = generations->current();
    const u64 add = *first->resolve("add");
    CHECK(generations->current() == first);
    // A function's state is not a symbol change.
    REQUIRE(p.update_function(add, {FunctionStatus::nonmatching, 50, 1, 0.1}));
    CHECK(generations->current() == first);
    // A rename is: the next generation has it, the old one stays as it was.
    REQUIRE(p.set_symbol(SymbolEdit{.va = add, .name = std::string("add_counter")}, ChangeOrigin{SymbolSource::agent, "s1", "test"}));
    const auto second = generations->current();
    CHECK(second != first);
    CHECK(second->symbols().at(add)->name == "add_counter");
    CHECK(first->symbols().at(add)->name != "add_counter");
    CHECK(second->resolve("add_counter") == add);
}

TEST_CASE("the types a header declares: in namespaces and extern \"C\" blocks, typedefs and aliases, not templates") {
    const auto declared = header_declared_types(R"(#pragma once
struct Point { int x, y; };
class Shape;
typedef struct { int a; } Pair, *PPair;
using Score = long long;
enum class Color : int { Red };
template <class T> struct Box { T value; };
extern "C" {
typedef void (*Callback)(int);
}
namespace game {
union Value { int i; float f; };
namespace detail { struct Hidden; }
}
namespace a::b { struct Deep {}; }
namespace { struct Local {}; }
int g_value;
)");
    std::vector<std::pair<std::string, std::string>> got;
    for (const auto& d : declared) got.emplace_back(d.name, d.keyword);
    CHECK(got == std::vector<std::pair<std::string, std::string>>{{"Point", "struct"},
                                                                  {"Shape", "class"},
                                                                  {"Pair", ""},
                                                                  {"PPair", ""},
                                                                  {"Score", ""},
                                                                  {"Color", "enum"},
                                                                  {"Callback", ""},
                                                                  {"game::Value", "union"},
                                                                  {"game::detail::Hidden", "struct"},
                                                                  {"a::b::Deep", "struct"}});
}

TEST_CASE("the project's headers compiled and read back: the fixtures' types equal their PDBs'") {
    const auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl not found; skipping");
        return;
    }
    for (const Arch arch : {Arch::x86, Arch::x64}) {
        const std::string a = arch == Arch::x86 ? "x86" : "x64";
        for (const std::string name : {"basic", "rtti"}) {
            CAPTURE(a);
            CAPTURE(name);
            auto dir = fs::TempDir::create("decomp-header-types").value();
            auto p = Project::init(dir.path() / "p", test::fixture(a + "/" + name + ".exe"), std::nullopt, "clang-cl-" + a).value();
            std::filesystem::copy_file(test::fixture("include/" + name + ".h"), p.root() / "include" / (name + ".h"));
            const auto program = p.open_program().value();
            const auto headers = compile_header_types(p, test::clang_setup(arch, tools->clang_cl, dir.path() / "work"), arch).value();
            CHECK(headers.declared.size() == (name == "basic" ? 1u : 6u));
            for (const auto& declared : headers.declared) {
                CAPTURE(declared.name);
                CHECK(declared.header == "include/" + name + ".h");
                const TypeLayout* layout = headers.catalog.find(declared.name);
                const TypeLayout* expected = program.pdb_types().catalog.find(declared.name);
                REQUIRE(layout);
                REQUIRE(expected);
                CHECK(compare_layouts(*layout, *expected) == std::vector<std::string>{});
            }
            // A header that disagrees with the PDB says how.
            REQUIRE(fs::write_text(p.root() / "include" / (name + ".h"), name == "basic" ? "struct Player { int hp; double speed; };\n"
                                                                                         : "struct Named { virtual const char* name() const; int extra; };\n"));
            const auto changed = compile_header_types(p, test::clang_setup(arch, tools->clang_cl, dir.path() / "work"), arch).value();
            const std::string type = name == "basic" ? "Player" : "Named";
            REQUIRE(changed.catalog.find(type));
            CHECK_FALSE(compare_layouts(*changed.catalog.find(type), *program.pdb_types().catalog.find(type)).empty());
        }
    }
}

TEST_CASE("header types: none without headers; headers that do not compile; toolchains without CodeView") {
    auto dir = fs::TempDir::create("decomp-header-types").value();
    auto p = Project::init(dir.path() / "p", test::fixture("x86/basic.exe"), std::nullopt, "clang-cl-x86").value();
    matching::MatchSetup gcc;
    gcc.toolchain.name = "mingw";
    gcc.toolchain.kind = matching::ToolchainKind::gcc;
    gcc.work_dir = dir.path() / "work";
    // No headers: nothing to compile.
    const auto none = compile_header_types(p, gcc, Arch::x86).value();
    CHECK(none.declared.empty());
    CHECK(none.catalog.empty());
    CHECK(project_headers(p).value().empty());

    REQUIRE(fs::create_directories(p.root() / "include" / "game"));
    REQUIRE(fs::write_text(p.root() / "include" / "game" / "broken.h", "struct Broken { int x\n"));
    REQUIRE(fs::write_text(p.root() / "include" / "notes.txt", "not a header"));
    CHECK(project_headers(p).value() == std::vector<std::string>{"include/game/broken.h"});
    const auto dwarf = compile_header_types(p, gcc, Arch::x86);
    REQUIRE_FALSE(dwarf);
    CHECK(dwarf.error().code == ErrorCode::unsupported);

    const auto tools = test::find_llvm();
    if (!tools) return;
    const auto broken = compile_header_types(p, test::clang_setup(Arch::x86, tools->clang_cl, dir.path() / "work"), Arch::x86);
    REQUIRE_FALSE(broken);
    CHECK(broken.error().message.find("do not compile") != std::string::npos);
    CHECK(broken.error().message.find("include/game/broken.h:1") != std::string::npos);
}
