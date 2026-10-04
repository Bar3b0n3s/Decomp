#include "formats/coff.hpp"

#include "core/fs.hpp"
#include "formats/pe.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>

namespace decomp::coff {

bool Section::is_code() const { return (characteristics & (pe::scn::cnt_code | pe::scn::mem_execute)) != 0; }
bool Section::is_bss() const { return (characteristics & pe::scn::cnt_uninitialized_data) != 0; }
bool Section::is_debug() const { return name.starts_with(".debug") || (characteristics & 0x00000200) != 0; }  // LNK_INFO

bool Symbol::is_section_symbol() const {
    return storage_class == storage::static_ && value == 0 && aux_count > 0 && !name.empty() && name[0] == '.';
}

Result<Object> Object::load(const std::filesystem::path& path) {
    TRY_ASSIGN(auto data, fs::read_file(path));
    auto obj = parse(std::move(data));
    if (!obj) return std::unexpected(std::move(obj.error()).with_context(fs::to_utf8(path)));
    return obj;
}

Result<Object> Object::parse(std::vector<std::byte> data) {
    Object obj;
    ByteSpan d = data;
    u32 section_count = 0, symtab_offset = 0, symbol_count = 0;
    usize section_table = 0;
    unsigned symbol_size = 18;

    if (read_le<u16>(d, 0) == 0 && read_le<u16>(d, 2) == 0xFFFF) {
        // ANON_OBJECT_HEADER_BIGOBJ (/bigobj)
        if (read_le<u16>(d, 4).value_or(0) < 2) return make_error(ErrorCode::unsupported, "unsupported anonymous object header");
        obj.bigobj_ = true;
        obj.machine_ = read_le<u16>(d, 6).value_or(0);
        section_count = read_le<u32>(d, 44).value_or(0);
        symtab_offset = read_le<u32>(d, 48).value_or(0);
        symbol_count = read_le<u32>(d, 52).value_or(0);
        section_table = 56;
        symbol_size = 20;
    } else {
        obj.machine_ = read_le<u16>(d, 0).value_or(0);
        section_count = read_le<u16>(d, 2).value_or(0);
        symtab_offset = read_le<u32>(d, 8).value_or(0);
        symbol_count = read_le<u32>(d, 12).value_or(0);
        section_table = 20 + read_le<u16>(d, 16).value_or(0);
    }
    if (obj.machine_ != pe::machine::i386 && obj.machine_ != pe::machine::amd64)
        return make_error(ErrorCode::unsupported, "not an x86/x64 COFF object (machine {:#06x})", obj.machine_);
    if (section_count > 1u << 20 || symbol_count > 1u << 26) return make_error(ErrorCode::parse, "implausible COFF header counts");
    if (u64(symtab_offset) + u64(symbol_count) * symbol_size > d.size())
        return make_error(ErrorCode::parse, "symbol table outside the file");

    usize strtab = symtab_offset + usize(symbol_count) * symbol_size;
    auto string_at = [&](u32 offset) -> std::string {
        return read_cstring_at(d, strtab + offset, 65536).value_or(std::string());
    };

    for (u32 i = 0; i < section_count; ++i) {
        usize h = section_table + usize(i) * 40;
        if (h + 40 > d.size()) return make_error(ErrorCode::parse, "section table outside the file");
        Section s;
        s.number = i + 1;
        char name[9] = {};
        std::memcpy(name, d.data() + h, 8);
        s.name = name;
        if (s.name.starts_with('/')) {
            u32 off = 0;
            auto [p, ec] = std::from_chars(s.name.data() + 1, s.name.data() + s.name.size(), off);
            if (ec == std::errc{}) s.name = string_at(off);
        }
        s.size = read_le<u32>(d, h + 16).value_or(0);
        u32 raw_offset = read_le<u32>(d, h + 20).value_or(0);
        u32 reloc_offset = read_le<u32>(d, h + 24).value_or(0);
        u32 reloc_count = read_le<u16>(d, h + 32).value_or(0);
        s.characteristics = read_le<u32>(d, h + 36).value_or(0);
        if (!s.is_bss() && s.size > 0) {
            if (u64(raw_offset) + s.size > d.size()) return make_error(ErrorCode::parse, "section {} data outside the file", s.name);
            s.data.assign(d.begin() + raw_offset, d.begin() + raw_offset + s.size);
        }
        if ((s.characteristics & pe::scn::lnk_nreloc_ovfl) && reloc_count == 0xFFFF) {
            reloc_count = read_le<u32>(d, reloc_offset).value_or(0);
            reloc_offset += 10;  // the first record only carries the real count
            --reloc_count;
        }
        if (u64(reloc_offset) + u64(reloc_count) * 10 > d.size())
            return make_error(ErrorCode::parse, "section {} relocations outside the file", s.name);
        for (u32 r = 0; r < reloc_count; ++r) {
            usize ro = reloc_offset + usize(r) * 10;
            s.relocations.push_back({read_le<u32>(d, ro).value_or(0), read_le<u32>(d, ro + 4).value_or(0),
                                     read_le<u16>(d, ro + 8).value_or(0)});
        }
        std::ranges::sort(s.relocations, {}, &Relocation::offset);
        obj.sections_.push_back(std::move(s));
    }

    obj.raw_to_symbol_.assign(symbol_count, -1);
    for (u32 i = 0; i < symbol_count;) {
        usize so = symtab_offset + usize(i) * symbol_size;
        Symbol sym;
        sym.index = i;
        if (read_le<u32>(d, so) == 0) {
            sym.name = string_at(read_le<u32>(d, so + 4).value_or(0));
        } else {
            char name[9] = {};
            std::memcpy(name, d.data() + so, 8);
            sym.name = name;
        }
        sym.value = read_le<u32>(d, so + 8).value_or(0);
        if (obj.bigobj_) {
            sym.section_number = read_le<i32>(d, so + 12).value_or(0);
            sym.type = read_le<u16>(d, so + 16).value_or(0);
            sym.storage_class = read_le<u8>(d, so + 18).value_or(0);
            sym.aux_count = read_le<u8>(d, so + 19).value_or(0);
        } else {
            sym.section_number = read_le<i16>(d, so + 12).value_or(0);
            sym.type = read_le<u16>(d, so + 14).value_or(0);
            sym.storage_class = read_le<u8>(d, so + 16).value_or(0);
            sym.aux_count = read_le<u8>(d, so + 17).value_or(0);
        }
        // Section definition aux record: COMDAT selection and associativity.
        if (sym.is_section_symbol() && sym.section_number > 0 && u32(sym.section_number) <= obj.sections_.size()) {
            usize ao = so + symbol_size;
            auto& sec = obj.sections_[sym.section_number - 1];
            if (sec.characteristics & pe::scn::lnk_comdat) {
                ComdatInfo c;
                c.checksum = read_le<u32>(d, ao + 8).value_or(0);
                c.associated_section = read_le<u16>(d, ao + 12).value_or(0);
                c.selection = read_le<u8>(d, ao + 14).value_or(0);
                if (obj.bigobj_) c.associated_section |= u32(read_le<u16>(d, ao + 16).value_or(0)) << 16;
                sec.comdat = c;
            }
        }
        obj.raw_to_symbol_[i] = static_cast<i32>(obj.symbols_.size());
        u32 step = 1u + sym.aux_count;
        obj.symbols_.push_back(std::move(sym));
        i += step;
    }
    return obj;
}

Arch Object::arch() const { return machine_ == pe::machine::amd64 ? Arch::x64 : Arch::x86; }

const Section* Object::section(i32 number) const {
    if (number <= 0 || u32(number) > sections_.size()) return nullptr;
    return &sections_[number - 1];
}

const Symbol* Object::symbol_at_index(u32 raw_index) const {
    if (raw_index >= raw_to_symbol_.size() || raw_to_symbol_[raw_index] < 0) return nullptr;
    return &symbols_[raw_to_symbol_[raw_index]];
}

const Symbol* Object::find_defined(std::string_view name) const {
    const Symbol* fallback = nullptr;
    for (const auto& s : symbols_) {
        if (s.name != name || !s.is_defined()) continue;
        if (s.is_external()) return &s;
        if (!fallback) fallback = &s;
    }
    return fallback;
}

std::vector<const Symbol*> Object::section_symbols(i32 section_number) const {
    std::vector<const Symbol*> out;
    for (const auto& s : symbols_)
        if (s.section_number == section_number && !s.is_section_symbol() && s.storage_class != storage::file)
            out.push_back(&s);
    std::ranges::stable_sort(out, {}, [](const Symbol* s) { return s->value; });
    return out;
}

std::vector<const Symbol*> Object::function_symbols() const {
    std::vector<const Symbol*> out;
    for (const auto& s : symbols_) {
        if (!s.is_defined() || s.is_section_symbol() || s.storage_class == storage::label) continue;
        auto sec = section(s.section_number);
        if (!sec || !sec->is_code()) continue;
        if (s.is_function() || s.is_external()) out.push_back(&s);
    }
    return out;
}

u32 Object::symbol_size(const Symbol& sym) const {
    auto sec = section(sym.section_number);
    if (!sec) return 0;
    u32 end = sec->size;
    for (const auto* other : section_symbols(sym.section_number)) {
        if (other->value > sym.value && other->value < end && other->storage_class != storage::label) end = other->value;
    }
    return end > sym.value ? end - sym.value : 0;
}

unsigned Object::relocation_size(u16 type) const {
    if (machine_ == pe::machine::i386) {
        switch (type) {
        case reloc_i386::dir16: case reloc_i386::rel16: case reloc_i386::section: return 2;
        case reloc_i386::dir32: case reloc_i386::dir32nb: case reloc_i386::rel32: case reloc_i386::secrel:
        case reloc_i386::token: return 4;
        case reloc_i386::secrel7: return 1;
        default: return 0;
        }
    }
    switch (type) {
    case reloc_amd64::addr64: return 8;
    case reloc_amd64::addr32: case reloc_amd64::addr32nb: case reloc_amd64::rel32: case reloc_amd64::rel32_1:
    case reloc_amd64::rel32_2: case reloc_amd64::rel32_3: case reloc_amd64::rel32_4: case reloc_amd64::rel32_5:
    case reloc_amd64::secrel: case reloc_amd64::token: case reloc_amd64::srel32: return 4;
    case reloc_amd64::section: return 2;
    case reloc_amd64::secrel7: return 1;
    default: return 0;
    }
}

bool Object::relocation_is_pc_relative(u16 type) const {
    if (machine_ == pe::machine::i386) return type == reloc_i386::rel32 || type == reloc_i386::rel16;
    return type >= reloc_amd64::rel32 && type <= reloc_amd64::rel32_5;
}

std::string Object::relocation_type_name(u16 type) const {
    if (machine_ == pe::machine::i386) {
        switch (type) {
        case reloc_i386::dir32: return "DIR32";
        case reloc_i386::dir32nb: return "DIR32NB";
        case reloc_i386::rel32: return "REL32";
        case reloc_i386::secrel: return "SECREL";
        case reloc_i386::section: return "SECTION";
        case reloc_i386::dir16: return "DIR16";
        case reloc_i386::rel16: return "REL16";
        default: return std::format("I386_{:#x}", type);
        }
    }
    switch (type) {
    case reloc_amd64::addr64: return "ADDR64";
    case reloc_amd64::addr32: return "ADDR32";
    case reloc_amd64::addr32nb: return "ADDR32NB";
    case reloc_amd64::rel32: return "REL32";
    case reloc_amd64::rel32_1: return "REL32_1";
    case reloc_amd64::rel32_2: return "REL32_2";
    case reloc_amd64::rel32_3: return "REL32_3";
    case reloc_amd64::rel32_4: return "REL32_4";
    case reloc_amd64::rel32_5: return "REL32_5";
    case reloc_amd64::secrel: return "SECREL";
    case reloc_amd64::section: return "SECTION";
    default: return std::format("AMD64_{:#x}", type);
    }
}

} // namespace decomp::coff
