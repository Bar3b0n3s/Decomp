// Translation units (analysis/units.hpp, project/units.hpp): one per object file of the link, derived
// from the PDB's modules, a link map's object files or the analysis; units.txt; per-unit progress.

#include "analysis/program.hpp"
#include "analysis/units.hpp"
#include "core/fs.hpp"
#include "formats/pdb.hpp"
#include "llvm_fixture.hpp"
#include "project/project.hpp"
#include "project/units.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

using namespace decomp;

TEST_CASE("units from a PDB whose modules list several files: each unit's source is its own") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    // first.cpp's line information names its header too (an inline function's code), so its module lists
    // two files; second.cpp's files follow them in the PDB.
    auto tmp = fs::TempDir::create("decomp-units-files").value();
    REQUIRE(fs::write_text(tmp.path() / "shared.h", "#pragma once\ninline int twice(volatile int* p) { return *p * 2; }\n"));
    REQUIRE(fs::write_text(tmp.path() / "first.cpp",
                           "#include \"shared.h\"\nvolatile int g_first = 2;\nint second_value(int);\n"
                           "extern \"C\" int entry() { return twice(&g_first) + second_value(1); }\n"));
    REQUIRE(fs::write_text(tmp.path() / "second.cpp", "int second_value(int x) { return x + 40; }\n"));
    for (const Arch arch : {Arch::x86, Arch::x64}) {
        CAPTURE(to_string(arch));
        const auto dir = tmp.path() / std::string(to_string(arch));
        const auto exe = test::build_program(arch, *tools, dir, {tmp.path() / "first.cpp", tmp.path() / "second.cpp"}, "files");
        REQUIRE(exe);
        const Program p = Program::open(*exe).value();
        const auto pdb = pdb::Reader::load(dir / "files.pdb").value();
        REQUIRE(pdb.modules().size() >= 2);
        CHECK(pdb.modules()[0].source_files.size() >= 2);
        auto names = [](const pdb::Module& m) {
            std::set<std::string> out;
            for (const auto& f : m.source_files) out.insert(fs::to_utf8(fs::from_utf8(f).filename()));
            return out;
        };
        CHECK(names(pdb.modules()[0]) == std::set<std::string>{"first.cpp", "shared.h"});
        CHECK(names(pdb.modules()[1]) == std::set<std::string>{"second.cpp"});
        const UnitLayout layout = units_from_pdb(pdb, p.image(), p.symbols());
        REQUIRE(layout.units.size() >= 2);
        CHECK(layout.units[0].source == "src/first.cpp");
        CHECK(layout.units[1].source == "src/second.cpp");
    }
}

TEST_CASE("units from a PDB: its module list in link order, every function in its module") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        const std::string a(arch);
        const Program p = Program::open(test::fixture(a + "/basic.exe")).value();
        const auto pdb = pdb::Reader::load(test::fixture(a + "/basic.pdb")).value();
        const UnitLayout layout = units_from_pdb(pdb, p.image(), p.symbols());

        REQUIRE(layout.units.size() == pdb.modules().size());
        for (usize i = 0; i < pdb.modules().size(); ++i) CHECK(layout.units[i].name == unit_name(pdb.modules()[i]));
        CHECK(layout.units[0].name == "basic.obj");
        CHECK(layout.units[0].kind == UnitKind::code);
        CHECK(layout.units[0].origin == UnitOrigin::pdb);
        CHECK(layout.units[0].source == "src/basic.cpp");  // the source file the module's line information names
        CHECK(layout.units[1].name == "other.obj");
        CHECK(layout.units[1].source == "src/other.cpp");
        CHECK(layout.units[2].name == "kernel32:kernel32.dll");  // an import library's member, as link maps name it
        CHECK(layout.units[2].kind == UnitKind::import);
        CHECK(layout.units[3].name == "Import:kernel32.dll");
        CHECK(layout.units[3].kind == UnitKind::import);
        CHECK(layout.units[4].name == "* Linker *");
        CHECK(layout.units[4].kind == UnitKind::linker);
        CHECK(layout.units[4].source.empty());

        // Every function is in a unit, and every procedure in the module whose symbols record it.
        for (const Symbol* f : p.symbols().functions()) {
            CAPTURE(f->name);
            CHECK_FALSE(layout.unit_of(f->va).empty());
        }
        for (const auto& proc : pdb.procedures()) {
            CAPTURE(proc.name);
            CHECK(layout.unit_of(p.image().image_base() + proc.rva) == unit_name(pdb.modules()[proc.module]));
        }
        CHECK(layout.unit_of(p.symbols().find("other_value")->va) == "other.obj");
        CHECK(layout.unit_of(p.symbols().find("?g_counter@@3HA")->va) == "basic.obj");  // data too
        const auto basic = layout.functions("basic.obj", p.symbols());
        CHECK(basic.size() >= 8);
        CHECK(std::ranges::is_sorted(basic, {}, &Symbol::va));
    }
}

TEST_CASE("unit names and kinds as link maps write them") {
    CHECK(normalize_unit_name("LIBC.LIB:printf.obj") == "LIBC:printf.obj");
    CHECK(normalize_unit_name("libcmt.lib:crt0.obj") == "libcmt:crt0.obj");
    CHECK(normalize_unit_name("main.obj") == "main.obj");
    CHECK(normalize_unit_name("Import:KERNEL32.dll") == "Import:KERNEL32.dll");
    CHECK(unit_kind_of("main.obj") == UnitKind::code);
    CHECK(unit_kind_of("LIBC:printf.obj") == UnitKind::library);
    CHECK(unit_kind_of("kernel32:KERNEL32.dll") == UnitKind::import);
    CHECK(unit_kind_of("Import:KERNEL32.dll") == UnitKind::import);
    CHECK(unit_kind_of("* Linker *") == UnitKind::linker);
    CHECK(unit_kind_of("<linker-defined>") == UnitKind::linker);

    pdb::Module m;
    m.name = "f:\\vctools\\crt\\build\\intel\\mt_obj\\printf.obj";
    m.object_name = "C:\\VC98\\LIB\\LIBCMT.lib";
    CHECK(unit_name(m) == "LIBCMT:printf.obj");
    m.name = "C:\\game\\Release\\player.obj";
    m.object_name = m.name;
    CHECK(unit_name(m) == "player.obj");
}

TEST_CASE("units.txt lines") {
    const Unit linker{"* Linker *", UnitKind::linker, "", UnitOrigin::pdb};
    CHECK(project::format_unit_line(linker) == "\"* Linker *\" kind=linker origin=pdb");
    const Unit back = project::parse_unit_line(project::format_unit_line(linker)).value();
    CHECK(back.name == linker.name);
    CHECK(back.kind == UnitKind::linker);
    CHECK(back.origin == UnitOrigin::pdb);

    const Unit code{"player.obj", UnitKind::code, "src/game/player.cpp", UnitOrigin::map};
    CHECK(project::format_unit_line(code) == "player.obj source=src/game/player.cpp origin=map");
    // A line written by hand: no origin, the kind from the name.
    const Unit hand = project::parse_unit_line("LIBC:printf.obj").value();
    CHECK(hand.origin == UnitOrigin::user);
    CHECK(hand.kind == UnitKind::library);
    const Unit spaced = project::parse_unit_line("my.obj source=\"src/my file.c\"").value();
    CHECK(spaced.source == "src/my file.c");
    CHECK_FALSE(project::parse_unit_line("a.obj kind=bogus"));
    CHECK_FALSE(project::parse_unit_line("a.obj color=red"));
    CHECK_FALSE(project::parse_unit_line("a.obj source"));
    // Sources stay C and C++ files under src/: sessions write them.
    for (const char* good : {"src/a.c", "src/game/player.cpp", "src/x.CC", "src/my file.cxx"}) {
        CAPTURE(good);
        CHECK(project::valid_unit_source(good));
    }
    for (const char* bad : {"a.cpp", "src/../decomp.json", "src/a/../../x.cpp", "src/./a.cpp", "src//a.cpp", "/src/a.cpp", "src\\a.cpp",
                            "C:/src/a.cpp", "src/a.h", "src/a", "src/functions/add_401060.cpp", "src/a.cpp/"}) {
        CAPTURE(bad);
        CHECK_FALSE(project::valid_unit_source(bad));
    }
    const auto escaping = project::parse_unit_line("a.obj source=src/../../etc/a.cpp");
    REQUIRE_FALSE(escaping);
    CHECK(escaping.error().message.find("not a C or C++ file under src/") != std::string::npos);
    // What write_project_file() may write.
    CHECK(project::writable_project_path("src/a.cpp"));
    CHECK(project::writable_project_path("src/functions/../a.cpp"));
    CHECK_FALSE(project::writable_project_path("src/../../a.cpp"));
    CHECK_FALSE(project::writable_project_path(".decomp/changes.jsonl"));
    CHECK_FALSE(project::writable_project_path("src/../.decomp/x"));
    CHECK_FALSE(project::writable_project_path(std::filesystem::current_path() / "a.cpp"));
    CHECK_FALSE(project::writable_project_path(""));
    CHECK_FALSE(project::writable_project_path("."));
}

TEST_CASE("unit sources from the PDB's paths stay under src/") {
    const Program p = Program::open(test::fixture("x86/basic.exe")).value();
    auto f = p.symbols().functions();
    REQUIRE(f.size() >= 4);
    SymbolDb db;
    UnitLayout layout;
    const char* names[] = {"a.obj", "b.obj", "c.obj", "d.obj"};
    for (usize i = 0; i < 4; ++i) {
        layout.units.push_back(Unit{names[i], UnitKind::code, "", UnitOrigin::pdb});
        Symbol s = *f[i];
        s.object = names[i];
        db.add(std::move(s));
    }
    // Four util.cpp files: the directories that tell them apart are kept, but not `..`, `.` or drives,
    // and characters a file name cannot hold are replaced.
    const std::map<std::string, std::string> sources = {{"a.obj", "..\\common\\util.cpp"},
                                                        {"b.obj", "C:\\game\\util.cpp"},
                                                        {"c.obj", "./engine/util.cpp"},
                                                        {"d.obj", "/build/<gen>/util.cpp"}};
    assign_sources(layout, db, sources);
    std::set<std::string> paths;
    for (const auto& u : layout.units) {
        CAPTURE(u.source);
        CHECK(project::valid_unit_source(u.source));
        paths.insert(u.source);
    }
    CHECK(paths == std::set<std::string>{"src/common/util.cpp", "src/game/util.cpp", "src/engine/util.cpp", "src/_gen_/util.cpp"});
}

TEST_CASE("units from object files: a function without one joins the unit around it") {
    const Program p = Program::open(test::fixture("x86/basic.exe")).value();
    auto f = p.symbols().functions();
    REQUIRE(f.size() >= 6);
    SymbolDb db;
    auto add = [&](usize i, std::string object) {
        Symbol s = *f[i];
        s.object = std::move(object);
        s.source = SymbolSource::map;
        db.add(std::move(s));
    };
    add(0, "a.obj");
    add(1, "");
    add(2, "a.obj");
    add(3, "LIBC.LIB:b.obj");  // a library match's name, normalized
    add(4, "");
    const UnitLayout layout = units_from_objects(db, p.image());
    REQUIRE(layout.units.size() == 2);
    CHECK(layout.units[0].name == "a.obj");
    CHECK(layout.units[0].origin == UnitOrigin::map);
    CHECK(layout.units[1].name == "LIBC:b.obj");
    CHECK(layout.units[1].kind == UnitKind::library);
    CHECK(layout.unit_of(f[1]->va) == "a.obj");
    CHECK(layout.unit_of(f[4]->va) == "LIBC:b.obj");  // after the last one: the unit before it
    // a.obj holds C++ functions (decorated names), so its source is C++.
    CHECK(layout.units[0].source == "src/a.cpp");
}

TEST_CASE("projects derive their units when created, and keep the ones the user made") {
    auto dir = fs::TempDir::create("decomp-units").value();
    auto p = project::Project::init(dir.path() / "p", test::fixture("x86/basic.exe"), std::nullopt, "clang-cl-x86").value();
    const auto units = project::load_units(p).value();
    REQUIRE(units.size() == 5);
    CHECK(units[0].name == "basic.obj");
    CHECK(units[0].origin == UnitOrigin::pdb);
    const auto text = fs::read_text(dir.path() / "p" / "units.txt").value();
    CHECK(text.find("basic.obj source=src/basic.cpp origin=pdb\n") != std::string::npos);
    CHECK(fs::read_text(dir.path() / "p" / "symbols.txt").value().find(" obj=other.obj") != std::string::npos);

    const Program program = p.open_program().value();
    const u64 other = program.symbols().find("other_value")->va;
    CHECK(program.symbols().at(other)->object == "other.obj");
    auto progress = project::compute_unit_progress(units, program.symbols(), *p.function_infos());
    REQUIRE(progress.size() == 5);  // every function is in a unit: no "(no unit)" row
    CHECK(progress[1].functions == 1);
    CHECK(progress[0].functions >= 8);

    // Derived again only on request; a unit added by hand stays, with its functions.
    CHECK_FALSE(project::derive_project_units(p, false));
    std::string edited = text + "mine.obj source=src/mine.cpp\n";
    REQUIRE(fs::write_text(dir.path() / "p" / "units.txt", edited));
    REQUIRE(p.assign_objects({{other, "mine.obj"}}).value() == 1);
    auto again = project::derive_project_units(p, true).value();
    CHECK(again.first.from == UnitOrigin::pdb);
    const auto after = project::load_units(p).value();
    CHECK(after.size() == 6);
    auto mine = std::ranges::find(after, std::string("mine.obj"), &Unit::name);
    REQUIRE(mine != after.end());
    CHECK(mine->origin == UnitOrigin::user);
    CHECK(p.open_program().value().symbols().at(other)->object == "mine.obj");
}

TEST_CASE("without a PDB or a map, the source file a function's strings name ties its unit") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    auto dir = fs::TempDir::create("decomp-unitguess").value();
    const auto a = dir.path() / "alpha.c";
    const auto b = dir.path() / "beta.c";
    // The reporter is called through a pointer: a call clang could see into would carry the file name
    // in a specialized copy of it instead.
    REQUIRE(fs::write_text(a, "extern void (*volatile g_report)(const char* file, int line);\n"
                              "__declspec(noinline) int one(int x) { if (x < 0) g_report(__FILE__, __LINE__); return x * 2; }\n"
                              "__declspec(noinline) int two(int x) { if (x > 100) g_report(__FILE__, __LINE__); return x + 1; }\n"));
    REQUIRE(fs::write_text(b, "int one(int x);\nint two(int x);\nvoid (*volatile g_report)(const char* file, int line);\n"
                              "__declspec(noinline) int three(int x) { if (x == 7) g_report(__FILE__, __LINE__); return x - 3; }\n"
                              "__declspec(noinline) int four(int x) { if (x == 9) g_report(__FILE__, __LINE__); return x ^ 5; }\n"
                              "int entry(void) { return one(1) + two(2) + three(3) + four(4); }\n"));
    auto exe = test::build_program(Arch::x86, *tools, dir.path() / "out", {a, b}, "guess");
    REQUIRE(exe);
    OpenOptions options;
    options.use_pdb = false;
    const Program p = Program::open(*exe, options).value();
    const UnitLayout found = units_by_analysis(p);
    const Program named = Program::open(*exe).value();
    auto unit_of = [&](const char* name) {
        const Symbol* s = named.symbols().find(name);
        REQUIRE(s);
        return std::string(found.unit_of(s->va));
    };
    CHECK(unit_of("_one") == "alpha.obj");
    CHECK(unit_of("_two") == "alpha.obj");
    CHECK(unit_of("_entry") == "beta.obj");  // names no file: with the functions before it
    CHECK(unit_of("_three") == "beta.obj");
    CHECK(unit_of("_four") == "beta.obj");
    const Unit* alpha = found.find("alpha.obj");
    REQUIRE(alpha);
    CHECK(alpha->source == "src/alpha.c");
    CHECK(alpha->origin == UnitOrigin::analysis);

    // Measured against the PDB's modules: the two units are found exactly.
    const auto pdb = pdb::Reader::load(dir.path() / "out" / "guess.pdb").value();
    const UnitLayout truth = units_from_pdb(pdb, named.image(), p.symbols());
    const UnitComparison c = compare_units(truth, found, p.symbols());
    CHECK(c.truth_units == 2);
    CHECK(c.exact_units == 2);
    CHECK(c.boundary_recall() == 1.0);
    CHECK(c.same_names);
}
