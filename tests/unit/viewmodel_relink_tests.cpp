// The Relink view's model (viewmodel/relink.hpp): a relink's result.json read back into rows, the first
// difference with the unit holding it, and the target's and the relinked image's bytes around it. The
// relink is made up from a real comparison: the fixture against a copy with one byte of add() changed
// and another link time.

#include "analysis/layout.hpp"
#include "analysis/program.hpp"
#include "core/bytes.hpp"
#include "core/fs.hpp"
#include "formats/pdb.hpp"
#include "project/relink.hpp"
#include "relink/compare.hpp"
#include "relink/linker.hpp"
#include "test_util.hpp"
#include "viewmodel/relink.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;

TEST_CASE("relink view model: a relink's result read back, with its first difference and the bytes around it") {
    auto program = Program::open(test::fixture("x86/basic.exe")).value();
    const auto reader = pdb::Reader::load(*program.pdb_path()).value();
    const auto layout = layout_from_pdb(reader, program.image());
    const u64 base = program.image().image_base();
    // The relinked image as the linker wrote it: one byte of add() changed, and another link time.
    auto relinked_file = std::vector<std::byte>(program.image().data().begin(), program.image().data().end());
    const u32 rva = static_cast<u32>(program.resolve("add").value() - base + 2);
    const u32 offset = program.image().rva_to_offset(rva).value();
    relinked_file[offset] = static_cast<std::byte>(static_cast<u8>(relinked_file[offset]) ^ 0xFF);
    write_le<u32>(relinked_file, program.image().pe_offset() + 8, 0x12345678);

    project::RelinkResult result;
    result.time = "2026-10-06T01:00:00Z";
    project::UnitLink basic;
    basic.unit = Unit{"basic.obj", UnitKind::code, "src/basic.cpp", UnitOrigin::pdb};
    basic.mode = project::LinkMode::source;
    basic.reason = "from its source on request";
    basic.bytes = 0x200;
    project::UnitSourceCheck check;
    check.unit = basic.unit;
    check.functions = {base + rva - 2};
    check.check = matching::UnitCheckResult{};
    check.check->unit = "basic.obj";
    matching::PlacedSection text;
    text.name = ".text$mn";
    text.symbol = "?add@@YAHHH@Z";
    text.size = 16;
    text.rva = rva - 2;
    text.state = matching::PlacementState::differs;
    text.first_difference = 2;
    text.differing_bytes = 1;
    check.check->sections.push_back(text);
    matching::PlacedSection pooled;
    pooled.name = ".rdata";
    pooled.symbol = "??_C@_05hello@";
    pooled.size = 6;
    pooled.comdat = true;
    pooled.rva = 0x2010;
    pooled.state = matching::PlacementState::discarded;
    pooled.unit = "other.obj";
    check.check->sections.push_back(pooled);
    Contribution missing;
    missing.rva = 0x3000;
    missing.size = 4;
    missing.section = ".data";
    missing.name = ".data";
    missing.unit = "basic.obj";
    check.check->missing.push_back(missing);
    check.check->problems.push_back("a gap before .text$mn ?dispatch@@YAHHH@Z");
    basic.check = std::move(check);
    result.units.push_back(basic);
    project::UnitLink other;
    other.unit = Unit{"other.obj", UnitKind::code, "", UnitOrigin::pdb};
    other.mode = project::LinkMode::split;
    other.reason = "no source";
    other.bytes = 0x40;
    result.units.push_back(other);
    project::UnitLink linker;
    linker.unit = Unit{"* Linker *", UnitKind::linker, "", UnitOrigin::pdb};
    linker.mode = project::LinkMode::linker;
    linker.reason = "made by the linker";
    result.units.push_back(linker);
    result.libraries = {"libs/kernel32.lib"};
    result.notes = {"compiled functions the target folds into others: linked with /opt:icf"};
    result.linker = relink::linker_fit(relink::OriginalLinker{relink::LinkerKind::lld, 18, 1, 3}, relink::LinkerKind::lld, "LLD 18.1.3");
    result.link.ok = true;
    result.link.exit_code = 0;
    result.link.command = {"lld-link", "@link.rsp"};
    result.link.output = "lld-link: warning: something";
    result.image = ".decomp/relink/out/basic.exe";
    auto compared = relinked_file;  // compare_images() stamps its copy
    result.comparison = relink::compare_images(program.image(), compared, layout, program.symbols());

    const auto report = vm::read_relink_report(project::to_json(result));
    CHECK(report.time == "2026-10-06T01:00:00Z");
    CHECK_FALSE(report.identical);
    REQUIRE(report.units.size() == 3);
    CHECK(report.units[0].unit == "basic.obj");
    CHECK(report.units[0].kind == "code");
    CHECK(report.units[0].mode == "source");
    CHECK(report.units[0].bytes == 0x200);
    REQUIRE(report.units[0].check);
    const auto& c = *report.units[0].check;
    CHECK_FALSE(c.complete);
    CHECK(c.functions == 1);
    REQUIRE(c.sections.size() == 2);
    CHECK(c.sections[0].state == "differs");
    CHECK(c.sections[0].rva == rva - 2);
    CHECK(c.sections[0].first_difference == 2u);
    CHECK(c.sections[0].differing_bytes == 1);
    CHECK(c.sections[1].state == "discarded");
    CHECK(c.sections[1].unit == "other.obj");
    CHECK(c.sections[1].comdat);
    CHECK(c.count("differs") == 1);
    REQUIRE(c.missing.size() == 1);
    CHECK(c.missing[0].rva == 0x3000);
    CHECK(c.missing[0].size == 4);
    CHECK(c.problems.size() == 1);
    CHECK_FALSE(report.units[1].check);
    CHECK(report.count("source") == 1);
    CHECK(report.count("split") == 1);
    CHECK(report.count("linker") == 1);
    CHECK(report.libraries == result.libraries);
    CHECK(report.notes == result.notes);
    CHECK(report.linker_kind == "lld");
    CHECK(report.linker_original == "lld-link of LLVM 18.1.3");
    CHECK(report.same_linker == true);
    CHECK(report.link_ok);
    CHECK(report.command == result.link.command);
    CHECK(report.link_output == result.link.output);
    CHECK(report.image == result.image);
    CHECK(report.compared);
    CHECK(report.differing_bytes == 1);
    CHECK(report.original_sha1 == result.comparison->original_sha1);
    CHECK(report.relinked_sha1 == result.comparison->relinked_sha1);
    // The first difference, and the unit holding it: the place to look.
    REQUIRE(report.first);
    CHECK(report.first->rva == rva);
    CHECK(report.first->unit == "basic.obj");
    CHECK(report.first->symbol.find("add") != std::string::npos);
    CHECK(report.headline().starts_with("differs from the target: 1 byte, the first at .text+"));
    CHECK(report.headline().find("in basic.obj") != std::string::npos);
    REQUIRE(report.sections.size() == 1);
    CHECK(report.sections[0].name == ".text");
    CHECK(report.sections[0].first_unit == "basic.obj");
    // The link time was taken over from the target.
    CHECK(std::ranges::any_of(report.stamped, [](const vm::RelinkStamped& s) { return s.name == "COFF header TimeDateStamp" && s.relinked == "78563412"; }));

    // The file as the linker wrote it, with the fields taken over put back, and the bytes around the difference.
    auto dir = fs::TempDir::create("decomp-vm-relink").value();
    REQUIRE(fs::write_file(dir.path() / "basic.exe", relinked_file));
    auto relinked = vm::load_stamped_relink(dir.path() / "basic.exe", report.stamped);
    REQUIRE(relinked);
    CHECK(relinked->timestamp() == program.image().timestamp());
    const auto rows = vm::hex_compare(program.image(), *relinked, rva, 1, 3);
    REQUIRE(rows.size() == 3);
    CHECK(rows[1].rva == (rva & ~0xFu));
    CHECK(rows[0].rva == rows[1].rva - 16);
    CHECK(rows[2].rva == rows[1].rva + 16);
    CHECK_FALSE(rows[0].any_difference());
    CHECK(rows[1].any_difference());
    CHECK_FALSE(rows[2].any_difference());
    const usize at = rva & 0xF;
    CHECK(rows[1].differs[at]);
    CHECK(std::ranges::count(rows[1].differs, true) == 1);
    REQUIRE(rows[1].original[at]);
    CHECK(rows[1].relinked[at] == static_cast<u8>(*rows[1].original[at] ^ 0xFF));
    // Rows neither image has bytes in are left out: before the image, between its sections, past its data.
    const auto start = vm::hex_compare(program.image(), *relinked, 4, 1, 3);
    REQUIRE(start.size() == 2);
    CHECK(start[0].rva == 0);
    CHECK(start[0].original[0] == 'M');
    const u32 data = program.image().sections().back().virtual_address;
    const auto gap = vm::hex_compare(program.image(), *relinked, data, 1, 2);
    REQUIRE(gap.size() == 1);
    CHECK(gap[0].rva == data);
    CHECK(vm::hex_compare(program.image(), *relinked, 0x7FFF0000, 0, 1).empty());
}

TEST_CASE("relink view model: headlines, and results with parts missing") {
    CHECK(vm::read_relink_report(Json::array()).error == "not a relink result");

    Json failed{{"identical", false}, {"units", Json::array()}, {"link", {{"ok", false}, {"exit_code", 1}}}, {"error", "the link failed"}};
    const auto f = vm::read_relink_report(failed);
    CHECK_FALSE(f.link_ok);
    CHECK(f.exit_code == 1);
    CHECK_FALSE(f.compared);
    CHECK(f.headline() == "the link failed");

    // An earlier relink's result, without the linker record.
    Json same{{"identical", true},
              {"units", Json::array({{{"unit", "a.obj"}, {"mode", "source"}}, {{"unit", "b.obj"}, {"mode", "split"}}, {{"unit", "c.obj"}, {"mode", "split"}}})},
              {"link", {{"ok", true}, {"exit_code", 0}}},
              {"comparison", {{"identical", true}, {"original_sha1", "ab"}, {"relinked_sha1", "ab"}}}};
    const auto s = vm::read_relink_report(same);
    CHECK(s.identical);
    CHECK(s.compared);
    CHECK_FALSE(s.same_linker.has_value());
    CHECK(s.headline() == "identical to the target (1 unit from source, 2 split)");
}
