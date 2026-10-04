#include "analysis/annotate.hpp"
#include "analysis/program.hpp"
#include "core/fs.hpp"
#include "formats/coff.hpp"
#include "matching/diff.hpp"
#include "llvm_fixture.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;
using namespace decomp::matching;

namespace {

struct Fixture {
    Program program;
    coff::Object exact, other, mutated;
};

Fixture load(const char* arch) {
    std::string a(arch);
    return {Program::open(test::fixture(a + "/basic.exe")).value(), coff::Object::load(test::fixture(a + "/basic.obj")).value(),
            coff::Object::load(test::fixture(a + "/other.obj")).value(),
            coff::Object::load(test::fixture(a + "/mutated.obj")).value()};
}

FunctionDiff diff(const Fixture& f, const char* name, const coff::Object& obj) {
    auto va = f.program.resolve(name);
    REQUIRE(va);
    return diff_function(f.program, *va, obj).value();
}

bool has_hint(const FunctionDiff& d, std::string_view needle) {
    return std::ranges::any_of(d.hints, [&](const std::string& h) { return h.find(needle) != std::string::npos; });
}

} // namespace

TEST_CASE("every fixture function is byte-exact against its own object") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        auto f = load(arch);
        usize checked = 0;
        for (const auto* sym : f.program.symbols().functions()) {
            const coff::Object* obj = find_candidate_symbol(f.exact, *sym) ? &f.exact : find_candidate_symbol(f.other, *sym) ? &f.other : nullptr;
            if (!obj) continue;  // linker thunks
            CAPTURE(sym->display);
            auto d = diff_function(f.program, sym->va, *obj).value();
            CHECK(d.byte_exact);
            CHECK(d.exact);
            CHECK(d.match_percent == 100.0);
            CHECK(d.hints.empty());
            ++checked;
        }
        CHECK(checked == 13);
    }
}

TEST_CASE("jump tables, strings, floats, statics and imports compare by meaning") {
    auto f = load("x86");
    auto d = diff(f, "dispatch", f.exact);
    CHECK(d.byte_exact);
    auto table_row = std::ranges::find_if(d.target.instructions, [](const SideInstruction& i) { return i.ins.flow == x86::Flow::indirect_jump; });
    REQUIRE(table_row != d.target.instructions.end());
    REQUIRE(table_row->refs.size() == 1);
    CHECK(table_row->refs[0]->kind == RefKind::table);
    CHECK(diff(f, "message", f.exact).target.instructions[0].refs[0]->kind == RefKind::string);
    CHECK(diff(f, "scale", f.exact).byte_exact);
    CHECK(diff(f, "helper", f.exact).byte_exact);  // static: PDB name vs mangled candidate name
    CHECK(diff(f, "entry", f.exact).byte_exact);   // import call through the IAT
    CHECK(diff(f, "other_value", f.other).byte_exact);

    auto x64 = load("x64");
    auto dx = diff(x64, "dispatch", x64.exact);
    CHECK(dx.byte_exact);  // relative (clang x64) jump table
}

TEST_CASE("mutations are classified") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        auto f = load(arch);

        auto counter = diff(f, "read_counter", f.mutated);
        CHECK_FALSE(counter.byte_exact);
        CHECK(counter.operand == 1);
        CHECK(counter.opcode == 0);
        CHECK(has_hint(counter, "g_counter2"));

        auto message = diff(f, "message", f.mutated);
        CHECK(message.operand == 1);
        CHECK(has_hint(message, "\"hello there\""));

        auto scale = diff(f, "scale", f.mutated);
        CHECK(scale.operand == 1);
        CHECK(has_hint(scale, "2.5f"));

        auto add = diff(f, "add", f.mutated);
        CHECK_FALSE(add.exact);
        CHECK(add.opcode + add.operand >= 1);

        auto hit = diff(f, "Player::Hit", f.mutated);
        CHECK_FALSE(hit.byte_exact);
        CHECK(hit.match_percent < 100.0);

        auto sum = diff(f, "sum_array", f.mutated);
        CHECK(sum.inserted + sum.deleted > 0);
        CHECK(has_hint(sum, "more instruction"));

        CHECK(diff(f, "Player::Score", f.mutated).byte_exact);  // unchanged in mutated.cpp
        CHECK(diff(f, "mix", f.mutated).byte_exact);
    }
}

TEST_CASE("unnamed target addresses produce binding hints") {
    auto f = load("x86");
    // Forget the name of g_counter: the candidate's symbol becomes a suggested binding.
    REQUIRE(f.program.symbols().remove(0x403000));
    auto d = diff(f, "read_counter", f.exact);
    CHECK_FALSE(d.exact);
    REQUIRE(d.bindings.size() == 1);
    CHECK(d.bindings[0].target_va == 0x403000);
    CHECK(d.bindings[0].candidate_symbol == "?g_counter@@3HA");
    CHECK(has_hint(d, "has no symbol"));
}

TEST_CASE("reports: text and JSON") {
    auto f = load("x86");
    auto d = diff(f, "add", f.mutated);
    auto text = to_text(d, {.compact = true});
    CHECK(text.starts_with("match "));
    CHECK(text.find("not matching") != std::string::npos);
    auto j = to_json(d);
    CHECK(j["byte_exact"] == false);
    CHECK(j["counts"]["opcode"].get<int>() == static_cast<int>(d.opcode));
    CHECK(j["rows"].size() == d.rows.size());
    CHECK(summary_line(diff(f, "add", f.exact)).find("MATCHING (byte-exact)") != std::string::npos);
}

TEST_CASE("missing candidate symbol lists what the object defines") {
    auto f = load("x86");
    auto r = diff_function(f.program, 0x401060, f.other);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::not_found);
    CHECK(r.error().message.find("other_value") != std::string::npos);
}

TEST_CASE("calls through linker thunks compare as calls to the destination") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    auto dir = fs::TempDir::create("decomp-thunks").value();
    // ExitProcess without dllimport: the linker adds an import thunk `jmp [__imp_ExitProcess]`.
    const auto source = dir.path() / "thunks.cpp";
    REQUIRE(fs::write_text(source, "extern \"C\" int _fltused = 0;\n"
                                   "extern \"C\" void __stdcall ExitProcess(unsigned int code);\n"
                                   "__declspec(noinline) void quit(unsigned int code) { ExitProcess(code + 1); }\n"
                                   "extern \"C\" void entry() { quit(3); }\n"));
    for (Arch arch : {Arch::x86, Arch::x64}) {
        CAPTURE(to_string(arch));
        const auto out = dir.path() / std::string(to_string(arch));
        auto exe = test::build_program(arch, *tools, out, {source}, "thunks");
        REQUIRE(exe);
        auto program = Program::open(*exe).value();
        const u64 va = *program.resolve("quit");
        auto list = program.function_instructions(va).value();
        auto call = std::ranges::find_if(list, [](const x86::Instruction& i) {
            return (i.flow == x86::Flow::call || i.flow == x86::Flow::jump) && i.branch_target;
        });
        REQUIRE(call != list.end());
        const u64 thunk = *call->branch_target;
        // lld names import thunks in the PDB; MSVC's incremental-linking thunks have no symbol at all.
        // Drop the name to exercise the unnamed case.
        if (program.symbols().at(thunk)) program.symbols().remove(thunk);
        REQUIRE(program.symbols().at(thunk) == nullptr);
        auto dest = program.thunk_destination(thunk);
        REQUIRE(dest);
        CHECK(program.symbols().at(*dest)->kind == SymbolKind::import);
        CHECK_FALSE(program.thunk_destination(va));  // a function is not a thunk

        auto listing = annotate_function(program, va).value();
        CHECK(to_text(listing).find("ExitProcess") != std::string::npos);

        auto obj = coff::Object::load(out / "thunks.obj").value();
        auto d = matching::diff_function(program, va, obj).value();
        CHECK(d.byte_exact);
    }
}

TEST_CASE("linker names must match exactly; readable names only when nothing better is known") {
    using matching::symbol_names_match;
    CHECK(symbol_names_match("?add@@YAHHH@Z", "add", "?add@@YAHHH@Z"));
    CHECK_FALSE(symbol_names_match("?add@@YAHHH@Z", "add", "?add@@YAHHI@Z"));  // add(int, unsigned)
    CHECK_FALSE(symbol_names_match("?add@@YAHHH@Z", "add", "_add"));           // extern "C"
    CHECK(symbol_names_match("_entry", "entry", "_entry"));
    CHECK_FALSE(symbol_names_match("_entry", "entry", "?entry@@YAXXZ"));       // missing extern "C"
    CHECK(symbol_names_match("helper", "", "?helper@@YAHH@Z"));                // static: only the PDB name is known
    CHECK(symbol_names_match("__imp__ExitProcess@4", "", "_ExitProcess@4"));
    CHECK(symbol_names_match("__imp__ExitProcess@4", "", "__imp__ExitProcess@4"));
    CHECK(symbol_names_match("exported_api", "", "_exported_api"));
}

TEST_CASE("a different declaration is not a match, even with identical code") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    auto dir = fs::TempDir::create("decomp-decl").value();
    auto exe = test::build_fixture_program(Arch::x86, *tools, dir.path() / "target");
    REQUIRE(exe);
    auto program = Program::open(*exe).value();
    auto setup = test::clang_setup(Arch::x86, tools->clang_cl, dir.path() / "work");
    // add(int, unsigned) generates the same code but mangles differently (?add@@YAHHI@Z).
    auto source = fs::read_text(test::fixture("src/basic.cpp")).value();
    const std::string from = "NOINLINE int add(int a, int b)", to = "NOINLINE int add(int a, unsigned b)";
    REQUIRE(source.find(from) != std::string::npos);
    source.replace(source.find(from), from.size(), to);
    source = "int add(int a, unsigned b);\n" + source;

    auto add = matching::compile_and_diff(program, setup, *program.resolve("add"), source).value();
    REQUIRE(add.diff);
    CHECK_FALSE(add.diff->byte_exact);
    CHECK(add.diff->equal == add.diff->target.instructions.size());  // the code itself is identical
    CHECK(std::ranges::any_of(add.diff->hints, [](const std::string& h) { return h.find("declaration differs") != std::string::npos; }));

    auto dispatch = matching::compile_and_diff(program, setup, *program.resolve("dispatch"), source).value();
    REQUIRE(dispatch.diff);
    CHECK_FALSE(dispatch.diff->byte_exact);
    CHECK(std::ranges::any_of(dispatch.diff->hints, [](const std::string& h) { return h.find("Declaration differs") != std::string::npos; }));
}

TEST_CASE("wide string literals compare in full; constants in images without .reloc compare by value") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    auto dir = fs::TempDir::create("decomp-wide").value();
    const auto source = dir.path() / "wide.cpp";
    const std::string body = "extern \"C\" int _fltused = 0;\n"
                             "__declspec(noinline) const wchar_t* wmsg() { return L\"hello\"; }\n"
                             "__declspec(noinline) unsigned magic() { return 0x401000u; }\n"
                             "extern \"C\" void entry() { wmsg(); magic(); }\n";
    REQUIRE(fs::write_text(source, body));
    // /fixed: no .reloc, so in-image constants can only be guessed to be addresses.
    auto exe = test::build_program(Arch::x86, *tools, dir.path() / "out", {source}, "wide", {"/fixed", "/base:0x400000"});
    REQUIRE(exe);
    auto program = Program::open(*exe).value();
    REQUIRE_FALSE(program.image().has_relocations());
    auto setup = test::clang_setup(Arch::x86, tools->clang_cl, dir.path() / "work");
    auto same = matching::compile_and_diff(program, setup, *program.resolve("wmsg"), body).value();
    REQUIRE(same.diff);
    CHECK(same.diff->byte_exact);

    // Same first character: a comparison that stopped at the first zero byte would call this equal.
    std::string other = body;
    other.replace(other.find("L\"hello\""), 8, "L\"hxxxx\"");
    auto changed = matching::compile_and_diff(program, setup, *program.resolve("wmsg"), other).value();
    REQUIRE(changed.diff);
    CHECK_FALSE(changed.diff->byte_exact);

    auto magic = matching::compile_and_diff(program, setup, *program.resolve("magic"), body).value();
    REQUIRE(magic.diff);
    CHECK(magic.diff->byte_exact);
}
