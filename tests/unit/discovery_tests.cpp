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
#include <map>

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

namespace {

// The idiom fixtures' functions: every function f has a label f_end in the map file.
std::vector<FunctionBounds> idiom_truth(const char* arch) {
    const auto m = map::load(test::fixture(std::string(arch) + "/idioms.map")).value();
    std::map<std::string, u64> by_name;
    for (const auto& e : m.entries) by_name[e.name] = e.va;
    std::vector<FunctionBounds> out;
    for (const auto& [name, va] : by_name)
        if (auto end = by_name.find(name + "_end"); end != by_name.end()) out.push_back({va, end->second, name});
    std::ranges::sort(out, {}, &FunctionBounds::start);
    return out;
}

u64 idiom(const std::vector<FunctionBounds>& truth, std::string_view name) {
    auto it = std::ranges::find(truth, name, &FunctionBounds::name);
    REQUIRE(it != truth.end());
    return it->start;
}

u64 idiom_end(const std::vector<FunctionBounds>& truth, std::string_view name) {
    auto it = std::ranges::find(truth, name, &FunctionBounds::name);
    REQUIRE(it != truth.end());
    return it->end;
}

} // namespace

TEST_CASE("discovery on MSVC and VC6 code layouts: tables after the code, byte tables, calls that do not return, tail calls") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        const Program p = Program::open(test::fixture(std::string(arch) + "/idioms.exe")).value();
        const auto truth = idiom_truth(arch);
        REQUIRE(truth.size() == (std::string_view(arch) == "x86" ? 11u : 9u));
        const auto c = compare_bounds(truth, function_bounds(p.symbols(), p.image()), p.image(), p.decoder());
        for (const auto& m : c.mismatches) MESSAGE(to_string(m.kind), " ", m.name, " found end ", m.found_end, " truth end ", m.truth_end);
        CHECK(c.exact == truth.size());
        CHECK(c.extra == 0);
    }
    // The x86 fixture has no relocations: the callback is found through the pointer to it in the data.
    const Program x86 = Program::open(test::fixture("x86/idioms.exe")).value();
    CHECK_FALSE(x86.image().has_relocations());
    const auto truth = idiom_truth("x86");
    const auto found = discover_functions(x86.image(), x86.decoder(), SymbolDb::from_pe(x86.image()));
    auto evidence = [&](std::string_view name) {
        auto f = std::ranges::find(found.functions, idiom(truth, name), &DiscoveredFunction::start);
        REQUIRE(f != found.functions.end());
        return f->evidence;
    };
    CHECK(evidence("_callback") == FunctionEvidence::address);
    CHECK(evidence("_tail_target") == FunctionEvidence::tail_jump);
    CHECK(evidence("_unreferenced") == FunctionEvidence::gap);
    CHECK(evidence("_switch_one_level") == FunctionEvidence::call);
    auto helper = std::ranges::find(found.functions, idiom(truth, "_exit_helper"), &DiscoveredFunction::start);
    REQUIRE(helper != found.functions.end());
    CHECK(helper->noreturn);
}

TEST_CASE("cross references go through incremental-linking thunks and include pointers stored in data") {
    const Program p = Program::open(test::fixture("x86/idioms.exe")).value();
    const auto truth = idiom_truth("x86");
    const u64 entry = idiom(truth, "_entry"), one = idiom(truth, "_switch_one_level"), callback = idiom(truth, "_callback");
    // The calls go to thunks; the index has them against the functions behind the thunks too.
    CHECK(p.callers_of(one) == std::vector<u64>{entry});
    const auto refs = p.xrefs_to(one);
    auto call = std::ranges::find_if(refs, [](const Xref& x) { return x.kind == XrefKind::call; });
    REQUIRE(call != refs.end());
    CHECK(call->function == entry);
    REQUIRE(call->via != 0);
    CHECK(p.image().read<u8>(call->via) == u8{0xE9});
    CHECK(std::ranges::any_of(p.xrefs_to(call->via), [&](const Xref& x) { return x.from == call->from && x.via == 0; }));
    // The callback is reached only through a pointer in the data, to its thunk.
    const auto m = map::load(test::fixture("x86/idioms.map")).value();
    auto table = std::ranges::find(m.entries, std::string("_callbacks"), &map::Entry::name);
    REQUIRE(table != m.entries.end());
    const auto data = p.xrefs_to(callback);
    auto pointer = std::ranges::find_if(data, [](const Xref& x) { return x.kind == XrefKind::pointer; });
    REQUIRE(pointer != data.end());
    CHECK(pointer->from == table->va);
    CHECK(pointer->function == 0);
    CHECK(pointer->via != 0);
    CHECK(std::ranges::contains(p.data_pointers(), std::pair{table->va, pointer->via}));
}

TEST_CASE("switch tables: the bound from the check, tables after the code, byte index tables, RVA entries") {
    struct Case {
        const char* arch;
        const char* one;
        const char* two;
        TableEncoding encoding;
    };
    for (const Case& k : {Case{"x86", "_switch_one_level", "_switch_two_level", TableEncoding::absolute},
                          Case{"x64", "switch_rva", "switch_rva_two_level", TableEncoding::rva}}) {
        CAPTURE(k.arch);
        const Program p = Program::open(test::fixture(std::string(k.arch) + "/idioms.exe")).value();
        const auto truth = idiom_truth(k.arch);

        const auto one = p.function_extent(idiom(truth, k.one)).value();
        REQUIRE(one.jump_tables.size() == 1);
        const JumpTable& t = one.jump_tables[0];
        CHECK(t.encoding == k.encoding);
        CHECK(t.bounded);
        CHECK(t.targets.size() == 5);
        CHECK(t.inside_code);
        CHECK(t.index_entries == 0);
        // The table is data at the end of the function, not instructions.
        REQUIRE(one.data_ranges.size() == 1);
        CHECK(one.data_ranges[0] == std::pair{t.table_va, t.table_va + 5 * 4});
        CHECK(one.data_ranges[0].second == one.end);

        const auto two = p.function_extent(idiom(truth, k.two)).value();
        REQUIRE(two.jump_tables.size() == 1);
        const JumpTable& u = two.jump_tables[0];
        CHECK(u.bounded);
        CHECK(u.targets.size() == 4);
        CHECK(u.index_entries == 10);
        CHECK(u.index_va == u.table_va + 16);
        CHECK(two.data_ranges.size() == 2);
        CHECK(two.end == u.index_va + 10);
        // The instructions skip the tables.
        const auto list = p.function_instructions(two).value();
        CHECK(std::ranges::none_of(list, [&](const x86::Instruction& ins) { return ins.address >= u.table_va; }));

        // No bounds check (the default cannot happen): the byte table is read up to the padding after
        // it, and the null entry is a case that cannot happen.
        const char* unchecked = std::string_view(k.arch) == "x86" ? "_switch_unchecked" : "switch_rva_unchecked";
        const auto three = p.function_extent(idiom(truth, unchecked)).value();
        REQUIRE(three.jump_tables.size() == 1);
        const JumpTable& w = three.jump_tables[0];
        CHECK_FALSE(w.bounded);
        REQUIRE(w.targets.size() == 4);
        CHECK(w.targets[3] == 0);
        CHECK(w.index_entries == 17);
        CHECK(w.index_va == w.table_va + 16);
        CHECK(three.end == idiom_end(truth, unchecked));
    }
}

TEST_CASE("x64 unwind data split into a chained entry is one function") {
    const Program p = Program::open(test::fixture("x64/idioms.exe")).value();
    const auto truth = idiom_truth("x64");
    const u64 chained = idiom(truth, "chained");
    const auto& runtime = p.image().runtime_functions();
    const auto part = std::ranges::find_if(runtime, [&](const pe::RuntimeFunction& f) { return f.chained_to != 0; });
    REQUIRE(part != runtime.end());
    CHECK(p.image().image_base() + part->chained_to == chained);
    const Symbol* s = p.symbols().at(chained);
    REQUIRE(s);
    CHECK(s->size == 20);
    CHECK_FALSE(p.symbols().at(p.image().image_base() + part->begin_rva));
}
