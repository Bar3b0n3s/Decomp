#include "viewmodel/binary.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;
using namespace decomp::vm;

TEST_CASE("binary: sections with their characteristics as words") {
    CHECK(section_flags(pe::scn::cnt_code | pe::scn::mem_execute | pe::scn::mem_read) == "code, execute, read");
    CHECK(section_flags(pe::scn::cnt_initialized_data | pe::scn::mem_read | pe::scn::mem_write) == "initialized data, read, write");
    CHECK(section_flags(0) == "");
    test::FixtureProject fx;
    const auto rows = section_rows(fx.program.image());
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].name == ".text");
    CHECK(rows[0].va == 0x401000);
    CHECK(rows[0].virtual_size == 0x2f4);
    CHECK(rows[0].flags.find("execute") != std::string::npos);
    CHECK(rows[2].name == ".data");
    CHECK(rows[2].flags.find("write") != std::string::npos);
}

TEST_CASE("binary: overlay spans by kind, overlapping queries and the topmost span") {
    HexOverlays o;
    o.add({0x1000, 0x1100, OverlayKind::function, "big"});
    o.add({0x1100, 0x1110, OverlayKind::function, "small"});
    o.add({0x1010, 0x1014, OverlayKind::relocation, "HIGHLOW"});
    o.add({0x1040, 0x1060, OverlayKind::jump_table, "table"});
    o.add({0x2000, 0x2000, OverlayKind::data, "empty becomes one byte"});
    o.add({0x0f00, 0x3000, OverlayKind::data, "covers everything"});
    o.finish();
    CHECK(o.size() == 6);
    CHECK(o.count(OverlayKind::function) == 2);

    auto labels = [](const std::vector<const OverlaySpan*>& spans) {
        std::vector<std::string> out;
        for (const auto* s : spans) out.push_back(s->label);
        return out;
    };
    // By kind in drawing order (function, data, ..., jump table, relocation), then by start.
    CHECK(labels(o.overlapping(0x1000, 0x1010)) == std::vector<std::string>{"big", "covers everything"});
    CHECK(labels(o.overlapping(0x1008, 0x1018)) == std::vector<std::string>{"big", "covers everything", "HIGHLOW"});
    CHECK(labels(o.overlapping(0x10f8, 0x1108)) == std::vector<std::string>{"big", "small", "covers everything"});
    CHECK(labels(o.overlapping(0x2000, 0x2001)) == std::vector<std::string>{"covers everything", "empty becomes one byte"});
    CHECK(o.overlapping(0x3000, 0x3010).empty());
    CHECK(o.overlapping(0x1110, 0x1110).empty());

    CHECK(o.top_at(0x1012)->label == "HIGHLOW");
    CHECK(o.top_at(0x1050)->label == "table");
    CHECK(o.top_at(0x1080)->label == "covers everything");  // data is drawn over functions
    CHECK(o.top_at(0x4000) == nullptr);
}

TEST_CASE("binary: overlays of the fixture") {
    test::FixtureProject fx;
    const auto strings = scan_strings(fx.program.image(), {}, &fx.program.symbols());
    const HexOverlays o = build_hex_overlays(fx.program, strings);
    CHECK(o.count(OverlayKind::function) == 13);  // ExitProcess's thunk has no size
    CHECK(o.count(OverlayKind::relocation) == 25);
    CHECK(o.count(OverlayKind::import) == 1);
    CHECK(o.count(OverlayKind::jump_table) >= 1);  // dispatch's switch
    CHECK(o.count(OverlayKind::string) >= 1);
    CHECK(o.count(OverlayKind::float_const) >= 1);

    const u64 add = fx.va("add");
    const OverlaySpan* top = o.top_at(add);
    REQUIRE(top);
    CHECK(top->kind == OverlayKind::function);
    CHECK(top->label == "int __cdecl add(int, int)");
    // The import slot is named after its DLL.
    const auto& imp = fx.program.image().imports().front();
    CHECK(o.top_at(imp.iat_va)->label == imp.dll + "!" + imp.name);
    const auto hello = std::ranges::find(strings, std::string("hello world"), &ImageString::text);
    REQUIRE(hello != strings.end());
    CHECK(o.top_at(hello->va)->kind == OverlayKind::string);
    // The relocated pointer to g_counter inside add.
    const auto in_add = o.overlapping(add, add + fx.size("add"));
    CHECK(std::ranges::any_of(in_add, [](const OverlaySpan* s) { return s->kind == OverlayKind::relocation; }));

    int polls = 0;
    const HexOverlays cut = build_hex_overlays(fx.program, strings, [&] { return ++polls > 0; });
    CHECK(cut.count(OverlayKind::jump_table) == 0);  // stopped before decoding the functions
    CHECK(cut.count(OverlayKind::function) == 13);
}

TEST_CASE("binary: addresses typed by the user") {
    test::FixtureProject fx;
    CHECK(parse_address(fx.program, "401060") == 0x401060u);  // hex without a prefix
    CHECK(parse_address(fx.program, "0x401060") == 0x401060u);
    CHECK(parse_address(fx.program, " 401060h ") == 0x401060u);
    CHECK(parse_address(fx.program, "add") == fx.va("add"));
    CHECK(parse_address(fx.program, "?add@@YAHHH@Z") == fx.va("add"));
    CHECK_FALSE(parse_address(fx.program, "0x10"));  // outside the image's sections
    CHECK_FALSE(parse_address(fx.program, "no such symbol"));
    CHECK_FALSE(parse_address(fx.program, ""));
}
