#include "analysis/annotate.hpp"
#include "analysis/cfg.hpp"
#include "analysis/demangle.hpp"
#include "analysis/program.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;

TEST_CASE("demangling and name equivalence") {
    CHECK(demangle("?add@@YAHHH@Z") == "int __cdecl add(int, int)");
    CHECK(demangle("?Hit@Player@@QAEXH@Z") == "public: void __thiscall Player::Hit(int)");
    CHECK(demangle("_Z3addii") == "add(int, int)");
    CHECK_FALSE(demangle("plain_c_name"));
    CHECK(qualified_name("?Hit@Player@@QAEXH@Z") == "Player::Hit");
    CHECK(qualified_name("?g_counter@@3HA") == "g_counter");
    CHECK(qualified_name("_ExitProcess@4") == "ExitProcess");
    CHECK(qualified_name("__imp__ExitProcess@4") == "ExitProcess");
    CHECK(qualified_name("@fast@8") == "fast");
    CHECK(qualified_name("_Z3addii") == "add");
    CHECK(undecorate("_entry") == "entry");
    CHECK(names_equivalent("?helper@@YAHH@Z", "helper"));
    CHECK(names_equivalent("__imp__ExitProcess@4", "__imp_ExitProcess"));
    CHECK_FALSE(names_equivalent("?add@@YAHHH@Z", "?sub@@YAHHH@Z"));
    CHECK(is_string_literal_symbol("??_C@_0M@LACCCNMM@hello?5world?$AA@"));
    CHECK(is_float_constant_symbol("__real@3fc00000"));
}

TEST_CASE("SymbolDb merges sources and resolves names") {
    SymbolDb db;
    db.add({0x401000, "sub_401000", "", "", SymbolKind::function, 0, SymbolSource::analysis, false, {}});
    db.add({0x401000, "?add@@YAHHH@Z", "", "add", SymbolKind::function, 15, SymbolSource::pdb_public, false, {}});
    const Symbol* s = db.at(0x401000);
    REQUIRE(s);
    CHECK(s->name == "?add@@YAHHH@Z");
    CHECK(s->size == 15);
    CHECK(s->display == "int __cdecl add(int, int)");
    CHECK(std::ranges::find(s->aliases, "sub_401000") != s->aliases.end());
    // A weaker source does not take over, it becomes an alias.
    db.add({0x401000, "add_alias", "", "", SymbolKind::function, 0, SymbolSource::analysis, false, {}});
    CHECK(db.at(0x401000)->name == "?add@@YAHHH@Z");
    CHECK(db.find("add") == db.at(0x401000));
    CHECK(db.find("?add@@YAHHH@Z") == db.at(0x401000));
    CHECK(db.find("add_alias") == db.at(0x401000));
    CHECK(db.containing(0x40100E) == db.at(0x401000));
    CHECK(db.containing(0x40100F) == nullptr);
    db.rename(0x401000, "my_add", SymbolSource::user);
    CHECK(db.at(0x401000)->name == "my_add");
    CHECK(db.find("my_add"));
}

TEST_CASE("Program loads the fixture with its PDB and resolves names") {
    auto p = Program::open(test::fixture("x86/basic.exe")).value();
    REQUIRE(p.pdb_path());
    CHECK(p.resolve("add") == 0x401060u);
    CHECK(p.resolve("?add@@YAHHH@Z") == 0x401060u);
    CHECK(p.resolve("Player::Hit") == 0x401000u);
    CHECK(p.resolve("helper") == 0x401160u);  // static function, PDB name only
    CHECK(p.resolve("0x401080") == 0x401080u);
    CHECK_FALSE(p.resolve("does_not_exist"));
    const Symbol* helper = p.symbols().at(0x401160);
    REQUIRE(helper);
    CHECK(helper->is_static);
    const Symbol* counter = p.symbols().at(0x403000);
    REQUIRE(counter);
    CHECK(counter->kind == SymbolKind::data);
    CHECK(counter->name == "?g_counter@@3HA");
    CHECK(p.symbols().at(0x402010)->kind == SymbolKind::string);
    CHECK(p.symbols().at(0x402004)->kind == SymbolKind::float_const);
    CHECK(p.describe_address(0x403008) == "int *g_table+0x4");
}

TEST_CASE("function extents, jump tables and xrefs") {
    auto p = Program::open(test::fixture("x86/basic.exe")).value();
    auto ext = p.function_extent(0x4010F0).value();  // dispatch
    CHECK(ext.from_symbol);
    CHECK(ext.end == 0x40115C);
    REQUIRE(ext.jump_tables.size() == 1);
    CHECK(ext.jump_tables[0].table_va == 0x40201C);
    CHECK(ext.jump_tables[0].targets.size() == 6);
    CHECK_FALSE(ext.jump_tables[0].inside_code);

    auto callers = p.callers_of(0x401060);  // add
    CHECK(std::ranges::find(callers, 0x4010F0u) != callers.end());  // dispatch
    CHECK(std::ranges::find(callers, 0x4011E0u) != callers.end());  // entry
    auto reads = p.xrefs_to(0x403000);
    CHECK(reads.size() >= 3);
}

TEST_CASE("recursive descent finds bounds without a symbol size") {
    auto p = Program::open(test::fixture("x86/basic.exe")).value();
    // Drop sizes to force recursive descent; sum_array has a loop.
    Symbol s = *p.symbols().at(0x401080);
    p.symbols().remove(0x401080);
    s.size = 0;
    p.symbols().add(s);
    auto ext = p.function_extent(0x401080).value();
    CHECK_FALSE(ext.from_symbol);
    CHECK(ext.end == 0x4010EB);
}

TEST_CASE("CFG blocks, edges and loops") {
    auto p = Program::open(test::fixture("x86/basic.exe")).value();
    auto ins = p.function_instructions(0x401080).value();  // sum_array
    auto cfg = build_cfg(ins);
    CHECK(cfg.blocks.size() >= 3);
    auto loops = std::ranges::count_if(cfg.blocks, &BasicBlock::loop_header);
    CHECK(loops >= 1);
    CHECK(cfg.blocks[0].first == 0);
    for (const auto& b : cfg.blocks) CHECK(b.first <= b.last);

    auto dispatch = p.function_extent(0x4010F0).value();
    auto dins = p.function_instructions(dispatch).value();
    auto dcfg = build_cfg(dins, dispatch.jump_tables);
    auto jmp = instruction_index(dins, 0x4010FE).value();
    CHECK(dcfg.blocks[dcfg.block_of[jmp]].successors.size() == 6);
}

TEST_CASE("annotated listing symbolizes operands and names frame slots") {
    auto p = Program::open(test::fixture("x86/basic.exe")).value();
    auto fn = annotate_function(p, 0x4010F0).value();
    CHECK(fn.display == "int __cdecl dispatch(int, int)");
    auto text = to_text(fn);
    CHECK(text.find("jmp dword ptr [ecx*4+switch_table_40201c]") != std::string::npos);
    CHECK(text.find("switch: 6 cases") != std::string::npos);
    CHECK(text.find("call add") != std::string::npos);
    CHECK(text.find("sub eax, dword ptr [g_counter]") != std::string::npos);
    CHECK(text.find("push g_table") != std::string::npos);
    CHECK(text.find("; arg_0") != std::string::npos);
    CHECK(text.find("tail call") != std::string::npos);
    CHECK(std::ranges::any_of(fn.callees, [](const Reference& r) { return r.display == "other_value"; }));

    auto msg = to_text(annotate_function(p, 0x401170).value());
    CHECK(msg.find("\"hello world\"") != std::string::npos);
    auto scale = to_text(annotate_function(p, 0x401180).value());
    CHECK(scale.find("1.5f") != std::string::npos);

    auto j = to_json(fn);
    CHECK(j["lines"].size() == fn.lines.size());
    CHECK(j["callees"].size() == fn.callees.size());
}
