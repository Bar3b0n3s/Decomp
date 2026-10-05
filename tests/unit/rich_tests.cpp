// The Rich header's compiler table (formats/rich.hpp) and the toolchain suggestion built on it
// (matching/suggest.hpp): reference headers of each Visual Studio release map to the right compiler.

#include "formats/rich.hpp"
#include "matching/suggest.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;
using pe::RichEntry;

namespace {

pe::BuildInfo build_of(std::vector<RichEntry> entries, u8 linker_major, u8 linker_minor) {
    pe::RichHeader header;
    header.entries = std::move(entries);
    header.checksum_ok = true;
    return pe::identify_build(header, linker_major, linker_minor);
}

bool has_note(const matching::ToolchainSuggestion& s, std::string_view text) {
    return std::ranges::any_of(s.notes, [&](const std::string& n) { return n.find(text) != std::string::npos; });
}

} // namespace

TEST_CASE("Rich product ids: tools, versions and releases") {
    const pe::RichProduct* cpp = pe::rich_product(0x000B);
    REQUIRE(cpp);
    CHECK(cpp->name == "Utc12_CPP");
    CHECK(cpp->tool == pe::RichTool::compiler);
    CHECK(cpp->language == "C++");
    CHECK(cpp->major == 12);
    CHECK(cpp->minor == 0);
    CHECK(cpp->release == pe::VsRelease::vc6);
    CHECK(pe::rich_product(0x0016)->variant == "Standard edition");
    CHECK(pe::rich_product(0x0004)->tool == pe::RichTool::linker);
    CHECK(pe::rich_product(0x000E)->tool == pe::RichTool::assembler);
    CHECK(pe::rich_product(0x0109)->variant == "LTCG");
    CHECK(pe::rich_product(0x010D)->variant == "PGO optimized");
    CHECK(pe::rich_product(0x0102)->name == "Linker1400");
    CHECK(pe::rich_product(0x0104)->name == "Utc1900_C");
    CHECK(pe::rich_product(0x0056)->name == "Linker624");
    CHECK(pe::rich_product(0x010E)->name == "Utc1900_POGO_O_CPP");
    CHECK_FALSE(pe::rich_product(0x010F));
    CHECK(pe::describe_rich_product(0x0060) == "C++ compiler 13.10 (Visual Studio .NET 2003)");
    CHECK(pe::describe_rich_product(0x0001) == "imported functions");
    CHECK(pe::describe_rich_product(0x0300) == "product 0x0300");
}

TEST_CASE("compiler versions of reference builds of each release") {
    struct Case {
        u16 product;
        u16 build;
        const char* version;
        const char* visual_studio;
        const char* name;
    };
    for (const Case& k : {
             Case{0x000A, 8168, "12.00.8168", "Visual C++ 6.0", "vc6"},
             Case{0x000B, 8804, "12.00.8804", "Visual C++ 6.0", "vc6"},
             Case{0x001D, 9466, "13.00.9466", "Visual Studio .NET 2002", "vs2002"},
             Case{0x0060, 3077, "13.10.3077", "Visual Studio .NET 2003", "vs2003"},
             Case{0x006E, 50727, "14.00.50727", "Visual Studio 2005", "vs2005"},
             Case{0x0084, 30729, "15.00.30729", "Visual Studio 2008", "vs2008"},
             Case{0x00AB, 40219, "16.00.40219", "Visual Studio 2010", "vs2010"},
             Case{0x00CF, 61030, "17.00.61030", "Visual Studio 2012", "vs2012"},
             Case{0x00E1, 40629, "18.00.40629", "Visual Studio 2013", "vs2013"},
             Case{0x0105, 23026, "19.00.23026", "Visual Studio 2015", "vs2015"},
             Case{0x0105, 24215, "19.00.24215", "Visual Studio 2015 Update 3", "vs2015"},
             Case{0x0105, 25017, "19.10.25017", "Visual Studio 2017 15.0", "vs2017"},
             Case{0x0105, 27054, "19.16.27054", "Visual Studio 2017 15.9", "vs2017"},
             Case{0x0105, 27508, "19.20.27508", "Visual Studio 2019 16.0", "vs2019"},
             Case{0x0105, 29913, "19.28.29913", "Visual Studio 2019 16.9", "vs2019"},
             Case{0x0105, 30159, "19.29.30159", "Visual Studio 2019 16.11", "vs2019"},
             Case{0x0105, 30705, "19.30.30705", "Visual Studio 2022 17.0", "vs2022"},
             Case{0x0105, 33523, "19.39.33523", "Visual Studio 2022 17.9", "vs2022"},
             Case{0x0105, 35213, "19.44.35213", "Visual Studio 2022 17.14", "vs2022"},
             Case{0x0105, 36231, "19.x.36231", "Visual Studio 2026", "vs2026"},  // newer than the build table
             Case{0x0102, 30159, "14.29.30159", "Visual Studio 2019 16.11", "vs2019"},
         }) {
        CAPTURE(k.product);
        CAPTURE(k.build);
        const pe::ToolVersion v = pe::tool_version(*pe::rich_product(k.product), k.build);
        CHECK(v.text() == k.version);
        CHECK(v.visual_studio == k.visual_studio);
        CHECK(v.suggested_name == k.name);
    }
    // The image's linker version gives the minor version of the toolset's own tools.
    const pe::ToolVersion v = pe::tool_version(*pe::rich_product(0x0104), 36231, 51);
    CHECK(v.minor_known);
    CHECK(v.text() == "19.51.36231");
    const auto build = build_of({{0x0104, 36231, 30}, {0x0102, 36231, 1}, {0x0104, 35213, 2}}, 14, 51);
    REQUIRE(build.main_compiler());
    CHECK(build.main_compiler()->version.text() == "19.51.36231");
    CHECK(build.compilers[1].version.text() == "19.44.35213");  // another build: from the table
}

TEST_CASE("a VC6 game: the newest build of the linker's release is the game's compiler") {
    // The runtime library's C objects (an older build) outnumber the game's own.
    const auto build = build_of({{0x0000, 0, 3},
                                 {0x0001, 0, 150},
                                 {0x000A, 8168, 140},
                                 {0x000B, 8804, 60},
                                 {0x000A, 8804, 9},
                                 {0x000E, 7299, 2},
                                 {0x0006, 1736, 1},
                                 {0x0004, 8447, 1}},
                                6, 0);
    CHECK(build.imports == 150);
    CHECK(build.unmarked == 3);
    REQUIRE(build.compilers.size() == 3);
    CHECK(build.compilers[0].entry.build == 8168);  // most objects first
    REQUIRE(build.linker);
    CHECK(build.linker->description() == "linker 6.00.8447");
    REQUIRE(build.assemblers.size() == 1);
    CHECK(build.assemblers[0].description() == "MASM 6.13.7299");
    CHECK(build.objects_in("C") == 149);
    CHECK(build.objects_in("C++") == 60);
    const pe::BuildTool* main = build.main_compiler();
    REQUIRE(main);
    CHECK(main->description() == "C++ compiler 12.00.8804");

    const auto s = matching::suggest_toolchain(build).value();
    CHECK(s.name == "vc6");
    CHECK(s.visual_studio == "Visual C++ 6.0");
    CHECK(s.compiler == "C++ compiler 12.00.8804");
    CHECK(s.build == 8804);
    CHECK(has_note(s, "149 C and 60 C++ objects"));
    CHECK(has_note(s, "Other builds of the same compiler made some objects (8168)"));
    CHECK(has_note(s, "2 objects were assembled with MASM 6.13.7299"));
    CHECK(has_note(s, "3 objects carry no tool id"));
    CHECK_FALSE(has_note(s, "optimizer"));

    // How toolchains fit: the same build, another service pack, another release, no compiler id.
    CHECK(matching::toolchain_fit(build, (0x000Bu << 16) | 8804).fit == matching::ToolchainFit::same_build);
    CHECK(matching::toolchain_fit(build, (0x000Au << 16) | 8804).fit == matching::ToolchainFit::same_build);  // C: the same compiler
    const auto sp = matching::toolchain_fit(build, (0x000Bu << 16) | 8168);
    CHECK(sp.fit == matching::ToolchainFit::same_release);
    CHECK(sp.text.find("build 8804") != std::string::npos);
    const auto other = matching::toolchain_fit(build, (0x0105u << 16) | 30159);
    CHECK(other.fit == matching::ToolchainFit::other_release);
    CHECK(other.toolchain_compiler == "C++ compiler 19.29.30159 (Visual Studio 2019 16.11)");
    CHECK(matching::toolchain_fit(build, std::nullopt).fit == matching::ToolchainFit::unknown);
}

TEST_CASE("toolchain suggestion notes: editions without an optimizer, LTCG, PGO, other releases") {
    const auto standard = build_of({{0x0016, 8804, 20}, {0x0004, 8447, 1}}, 6, 0);
    const auto s = matching::suggest_toolchain(standard).value();
    CHECK(s.name == "vc6");
    CHECK(has_note(s, "The Standard edition compiler has no optimizer"));

    // VS2019 with whole-program optimization, a library from VS2017, and a newer one from VS2022.
    const auto modern = build_of({{0x0109, 30159, 40}, {0x0108, 30159, 5}, {0x0105, 27054, 12}, {0x0105, 33523, 4}, {0x0102, 30159, 1}}, 14, 29);
    const pe::BuildTool* main = modern.main_compiler();
    REQUIRE(main);
    CHECK(main->description() == "C++ compiler 19.29.30159 (LTCG)");  // the linker's release, not the newest
    const auto m = matching::suggest_toolchain(modern).value();
    CHECK(m.name == "vs2019");
    CHECK(m.visual_studio == "Visual Studio 2019 16.11");
    CHECK(has_note(m, "link-time code generation"));
    CHECK(has_note(m, "12 objects came from Visual Studio 2017 15.9"));
    CHECK(has_note(m, "4 objects came from Visual Studio 2022 17.9"));

    const auto pgo = build_of({{0x010D, 30159, 7}, {0x0102, 30159, 1}}, 14, 29);
    CHECK(has_note(matching::suggest_toolchain(pgo).value(), "optimized with a profile"));

    pe::RichHeader edited;
    edited.entries = {{0x000B, 8804, 1}};
    edited.checksum_ok = false;
    CHECK(has_note(matching::suggest_toolchain(pe::identify_build(edited, 6, 0)).value(), "checksum does not match"));

    // Nothing to suggest without a compiler.
    CHECK_FALSE(matching::suggest_toolchain(build_of({{0x0001, 0, 20}, {0x0004, 8447, 1}}, 6, 0)));
}

TEST_CASE("Rich header parsing: the DanS marker, the entries and the checksum") {
    std::vector<std::byte> data(0x100, std::byte{0});
    auto put32 = [&](usize off, u32 v) {
        for (int i = 0; i < 4; ++i) data[off + i] = std::byte(static_cast<u8>(v >> (8 * i)));
    };
    data[0] = std::byte{'M'};
    data[1] = std::byte{'Z'};
    put32(0x3C, 0xC0);
    for (usize i = 0x40; i < 0x70; ++i) data[i] = std::byte(static_cast<u8>(i * 7));  // a DOS stub
    const std::vector<RichEntry> entries = {{0x0105, 30159, 25}, {0x0102, 30159, 1}, {0x0001, 0, 80}};
    const u32 key = pe::rich_checksum(ByteSpan(data), 0x70, entries);
    put32(0x70, 0x536E6144u ^ key);
    put32(0x74, key);
    put32(0x78, key);
    put32(0x7C, key);
    for (usize i = 0; i < entries.size(); ++i) {
        put32(0x80 + 8 * i, ((static_cast<u32>(entries[i].product_id) << 16) | entries[i].build) ^ key);
        put32(0x84 + 8 * i, entries[i].count ^ key);
    }
    put32(0x98, 0x68636952u);
    put32(0x9C, key);
    const auto h = pe::parse_rich_header(ByteSpan(data), 0xC0);
    REQUIRE(h);
    CHECK(h->offset == 0x70);
    CHECK(h->key == key);
    CHECK(h->checksum_ok);
    REQUIRE(h->entries.size() == 3);
    CHECK(h->entries[0].product_id == 0x0105);
    CHECK(h->entries[0].build == 30159);
    CHECK(h->entries[2].count == 80);
    // A changed DOS stub byte breaks the checksum; without "Rich" there is no header.
    data[0x50] = std::byte{0xFF};
    CHECK_FALSE(pe::parse_rich_header(ByteSpan(data), 0xC0)->checksum_ok);
    put32(0x98, 0);
    CHECK_FALSE(pe::parse_rich_header(ByteSpan(data), 0xC0));
}
