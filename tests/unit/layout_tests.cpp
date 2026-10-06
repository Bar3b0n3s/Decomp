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
#include <set>

using namespace decomp;
using namespace decomp::matching;

namespace {

ImageLayout pdb_layout(const Program& program) {
    auto reader = pdb::Reader::load(*program.pdb_path());
    REQUIRE(reader);
    return layout_from_pdb(*reader, program.image());
}

// A unit's contributions that hold something (the empty ones only align what follows).
std::vector<const Contribution*> of(const ImageLayout& layout, std::string_view unit) {
    auto all = layout.of_unit(unit);
    std::erase_if(all, [](const Contribution* c) { return c->size == 0; });
    return all;
}

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
        if (c.linker || c.size == 0) continue;
        CAPTURE(c.unit);
        CAPTURE(c.rva);
        const auto* g = guessed.at(c.rva);
        REQUIRE(g);
        CHECK(g->unit == c.unit);
        CHECK_FALSE(g->linker);
    }
    // The import thunk, the debug directory and the import tables are the linker's.
    for (const auto& c : truth.contributions) {
        if (!c.linker || c.section == ".reloc" || c.size == 0) continue;
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
        // Built with the installed LLVM, which compiles the sources too: another version than the committed
        // fixture's can compile a function differently.
        const auto exe = test::build_fixture_program(arch, *tools, tmp.path() / std::string(to_string(arch)));
        REQUIRE(exe);
        auto program = Program::open(*exe);
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

TEST_CASE("link order: the order the image shows, not the module list's") {
    // As link.exe's PDBs have it: the modules listed c, a, b; the image holds a's code before c's and
    // a's, b's and c's data in that order. b has no code; d holds nothing.
    auto contribution = [](u32 rva, const char* section, const char* name, u32 characteristics, const char* unit) {
        Contribution c;
        c.rva = rva;
        c.size = 0x10;
        c.section = section;
        c.name = name;
        c.characteristics = characteristics;
        c.unit = unit;
        return c;
    };
    constexpr u32 code = 0x60500020, data = 0xC0400040, aligned_data = 0xC0300040;
    ImageLayout layout;
    layout.contributions = {
        contribution(0x1000, ".text", ".text$mn", code, "a.obj"),       contribution(0x1010, ".text", ".text$mn", code, "c.obj"),
        contribution(0x3000, ".data", ".data", data, "a.obj"),           contribution(0x3010, ".data", ".data", aligned_data, "b.obj"),
        contribution(0x3020, ".data", ".data", data, "c.obj"),           contribution(0x3030, ".data", ".data", data, "c.obj"),
    };
    layout.contributions.push_back(contribution(0x2000, ".rdata", ".rdata", 0x40300040, "* Linker *"));
    layout.contributions.back().linker = true;
    CHECK(link_order(layout, {"c.obj", "d.obj", "a.obj", "b.obj", "* Linker *"}) ==
          std::vector<std::string>{"d.obj", "a.obj", "b.obj", "c.obj", "* Linker *"});
    // An order the image agrees with stays as it is.
    CHECK(link_order(layout, {"a.obj", "b.obj", "c.obj", "d.obj"}) == std::vector<std::string>{"a.obj", "b.obj", "c.obj", "d.obj"});
    // Groups that disagree (a cycle) still give every unit once.
    layout.contributions.push_back(contribution(0x5000, ".tls", ".tls", data, "c.obj"));
    layout.contributions.push_back(contribution(0x5010, ".tls", ".tls", data, "a.obj"));
    const auto order = link_order(layout, {"c.obj", "a.obj", "b.obj"});
    CHECK(order.size() == 3);
    CHECK(std::set<std::string>(order.begin(), order.end()) == std::set<std::string>{"a.obj", "b.obj", "c.obj"});
}
