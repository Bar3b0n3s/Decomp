#include "formats/coff.hpp"
#include "formats/pdb.hpp"
#include "formats/pe.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;

namespace {

const pdb::Procedure* find_proc(const pdb::Reader& r, std::string_view name) {
    for (const auto& p : r.procedures())
        if (p.name == name) return &p;
    return nullptr;
}

const pdb::PublicSymbol* find_public(const pdb::Reader& r, std::string_view name) {
    for (const auto& p : r.publics())
        if (p.name == name) return &p;
    return nullptr;
}

} // namespace

TEST_CASE("PE32 fixture headers, sections, imports, exports, relocations") {
    auto image = pe::Image::load(test::fixture("x86/basic.exe")).value();
    CHECK(image.arch() == Arch::x86);
    CHECK_FALSE(image.is_pe32_plus());
    CHECK(image.image_base() == 0x400000);
    CHECK(image.entry_rva() == 0x11E0);
    CHECK(image.entry_point() == 0x4011E0);
    CHECK(image.image_size() == 20480);

    std::vector<std::string> names;
    for (const auto& s : image.sections()) names.push_back(s.name);
    CHECK(names == std::vector<std::string>{".text", ".rdata", ".data", ".reloc"});
    CHECK(image.is_code(0x401000));
    CHECK_FALSE(image.is_code(0x402000));
    CHECK(image.is_readonly_data(0x402000));

    REQUIRE(image.exports().size() == 1);
    CHECK(image.exports()[0].name == "exported_api");
    CHECK(image.exports()[0].ordinal == 1);
    CHECK(image.exports()[0].rva == 0x11C0);

    REQUIRE(image.imports().size() == 1);
    CHECK(image.imports()[0].dll == "kernel32.dll");
    CHECK(image.imports()[0].name == "ExitProcess");
    CHECK(image.imports()[0].iat_va == 0x402110);

    CHECK(image.base_relocations().size() == 25);  // 25 HIGHLOW; the ABSOLUTE padding entry is skipped
    CHECK(image.has_relocations());
    // dispatch's `jmp dword ptr [ecx*4+0x40201c]` has a relocated displacement at 0x401101.
    CHECK(image.is_relocated(0x401101));
    CHECK_FALSE(image.is_relocated(0x401100));

    REQUIRE(image.codeview());
    CHECK(image.codeview()->signature == "RSDS");
    CHECK(image.codeview()->pdb_path == "basic.pdb");
    CHECK(image.codeview()->age == 1);
    CHECK(image.rich_entries().empty());  // lld-link does not write a Rich header

    // The string literal lives in .rdata.
    CHECK(image.read_cstring(0x402010) == "hello world");
    CHECK(image.read<u32>(0x403000) == 3u);  // g_counter
}

TEST_CASE("PE32 /FIXED fixture has no base relocations") {
    auto image = pe::Image::load(test::fixture("x86/basic_fixed.exe")).value();
    CHECK(image.base_relocations().empty());
    CHECK_FALSE(image.has_relocations());
    CHECK(image.relocs_stripped());
}

TEST_CASE("PE32+ fixture headers and .pdata") {
    auto image = pe::Image::load(test::fixture("x64/basic.exe")).value();
    CHECK(image.arch() == Arch::x64);
    CHECK(image.is_pe32_plus());
    CHECK(image.image_base() == 0x140000000ull);
    CHECK(image.entry_rva() == 0x11F0);
    REQUIRE(image.imports().size() == 1);
    CHECK(image.imports()[0].iat_va == 0x140002140ull);
    CHECK(image.runtime_functions().size() == 3);
    for (const auto& f : image.runtime_functions()) CHECK(f.end_rva > f.begin_rva);
}

TEST_CASE("Rich header decoding") {
    // Minimal synthetic image: DOS header, Rich header at 0x80, then a PE header with no sections.
    std::vector<std::byte> data(0x200, std::byte{0});
    auto put32 = [&](usize off, u32 v) {
        for (int i = 0; i < 4; ++i) data[off + i] = std::byte(static_cast<u8>(v >> (8 * i)));
    };
    data[0] = std::byte{'M'};
    data[1] = std::byte{'Z'};
    put32(0x3C, 0x100);
    put32(0x100, 0x00004550u);  // "PE\0\0"
    data[0x104] = std::byte{0x4C};  // machine i386
    data[0x105] = std::byte{0x01};
    data[0x114] = std::byte{0xE0};  // SizeOfOptionalHeader = 0xE0
    data[0x118] = std::byte{0x0B};  // PE32 magic
    data[0x119] = std::byte{0x01};
    data[0x11A] = std::byte{6};     // linker 6.00
    const std::vector<pe::RichEntry> entries = {{0x000B, 8804, 12}, {0x0004, 8447, 1}};  // VC6 C++ compiler, linker 6.00
    // The key is the checksum of the DOS header and stub before the header, and of the entries.
    const u32 key = pe::rich_checksum(ByteSpan(data), 0x80, entries);
    put32(0x80, 0x536E6144u ^ key);  // DanS
    put32(0x84, key);
    put32(0x88, key);
    put32(0x8C, key);
    for (usize i = 0; i < entries.size(); ++i) {
        put32(0x90 + 8 * i, ((static_cast<u32>(entries[i].product_id) << 16) | entries[i].build) ^ key);
        put32(0x94 + 8 * i, entries[i].count ^ key);
    }
    put32(0xA0, 0x68636952u);  // "Rich"
    put32(0xA4, key);
    auto image = pe::Image::parse(data).value();
    REQUIRE(image.rich_entries().size() == 2);
    CHECK(image.rich_entries()[0].product_id == 0x000B);
    CHECK(image.rich_entries()[0].build == 8804);
    CHECK(image.rich_entries()[0].count == 12);
    CHECK(image.rich_entries()[1].product_id == 0x0004);
    REQUIRE(image.rich_header());
    CHECK(image.rich_header()->offset == 0x80);
    CHECK(image.rich_header()->checksum_ok);
    CHECK(pe::describe_rich_product(0x000B) == "C++ compiler 12.00 (Visual C++ 6.0)");
    const auto build = image.build_info();
    REQUIRE(build);
    REQUIRE(build->main_compiler());
    CHECK(build->main_compiler()->description() == "C++ compiler 12.00.8804");
    REQUIRE(build->linker);
    CHECK(build->linker->description() == "linker 6.00.8447");

    // An entry changed after linking no longer matches the checksum.
    put32(0x94, 13 ^ key);
    auto edited = pe::Image::parse(std::move(data)).value();
    REQUIRE(edited.rich_header());
    CHECK_FALSE(edited.rich_header()->checksum_ok);
    CHECK(edited.rich_entries()[0].count == 13);
}

TEST_CASE("PE parser rejects non-PE data") {
    std::vector<std::byte> junk(64, std::byte{0x41});
    auto r = pe::Image::parse(junk);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::parse);
}

TEST_CASE("PDB fixture procedures, publics, data, modules, contributions") {
    auto reader = pdb::Reader::load(test::fixture("x86/basic.pdb")).value();
    auto image = pe::Image::load(test::fixture("x86/basic.exe")).value();
    REQUIRE(image.codeview());
    CHECK(reader.matches(image.codeview()->guid, image.codeview()->age));

    struct Expected {
        const char* name;
        u32 rva, size;
        bool global;
    };
    const Expected expected[] = {
        {"Player::Hit", 0x1000, 30, true}, {"Player::Score", 0x1020, 50, true}, {"add", 0x1060, 15, true},
        {"read_counter", 0x1070, 6, true}, {"sum_array", 0x1080, 107, true},   {"dispatch", 0x10F0, 108, true},
        {"helper", 0x1160, 11, false},     {"message", 0x1170, 6, true},       {"scale", 0x1180, 17, true},
        {"mix", 0x11A0, 23, true},         {"exported_api", 0x11C0, 21, true}, {"entry", 0x11E0, 238, true},
        {"other_value", 0x12D0, 30, true},
    };
    for (const auto& e : expected) {
        CAPTURE(e.name);
        auto p = find_proc(reader, e.name);
        REQUIRE(p);
        CHECK(p->rva == e.rva);
        CHECK(p->size == e.size);
        CHECK(p->global == e.global);
    }

    auto pub = find_public(reader, "?add@@YAHHH@Z");
    REQUIRE(pub);
    CHECK(pub->rva == 0x1060);
    CHECK(pub->is_function);
    auto str = find_public(reader, "??_C@_0M@LACCCNMM@hello?5world?$AA@");
    REQUIRE(str);
    CHECK(str->rva == 0x2010);
    CHECK_FALSE(str->is_function);
    CHECK(find_public(reader, "__imp__ExitProcess@4"));

    bool saw_counter = false, saw_static = false;
    for (const auto& d : reader.data_symbols()) {
        if (d.name == "g_counter") saw_counter = d.global && d.rva == 0x3000;
        if (d.name == "s_calls") saw_static = !d.global;
    }
    CHECK(saw_counter);
    CHECK(saw_static);

    REQUIRE(reader.modules().size() >= 2);
    CHECK(reader.modules()[0].name == "C:\\fixtures\\x86\\basic.obj");
    CHECK(reader.modules()[1].name == "C:\\fixtures\\x86\\other.obj");
    auto add_proc = find_proc(reader, "add");
    bool covered = std::ranges::any_of(reader.contributions(), [&](const pdb::Contribution& c) {
        return c.module == 0 && c.rva <= add_proc->rva && add_proc->rva < c.rva + c.size;
    });
    CHECK(covered);
}

TEST_CASE("COFF object fixture: COMDAT sections, symbols, relocations") {
    auto obj = coff::Object::load(test::fixture("x86/basic.obj")).value();
    CHECK(obj.arch() == Arch::x86);
    CHECK_FALSE(obj.is_bigobj());

    auto add = obj.find_defined("?add@@YAHHH@Z");
    REQUIRE(add);
    CHECK(add->is_function());
    CHECK(add->is_external());
    auto sec = obj.section(add->section_number);
    REQUIRE(sec);
    CHECK(sec->name == ".text");
    CHECK(sec->is_code());
    REQUIRE(sec->comdat);
    CHECK(obj.symbol_size(*add) == 15);

    // add: mov eax,[esp+8]; add eax,[esp+4]; add eax,[?g_counter@@3HA]; ret
    REQUIRE(sec->relocations.size() == 1);
    const auto& rel = sec->relocations[0];
    CHECK(rel.type == coff::reloc_i386::dir32);
    CHECK(obj.relocation_size(rel.type) == 4);
    CHECK(obj.relocation_type_name(rel.type) == "DIR32");
    auto target = obj.symbol_at_index(rel.symbol_index);
    REQUIRE(target);
    CHECK(target->name == "?g_counter@@3HA");
    bool target_is_code = target->is_defined() && obj.section(target->section_number)->is_code();
    CHECK_FALSE(target_is_code);

    auto functions = obj.function_symbols();
    auto has = [&](std::string_view n) {
        return std::ranges::any_of(functions, [&](const coff::Symbol* s) { return s->name == n; });
    };
    CHECK(has("?Hit@Player@@QAEXH@Z"));
    CHECK(has("?dispatch@@YAHHH@Z"));
    CHECK(has("_entry"));
    CHECK_FALSE(has("?g_counter@@3HA"));
}

TEST_CASE("COFF x64 object uses REL32 for RIP-relative data") {
    auto obj = coff::Object::load(test::fixture("x64/basic.obj")).value();
    CHECK(obj.arch() == Arch::x64);
    auto read_counter = obj.find_defined("?read_counter@@YAHXZ");
    REQUIRE(read_counter);
    auto sec = obj.section(read_counter->section_number);
    REQUIRE(sec);
    REQUIRE(sec->relocations.size() == 1);
    CHECK(sec->relocations[0].type == coff::reloc_amd64::rel32);
    CHECK(obj.relocation_is_pc_relative(sec->relocations[0].type));
    CHECK(obj.symbol_at_index(sec->relocations[0].symbol_index)->name == "?g_counter@@3HA");
}
