// Relinking (project/relink.hpp): the fixtures linked again by lld-link from split objects of their
// original bytes and from their units' sources, and compared with the originals.

#include "analysis/layout.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "formats/pe.hpp"
#include "llvm_fixture.hpp"
#include "matching/unit_source.hpp"
#include "project/project.hpp"
#include "project/relink.hpp"
#include "project/units.hpp"
#include "relink/compare.hpp"
#include "relink/linker.hpp"
#include "test_util.hpp"

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
        CHECK_MESSAGE(r.result.identical(), r.summary);
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
    }
}
