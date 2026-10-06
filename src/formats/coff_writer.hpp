#pragma once

// Writing COFF objects and import libraries (docs/architecture.md#relinking): the split objects that carry
// a target's unmatched code and data into a relink, and the import libraries made from its import table.

#include "core/bytes.hpp"
#include "core/result.hpp"
#include "formats/coff.hpp"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::coff {

// IMAGE_COMDAT_SELECT_*.
namespace comdat_select {
inline constexpr u8 noduplicates = 1, any = 2, same_size = 3, exact_match = 4, associative = 5, largest = 6;
} // namespace comdat_select

// Section characteristics a writer needs besides pe::scn.
namespace scn_flags {
inline constexpr u32 lnk_info = 0x00000200;
inline constexpr u32 lnk_remove = 0x00000800;
inline constexpr u32 align_mask = 0x00F00000;
} // namespace scn_flags

// IMAGE_SCN_ALIGN_<n>BYTES for a power of two up to 8192; 0 for anything else.
u32 alignment_flags(u32 alignment);
// The alignment IMAGE_SCN_ALIGN_* in `characteristics` asks for (1 when none does).
u32 alignment_of(u32 characteristics);

// Builds a COFF object in memory. Sections get their section symbol (with the section definition aux
// record) when they are added; symbols are referred to by the handles add_symbol() and friends return,
// and become symbol table indexes when the object is written.
class ObjectWriter {
public:
    explicit ObjectWriter(u16 machine) : machine_(machine) {}

    u16 machine() const { return machine_; }

    // Adds a section with its data; returns its 1-based number.
    u32 add_section(std::string name, u32 characteristics, std::vector<std::byte> data);
    // Adds a section of uninitialized data (IMAGE_SCN_CNT_UNINITIALIZED_DATA): a size and no data.
    u32 add_bss_section(std::string name, u32 characteristics, u32 size);
    // Makes a section a COMDAT. For every selection but `associative`, `symbol` (defined in the section)
    // is its COMDAT symbol, written right after the section symbol; an associative section goes with
    // `associated_section`.
    void set_comdat(u32 section, u8 selection, std::optional<u32> symbol, u32 associated_section = 0);
    std::vector<std::byte>& data(u32 section) { return sections_.at(section - 1).data; }
    usize section_count() const { return sections_.size(); }

    // Defines a symbol at `value` in `section` (1-based); returns its handle. An external name is defined
    // once: defining it again returns the first definition.
    u32 add_symbol(std::string name, u32 value, i32 section, u8 storage_class = storage::external, u16 type = 0);
    // The handle of the undefined external `name` (added on first use).
    u32 undefined(std::string_view name);
    // An absolute symbol (@feat.00, @comp.id); static.
    u32 add_absolute(std::string name, u32 value);
    // The handle of a section's own symbol.
    u32 section_symbol(u32 section) const { return sections_.at(section - 1).symbol; }
    // A defined external symbol by name.
    std::optional<u32> find_defined(std::string_view name) const;

    void add_relocation(u32 section, u32 offset, u32 symbol, u16 type);
    // Linker directives (.drectve), added to one section, each after a space.
    void add_directive(std::string_view directive);

    // The object file. Fails when it would have more sections than a regular COFF header can count.
    Result<std::vector<std::byte>> write() const;

private:
    struct SectionEntry {
        std::string name;
        u32 characteristics = 0;
        std::vector<std::byte> data;
        u32 bss_size = 0;
        bool bss = false;
        std::vector<Relocation> relocations;  // symbol_index holds a handle until written
        u32 symbol = 0;                       // its section symbol's handle
        std::optional<u8> selection;
        std::optional<u32> comdat_symbol;
        u32 associated = 0;
    };
    struct SymbolEntry {
        std::string name;
        u32 value = 0;
        i32 section = 0;
        u16 type = 0;
        u8 storage_class = 0;
        u32 section_definition = 0;  // the section it is the section symbol of (with an aux record), or 0
    };

    u16 machine_;
    std::vector<SectionEntry> sections_;
    std::vector<SymbolEntry> symbols_;
    std::map<std::string, u32, std::less<>> undefined_;
    std::map<std::string, u32, std::less<>> defined_;
    std::string directives_;
};

// Short import object types and name types (IMPORT_OBJECT_*).
namespace import_type {
inline constexpr u8 code = 0, data = 1, const_ = 2;
} // namespace import_type
namespace import_name_type {
inline constexpr u8 ordinal = 0, name = 1, no_prefix = 2, undecorate = 3;
} // namespace import_name_type

// One function or variable a DLL exports, as an import library describes it.
struct ImportEntry {
    std::string symbol = {};                      // what objects reference, without __imp_: "_ExitProcess@4", "ExitProcess"
    std::string name = {};                        // the name the DLL exports; empty for an import by ordinal
    std::optional<u16> ordinal = std::nullopt;    // set for an import by ordinal
    u16 hint = 0;                                 // the hint written beside the name
    u8 type = import_type::code;                  // code imports also define `symbol`, the jump thunk
};

// The name type that makes the linker import `entry.name` given `entry.symbol` (an exact name, the
// symbol without its prefix, or undecorated), or nullopt when none does.
std::optional<u8> import_name_type_for(const ImportEntry& entry, u16 machine);

// An import library for `dll` ("KERNEL32.dll") in the layout lib.exe and llvm-lib write: the import
// descriptor, the null import descriptor and the null thunk objects, then a short import object per entry
// in the given order, with both linker members and the long names member. Fails for an entry whose name no
// name type gives.
Result<std::vector<std::byte>> write_import_library(std::string_view dll, u16 machine, std::span<const ImportEntry> entries);

// A member of an archive written by write_archive().
struct ArchiveMember {
    std::string name;
    std::vector<std::byte> data;
    std::vector<std::string> symbols;  // what the member defines, for the linker members
};
// A COFF archive with the first and second linker members and, when a name needs it, the long names member.
std::vector<std::byte> write_archive(std::span<const ArchiveMember> members);

} // namespace decomp::coff
