#include "core/fs.hpp"
#include "core/process.hpp"
#include "formats/archive.hpp"
#include "formats/coff_writer.hpp"
#include "formats/pe.hpp"
#include "llvm_fixture.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;
using namespace decomp::coff;

namespace {

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (int v : values) out.push_back(static_cast<std::byte>(v));
    return out;
}

const coff::Symbol* symbol_named(const Object& obj, std::string_view name) {
    for (const auto& s : obj.symbols())
        if (s.name == name) return &s;
    return nullptr;
}

} // namespace

TEST_CASE("coff writer: alignment flags") {
    CHECK(alignment_flags(1) == 0x00100000);
    CHECK(alignment_flags(16) == 0x00500000);
    CHECK(alignment_flags(8192) == 0x00E00000);
    CHECK(alignment_flags(3) == 0);
    CHECK(alignment_of(0x60500020) == 16);
    CHECK(alignment_of(0x40000040) == 1);
}

TEST_CASE("coff writer: sections, symbols, relocations and directives round trip") {
    ObjectWriter w(pe::machine::i386);
    const u32 text = w.add_section(".text$mn", pe::scn::cnt_code | pe::scn::mem_execute | pe::scn::mem_read | alignment_flags(16),
                                   bytes({0xA1, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0xC3}));
    const u32 bss = w.add_bss_section(".bss", pe::scn::cnt_uninitialized_data | pe::scn::mem_read | pe::scn::mem_write | alignment_flags(4), 12);
    const u32 str = w.add_section(".rdata", pe::scn::cnt_initialized_data | pe::scn::mem_read | alignment_flags(1), bytes({'h', 'i', 0}));
    const u32 str_sym = w.add_symbol("??_C@_02DKCKIIND@hi?$AA@", 0, static_cast<i32>(str));
    w.set_comdat(str, comdat_select::any, str_sym);
    w.add_absolute("@feat.00", 1);
    w.add_symbol("_a_function_with_a_long_name", 0, static_cast<i32>(text), storage::external, 0x20);
    const u32 counter = w.add_symbol("_counter", 4, static_cast<i32>(bss));
    const u32 callee = w.undefined("_callee");
    CHECK(w.undefined("_callee") == callee);
    CHECK(w.add_symbol("_counter", 8, static_cast<i32>(bss)) == counter);  // defined once
    w.add_relocation(text, 1, counter, reloc_i386::dir32);
    w.add_relocation(text, 6, callee, reloc_i386::rel32);
    w.add_directive("/EXPORT:a_function_with_a_long_name=_a_function_with_a_long_name");
    w.add_directive("/INCLUDE:__imp__ExitProcess@4");
    auto data = w.write();
    REQUIRE(data);
    auto obj = Object::parse(std::move(*data));
    REQUIRE(obj);

    REQUIRE(obj->sections().size() == 4);
    const auto& t = obj->sections()[0];
    CHECK(t.name == ".text$mn");
    CHECK(t.size == 11);
    CHECK(t.is_code());
    CHECK(alignment_of(t.characteristics) == 16);
    REQUIRE(t.relocations.size() == 2);
    CHECK(t.relocations[0].offset == 1);
    CHECK(t.relocations[0].type == reloc_i386::dir32);
    CHECK(obj->symbol_at_index(t.relocations[0].symbol_index)->name == "_counter");
    CHECK(obj->symbol_at_index(t.relocations[1].symbol_index)->name == "_callee");
    CHECK_FALSE(obj->symbol_at_index(t.relocations[1].symbol_index)->is_defined());

    const auto& b = obj->sections()[1];
    CHECK(b.is_bss());
    CHECK(b.size == 12);
    CHECK(b.data.empty());

    const auto& r = obj->sections()[2];
    REQUIRE(r.comdat);
    CHECK(r.comdat->selection == comdat_select::any);
    CHECK((r.characteristics & pe::scn::lnk_comdat) != 0);
    // The COMDAT symbol follows the section symbol.
    const auto* section_symbol = symbol_named(*obj, ".rdata");
    REQUIRE(section_symbol);
    CHECK(obj->symbol_at_index(section_symbol->index + 2)->name == "??_C@_02DKCKIIND@hi?$AA@");

    const auto& d = obj->sections()[3];
    CHECK(d.name == ".drectve");
    CHECK(std::string(reinterpret_cast<const char*>(d.data.data()), d.data.size()) ==
          " /EXPORT:a_function_with_a_long_name=_a_function_with_a_long_name /INCLUDE:__imp__ExitProcess@4");

    const auto* f = obj->find_defined("_a_function_with_a_long_name");
    REQUIRE(f);
    CHECK(f->section_number == 1);
    CHECK(f->is_function());
    const auto* c = obj->find_defined("_counter");
    REQUIRE(c);
    CHECK(c->value == 4);
    const auto* feat = symbol_named(*obj, "@feat.00");
    REQUIRE(feat);
    CHECK(feat->section_number == -1);
    CHECK(feat->value == 1);
}

TEST_CASE("coff writer: relocation counts past 65535 overflow into the first record") {
    ObjectWriter w(pe::machine::amd64);
    const u32 data = w.add_section(".data", pe::scn::cnt_initialized_data | pe::scn::mem_read | pe::scn::mem_write | alignment_flags(8),
                                   std::vector<std::byte>(8 * 70000));
    const u32 target = w.undefined("target");
    for (u32 i = 0; i < 70000; ++i) w.add_relocation(data, 8 * i, target, reloc_amd64::addr64);
    auto bytes_out = w.write();
    REQUIRE(bytes_out);
    auto obj = Object::parse(std::move(*bytes_out));
    REQUIRE(obj);
    const auto& s = obj->sections()[0];
    CHECK((s.characteristics & pe::scn::lnk_nreloc_ovfl) != 0);
    REQUIRE(s.relocations.size() == 70000);
    CHECK(s.relocations.back().offset == 8 * 69999);
}

TEST_CASE("coff writer: import name types") {
    CHECK(import_name_type_for({.symbol = "_ExitProcess@4", .name = "ExitProcess"}, pe::machine::i386) == import_name_type::undecorate);
    CHECK(import_name_type_for({.symbol = "_printf", .name = "printf"}, pe::machine::i386) == import_name_type::no_prefix);
    CHECK(import_name_type_for({.symbol = "ExitProcess", .name = "ExitProcess"}, pe::machine::amd64) == import_name_type::name);
    CHECK(import_name_type_for({.symbol = "?f@@YAXXZ", .name = "?f@@YAXXZ"}, pe::machine::i386) == import_name_type::name);
    CHECK(import_name_type_for({.symbol = "_Ord", .ordinal = 7}, pe::machine::i386) == import_name_type::ordinal);
    CHECK_FALSE(import_name_type_for({.symbol = "_Other@4", .name = "ExitProcess"}, pe::machine::i386));
}

TEST_CASE("coff writer: import libraries list descriptors and short import objects") {
    std::vector<ImportEntry> entries{{.symbol = "_ExitProcess@4", .name = "ExitProcess", .hint = 0x167},
                                     {.symbol = "_GetVersion@0", .name = "GetVersion", .hint = 0x2a5}};
    auto lib = write_import_library("KERNEL32.dll", pe::machine::i386, entries);
    REQUIRE(lib);
    auto archive = archive::Archive::parse(*lib);
    REQUIRE(archive);
    REQUIRE(archive->members().size() == 5);
    for (const auto& m : archive->members()) CHECK(m.name == "KERNEL32.dll");
    const auto& exit = archive->members()[3];
    REQUIRE(exit.import);
    CHECK(exit.import->symbol == "_ExitProcess@4");
    CHECK(exit.import->dll == "KERNEL32.dll");
    CHECK(exit.import->ordinal_or_hint == 0x167);
    CHECK(exit.import->type == import_type::code);
    CHECK(exit.import->name_type == import_name_type::undecorate);
    std::vector<std::string> index;
    for (const auto& [name, member] : archive->index()) index.push_back(name);
    std::ranges::sort(index);
    CHECK(index == std::vector<std::string>{"_ExitProcess@4", "_GetVersion@0", "__IMPORT_DESCRIPTOR_KERNEL32", "__NULL_IMPORT_DESCRIPTOR",
                                            "__imp__ExitProcess@4", "__imp__GetVersion@0", "\x7fKERNEL32_NULL_THUNK_DATA"});
    // The descriptor object points at the DLL's name and the lookup and address tables.
    auto descriptor = Object::parse(archive->members()[0].data);
    REQUIRE(descriptor);
    CHECK(descriptor->sections()[0].name == ".idata$2");
    CHECK(descriptor->sections()[0].relocations.size() == 3);
    CHECK(descriptor->sections()[1].name == ".idata$6");

    std::vector<ImportEntry> bad{{.symbol = "_Other@4", .name = "ExitProcess"}};
    CHECK_FALSE(write_import_library("KERNEL32.dll", pe::machine::i386, bad));
}

TEST_CASE("coff writer: long member names go to the long names member") {
    std::vector<ArchiveMember> members{{"a_member_with_a_long_name.obj", bytes({1, 2, 3}), {"_one"}},
                                       {"short.obj", bytes({4}), {"_two", "_three"}}};
    auto data = write_archive(members);
    auto archive = archive::Archive::parse(data);
    REQUIRE(archive);
    REQUIRE(archive->members().size() == 2);
    CHECK(archive->members()[0].name == "a_member_with_a_long_name.obj");
    CHECK(archive->members()[1].name == "short.obj");
    CHECK(archive->members()[1].data == bytes({4}));
    CHECK(archive->index().size() == 3);
}

TEST_CASE("pe: identity fields and header descriptions of the fixture") {
    auto image = pe::Image::load(test::fixture("x86/basic.exe"));
    REQUIRE(image);
    auto fields = image->identity_fields();
    std::vector<std::string> names;
    for (const auto& f : fields) names.push_back(f.name);
    CHECK(std::ranges::find(names, "COFF header TimeDateStamp") != names.end());
    CHECK(std::ranges::find(names, "optional header CheckSum") != names.end());
    CHECK(std::ranges::find(names, "CodeView GUID") != names.end());
    CHECK(std::ranges::find(names, "CodeView age") != names.end());
    CHECK(std::ranges::find(names, "export directory TimeDateStamp") != names.end());
    // lld-link /Brepro: a CodeView entry and an empty repro entry.
    REQUIRE(image->debug_entries().size() == 2);
    CHECK(image->debug_entries()[0].type == pe::debug_type::codeview);
    CHECK(image->debug_entries()[1].type == pe::debug_type::repro);
    const u32 timestamp_offset = image->pe_offset() + 8;
    CHECK(read_le<u32>(image->data(), timestamp_offset) == image->timestamp());
    CHECK(image->describe_header_offset(timestamp_offset) == "COFF header TimeDateStamp");
    CHECK(image->describe_header_offset(image->optional_header().checksum_offset) == "optional header CheckSum");
    CHECK(image->describe_header_offset(image->header_size()) == "");
    CHECK(image->export_name() == "basic.exe");
    CHECK(image->optional_header().section_alignment == 0x1000);
    CHECK(image->optional_header().file_alignment == 0x200);
    REQUIRE(image->imports().size() == 1);
    CHECK(image->imports()[0].name == "ExitProcess");
}

TEST_CASE("coff writer: lld-link links written objects against a written import library") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("lld-link not found; skipping");
        return;
    }
    auto tmp = fs::TempDir::create("decomp-coff-writer").value();
    for (Arch arch : {Arch::x86, Arch::x64}) {
        CAPTURE(to_string(arch));
        const bool x64 = arch == Arch::x64;
        const u16 machine = x64 ? pe::machine::amd64 : pe::machine::i386;
        const std::string imp = x64 ? "__imp_ExitProcess" : "__imp__ExitProcess@4";
        ObjectWriter w(machine);
        // entry: call [__imp_ExitProcess] (x86: absolute, x64: RIP-relative), then the data the code points at.
        const u32 text = w.add_section(".text", pe::scn::cnt_code | pe::scn::mem_execute | pe::scn::mem_read | alignment_flags(16),
                                       bytes({0xFF, 0x15, 0, 0, 0, 0, 0xC3}));
        w.add_symbol(x64 ? "entry" : "_entry", 0, static_cast<i32>(text), storage::external, 0x20);
        w.add_relocation(text, 2, w.undefined(imp), x64 ? reloc_amd64::rel32 : reloc_i386::dir32);
        const u32 data = w.add_section(".data", pe::scn::cnt_initialized_data | pe::scn::mem_read | pe::scn::mem_write | alignment_flags(4),
                                       std::vector<std::byte>(x64 ? 8 : 4));
        w.add_relocation(data, 0, w.section_symbol(text), x64 ? reloc_amd64::addr64 : reloc_i386::dir32);
        w.add_absolute("@feat.00", 1);
        auto object = w.write();
        REQUIRE(object);
        const auto dir = tmp.path() / std::string(to_string(arch));
        REQUIRE(fs::create_directories(dir));
        REQUIRE(fs::write_file(dir / "entry.obj", *object));
        std::vector<ImportEntry> entries{{.symbol = x64 ? "ExitProcess" : "_ExitProcess@4", .name = "ExitProcess", .hint = 0x167}};
        auto lib = write_import_library("KERNEL32.dll", machine, entries);
        REQUIRE(lib);
        REQUIRE(fs::write_file(dir / "kernel32.lib", *lib));
        ProcessSpec ld;
        ld.argv = {tools->lld_link, "/nologo", "/nodefaultlib", "/entry:entry", "/subsystem:console", "/release",
                   "/out:" + fs::to_utf8(dir / "entry.exe"), fs::to_utf8(dir / "entry.obj"), fs::to_utf8(dir / "kernel32.lib")};
        auto r = run_process(ld);
        REQUIRE(r);
        REQUIRE_MESSAGE(r->ok(), (r->out + r->err));
        auto image = pe::Image::load(dir / "entry.exe");
        REQUIRE(image);
        REQUIRE(image->imports().size() == 1);
        CHECK(image->imports()[0].dll == "KERNEL32.dll");
        CHECK(image->imports()[0].name == "ExitProcess");
        CHECK(image->imports()[0].hint == 0x167);
        // The data's pointer to .text is base-relocated.
        CHECK_FALSE(image->base_relocations().empty());
        // /release: the linker's checksum is the one compute_checksum() gives.
        CHECK(image->optional_header().checksum != 0);
        CHECK(pe::compute_checksum(image->data(), image->optional_header().checksum_offset) == image->optional_header().checksum);
    }
}
