// Function discovery without a PDB, measured against the PDB (analysis/discovery.hpp, analysis/bounds.hpp),
// and the link map reader (formats/map.hpp).

#include "analysis/bounds.hpp"
#include "analysis/discovery.hpp"
#include "analysis/program.hpp"
#include "core/fs.hpp"
#include "formats/map.hpp"
#include "project/project.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <format>

using namespace decomp;

namespace {

Program open_without_pdb(const char* arch) {
    OpenOptions options;
    options.use_pdb = false;
    return Program::open(test::fixture(std::string(arch) + "/basic.exe"), options).value();
}

} // namespace

TEST_CASE("discovery finds every function of the fixtures, with exact bounds, without their PDB") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        const Program p = open_without_pdb(arch);
        CHECK(p.pdb_status() == PdbStatus::absent);
        const auto truth = pdb_function_bounds(test::fixture(std::string(arch) + "/basic.pdb"), p.image().image_base(), p.image()).value();
        REQUIRE(truth.size() >= 12);
        const auto found = function_bounds(p.symbols(), p.image());
        const auto c = compare_bounds(truth, found, p.image(), p.decoder());
        for (const auto& m : c.mismatches) MESSAGE(to_string(m.kind), " ", m.start, " ", m.name);
        CHECK(c.exact == c.truth);
        // The PDB has no procedure for the linker's import thunk; discovery names it after the import.
        for (const auto& m : c.mismatches) {
            CHECK(m.kind == BoundsMismatch::Kind::extra);
            CHECK(p.thunk_destination(m.start));
            CHECK(p.symbols().at(m.start)->name == "ExitProcess");
        }
        // Functions nothing names get sub_<va>; the entry point keeps its name.
        CHECK(p.symbols().find("entry"));
        for (const auto& f : found)
            if (!p.symbols().at(f.start)->name.empty()) CHECK(p.symbols().at(f.start)->source != SymbolSource::pdb);
    }
}

TEST_CASE("discovery reports how it found each function and which never return") {
    const Program bare = open_without_pdb("x86");
    const auto result = discover_functions(bare.image(), bare.decoder(), SymbolDb::from_pe(bare.image()));
    REQUIRE_FALSE(result.functions.empty());
    CHECK(std::ranges::is_sorted(result.functions, {}, &DiscoveredFunction::start));
    const auto entry = std::ranges::find(result.functions, bare.image().entry_point(), &DiscoveredFunction::start);
    REQUIRE(entry != result.functions.end());
    CHECK(entry->evidence == FunctionEvidence::entry);
    // add() is reached by calls from the entry point.
    const u64 add = Program::open(test::fixture("x86/basic.exe")).value().symbols().find("?add@@YAHHH@Z")->va;
    const auto f = std::ranges::find(result.functions, add, &DiscoveredFunction::start);
    REQUIRE(f != result.functions.end());
    CHECK(f->evidence == FunctionEvidence::call);
    CHECK_FALSE(f->noreturn);
    CHECK(result.passes >= 1);
    CHECK(result.instructions > 50);
}

TEST_CASE("a project made without a PDB lists the discovered functions with their sizes") {
    auto dir = fs::TempDir::create("decomp-discovery").value();
    std::filesystem::copy_file(test::fixture("x86/basic.exe"), dir.path() / "basic.exe");
    auto project = project::Project::init(dir.path() / "p", dir.path() / "basic.exe", std::nullopt, "").value();
    usize sized = 0;
    for (const auto& s : project.symbols())
        if (s.kind == SymbolKind::function && s.size > 0) ++sized;
    CHECK(sized >= 13);
    // Opening the project again does not discover anything new: symbols.txt is the list.
    const auto program = project.open_program().value();
    CHECK(function_bounds(program.symbols(), program.image()).size() == sized);
}

TEST_CASE("padding: int3, nop, zero fill and the filler instructions compilers align with") {
    // Assemble a tiny image in memory: code followed by fillers.
    const Program p = open_without_pdb("x86");
    const x86::Decoder& d = p.decoder();
    // Find a run of int3 padding after some function in the fixture.
    const auto found = function_bounds(p.symbols(), p.image());
    usize checked = 0;
    for (usize i = 0; i + 1 < found.size(); ++i) {
        const u64 end = found[i].end, next = found[i + 1].start;
        if (next <= end) continue;
        CHECK(end + padding_length(p.image(), d, end, next) == next);
        ++checked;
    }
    CHECK(checked > 3);
    CHECK(padding_length(p.image(), d, found.front().start, found.front().end) == 0);
}

TEST_CASE("bounds comparison: exact, wrong ends, missed and extra functions") {
    const Program p = open_without_pdb("x86");
    const auto found = function_bounds(p.symbols(), p.image());
    REQUIRE(found.size() >= 3);
    std::vector<FunctionBounds> truth(found.begin(), found.end());
    auto same = compare_bounds(truth, found, p.image(), p.decoder());
    CHECK(same.exact == truth.size());
    CHECK(same.exact_rate() == doctest::Approx(1.0));

    std::vector<FunctionBounds> analysis(found.begin(), found.end());
    analysis[0].end -= 1;               // a wrong end
    analysis.erase(analysis.begin() + 1);  // a missed function
    analysis.push_back({found[2].start + 1, found[2].start + 2, "split"});  // an extra start
    std::ranges::sort(analysis, {}, &FunctionBounds::start);
    const auto c = compare_bounds(truth, analysis, p.image(), p.decoder());
    CHECK(c.exact == truth.size() - 2);
    CHECK(c.start_only == 1);
    CHECK(c.missed == 1);
    CHECK(c.extra == 1);
    CHECK(c.mismatches.size() == 3);
    CHECK(std::ranges::is_sorted(c.mismatches, {}, &BoundsMismatch::start));
    const Json j = to_json(c, 2);
    CHECK(j["mismatches"].size() == 2);
    CHECK(j["exact"] == truth.size() - 2);

    // Truth without ends (a map file): an end is exact when only padding follows up to the next start.
    std::vector<FunctionBounds> starts = truth;
    for (auto& t : starts) t.end = 0;
    const auto by_start = compare_bounds(starts, found, p.image(), p.decoder());
    CHECK_FALSE(by_start.truth_has_ends);
    CHECK(by_start.exact == starts.size());
    std::vector<FunctionBounds> short_ends(found.begin(), found.end());
    short_ends[0].end -= 1;
    CHECK(compare_bounds(starts, short_ends, p.image(), p.decoder()).exact == starts.size() - 1);
}

TEST_CASE("link maps: link.exe and lld-link formats, publics and statics") {
    const char* msvc = R"( TestApp

 Timestamp is 3a1f3b2c (Fri Nov 24 23:05:16 2000)

 Preferred load address is 00400000

 Start         Length     Name                   Class
 0001:00000000 00003e6aH .text                   CODE
 0002:00000000 00000488H .rdata                  DATA

  Address         Publics by Value              Rva+Base     Lib:Object

 0001:00000000       _main                      00401000 f   main.obj
 0001:00000020       ?Foo@@YAXXZ                00401020 f   main.obj
 0001:00000040       _printf                    00401040 f   LIBC:printf.obj
 0002:00000000       ??_C@_05ABCD@hello?$AA@    00404000     main.obj
 0000:00000000       ___safe_se_handler_count   00000000     <absolute>

 entry point at        0001:00000040

 Static symbols

 0001:00000060       _helper                    00401060 f   main.obj
)";
    const auto m = map::parse(msvc).value();
    CHECK(m.module == "TestApp");
    CHECK(m.preferred_base == 0x400000);
    REQUIRE(m.sections.size() == 2);
    CHECK(m.sections[0].name == ".text");
    CHECK(m.sections[0].length == 0x3e6a);
    CHECK(m.sections[0].klass == "CODE");
    REQUIRE(m.entries.size() == 6);
    CHECK(m.has_function_flags);
    CHECK(m.entries[0].name == "_main");
    CHECK(m.entries[0].va == 0x401000);
    CHECK(m.entries[0].function);
    CHECK(m.entries[0].object == "main.obj");
    CHECK(m.entries[2].object == "LIBC:printf.obj");
    CHECK_FALSE(m.entries[3].function);
    CHECK(m.entries[4].section == 0);
    CHECK(m.entries[5].is_static);
    CHECK(m.entries[5].name == "_helper");
    REQUIRE(m.entry_point);
    CHECK(m.entry_point->second == 0x40);

    // lld-link writes no `f` flags and 16-digit addresses.
    const char* lld = R"( basic

 Timestamp is 6ac374cb (Mon Oct  5 09:58:35 2026)

 Preferred load address is 0000000000400000

 Start         Length     Name                   Class
 0001:00000000 000002deH .text                   CODE

  Address         Publics by Value              Rva+Base               Lib:Object

 0001:00000060       ?add@@YAHHH@Z              0000000000401060     basic.obj
 0002:000000a8       __imp__ExitProcess@4       00000000004020a8     kernel32:kernel32.dll
)";
    const auto l = map::parse(lld).value();
    CHECK_FALSE(l.has_function_flags);
    REQUIRE(l.entries.size() == 2);
    CHECK(l.entries[0].va == 0x401060);
    CHECK(l.entries[1].object == "kernel32:kernel32.dll");

    CHECK_FALSE(map::parse(""));
    CHECK_FALSE(map::parse("hello\nworld\n"));
}

TEST_CASE("a map file as ground truth gives starts; ends are checked against the padding up to the next start") {
    auto dir = fs::TempDir::create("decomp-discovery").value();
    // The x86 fixture's functions, written as an lld-link map.
    const Program with_pdb = Program::open(test::fixture("x86/basic.exe")).value();
    std::string text = " basic\n\n Preferred load address is 00400000\n\n Start         Length     Name                   Class\n"
                       " 0001:00000000 000002deH .text                   CODE\n\n  Address         Publics by Value              Rva+Base     Lib:Object\n\n";
    usize functions = 0;
    for (const Symbol* s : with_pdb.symbols().functions())
        if (with_pdb.image().is_code(s->va)) {  // the import thunk too, as link.exe lists it
            text += std::format(" 0001:{:08x}       {}       {:08x} f   basic.obj\n", s->va - 0x401000, s->name, s->va);
            ++functions;
        }
    REQUIRE(fs::write_text(dir.path() / "basic.map", text));
    const Program bare = open_without_pdb("x86");
    const auto truth = map_function_bounds(dir.path() / "basic.map", bare.image()).value();
    CHECK(truth.size() == functions);
    CHECK(truth.front().end == 0);
    const auto c = compare_bounds(truth, function_bounds(bare.symbols(), bare.image()), bare.image(), bare.decoder());
    CHECK(c.exact == functions);
}
