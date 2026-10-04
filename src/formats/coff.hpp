#pragma once

#include "core/result.hpp"
#include "formats/image.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::coff {

namespace storage {
inline constexpr u8 external = 2;
inline constexpr u8 static_ = 3;
inline constexpr u8 label = 6;
inline constexpr u8 function = 101;
inline constexpr u8 file = 103;
inline constexpr u8 section = 104;
inline constexpr u8 weak_external = 105;
} // namespace storage

namespace reloc_i386 {
inline constexpr u16 absolute = 0x0000, dir16 = 0x0001, rel16 = 0x0002, dir32 = 0x0006, dir32nb = 0x0007,
                     seg12 = 0x0009, section = 0x000A, secrel = 0x000B, token = 0x000C, secrel7 = 0x000D,
                     rel32 = 0x0014;
} // namespace reloc_i386

namespace reloc_amd64 {
inline constexpr u16 absolute = 0x0000, addr64 = 0x0001, addr32 = 0x0002, addr32nb = 0x0003, rel32 = 0x0004,
                     rel32_1 = 0x0005, rel32_2 = 0x0006, rel32_3 = 0x0007, rel32_4 = 0x0008, rel32_5 = 0x0009,
                     section = 0x000A, secrel = 0x000B, secrel7 = 0x000C, token = 0x000D, srel32 = 0x000E;
} // namespace reloc_amd64

struct Relocation {
    u32 offset = 0;        // within the section
    u32 symbol_index = 0;  // raw symbol table index
    u16 type = 0;
};

struct ComdatInfo {
    u8 selection = 0;  // IMAGE_COMDAT_SELECT_*
    u32 associated_section = 0;
    u32 checksum = 0;
};

struct Section {
    std::string name;
    u32 characteristics = 0;
    u32 size = 0;  // SizeOfRawData (bss sections have no data)
    std::vector<std::byte> data;
    std::vector<Relocation> relocations;  // sorted by offset
    std::optional<ComdatInfo> comdat;
    u32 number = 0;  // 1-based section number

    bool is_code() const;
    bool is_bss() const;
    bool is_debug() const;
};

struct Symbol {
    u32 index = 0;  // raw symbol table index
    std::string name;
    u32 value = 0;
    i32 section_number = 0;  // 1-based; 0 = undefined/common, -1 = absolute, -2 = debug
    u16 type = 0;
    u8 storage_class = 0;
    u8 aux_count = 0;

    bool is_defined() const { return section_number > 0; }
    bool is_external() const { return storage_class == storage::external || storage_class == storage::weak_external; }
    bool is_function() const { return (type & 0xF0) == 0x20; }
    bool is_section_symbol() const;  // static symbol naming a section at offset 0 (with a section aux record)
};

class Object {
public:
    static Result<Object> load(const std::filesystem::path& path);
    static Result<Object> parse(std::vector<std::byte> data);

    u16 machine() const { return machine_; }
    Arch arch() const;
    bool is_bigobj() const { return bigobj_; }

    const std::vector<Section>& sections() const { return sections_; }
    const Section* section(i32 number) const;  // 1-based

    // Primary symbol records in table order (aux records are skipped).
    const std::vector<Symbol>& symbols() const { return symbols_; }
    const Symbol* symbol_at_index(u32 raw_index) const;
    // Defined symbol by name (external preferred over static).
    const Symbol* find_defined(std::string_view name) const;
    // Defined symbols of a section ordered by value (section symbols excluded).
    std::vector<const Symbol*> section_symbols(i32 section_number) const;
    // Defined function symbols in code sections.
    std::vector<const Symbol*> function_symbols() const;
    // Size of the defined symbol: up to the next symbol in its section or the section end.
    u32 symbol_size(const Symbol& sym) const;

    // Field size in bytes affected by a relocation type for this object's machine (0 if unknown).
    unsigned relocation_size(u16 type) const;
    bool relocation_is_pc_relative(u16 type) const;
    std::string relocation_type_name(u16 type) const;

private:
    u16 machine_ = 0;
    bool bigobj_ = false;
    std::vector<Section> sections_;
    std::vector<Symbol> symbols_;
    std::vector<i32> raw_to_symbol_;  // raw index -> position in symbols_ (-1 for aux records)
};

} // namespace decomp::coff
