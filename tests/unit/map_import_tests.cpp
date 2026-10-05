// Symbols from the build's link map (SymbolDb::add_map, Program's map option) and finding a project's
// functions again with it (project/analyze.hpp).

#include "analysis/bounds.hpp"
#include "analysis/program.hpp"
#include "core/file_lock.hpp"
#include "core/fs.hpp"
#include "formats/map.hpp"
#include "project/analyze.hpp"
#include "project/project.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <map>

using namespace decomp;

namespace {

std::string map_header(u64 preferred_base) {
    return std::format(" app\n\n Timestamp is 00000000 (Repro mode)\n\n Preferred load address is {:08x}\n\n"
                       " Start         Length     Name                   Class\n 0001:00000000 00001000H .text                   CODE\n\n"
                       "  Address         Publics by Value              Rva+Base     Lib:Object\n\n",
                       preferred_base);
}

// One "Publics by Value" line, link.exe style.
std::string map_line(usize section, u64 offset, std::string_view name, u64 va, bool function, std::string_view object) {
    return std::format(" {:04x}:{:08x}       {:<26} {:08x} {}   {}\n", section, offset, name, va, function ? "f" : " ", object);
}

// 1-based number and start of the first image section that `pick` accepts.
std::pair<usize, u64> section_where(const BinaryImage& image, auto pick) {
    const auto& sections = image.image_sections();
    for (usize i = 0; i < sections.size(); ++i)
        if (pick(sections[i])) return {i + 1, sections[i].va};
    FAIL("no such section");
    return {0, 0};
}

std::string entry_line(const BinaryImage& image) {
    const auto [section, va] = section_where(image, [&](const ImageSection& s) { return s.contains(image.entry_point()); });
    return std::format("\n entry point at        {:04x}:{:08x}\n\n", section, image.entry_point() - va);
}

// The x86 idiom fixture's functions (each f has a label f_end in its lld-link map), and that map
// rewritten as link.exe writes it: functions flagged `f`, the f_end labels not, _unreferenced static.
struct IdiomMap {
    std::vector<FunctionBounds> functions;
    std::filesystem::path path;
};

// `entry_shift` moves the entry point the map records, to make a map of another build.
IdiomMap write_idiom_map(const std::filesystem::path& dir, u32 entry_shift = 0) {
    const auto lld = map::load(test::fixture("x86/idioms.map")).value();
    REQUIRE(lld.entry_point);
    const u32 entry_offset = lld.entry_point->second + entry_shift;
    std::map<std::string, u64> by_name;
    for (const auto& e : lld.entries) by_name[e.name] = e.va;
    IdiomMap out;
    for (const auto& [name, va] : by_name)
        if (auto end = by_name.find(name + "_end"); end != by_name.end()) out.functions.push_back({va, end->second, name});
    std::ranges::sort(out.functions, {}, &FunctionBounds::start);
    auto is_function = [&](std::string_view name) { return std::ranges::contains(out.functions, name, &FunctionBounds::name); };

    std::string text = map_header(lld.preferred_base), statics;
    for (const auto& e : lld.entries) {
        const std::string line = map_line(e.section, e.offset, e.name, e.va, is_function(e.name), e.object);
        (e.name == "_unreferenced" ? statics : text) += line;
    }
    text += std::format("\n entry point at        0001:{:08x}\n\n Static symbols\n\n", entry_offset) + statics;
    out.path = dir / "idioms.map";
    REQUIRE(fs::write_text(out.path, text));
    return out;
}

u64 start_of(const IdiomMap& m, std::string_view name) {
    auto it = std::ranges::find(m.functions, name, &FunctionBounds::name);
    REQUIRE(it != m.functions.end());
    return it->start;
}

std::optional<Symbol> project_symbol(const project::Project& p, u64 va) {
    for (const Symbol& s : p.symbols())
        if (s.va == va) return s;
    return std::nullopt;
}

} // namespace

TEST_CASE("map symbols: names, object files, statics and kinds, at the image's base") {
    const Program program = Program::open(test::fixture("x86/basic.exe"), OpenOptions{.use_pdb = false, .discover = false}).value();
    const pe::Image& image = program.image();
    const auto [text_section, text_va] = section_where(image, [](const ImageSection& s) { return s.executable; });
    const auto [rdata_section, rdata_va] = section_where(image, [](const ImageSection& s) { return !s.executable && !s.writable; });
    // The map was written for a load at 0x10000000: its addresses move to the image's base.
    const u64 map_base = 0x10000000;
    auto at_map_base = [&](u64 va) { return va - image.image_base() + map_base; };
    const u64 main = text_va + 0x10, helper = text_va + 0x40, label = text_va + 0x13;
    std::string text = map_header(map_base);
    text += map_line(text_section, 0x10, "_main", at_map_base(main), true, "main.obj");
    text += map_line(text_section, 0x10, "$main_label", at_map_base(main), false, "main.obj");
    text += map_line(text_section, 0x13, "$inner", at_map_base(label), false, "main.obj");
    text += map_line(rdata_section, 0, "??_C@_05ABCD@hello?$AA@", at_map_base(rdata_va), false, "main.obj");
    text += map_line(rdata_section, 8, "_g_table", at_map_base(rdata_va + 8), false, "LIBC:data.obj");
    text += " 0000:00000000       ___safe_se_handler_count   00000000     <absolute>\n";
    text += entry_line(image) + " Static symbols\n\n";
    text += map_line(text_section, 0x40, "_helper", at_map_base(helper), true, "main.obj");
    const auto m = map::parse(text).value();
    CHECK(m.timestamp == 0u);
    REQUIRE(map::check_image(m, image));

    SymbolDb db = SymbolDb::from_pe(image);
    const usize named = db.add_map(m, image);
    CHECK(named == 5);  // the label at _main's start is an alias, the absolute symbol is skipped

    const Symbol* f = db.at(main);
    REQUIRE(f);
    CHECK(f->name == "_main");
    CHECK(f->kind == SymbolKind::function);
    CHECK(f->source == SymbolSource::map);
    CHECK(f->object == "main.obj");
    CHECK(std::ranges::contains(f->aliases, "$main_label"));
    CHECK(db.find("$main_label") == f);
    // In code without the `f` flag: a label, which starts no function.
    REQUIRE(db.at(label));
    CHECK(db.at(label)->kind == SymbolKind::label);
    REQUIRE(db.at(rdata_va));
    CHECK(db.at(rdata_va)->kind == SymbolKind::string);
    REQUIRE(db.at(rdata_va + 8));
    CHECK(db.at(rdata_va + 8)->kind == SymbolKind::data);
    CHECK(db.at(rdata_va + 8)->object == "LIBC:data.obj");
    const Symbol* h = db.at(helper);
    REQUIRE(h);
    CHECK(h->is_static);
    CHECK(h->kind == SymbolKind::function);

    // A PDB name stays: the map's becomes an alias.
    SymbolDb with_pdb;
    Symbol pdb_main;
    pdb_main.va = main;
    pdb_main.name = "?main@@YAHXZ";
    pdb_main.kind = SymbolKind::function;
    pdb_main.source = SymbolSource::pdb;
    with_pdb.add(pdb_main);
    with_pdb.add_map(m, image);
    CHECK(with_pdb.at(main)->name == "?main@@YAHXZ");
    CHECK(with_pdb.at(main)->object == "main.obj");
    CHECK(std::ranges::contains(with_pdb.at(main)->aliases, "_main"));
}

TEST_CASE("a program opened with its map: the map's names, and exact bounds from its starts") {
    auto dir = fs::TempDir::create("decomp-map").value();
    const IdiomMap m = write_idiom_map(dir.path());
    REQUIRE(m.functions.size() == 11);
    const Program p = Program::open(test::fixture("x86/idioms.exe"), OpenOptions{.map = m.path}).value();
    for (const auto& f : m.functions) {
        CAPTURE(f.name);
        const Symbol* s = p.symbols().at(f.start);
        REQUIRE(s);
        CHECK(s->name == f.name);
        CHECK(s->source == SymbolSource::map);
        CHECK(s->kind == SymbolKind::function);
        CHECK(s->object == "idioms.obj");
        CHECK(s->size == f.end - f.start);
    }
    CHECK(p.symbols().at(start_of(m, "_unreferenced"))->is_static);
    // The f_end labels are not functions.
    const Symbol* end = p.symbols().find("_entry_end");
    REQUIRE(end);
    CHECK(end->kind == SymbolKind::label);
    const auto c = compare_bounds(m.functions, function_bounds(p.symbols(), p.image()), p.image(), p.decoder());
    CHECK(c.exact == m.functions.size());
    CHECK(c.extra == 0);

    // A map of another build (its entry point is elsewhere) is refused.
    auto other = fs::TempDir::create("decomp-map").value();
    const IdiomMap wrong = write_idiom_map(other.path(), 0x10);
    auto refused = Program::open(test::fixture("x86/idioms.exe"), OpenOptions{.map = wrong.path});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("not for this image") != std::string::npos);
}

TEST_CASE("decomp init with a map writes the map's names and object files to symbols.txt") {
    auto dir = fs::TempDir::create("decomp-map").value();
    const IdiomMap m = write_idiom_map(dir.path());
    auto p = project::Project::init(dir.path() / "p", test::fixture("x86/idioms.exe"), std::nullopt, "clang-cl-x86", m.path).value();
    const auto s = project_symbol(p, start_of(m, "_switch_two_level"));
    REQUIRE(s);
    CHECK(s->name == "_switch_two_level");
    CHECK(s->source == SymbolSource::map);
    CHECK(s->object == "idioms.obj");
    const std::string text = fs::read_text(p.root() / project::Project::kSymbolsFile).value();
    CHECK(text.find("_switch_two_level size=0x52 source=map obj=idioms.obj") != std::string::npos);
    // The object file survives a reload.
    auto reloaded = project::Project::load(p.root()).value();
    CHECK(project_symbol(reloaded, start_of(m, "_switch_two_level"))->object == "idioms.obj");
}

TEST_CASE("re-analysis with the map renames the functions, keeps the work recorded for them, and keeps user names") {
    auto dir = fs::TempDir::create("decomp-map").value();
    const IdiomMap m = write_idiom_map(dir.path());
    auto p = project::Project::init(dir.path() / "p", test::fixture("x86/idioms.exe"), std::nullopt, "clang-cl-x86").value();

    // Work on one function under the name the analysis gave it; the user names another.
    const u64 worked = start_of(m, "_switch_one_level"), named = start_of(m, "_tail_target");
    const Symbol before = project_symbol(p, worked).value();
    CHECK(before.source == SymbolSource::analysis);
    CHECK(before.name.starts_with("sub_"));
    REQUIRE(p.modify_function(worked, [](project::FunctionInfo& i) {
        i.status = project::FunctionStatus::nonmatching;
        i.attempts = 1;
    }));
    REQUIRE(p.save_notes(before, "tried /O2\n"));
    REQUIRE(p.write_matched_source(before, "int switch_one_level(int x) { return x; }\n"));
    REQUIRE(p.set_symbol(project::SymbolEdit{.va = named, .name = "my_tail"}, project::ChangeOrigin{}));  // by the user
    const auto old_dir = p.function_dir(before), old_source = p.matched_source_path(before);

    const auto s = project::analyze(p, project::AnalyzeOptions{.map = m.path}).value();
    CHECK(s.functions_before == m.functions.size());
    CHECK(s.functions == m.functions.size());
    CHECK(s.added == 0);
    CHECK(s.removed == 0);
    CHECK(s.resized == 0);
    CHECK(s.renamed == m.functions.size() - 1);  // all but the user's
    CHECK(s.moved == 2);
    CHECK(s.map_symbols > m.functions.size());

    const Symbol after = project_symbol(p, worked).value();
    CHECK(after.name == "_switch_one_level");
    CHECK(after.source == SymbolSource::map);
    CHECK(after.object == "idioms.obj");
    CHECK(after.size == before.size);
    CHECK(p.function_info(worked).status == project::FunctionStatus::nonmatching);
    CHECK(p.function_info(worked).attempts == 1);
    CHECK(p.notes(after) == "tried /O2\n");
    CHECK(std::filesystem::exists(p.matched_source_path(after)));
    CHECK_FALSE(std::filesystem::exists(old_dir));
    CHECK_FALSE(std::filesystem::exists(old_source));

    const Symbol user = project_symbol(p, named).value();
    CHECK(user.name == "my_tail");
    CHECK(user.source == SymbolSource::user);
    CHECK(std::ranges::contains(user.aliases, "_tail_target"));
    for (const auto& f : m.functions) {
        CAPTURE(f.name);
        CHECK(project_symbol(p, f.start).value().size == f.end - f.start);
    }

    // Again: nothing changes.
    const auto again = project::analyze(p, project::AnalyzeOptions{.map = m.path}).value();
    CHECK(again.added == 0);
    CHECK(again.removed == 0);
    CHECK(again.resized == 0);
    CHECK(again.renamed == 0);
    CHECK(again.moved == 0);
    const auto plain = project::analyze(p).value();
    CHECK(plain.functions == m.functions.size());
    CHECK(plain.renamed == 0);

    // Not while a run is active.
    auto run_lock = p.try_lock_active_run().value();
    REQUIRE(run_lock);
    CHECK_FALSE(project::analyze(p));
}
