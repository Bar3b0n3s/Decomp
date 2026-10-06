// Relinking (project/relink.hpp): the fixtures linked again by lld-link from split objects of their
// original bytes and from their units' sources, and compared with the originals.

#include "analysis/layout.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "formats/coff.hpp"
#include "formats/coff_writer.hpp"
#include "formats/pdb.hpp"
#include "formats/pe.hpp"
#include "llvm_fixture.hpp"
#include "matching/unit_source.hpp"
#include "project/project.hpp"
#include "project/relink.hpp"
#include "project/units.hpp"
#include "relink/compare.hpp"
#include "relink/linker.hpp"
#include "relink/split.hpp"
#include "test_util.hpp"
#include "viewmodel/relink.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;

namespace {

std::string stamped_names(const relink::ImageComparison& c) {
    std::vector<std::string> names;
    for (const auto& s : c.stamped) names.push_back(s.name);
    return join(names, ", ");
}

// A unit source with every function of `unit` composed from `source` (a whole translation unit).
std::string unit_source_from(const project::Project& p, const Program& program, const matching::MatchSetup& setup, const std::string& unit,
                             const std::string& source) {
    auto composed = project::compose_unit_source(p, program, setup, unit, source);
    REQUIRE(composed);
    CHECK(composed->rejected.empty());
    return composed->content;
}

struct Relinked {
    project::RelinkResult result;
    std::string summary;
};

Relinked relink_and_compare(const project::Project& p, const Program& program, const matching::MatchSetup& setup, project::RelinkOptions options = {}) {
    auto r = project::relink_project(p, program, setup, options);
    REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
    std::string summary;
    for (const auto& u : r->units) summary += std::format("{}: {} ({})\n", u.unit.name, to_string(u.mode), u.reason);
    for (const auto& n : r->notes) summary += "note: " + n + "\n";
    summary += "linker: " + r->linker.text + "\n";
    if (!r->link.ok) summary += r->link.output;
    if (r->comparison && r->comparison->first)
        summary += std::format("first difference: {} in {} {}\n", r->comparison->first->where, r->comparison->first->unit, r->comparison->first->symbol);
    return {std::move(*r), summary};
}

} // namespace

TEST_CASE("relink: link flags reproduce the fixture's headers") {
    auto image = pe::Image::load(test::fixture("x86/basic.exe")).value();
    const auto flags = relink::image_link_flags(image, "entry");
    auto has = [&](std::string_view f) { return std::ranges::find(flags, f) != flags.end(); };
    CHECK(has("/machine:x86"));
    CHECK(has("/subsystem:console,6.00"));
    CHECK(has("/base:0x400000"));
    CHECK(has("/debug"));
    CHECK(has("/pdbaltpath:basic.pdb"));
    CHECK(has("/Brepro"));
    CHECK(has("/entry:entry"));
    CHECK(has("/dynamicbase"));
    CHECK_FALSE(has("/fixed"));
    CHECK_FALSE(has("/release"));
    auto fixed = pe::Image::load(test::fixture("x86/basic_fixed.exe")).value();
    const auto fixed_flags = relink::image_link_flags(fixed, "entry");
    CHECK(std::ranges::find(fixed_flags, "/fixed") != fixed_flags.end());
}

TEST_CASE("relink: the linker that made an image, and whether the relink's is the same") {
    // The fixtures: no Rich header, linker version 14.0 and objects compiled by LLVM 18's clang-cl.
    auto image = pe::Image::load(test::fixture("x86/basic.exe")).value();
    std::vector<std::string> compilers;
    const auto reader = pdb::Reader::load(test::fixture("x86/basic.pdb")).value();
    for (const auto& m : reader.modules()) compilers.push_back(m.compiler);
    const auto original = relink::original_linker(image, compilers);
    REQUIRE(original);
    CHECK(original->kind == relink::LinkerKind::lld);
    CHECK(original->major == 18);
    CHECK(original->text().starts_with("lld-link of LLVM 18."));
    CHECK_FALSE(relink::original_linker(image, {}));  // without the PDB's compile records: not known

    const relink::OriginalLinker lld18{relink::LinkerKind::lld, 18, 1, 3};
    CHECK(relink::linker_fit(lld18, relink::LinkerKind::lld, "Ubuntu LLD 18.1.3").same == true);
    const auto newer = relink::linker_fit(lld18, relink::LinkerKind::lld, "LLD 20.1.8 (https://github.com/llvm/llvm-project 87f0227cb601)");
    CHECK(newer.same == false);
    CHECK(newer.text.find("lld-link of LLVM 18.1.3") != std::string::npos);
    CHECK(newer.text.find("lld-link of LLVM 20.1.8") != std::string::npos);
    CHECK(relink::linker_fit(lld18, relink::LinkerKind::msvc, "Microsoft (R) Incremental Linker Version 14.29.30133.0").same == false);

    const relink::OriginalLinker vs2019{relink::LinkerKind::msvc, 14, 29, 30133};
    CHECK(vs2019.text() == "link.exe 14.29.30133");
    CHECK(relink::linker_fit(vs2019, relink::LinkerKind::msvc, "Microsoft (R) Incremental Linker Version 14.29.30133.0").same == true);
    CHECK(relink::linker_fit(vs2019, relink::LinkerKind::msvc, "Microsoft (R) Incremental Linker Version 14.40.33811.0").same == false);
    const relink::OriginalLinker vc6{relink::LinkerKind::msvc, 6, 0, 8447};
    CHECK(relink::linker_fit(vc6, relink::LinkerKind::msvc, "Microsoft (R) Incremental Linker Version 6.00.8447").same == true);
    // Unknown either way: nothing to compare.
    CHECK_FALSE(relink::linker_fit(std::nullopt, relink::LinkerKind::lld, "LLD 18.1.3").same.has_value());
    CHECK_FALSE(relink::linker_fit(vs2019, relink::LinkerKind::msvc, "").same.has_value());

    if (auto tools = test::find_llvm()) {
        relink::Linker lld;
        lld.path = tools->lld_link;
        lld.kind = relink::LinkerKind::lld;
        const auto version = relink::detect_linker_version(lld);
        CHECK(version.find("LLD ") != std::string::npos);
        CHECK(relink::linker_fit(original, lld.kind, version).same.has_value());
    }
}

TEST_CASE("relink: split objects carry their compiler's marks") {
    auto program = Program::open(test::fixture("x86/basic.exe")).value();
    const auto reader = pdb::Reader::load(*program.pdb_path()).value();
    const auto layout = layout_from_pdb(reader, program.image());
    relink::SplitObjectSpec spec;
    for (const auto* c : layout.of_unit("other.obj"))
        if (!c->linker) spec.contributions.push_back(c);
    REQUIRE_FALSE(spec.contributions.empty());
    auto value_of = [](const std::vector<std::byte>& bytes, std::string_view name) -> std::optional<u32> {
        auto object = coff::Object::parse(bytes);
        if (!object) return std::nullopt;
        for (const auto& s : object->symbols())
            if (s.name == name) return s.value;
        return std::nullopt;
    };
    // Without: SAFESEH-compatible on x86, and no compiler for the Rich header.
    const auto plain = relink::write_split_object(program.image(), spec).value();
    CHECK(value_of(plain, "@feat.00") == 1u);
    CHECK_FALSE(value_of(plain, "@comp.id"));
    // A unit cl.exe made: its compiler's id and feature bits, as link.exe counts them.
    spec.comp_id = 0x01058d87;
    spec.feat00 = 0x80010191;
    const auto marked = relink::write_split_object(program.image(), spec).value();
    CHECK(value_of(marked, "@comp.id") == 0x01058d87u);
    CHECK(value_of(marked, "@feat.00") == 0x80010191u);
}

TEST_CASE("relink: split x64 .pdata says which function and unwind data each entry is for") {
    // As compilers write it: relocations against the sections holding the function and its unwind data,
    // and a function's COMDAT taking its .pdata along. link.exe builds the exception table from these.
    auto program = Program::open(test::fixture("x64/basic.exe")).value();
    const auto reader = pdb::Reader::load(*program.pdb_path()).value();
    const auto layout = layout_from_pdb(reader, program.image());
    relink::SplitObjectSpec spec;
    for (const auto* c : layout.of_unit("basic.obj"))
        if (!c->linker && c->size) spec.contributions.push_back(c);
    const auto object = coff::Object::parse(relink::write_split_object(program.image(), spec).value()).value();
    int entries = 0;
    for (const auto& s : object.sections()) {
        if (s.name != ".pdata") continue;
        CAPTURE(s.number);
        REQUIRE(s.size == 12);
        ++entries;
        REQUIRE(s.relocations.size() == 3);
        std::vector<std::string> targets;
        for (const auto& r : s.relocations) {
            CHECK(r.type == coff::reloc_amd64::addr32nb);
            const auto* symbol = object.symbol_at_index(r.symbol_index);
            REQUIRE(symbol);
            REQUIRE(symbol->is_defined());
            targets.push_back(object.section(symbol->section_number)->name);
        }
        // The function, its end, and its unwind information.
        CHECK(targets[0].starts_with(".text"));
        CHECK(targets[1] == targets[0]);
        CHECK(targets[2] == ".xdata");
        const auto* function = object.section(object.symbol_at_index(s.relocations[0].symbol_index)->section_number);
        if (function->comdat) {
            REQUIRE(s.comdat);
            CHECK(s.comdat->selection == coff::comdat_select::associative);
            CHECK(s.comdat->associated_section == function->number);
        }
        // The addresses are offsets in their sections now: the function's entry starts its section.
        CHECK(read_le<u32>(s.data, 0) == 0u);
    }
    CHECK(entries == 3);
}

TEST_CASE("relink: split objects define their units' public names") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("lld-link not found; skipping");
        return;
    }
    // As the original objects did: nothing refers to g_counter by name in a relink of split objects, but a
    // linker looks some names up itself (link.exe the security cookie).
    auto tmp = fs::TempDir::create("decomp-relink-names").value();
    auto p = project::Project::init(tmp.path() / "p", test::fixture("x86/basic.exe"), std::nullopt, "clang-cl-x86").value();
    auto program = p.open_program().value();
    const auto setup = test::clang_setup(program.arch(), tools->clang_cl, tmp.path() / "work", tmp.path() / "cache");
    project::RelinkOptions options;
    options.all_split = true;
    const auto r = relink_and_compare(p, program, setup, options);
    const auto basic = std::ranges::find_if(r.result.units, [](const project::UnitLink& u) { return u.unit.name == "basic.obj"; });
    REQUIRE(basic != r.result.units.end());
    const auto object = coff::Object::load(project::relink_dir(p) / basic->object).value();
    const auto* counter = object.find_defined("?g_counter@@3HA");
    REQUIRE(counter);
    CHECK(counter->is_external());
    CHECK(object.section(counter->section_number)->name == ".data");
    CHECK(object.find_defined("?add@@YAHHH@Z"));
    // A static name stays out: another unit can have one of the same name.
    int statics = 0;
    for (const auto& [va, s] : program.symbols()) {
        if (!s.is_static || s.name.empty()) continue;
        ++statics;
        CHECK_FALSE(object.find_defined(s.name));
    }
    CHECK(statics > 0);
}

TEST_CASE("relink: comparing images takes the build's identity over and finds the first difference") {
    auto program = Program::open(test::fixture("x86/basic.exe")).value();
    auto reader = pdb::Reader::load(*program.pdb_path()).value();
    const auto layout = layout_from_pdb(reader, program.image());
    // The same bytes with another timestamp and PDB GUID: identical once they are taken over.
    auto copy = std::vector<std::byte>(program.image().data().begin(), program.image().data().end());
    write_le<u32>(copy, program.image().pe_offset() + 8, 0x12345678);
    const auto& cv = program.image().debug_entries()[0];
    write_le<u32>(copy, cv.file_offset + 4, 0xDEADBEEF);
    auto same = relink::compare_images(program.image(), copy, layout, program.symbols());
    CHECK(same.identical);
    CHECK(stamped_names(same) == "COFF header TimeDateStamp, CodeView GUID");
    CHECK(same.relinked_unstamped_sha1 != same.original_sha1);
    // A changed instruction in add(): found there, in basic.obj.
    const u64 add = program.resolve("add").value();
    const u32 offset = program.image().rva_to_offset(static_cast<u32>(add - program.image().image_base() + 2)).value();
    copy[offset] = static_cast<std::byte>(static_cast<u8>(copy[offset]) ^ 0xFF);
    auto changed = relink::compare_images(program.image(), copy, layout, program.symbols());
    CHECK_FALSE(changed.identical);
    REQUIRE(changed.first);
    CHECK(changed.first->unit == "basic.obj");
    CHECK(changed.first->rva == static_cast<u32>(add - program.image().image_base() + 2));
    CHECK(changed.first->symbol.find("add") != std::string::npos);
    CHECK(changed.differing_bytes == 1);
    REQUIRE(changed.sections.size() == 1);
    CHECK(changed.sections[0].name == ".text");
}

TEST_CASE("relink: the fixtures relink byte-identically from split objects") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("lld-link not found; skipping");
        return;
    }
    auto tmp = fs::TempDir::create("decomp-relink-split").value();
    // Identical only with the linker that made the fixtures, LLVM 18's lld-link: another version can lay
    // them out differently (where the import directory after the export names starts, for one), which the
    // relink says.
    relink::Linker lld;
    lld.path = tools->lld_link;
    lld.kind = relink::LinkerKind::lld;
    std::vector<std::string> compilers;
    const auto reader = pdb::Reader::load(test::fixture("x86/basic.pdb")).value();
    for (const auto& m : reader.modules()) compilers.push_back(m.compiler);
    const auto fit = relink::linker_fit(relink::original_linker(pe::Image::load(test::fixture("x86/basic.exe")).value(), compilers),
                                        lld.kind, relink::detect_linker_version(lld));
    REQUIRE(fit.same.has_value());
    if (!*fit.same) MESSAGE(fit.text << ": the relinks are only checked to link");
    // With PDBs: plain code and data, C++ and structured exception handling (x86 /SAFESEH tables, x64
    // unwind data and funclets), RTTI, a static library's members. Without: basic_fixed (no base
    // relocations either) and the hand-written idioms, with and without their link maps.
    struct Case {
        const char* binary;
        const char* map = nullptr;
    };
    const std::vector<Case> cases = {
        {"x86/basic.exe"},       {"x64/basic.exe"},       {"x86/eh.exe"},     {"x64/eh.exe"},
        {"x86/rtti.exe"},        {"x64/rtti.exe"},        {"x86/libuser.exe"}, {"x64/libuser.exe"},
        {"x86/basic_fixed.exe"}, {"x64/basic_fixed.exe"}, {"x86/idioms.exe"},  {"x64/idioms.exe"},
        {"x86/idioms.exe", "x86/idioms.map"},             {"x64/idioms.exe", "x64/idioms.map"},
    };
    int n = 0;
    for (const auto& c : cases) {
        CAPTURE(c.binary);
        CAPTURE(c.map != nullptr);
        const std::string arch = std::string(c.binary).substr(0, 3);
        auto map = c.map ? std::optional<std::filesystem::path>(test::fixture(c.map)) : std::nullopt;
        auto p = project::Project::init(tmp.path() / std::format("p{}", n++), test::fixture(c.binary), std::nullopt, "clang-cl-" + arch, map).value();
        auto program = p.open_program().value();
        const auto setup = test::clang_setup(program.arch(), tools->clang_cl, tmp.path() / "work", tmp.path() / "cache");
        project::RelinkOptions options;
        options.all_split = true;
        auto r = relink_and_compare(p, program, setup, options);
        CHECK_MESSAGE(r.result.link.ok, r.summary);
        if (*fit.same) CHECK_MESSAGE(r.result.identical(), r.summary);
        // The images with PDBs say which linker made them.
        if (program.pdb_path()) CHECK(r.result.linker.same == fit.same);
        CHECK(r.result.count(project::LinkMode::source) == 0);
        const auto saved = project::last_relink(p);
        REQUIRE(saved);
        CHECK(saved->value("identical", false) == r.result.identical());
    }
}

TEST_CASE("relink: units built from their sources, the rest from split objects") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    auto tmp = fs::TempDir::create("decomp-relink-source").value();
    const auto basic_source = fs::read_text(test::fixture("src/basic.cpp")).value();
    const auto other_source = fs::read_text(test::fixture("src/other.cpp")).value();
    for (Arch arch : {Arch::x86, Arch::x64}) {
        CAPTURE(to_string(arch));
        const std::string a(to_string(arch));
        // Built with the installed LLVM, which compiles the sources too.
        const auto exe = test::build_fixture_program(arch, *tools, tmp.path() / a / "target");
        REQUIRE(exe);
        auto p = project::Project::init(tmp.path() / a / "p", *exe, std::nullopt, "clang-cl-" + a).value();
        p.config().flags = test::fixture_flags();
        REQUIRE(p.save_config());
        auto program = p.open_program().value();
        const auto setup = test::clang_setup(arch, tools->clang_cl, tmp.path() / a / "work", tmp.path() / a / "cache");
        const auto units = project::load_units(p).value();
        auto source_of = [&](const std::string& unit) {
            return std::ranges::find(units, unit, &Unit::name)->source;
        };

        // Composed from another unit's translation unit, none of basic.obj's functions are there.
        const auto wrong = project::compose_unit_source(p, program, setup, "basic.obj", other_source).value();
        CHECK(wrong.rejected.size() == 12);
        CHECK_FALSE(wrong.check.complete());
        CHECK(wrong.check.summary() == "0 of 12 functions in its source");

        // One unit complete: other.obj from its source, basic.obj from its bytes.
        REQUIRE(fs::write_text(p.root() / source_of("other.obj"), unit_source_from(p, program, setup, "other.obj", other_source)));
        auto r = relink_and_compare(p, program, setup);
        CHECK_MESSAGE(r.result.identical(), r.summary);
        CHECK(r.result.count(project::LinkMode::source) == 1);

        // Both: the whole program from source.
        REQUIRE(fs::write_text(p.root() / source_of("basic.obj"), unit_source_from(p, program, setup, "basic.obj", basic_source)));
        const auto checks = project::check_unit_sources(p, program, setup).value();
        REQUIRE(checks.size() == 2);
        for (const auto& c : checks) CHECK_MESSAGE(c.complete(), (c.unit.name + ": " + c.summary()));
        r = relink_and_compare(p, program, setup);
        CHECK_MESSAGE(r.result.identical(), r.summary);
        CHECK(r.result.count(project::LinkMode::source) == 2);
        CHECK(r.result.count(project::LinkMode::split) == 0);

        // A global with another value: the unit's check fails, so it is split and the relink stays identical...
        REQUIRE(fs::write_text(p.root() / source_of("basic.obj"),
                               unit_source_from(p, program, setup, "basic.obj", replace_all(basic_source, "int g_counter = 3;", "int g_counter = 4;"))));
        r = relink_and_compare(p, program, setup);
        CHECK_MESSAGE(r.result.identical(), r.summary);
        const auto basic = std::ranges::find_if(r.result.units, [](const project::UnitLink& u) { return u.unit.name == "basic.obj"; });
        REQUIRE(basic != r.result.units.end());
        CHECK(basic->mode == project::LinkMode::split);
        CHECK(basic->reason.find(".data") != std::string::npos);
        // ...and linked from its source anyway, the relink differs there, in basic.obj's data.
        project::RelinkOptions forced;
        forced.source = {"basic.obj"};
        r = relink_and_compare(p, program, setup, forced);
        CHECK_FALSE(r.result.identical());
        REQUIRE(r.result.comparison);
        REQUIRE(r.result.comparison->first);
        CHECK(r.result.comparison->first->unit == "basic.obj");
        CHECK(r.result.comparison->first->where.starts_with(".data"));
        CHECK(r.result.comparison->first->symbol.find("g_counter") != std::string::npos);
        // What the Relink view shows from result.json: the first differing bytes, the target's 3 where the
        // relink has 4, in basic.obj.
        const auto report = vm::read_relink_report(*project::last_relink(p));
        CHECK_FALSE(report.identical);
        REQUIRE(report.first);
        CHECK(report.first->unit == "basic.obj");
        CHECK(report.headline().find("in basic.obj") != std::string::npos);
        auto relinked = vm::load_stamped_relink(p.root() / fs::from_utf8(report.image), report.stamped);
        REQUIRE(relinked);
        const auto rows = vm::hex_compare(program.image(), *relinked, *report.first->rva, 0, 1);
        REQUIRE(rows.size() == 1);
        const usize at = *report.first->rva - rows[0].rva;
        CHECK(rows[0].differs[at]);
        CHECK(rows[0].original[at] == 3);
        CHECK(rows[0].relinked[at] == 4);
    }
}

TEST_CASE("relink: exception handling and unwind tables from source") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    // The corpus's C++ exceptions and structured exception handling (x86 handler tables and SAFESEH, x64
    // unwind data, catch funclets, SEH filters), with the runtime they need: every unit from its source.
    auto tmp = fs::TempDir::create("decomp-relink-eh").value();
    const auto corpus = test::source_dir() / "tests" / "corpus";
    const std::vector<std::filesystem::path> sources = {corpus / "eh.cpp", corpus / "seh.c", corpus / "eh_rt.c", test::fixture("src/eh_main.c")};
    for (Arch arch : {Arch::x86, Arch::x64}) {
        CAPTURE(to_string(arch));
        const std::string a(to_string(arch));
        const std::string type_info = arch == Arch::x86 ? "_corpus_type_info_vftable" : "corpus_type_info_vftable";
        const auto exe = test::build_program(arch, *tools, tmp.path() / a / "target", sources, "eh",
                                             {"/alternatename:??_7type_info@@6B@=" + type_info}, {"/EHsc"});
        REQUIRE(exe);
        auto p = project::Project::init(tmp.path() / a / "p", *exe, std::nullopt, "clang-cl-" + a).value();
        p.config().flags = test::fixture_flags();
        p.config().flags.push_back("/EHsc");
        REQUIRE(p.save_config());
        auto program = p.open_program().value();
        auto setup = test::clang_setup(arch, tools->clang_cl, tmp.path() / a / "work", tmp.path() / a / "cache");
        setup.flags = p.config().flags;
        const auto units = project::load_units(p).value();
        for (const auto& source : sources) {
            const std::string unit = fs::to_utf8(source.stem()) + ".obj";
            CAPTURE(unit);
            auto composed = project::compose_unit_source(p, program, setup, unit, fs::read_text(source).value());
            REQUIRE(composed);
            CHECK_MESSAGE(composed->check.complete(), composed->check.summary());
            REQUIRE(fs::write_text(p.root() / std::ranges::find(units, unit, &Unit::name)->source, composed->content));
        }
        auto r = relink_and_compare(p, program, setup);
        CHECK_MESSAGE(r.result.identical(), r.summary);
        CHECK(r.result.count(project::LinkMode::source) == 4);
    }
}

TEST_CASE("relink: functions folded by identical COMDAT folding") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    // twice_b (same unit) and twice_c (the other unit) fold into twice_a, value_c into value_a.
    auto tmp = fs::TempDir::create("decomp-relink-icf").value();
    const std::string a_source = "#define NOINLINE __declspec(noinline)\n"
                                 "extern \"C\" __declspec(dllimport) void __stdcall ExitProcess(unsigned int code);\n"
                                 "int g_value = 5;\nint twice_c(int x);\nint value_c();\n"
                                 "NOINLINE int twice_a(int x) { return x * 2; }\n"
                                 "NOINLINE int twice_b(int x) { return x * 2; }\n"
                                 "NOINLINE int value_a() { return g_value + 1; }\n"
                                 "extern \"C\" void entry() {\n"
                                 "    ExitProcess(static_cast<unsigned>(twice_a(1) + twice_b(2) + twice_c(3) + value_a() + value_c()));\n}\n";
    const std::string b_source = "#define NOINLINE __declspec(noinline)\nextern int g_value;\n"
                                 "NOINLINE int twice_c(int x) { return x * 2; }\n"
                                 "NOINLINE int value_c() { return g_value + 1; }\n";
    REQUIRE(fs::write_text(tmp.path() / "icf_a.cpp", a_source));
    REQUIRE(fs::write_text(tmp.path() / "icf_b.cpp", b_source));
    for (Arch arch : {Arch::x86, Arch::x64}) {
        CAPTURE(to_string(arch));
        const std::string a(to_string(arch));
        const auto exe = test::build_program(arch, *tools, tmp.path() / a / "target", {tmp.path() / "icf_a.cpp", tmp.path() / "icf_b.cpp"}, "icf",
                                             {"/opt:icf"});
        REQUIRE(exe);
        auto p = project::Project::init(tmp.path() / a / "p", *exe, std::nullopt, "clang-cl-" + a).value();
        p.config().flags = test::fixture_flags();
        REQUIRE(p.save_config());
        auto program = p.open_program().value();
        // The folded names are aliases of the functions they share an address with.
        const Symbol* twice = program.symbols().find("?twice_c@@YAHH@Z");
        REQUIRE(twice);
        CHECK(twice->va == program.symbols().find("?twice_a@@YAHH@Z")->va);
        const auto setup = test::clang_setup(arch, tools->clang_cl, tmp.path() / a / "work", tmp.path() / a / "cache");
        project::RelinkOptions split;
        split.all_split = true;
        auto r = relink_and_compare(p, program, setup, split);
        CHECK_MESSAGE(r.result.identical(), r.summary);
        // icf_b.obj from its source: its functions are the other unit's copies, which the split unit keeps.
        const auto units = project::load_units(p).value();
        auto write = [&](const std::string& unit, const std::string& text) {
            auto composed = project::compose_unit_source(p, program, setup, unit, text);
            REQUIRE(composed);
            CHECK_MESSAGE(composed->check.complete(), composed->check.summary());
            REQUIRE(fs::write_text(p.root() / std::ranges::find(units, unit, &Unit::name)->source, composed->content));
            return *composed;
        };
        const auto b = write("icf_b.obj", b_source);
        CHECK(std::ranges::count(b.check.check->sections, matching::PlacementState::discarded, &matching::PlacedSection::state) == 2);
        // ...which the split unit cannot stand for under their names: icf_b.obj stays split until icf_a.obj is
        // built from source too.
        r = relink_and_compare(p, program, setup);
        CHECK_MESSAGE(r.result.identical(), r.summary);
        const auto b_link = std::ranges::find_if(r.result.units, [](const project::UnitLink& u) { return u.unit.name == "icf_b.obj"; });
        REQUIRE(b_link != r.result.units.end());
        CHECK(b_link->mode == project::LinkMode::split);
        CHECK(b_link->reason.find("folded into icf_a.obj") != std::string::npos);
        // Both from source: the relink folds them again.
        const auto composed_a = write("icf_a.obj", a_source);
        CHECK(std::ranges::any_of(composed_a.check.check->sections, [](const matching::PlacedSection& s) { return !s.folded_into.empty(); }));
        r = relink_and_compare(p, program, setup);
        CHECK_MESSAGE(r.result.identical(), r.summary);
        CHECK(r.result.count(project::LinkMode::source) == 2);
        CHECK(std::ranges::any_of(r.result.notes, [](const std::string& n) { return n.find("/opt:icf") != std::string::npos; }));
    }
}
