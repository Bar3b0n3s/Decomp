// MSVC run-time type information and vftables (analysis/rtti.hpp), checked against the PDB of the
// fixture they come from (tests/fixtures/src/rtti.cpp).

#include "analysis/annotate.hpp"
#include "analysis/program.hpp"
#include "analysis/rtti.hpp"
#include "core/fs.hpp"
#include "project/analyze.hpp"
#include "project/project.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;

TEST_CASE("RTTI: classes, their bases and vftables, named as the PDB names them") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        const Program with_pdb = Program::open(test::fixture(std::string(arch) + "/rtti.exe")).value();
        REQUIRE(with_pdb.pdb_status() == PdbStatus::matched);
        const pe::Image& image = with_pdb.image();
        const RttiInfo rtti = find_rtti(image);
        std::vector<std::string> names;
        for (const auto& c : rtti.classes) names.push_back(c.name);
        CHECK(names == std::vector<std::string>{"Base", "Middle", "Named", "Unit", "game::Shape", "game::Square"});
        CHECK(rtti.find("Named")->is_struct);
        CHECK_FALSE(rtti.find("Unit")->is_struct);
        CHECK(rtti.find("Square@game@@") == rtti.find("game::Square"));

        // Multiple inheritance: a vftable for each base with one, named after it.
        const RttiClass* unit = rtti.find("Unit");
        REQUIRE(unit);
        CHECK((unit->attributes & 1) != 0);
        REQUIRE(unit->vftables.size() == 2);
        CHECK(unit->vftables[0].offset == 0);
        CHECK(unit->vftable_name(unit->vftables[0]) == "??_7Unit@@6BSquare@game@@@");
        CHECK(unit->vftable_name(unit->vftables[1]) == "??_7Unit@@6BNamed@@@");
        CHECK(unit->locator_name(unit->vftables[1]) == "??_R4Unit@@6BNamed@@@");
        std::vector<std::string> direct;
        for (const auto& b : unit->bases)
            if (b.direct) direct.push_back(b.name);
        CHECK(direct == std::vector<std::string>{"game::Square", "Named"});
        // The slots: Unit::area overrides, game::Square::sides is inherited.
        REQUIRE(unit->vftables[0].slots.size() == 2);
        CHECK(unit->vftables[0].slots[0] == with_pdb.resolve("Unit::area"));
        CHECK(unit->vftables[0].slots[1] == with_pdb.resolve("game::Square::sides"));
        REQUIRE(unit->vftables[1].slots.size() == 1);
        CHECK(unit->vftables[1].slots[0] == with_pdb.resolve("Unit::name"));
        // Single inheritance: one vftable, without a base in its name.
        const RttiClass* square = rtti.find("game::Square");
        REQUIRE(square->vftables.size() == 1);
        CHECK(square->vftable_name(square->vftables[0]) == "??_7Square@game@@6B@");
        // A virtual base.
        const RttiClass* middle = rtti.find("Middle");
        REQUIRE(middle->bases.size() == 1);
        CHECK(middle->bases[0].name == "Base");
        CHECK(middle->bases[0].pdisp >= 0);

        // Every name the RTTI gives is the PDB's name at that address.
        SymbolDb ours;
        add_rtti_symbols(ours, rtti, image.arch());
        usize compared = 0;
        for (const auto& [va, s] : ours) {
            CAPTURE(s.name);
            const Symbol* truth = with_pdb.symbols().at(va);
            REQUIRE(truth);
            CHECK((truth->name == s.name || std::ranges::contains(truth->aliases, s.name)));
            ++compared;
        }
        CHECK(compared >= 25);
    }
}

TEST_CASE("RTTI names the vftables of a target without a PDB, and listings say which slots hold a function") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        const Program with_pdb = Program::open(test::fixture(std::string(arch) + "/rtti.exe")).value();
        OpenOptions options;
        options.use_pdb = false;
        const Program p = Program::open(test::fixture(std::string(arch) + "/rtti.exe"), options).value();
        const auto vftable = p.resolve("??_7Unit@@6BNamed@@@");
        REQUIRE(vftable);
        CHECK(vftable == with_pdb.resolve("??_7Unit@@6BNamed@@@"));
        const Symbol* s = p.symbols().at(*vftable);
        REQUIRE(s);
        CHECK(s->kind == SymbolKind::data);
        CHECK(s->display.find("vftable") != std::string::npos);

        const u64 sides = *with_pdb.resolve("game::Square::sides");
        const auto fn = annotate_function(p, sides).value();
        CHECK(fn.virtual_slots == std::vector<std::string>{"slot 1 of Unit's vftable for game::Square", "slot 1 of game::Square's vftable"});
        CHECK(to_text(fn).find("; virtual:  slot 1 of game::Square's vftable") != std::string::npos);
    }
}

TEST_CASE("a project without the PDB has the RTTI's names; re-analysis gives them to a project that has none") {
    auto dir = fs::TempDir::create("decomp-rtti").value();
    const auto exe = dir.path() / "rtti.exe";  // without its PDB
    std::filesystem::copy_file(test::fixture("x86/rtti.exe"), exe);
    const Program truth = Program::open(test::fixture("x86/rtti.exe")).value();
    const u64 vftable = *truth.resolve("??_7Unit@@6BNamed@@@");
    auto p = project::Project::init(dir.path() / "p", exe, std::nullopt, "clang-cl-x86").value();
    auto name_at = [&](u64 va) {
        for (const Symbol& s : p.symbols())
            if (s.va == va) return s.name;
        return std::string();
    };
    CHECK(name_at(vftable) == "??_7Unit@@6BNamed@@@");

    // A project made before the RTTI was read.
    SymbolDb older;
    for (Symbol s : p.symbols())
        if (!s.name.starts_with("??_")) older.add(std::move(s));
    REQUIRE(p.save_symbols(older));
    CHECK(name_at(vftable).empty());
    const auto summary = project::analyze(p).value();
    CHECK(summary.added == 0);
    CHECK(summary.removed == 0);
    CHECK(name_at(vftable) == "??_7Unit@@6BNamed@@@");
}

TEST_CASE("MSVC number encoding and class names") {
    CHECK(encode_ms_number(0) == "A@");
    CHECK(encode_ms_number(1) == "0");
    CHECK(encode_ms_number(4) == "3");
    CHECK(encode_ms_number(10) == "9");
    CHECK(encode_ms_number(16) == "BA@");
    CHECK(encode_ms_number(-1) == "?0");
    CHECK(encode_ms_number(0x40) == "EA@");
    RttiBase b;
    b.decorated = "Base@@";
    b.pdisp = 0;
    b.vdisp = 4;
    b.attributes = 0x50;
    CHECK(base_descriptor_name(b) == "??_R1A@A@3FA@Base@@8");
    CHECK(class_display_name("Square@game@@") == "game::Square");
    CHECK(class_display_name("Unit@@") == "Unit");
    CHECK(class_display_name("?$vector@H@std@@") == "std::vector<int>");
}
