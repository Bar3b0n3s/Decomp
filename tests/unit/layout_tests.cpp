// The image layout (analysis/layout.hpp) and the unit check (matching/unit_check.hpp): what each unit
// contributed to the fixtures, and compiled units placed and compared against it.

#include "analysis/layout.hpp"
#include "analysis/program.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "formats/coff.hpp"
#include "llvm_fixture.hpp"
#include "matching/toolchain.hpp"
#include "matching/unit_check.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;
using namespace decomp::matching;

namespace {

ImageLayout pdb_layout(const Program& program) {
    auto reader = pdb::Reader::load(*program.pdb_path());
    REQUIRE(reader);
    return layout_from_pdb(*reader, program.image());
}

std::vector<const Contribution*> of(const ImageLayout& layout, std::string_view unit) { return layout.of_unit(unit); }

std::optional<coff::Object> compile(const MatchSetup& setup, const std::string& source, std::string_view file_name) {
    Compiler compiler(setup.toolchain, setup.work_dir, setup.cache_dir);
    CompileRequest req;
    req.source = source;
    req.flags = setup.flags;
    req.file_name = std::string(file_name);
    auto r = compiler.compile(req);
    REQUIRE(r);
    REQUIRE_MESSAGE(r->ok, r->output);
    auto obj = coff::Object::parse(r->object_data);
    REQUIRE(obj);
    return std::move(*obj);
}

std::vector<u64> unit_functions(const Program& program, const ImageLayout& layout, std::string_view unit) {
    std::vector<u64> out;
    for (const auto* f : program.symbols().functions()) {
        const auto* c = layout.at(static_cast<u32>(f->va - program.image().image_base()));
        if (c && c->unit == unit) out.push_back(f->va);
    }
    return out;
}

const PlacedSection* section_named(const UnitCheckResult& r, std::string_view symbol) {
    for (const auto& s : r.sections)
        if (s.symbol == symbol) return &s;
    return nullptr;
}

} // namespace

TEST_CASE("layout: the PDB's section contributions, named after their input sections") {
    for (const char* arch : {"x86", "x64"}) {
        CAPTURE(arch);
        auto program = Program::open(test::fixture(std::string(arch) + "/basic.exe"));
        REQUIRE(program);
        const auto layout = pdb_layout(*program);
        CHECK(layout.source == LayoutSource::pdb);
        // other.obj: other_value and its string.
        auto other = of(layout, "other.obj");
        REQUIRE(other.size() == 2);
        CHECK(other[0]->name == ".text");
        CHECK(other[0]->rva == program->resolve("other_value").value() - program->image().image_base());
        CHECK(other[1]->name == ".rdata");
        CHECK(other[1]->size == 6);
        // basic.obj: its uninitialized data follows the initialized data in .data, as .bss.
        auto basic = of(layout, "basic.obj");
        auto bss = std::ranges::find(basic, std::string(".bss"), &Contribution::name);
        REQUIRE(bss != basic.end());
        CHECK((*bss)->section == ".data");
        CHECK((*bss)->size == 8);
        // The linker's own: the debug directory, import tables.
        CHECK(std::ranges::any_of(layout.contributions, [](const Contribution& c) { return c.linker && c.section == ".rdata"; }));
        CHECK(layout.at(0) == nullptr);
        if (std::string(arch) == "x64") {
            // Unwind information pieces in .rdata are .xdata; .pdata keeps its name.
            CHECK(std::ranges::count(basic, std::string(".xdata"), &Contribution::name) == 3);
            CHECK(std::ranges::count(basic, std::string(".pdata"), &Contribution::name) == 3);
        }
    }
}

TEST_CASE("layout: without a PDB, cut by the units of the symbols and the linker's own tables") {
    auto program = Program::open(test::fixture("x86/basic.exe"));
    REQUIRE(program);
    auto reader = pdb::Reader::load(*program->pdb_path());
    REQUIRE(reader);
    const auto units = units_from_pdb(*reader, program->image(), program->symbols());
    const auto truth = layout_from_pdb(*reader, program->image());
    const auto guessed = layout_from_units(*program, units);
    CHECK(guessed.source == LayoutSource::symbols);
    // Every byte the PDB gives a unit's object, the guess gives the same unit (padding may go either way).
    for (const auto& c : truth.contributions) {
        if (c.linker) continue;
        CAPTURE(c.unit);
        CAPTURE(c.rva);
        const auto* g = guessed.at(c.rva);
        REQUIRE(g);
        CHECK(g->unit == c.unit);
        CHECK_FALSE(g->linker);
    }
    // The import thunk, the debug directory and the import tables are the linker's.
    for (const auto& c : truth.contributions) {
        if (!c.linker || c.section == ".reloc") continue;
        CAPTURE(c.rva);
        const auto* g = guessed.at(c.rva);
        REQUIRE(g);
        CHECK(g->linker);
    }
}

TEST_CASE("unit check: the fixture's units compiled from their sources match the image") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl not found; skipping");
        return;
    }
    auto tmp = fs::TempDir::create("decomp-unit-check").value();
    for (Arch arch : {Arch::x86, Arch::x64}) {
        CAPTURE(to_string(arch));
        auto program = Program::open(test::fixture(std::string(to_string(arch)) + "/basic.exe"));
        REQUIRE(program);
        const auto layout = pdb_layout(*program);
        auto setup = test::clang_setup(arch, tools->clang_cl, tmp.path() / "work", tmp.path() / "cache");
        const auto basic_source = fs::read_text(test::fixture("src/basic.cpp")).value();
        const auto other_source = fs::read_text(test::fixture("src/other.cpp")).value();

        auto basic = compile(setup, basic_source, "basic.cpp");
        auto result = check_unit(*program, layout, *basic, "basic.obj", unit_functions(*program, layout, "basic.obj"));
        CHECK_MESSAGE(result.ok(), result.summary());
        CHECK(result.count(PlacementState::unplaced) == 0);
        CHECK(result.missing.empty());
        // Strings, constants and the switch's table are placed and equal.
        const auto* hello = section_named(result, "??_C@_0M@LACCCNMM@hello?5world?$AA@");
        REQUIRE(hello);
        CHECK(hello->state == PlacementState::equal);

        auto other = compile(setup, other_source, "other.cpp");
        result = check_unit(*program, layout, *other, "other.obj", unit_functions(*program, layout, "other.obj"));
        CHECK_MESSAGE(result.ok(), result.summary());
        CHECK(result.count(PlacementState::equal) == 2);

        // A global with another initial value differs where the image has it.
        auto changed = compile(setup, replace_all(basic_source, "int g_counter = 3;", "int g_counter = 4;"), "basic.cpp");
        result = check_unit(*program, layout, *changed, "basic.obj", unit_functions(*program, layout, "basic.obj"));
        CHECK_FALSE(result.ok());
        const auto data = std::ranges::find(result.sections, std::string(".data"), &PlacedSection::name);
        REQUIRE(data != result.sections.end());
        CHECK(data->state == PlacementState::differs);
        CHECK(data->first_difference == 0u);

        // A unit without one of its functions misses that function's contribution.
        auto fewer = compile(setup, replace_all(basic_source, "NOINLINE const char* message() { return \"hello world\"; }", "const char* message();"), "basic.cpp");
        result = check_unit(*program, layout, *fewer, "basic.obj", unit_functions(*program, layout, "basic.obj"));
        CHECK_FALSE(result.ok());
        CHECK_FALSE(result.missing.empty());

        // A string another unit has is that unit's copy: the compiled one is discarded.
        auto pooled = compile(setup, other_source + "const char* hello_again() { return \"hello world\"; }\n", "other.cpp");
        result = check_unit(*program, layout, *pooled, "other.obj", unit_functions(*program, layout, "other.obj"));
        const auto* pooled_string = section_named(result, "??_C@_0M@LACCCNMM@hello?5world?$AA@");
        REQUIRE(pooled_string);
        CHECK(pooled_string->state == PlacementState::discarded);
        CHECK(pooled_string->unit == "basic.obj");
        // The new function is nowhere in the image: the linker would put it after other_value, where the
        // image has the import thunk.
        CHECK_FALSE(result.ok());
        const auto extra = std::ranges::find_if(result.sections, [](const PlacedSection& p) { return p.symbol.find("hello_again") != std::string::npos; });
        REQUIRE(extra != result.sections.end());
        CHECK(extra->state == PlacementState::differs);
    }
}
