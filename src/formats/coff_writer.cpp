#include "formats/coff_writer.hpp"

#include "formats/pe.hpp"

#include <algorithm>
#include <bit>
#include <format>

namespace decomp::coff {

u32 alignment_flags(u32 alignment) {
    if (alignment == 0 || !std::has_single_bit(alignment) || alignment > 8192) return 0;
    return static_cast<u32>(std::countr_zero(alignment) + 1) << 20;
}

u32 alignment_of(u32 characteristics) {
    const u32 code = (characteristics & scn_flags::align_mask) >> 20;
    return code == 0 || code > 14 ? 1u : 1u << (code - 1);
}

u32 ObjectWriter::add_section(std::string name, u32 characteristics, std::vector<std::byte> data) {
    SectionEntry s;
    s.name = name;
    s.characteristics = characteristics;
    s.data = std::move(data);
    sections_.push_back(std::move(s));
    const u32 number = static_cast<u32>(sections_.size());
    symbols_.push_back({std::move(name), 0, static_cast<i32>(number), 0, storage::static_, number});
    sections_.back().symbol = static_cast<u32>(symbols_.size() - 1);
    return number;
}

u32 ObjectWriter::add_bss_section(std::string name, u32 characteristics, u32 size) {
    const u32 number = add_section(std::move(name), characteristics, {});
    sections_.back().bss = true;
    sections_.back().bss_size = size;
    return number;
}

void ObjectWriter::set_comdat(u32 section, u8 selection, std::optional<u32> symbol, u32 associated_section) {
    auto& s = sections_.at(section - 1);
    s.characteristics |= pe::scn::lnk_comdat;
    s.selection = selection;
    s.comdat_symbol = selection == comdat_select::associative ? std::nullopt : symbol;
    s.associated = associated_section;
}

u32 ObjectWriter::add_symbol(std::string name, u32 value, i32 section, u8 storage_class, u16 type) {
    const bool external = storage_class == storage::external;
    if (external) {
        if (auto it = defined_.find(name); it != defined_.end()) return it->second;
        if (auto it = undefined_.find(name); it != undefined_.end()) {
            // An earlier reference becomes this definition.
            auto& sym = symbols_[it->second];
            sym.value = value;
            sym.section = section;
            sym.type = type;
            defined_.emplace(name, it->second);
            const u32 handle = it->second;
            undefined_.erase(it);
            return handle;
        }
    }
    symbols_.push_back({name, value, section, type, storage_class, 0});
    const u32 handle = static_cast<u32>(symbols_.size() - 1);
    if (external) defined_.emplace(std::move(name), handle);
    return handle;
}

u32 ObjectWriter::undefined(std::string_view name) {
    if (auto it = defined_.find(name); it != defined_.end()) return it->second;
    if (auto it = undefined_.find(name); it != undefined_.end()) return it->second;
    symbols_.push_back({std::string(name), 0, 0, 0, storage::external, 0});
    const u32 handle = static_cast<u32>(symbols_.size() - 1);
    undefined_.emplace(std::string(name), handle);
    return handle;
}

u32 ObjectWriter::add_common(std::string name, u32 size) {
    if (auto it = defined_.find(name); it != defined_.end()) return it->second;
    const u32 handle = undefined(name);
    symbols_[handle].value = std::max(symbols_[handle].value, size);
    return handle;
}

u32 ObjectWriter::add_absolute(std::string name, u32 value) {
    symbols_.push_back({std::move(name), value, -1, 0, storage::static_, 0});
    return static_cast<u32>(symbols_.size() - 1);
}

std::optional<u32> ObjectWriter::find_defined(std::string_view name) const {
    if (auto it = defined_.find(name); it != defined_.end()) return it->second;
    return std::nullopt;
}

void ObjectWriter::add_relocation(u32 section, u32 offset, u32 symbol, u16 type) {
    sections_.at(section - 1).relocations.push_back({offset, symbol, type});
}

void ObjectWriter::add_directive(std::string_view directive) {
    directives_ += ' ';
    directives_ += directive;
}

namespace {

// Names longer than 8 bytes go to the string table, which starts with its own size.
struct StringTable {
    std::vector<std::byte> bytes{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    std::map<std::string, u32, std::less<>> offsets;

    u32 add(std::string_view s) {
        if (auto it = offsets.find(s); it != offsets.end()) return it->second;
        const u32 offset = static_cast<u32>(bytes.size());
        append_string(bytes, s);
        bytes.push_back(std::byte{0});
        offsets.emplace(std::string(s), offset);
        return offset;
    }
    std::vector<std::byte> finish() {
        write_le<u32>(bytes, 0, static_cast<u32>(bytes.size()));
        return bytes;
    }
};

void append_name8(std::vector<std::byte>& out, std::string_view name) {
    std::array<std::byte, 8> field{};
    std::memcpy(field.data(), name.data(), std::min<usize>(name.size(), 8));
    out.insert(out.end(), field.begin(), field.end());
}

} // namespace

Result<std::vector<std::byte>> ObjectWriter::write() const {
    // The directives join the sections as the last one.
    std::vector<SectionEntry> sections = sections_;
    std::vector<SymbolEntry> symbols = symbols_;
    if (!directives_.empty()) {
        SectionEntry d;
        d.name = ".drectve";
        d.characteristics = scn_flags::lnk_info | scn_flags::lnk_remove | alignment_flags(1);
        append_string(d.data, directives_);
        sections.push_back(std::move(d));
        const u32 number = static_cast<u32>(sections.size());
        symbols.push_back({".drectve", 0, static_cast<i32>(number), 0, storage::static_, number});
        sections.back().symbol = static_cast<u32>(symbols.size() - 1);
    }
    // .sxdata holds symbol table indexes: filled in once they are known.
    std::optional<usize> sxdata;
    if (!safe_seh_.empty()) {
        SectionEntry x;
        x.name = ".sxdata";
        x.characteristics = scn_flags::lnk_info | alignment_flags(4);
        x.data.resize(4 * safe_seh_.size());
        sections.push_back(std::move(x));
        sxdata = sections.size() - 1;
        const u32 number = static_cast<u32>(sections.size());
        symbols.push_back({".sxdata", 0, static_cast<i32>(number), 0, storage::static_, number});
        sections.back().symbol = static_cast<u32>(symbols.size() - 1);
    }
    if (sections.size() > 65279) return make_error(ErrorCode::unsupported, "a COFF object holds at most 65279 sections, not {}", sections.size());

    // Symbol table order: absolute symbols, then each section's symbol followed by its COMDAT symbol,
    // then everything else in the order it was added.
    std::vector<u32> order;
    std::vector<bool> placed(symbols.size(), false);
    auto place = [&](u32 handle) {
        if (!placed[handle]) {
            placed[handle] = true;
            order.push_back(handle);
        }
    };
    for (u32 h = 0; h < symbols.size(); ++h)
        if (symbols[h].section == -1) place(h);
    for (const auto& s : sections) {
        place(s.symbol);
        if (s.comdat_symbol) place(*s.comdat_symbol);
    }
    for (u32 h = 0; h < symbols.size(); ++h) place(h);
    std::vector<u32> raw_index(symbols.size());
    u32 raw = 0;
    for (u32 h : order) {
        raw_index[h] = raw;
        raw += 1 + (symbols[h].section_definition ? 1 : 0);
    }

    if (sxdata)
        for (usize i = 0; i < safe_seh_.size(); ++i) write_le<u32>(sections[*sxdata].data, 4 * i, raw_index.at(safe_seh_[i]));

    StringTable strings;
    std::vector<std::byte> out;
    append_le<u16>(out, machine_);
    append_le<u16>(out, static_cast<u16>(sections.size()));
    append_le<u32>(out, 0);  // TimeDateStamp
    const usize symtab_field = out.size();
    append_le<u32>(out, 0);  // PointerToSymbolTable, patched below
    append_le<u32>(out, raw);
    append_le<u16>(out, 0);  // SizeOfOptionalHeader
    append_le<u16>(out, 0);

    const usize headers = out.size();
    out.resize(headers + 40 * sections.size());
    for (usize i = 0; i < sections.size(); ++i) {
        const auto& s = sections[i];
        std::vector<std::byte> header;
        if (s.name.size() <= 8) {
            append_name8(header, s.name);
        } else {
            append_name8(header, std::format("/{}", strings.add(s.name)));
        }
        const u32 size = s.bss ? s.bss_size : static_cast<u32>(s.data.size());
        u32 raw_pointer = 0;
        if (!s.bss && !s.data.empty()) {
            raw_pointer = static_cast<u32>(out.size());
            append_bytes(out, s.data);
        }
        auto relocations = s.relocations;
        std::ranges::stable_sort(relocations, {}, &Relocation::offset);
        u32 reloc_pointer = 0;
        u32 characteristics = s.characteristics;
        u16 reloc_count = 0;
        if (!relocations.empty()) {
            reloc_pointer = static_cast<u32>(out.size());
            if (relocations.size() > 0xFFFE) {
                // IMAGE_SCN_LNK_NRELOC_OVFL: the first record carries the real count, itself included.
                characteristics |= pe::scn::lnk_nreloc_ovfl;
                reloc_count = 0xFFFF;
                append_le<u32>(out, static_cast<u32>(relocations.size() + 1));
                append_le<u32>(out, 0);
                append_le<u16>(out, 0);
            } else {
                reloc_count = static_cast<u16>(relocations.size());
            }
            for (const auto& r : relocations) {
                append_le<u32>(out, r.offset);
                append_le<u32>(out, raw_index.at(r.symbol_index));
                append_le<u16>(out, r.type);
            }
        }
        append_le<u32>(header, 0);  // VirtualSize
        append_le<u32>(header, 0);  // VirtualAddress
        append_le<u32>(header, size);
        append_le<u32>(header, raw_pointer);
        append_le<u32>(header, reloc_pointer);
        append_le<u32>(header, 0);  // PointerToLinenumbers
        append_le<u16>(header, reloc_count);
        append_le<u16>(header, 0);
        append_le<u32>(header, characteristics);
        std::ranges::copy(header, out.begin() + static_cast<std::ptrdiff_t>(headers + 40 * i));
    }

    write_le<u32>(out, symtab_field, static_cast<u32>(out.size()));
    for (u32 h : order) {
        const auto& sym = symbols[h];
        if (sym.name.size() <= 8) {
            append_name8(out, sym.name);
        } else {
            append_le<u32>(out, 0);
            append_le<u32>(out, strings.add(sym.name));
        }
        append_le<u32>(out, sym.value);
        append_le<i16>(out, static_cast<i16>(sym.section));
        append_le<u16>(out, sym.type);
        out.push_back(static_cast<std::byte>(sym.storage_class));
        out.push_back(static_cast<std::byte>(sym.section_definition ? 1 : 0));
        if (sym.section_definition) {
            const auto& s = sections[sym.section_definition - 1];
            const u32 size = s.bss ? s.bss_size : static_cast<u32>(s.data.size());
            append_le<u32>(out, size);
            append_le<u16>(out, static_cast<u16>(std::min<usize>(s.relocations.size(), 0xFFFF)));
            append_le<u16>(out, 0);  // NumberOfLinenumbers
            append_le<u32>(out, 0);  // CheckSum
            append_le<u16>(out, static_cast<u16>(s.associated));
            out.push_back(static_cast<std::byte>(s.selection.value_or(0)));
            out.push_back(std::byte{0});
            out.push_back(std::byte{0});
            out.push_back(std::byte{0});
        }
    }
    append_bytes(out, strings.finish());
    return out;
}

// --- Import libraries ---------------------------------------------------------------------------------

namespace {

constexpr u32 kIdataCharacteristics = pe::scn::cnt_initialized_data | pe::scn::mem_read | pe::scn::mem_write;

u16 image_relative_relocation(u16 machine) {
    return machine == pe::machine::amd64 ? reloc_amd64::addr32nb : reloc_i386::dir32nb;
}

std::string library_name(std::string_view dll) {
    const auto dot = dll.rfind('.');
    return std::string(dot == std::string_view::npos ? dll : dll.substr(0, dot));
}

// The import descriptor object: .idata$2 (the descriptor, pointing at the lookup and address tables and
// the name) and .idata$6 (the DLL's name).
Result<std::vector<std::byte>> import_descriptor(std::string_view dll, u16 machine, std::optional<u32> comp_id) {
    const std::string lib = library_name(dll);
    ObjectWriter w(machine);
    if (comp_id) w.add_absolute("@comp.id", *comp_id);
    const u32 descriptor = w.add_section(".idata$2", kIdataCharacteristics | alignment_flags(4), std::vector<std::byte>(20));
    std::vector<std::byte> name;
    append_string(name, dll);
    name.push_back(std::byte{0});
    if (name.size() % 2) name.push_back(std::byte{0});
    const u32 name_section = w.add_section(".idata$6", kIdataCharacteristics | alignment_flags(2), std::move(name));
    w.add_symbol("__IMPORT_DESCRIPTOR_" + lib, 0, static_cast<i32>(descriptor));
    const u32 lookup = w.add_symbol(".idata$4", 0, 0, storage::section);
    const u32 address = w.add_symbol(".idata$5", 0, 0, storage::section);
    w.undefined("__NULL_IMPORT_DESCRIPTOR");
    w.undefined("\x7f" + lib + "_NULL_THUNK_DATA");
    const u16 rel = image_relative_relocation(machine);
    w.add_relocation(descriptor, 12, w.section_symbol(name_section), rel);
    w.add_relocation(descriptor, 0, lookup, rel);
    w.add_relocation(descriptor, 16, address, rel);
    return w.write();
}

Result<std::vector<std::byte>> null_import_descriptor(u16 machine, std::optional<u32> comp_id) {
    ObjectWriter w(machine);
    if (comp_id) w.add_absolute("@comp.id", *comp_id);
    const u32 s = w.add_section(".idata$3", kIdataCharacteristics | alignment_flags(4), std::vector<std::byte>(20));
    w.add_symbol("__NULL_IMPORT_DESCRIPTOR", 0, static_cast<i32>(s));
    return w.write();
}

Result<std::vector<std::byte>> null_thunk(std::string_view dll, u16 machine, std::optional<u32> comp_id) {
    const usize pointer = machine == pe::machine::amd64 ? 8 : 4;
    ObjectWriter w(machine);
    if (comp_id) w.add_absolute("@comp.id", *comp_id);
    const u32 address = w.add_section(".idata$5", kIdataCharacteristics | alignment_flags(static_cast<u32>(pointer)),
                                      std::vector<std::byte>(pointer));
    w.add_section(".idata$4", kIdataCharacteristics | alignment_flags(static_cast<u32>(pointer)), std::vector<std::byte>(pointer));
    w.add_symbol("\x7f" + library_name(dll) + "_NULL_THUNK_DATA", 0, static_cast<i32>(address));
    return w.write();
}

std::vector<std::byte> short_import(const ImportEntry& entry, u8 name_type, std::string_view dll, u16 machine) {
    std::vector<std::byte> out;
    append_le<u16>(out, 0);       // Sig1
    append_le<u16>(out, 0xFFFF);  // Sig2
    append_le<u16>(out, 0);       // Version
    append_le<u16>(out, machine);
    append_le<u32>(out, 0);       // TimeDateStamp
    append_le<u32>(out, static_cast<u32>(entry.symbol.size() + 1 + dll.size() + 1));
    append_le<u16>(out, entry.ordinal ? *entry.ordinal : entry.hint);
    append_le<u16>(out, static_cast<u16>((entry.type & 3) | ((name_type & 7) << 2)));
    append_string(out, entry.symbol);
    out.push_back(std::byte{0});
    append_string(out, dll);
    out.push_back(std::byte{0});
    return out;
}

} // namespace

std::optional<u8> import_name_type_for(const ImportEntry& entry, u16 machine) {
    if (entry.ordinal) return import_name_type::ordinal;
    std::string_view symbol = entry.symbol;
    if (symbol == entry.name) return import_name_type::name;
    if (machine == pe::machine::i386 && !symbol.empty() && (symbol[0] == '_' || symbol[0] == '@' || symbol[0] == '?')) {
        if (symbol.substr(1) == entry.name) return import_name_type::no_prefix;
        // Undecorated: without the prefix and from the first '@' on (_f@4, @f@8).
        if (symbol[0] != '?') {
            std::string_view base = symbol.substr(1);
            if (auto at = base.find('@'); at != std::string_view::npos) base = base.substr(0, at);
            if (base == entry.name) return import_name_type::undecorate;
        }
    }
    return std::nullopt;
}

Result<std::vector<std::byte>> write_import_library(std::string_view dll, u16 machine, std::span<const ImportEntry> entries,
                                                    std::optional<u32> comp_id) {
    const std::string lib = library_name(dll);
    std::vector<ArchiveMember> members;
    TRY_ASSIGN(auto descriptor, import_descriptor(dll, machine, comp_id));
    members.push_back({std::string(dll), std::move(descriptor), {"__IMPORT_DESCRIPTOR_" + lib}});
    TRY_ASSIGN(auto null_descriptor, null_import_descriptor(machine, comp_id));
    members.push_back({std::string(dll), std::move(null_descriptor), {"__NULL_IMPORT_DESCRIPTOR"}});
    TRY_ASSIGN(auto thunk, null_thunk(dll, machine, comp_id));
    members.push_back({std::string(dll), std::move(thunk), {"\x7f" + lib + "_NULL_THUNK_DATA"}});
    for (const auto& entry : entries) {
        auto name_type = import_name_type_for(entry, machine);
        if (!name_type)
            return make_error(ErrorCode::invalid_argument, "{}: no import name type makes {} import {}", dll, entry.symbol, entry.name);
        std::vector<std::string> symbols{"__imp_" + entry.symbol};
        if (entry.type == import_type::code) symbols.push_back(entry.symbol);
        members.push_back({std::string(dll), short_import(entry, *name_type, dll, machine), std::move(symbols)});
    }
    return write_archive(members);
}

namespace {

void append_be32(std::vector<std::byte>& out, u32 v) {
    out.push_back(static_cast<std::byte>(v >> 24));
    out.push_back(static_cast<std::byte>(v >> 16));
    out.push_back(static_cast<std::byte>(v >> 8));
    out.push_back(static_cast<std::byte>(v));
}

void append_field(std::vector<std::byte>& out, std::string_view text, usize width) {
    std::string field(text.substr(0, width));
    field.resize(width, ' ');
    append_string(out, field);
}

void append_member(std::vector<std::byte>& out, std::string_view name, ByteSpan data) {
    append_field(out, name, 16);
    append_field(out, "0", 12);  // date: none, for reproducible output
    append_field(out, "", 6);
    append_field(out, "", 6);
    append_field(out, "0", 8);
    append_field(out, std::to_string(data.size()), 10);
    append_string(out, "`\n");
    append_bytes(out, data);
    if (data.size() % 2) out.push_back(std::byte{'\n'});
}

} // namespace

std::vector<std::byte> write_archive(std::span<const ArchiveMember> members) {
    // Long member names: "/<offset>" into the "//" member, each name ending in a NUL.
    std::vector<std::byte> longnames;
    std::vector<std::string> header_names;
    std::map<std::string, std::string> long_name_of;
    for (const auto& m : members) {
        if (m.name.size() < 16) {
            header_names.push_back(m.name + "/");
            continue;
        }
        auto [it, inserted] = long_name_of.emplace(m.name, "");
        if (inserted) {
            it->second = std::format("/{}", longnames.size());
            append_string(longnames, m.name);
            longnames.push_back(std::byte{0});
        }
        header_names.push_back(it->second);
    }

    // Symbols in member order (first linker member) and sorted (second linker member).
    std::vector<std::pair<std::string, usize>> symbols;
    usize names_size = 0;
    for (usize i = 0; i < members.size(); ++i)
        for (const auto& s : members[i].symbols) {
            symbols.emplace_back(s, i);
            names_size += s.size() + 1;
        }
    auto padded = [](usize n) { return n + (n % 2); };
    const usize first_size = 4 + 4 * symbols.size() + names_size;
    const usize second_size = 4 + 4 * members.size() + 4 + 2 * symbols.size() + names_size;
    usize offset = 8 + 60 + padded(first_size) + 60 + padded(second_size);
    if (!longnames.empty()) offset += 60 + padded(longnames.size());
    std::vector<u32> member_offsets;
    for (const auto& m : members) {
        member_offsets.push_back(static_cast<u32>(offset));
        offset += 60 + padded(m.data.size());
    }

    std::vector<std::byte> first;
    append_be32(first, static_cast<u32>(symbols.size()));
    for (const auto& [name, member] : symbols) append_be32(first, member_offsets[member]);
    for (const auto& [name, member] : symbols) {
        append_string(first, name);
        first.push_back(std::byte{0});
    }

    auto sorted = symbols;
    std::ranges::stable_sort(sorted, {}, &std::pair<std::string, usize>::first);
    std::vector<std::byte> second;
    append_le<u32>(second, static_cast<u32>(members.size()));
    for (u32 o : member_offsets) append_le<u32>(second, o);
    append_le<u32>(second, static_cast<u32>(sorted.size()));
    for (const auto& [name, member] : sorted) append_le<u16>(second, static_cast<u16>(member + 1));
    for (const auto& [name, member] : sorted) {
        append_string(second, name);
        second.push_back(std::byte{0});
    }

    std::vector<std::byte> out;
    append_string(out, "!<arch>\n");
    append_member(out, "/", first);
    append_member(out, "/", second);
    if (!longnames.empty()) append_member(out, "//", longnames);
    for (usize i = 0; i < members.size(); ++i) append_member(out, header_names[i], members[i].data);
    return out;
}

} // namespace decomp::coff
