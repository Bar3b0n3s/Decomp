// The top-level items of C and C++ sources (matching/source_items.hpp), which unit sources are composed of.

#include "matching/source_items.hpp"

#include <doctest/doctest.h>

#include <ostream>  // doctest prints std::string_view with operator<<, which MSVC declares without it
#include <string>
#include <string_view>

using namespace decomp;
using namespace decomp::matching;

TEST_CASE("a source's top-level items: directives, declarations, function definitions and blocks") {
    const std::string source = R"(// The fixture's shapes.
#define NOINLINE __declspec(noinline)
#include "types.h"

extern "C" int _fltused = 0;  // the ABI wants it
int g_table[8] = {1, 1, 2, 3, 5, 8, 13, 21};
static int s_calls = 0;
int other_value(int x);  // other.cpp

struct Player {
    int hp;
    NOINLINE void Hit(int dmg);
};

void Player::Hit(int dmg) {
    hp -= dmg;
    if (hp < 0) hp = 0;
}

int Player::Score() const { return hp * 10; }

/* a static helper */
NOINLINE static int helper(int x) {
    ++s_calls;
    return x * 3 + 1;
}

extern "C" {
int c_api(int);
}

namespace ns {
int f() { return 1; }
}

const char* message() { return "}{;"; }
bool operator==(const Player& a, const Player& b) { return a.hp == b.hp; }
int (*g_callback)(int) = 0;
#pragma optimize("", off)
int Player::*member = &Player::hp;
// the end
)";
    const auto items = parse_source_items(source);
    REQUIRE(items.size() == 18);
    auto check = [&](usize i, ItemKind kind, std::string_view name) {
        CAPTURE(i);
        CHECK(items[i].kind == kind);
        CHECK(items[i].name == name);
    };
    check(0, ItemKind::preprocessor, "define");
    CHECK(items[0].text.starts_with("// The fixture's shapes.\n#define"));  // leading comments go with the item
    CHECK(item_body(items[0]) == "#define NOINLINE __declspec(noinline)");
    check(1, ItemKind::preprocessor, "include");
    check(2, ItemKind::declaration, "");
    CHECK(items[2].text.ends_with("// the ABI wants it"));  // a comment on its line is the item's
    check(3, ItemKind::declaration, "");
    check(4, ItemKind::declaration, "");
    CHECK(items[4].is_static);
    check(5, ItemKind::declaration, "other_value");
    CHECK(items[5].function_declaration);
    check(6, ItemKind::declaration, "");  // struct Player { ... };
    CHECK_FALSE(items[6].function_declaration);
    CHECK(item_body(items[6]).ends_with("};"));
    check(7, ItemKind::function, "Player::Hit");
    CHECK(items[7].line == 15);
    check(8, ItemKind::function, "Player::Score");
    check(9, ItemKind::function, "helper");
    CHECK(items[9].is_static);
    CHECK(items[9].text.find("/* a static helper */") != std::string::npos);
    check(10, ItemKind::block, "");  // extern "C" { ... }
    check(11, ItemKind::block, "");  // namespace ns { ... }
    check(12, ItemKind::function, "message");  // braces inside a string do not count
    CHECK(item_body(items[12]).ends_with("\"}{;\"; }"));
    check(13, ItemKind::function, "operator==");
    check(14, ItemKind::declaration, "");  // a function pointer is data
    CHECK_FALSE(items[14].function_declaration);
    check(15, ItemKind::preprocessor, "pragma");
    check(16, ItemKind::declaration, "");
    check(17, ItemKind::comment, "");
}

TEST_CASE("more declarators: operators, destructors, templates, trailing return types") {
    auto only = [](std::string_view source) {
        const auto items = parse_source_items(source);
        REQUIRE(items.size() == 1);
        return items[0];
    };
    CHECK(only("Vec::~Vec() { free(p); }").name == "Vec::~Vec");
    CHECK(only("Vec& Vec::operator=(const Vec& o) { return *this; }").name == "Vec::operator=");
    CHECK(only("void* operator new(unsigned n) { return 0; }").name == "operator new");
    CHECK(only("int Functor::operator()(int x) const { return x; }").name == "Functor::operator()");
    CHECK(only("int Box<int>::get() { return v; }").name == "Box<int>::get");
    const auto trailing = only("auto f() -> int { return 1; }");
    CHECK(trailing.kind == ItemKind::function);
    CHECK(trailing.name == "f");
    const auto ctor = only("Player::Player(int h) : hp(h), speed(1) { }");
    CHECK(ctor.kind == ItemKind::function);
    CHECK(ctor.name == "Player::Player");
    CHECK(only("__declspec(noinline) int __cdecl add(int a, int b) { return a + b; }").name == "add");
    const auto decl = only("__declspec(dllimport) void __stdcall ExitProcess(unsigned int code);");
    CHECK(decl.function_declaration);
    CHECK(decl.name == "ExitProcess");
    const auto continued = parse_source_items("#define TWICE(x) \\\n    ((x) + (x))\nint y;\n");
    REQUIRE(continued.size() == 2);
    CHECK(continued[0].text == "#define TWICE(x) \\\n    ((x) + (x))");
    CHECK(continued[1].line == 3);
}

TEST_CASE("normalized text: comments and the spaces words do not need are gone") {
    CHECK(normalized("int  f( int a ) ;") == "int f(int a);");
    CHECK(normalized("int /* x */ a; // the a") == "int a;");
    CHECK(normalized("const char* s = \"a  b\";") == "const char*s=\"a  b\";");
    CHECK(normalized("unsigned\tlong\nx;") == "unsigned long x;");
    CHECK(normalized("struct S {\n    int a;\n};") == normalized("struct S { int a; };"));
}
