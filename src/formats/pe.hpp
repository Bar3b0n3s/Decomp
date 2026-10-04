#pragma once

#include "core/result.hpp"
#include "formats/image.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace decomp::pe {

namespace machine {
inline constexpr u16 i386 = 0x14C;
inline constexpr u16 amd64 = 0x8664;
} // namespace machine

namespace scn {
inline constexpr u32 cnt_code = 0x00000020;
inline constexpr u32 cnt_initialized_data = 0x00000040;
inline constexpr u32 cnt_uninitialized_data = 0x00000080;
inline constexpr u32 lnk_comdat = 0x00001000;
inline constexpr u32 lnk_nreloc_ovfl = 0x01000000;
inline constexpr u32 mem_discardable = 0x02000000;
inline constexpr u32 mem_execute = 0x20000000;
inline constexpr u32 mem_read = 0x40000000;
inline constexpr u32 mem_write = 0x80000000;
} // namespace scn

struct SectionHeader {
    std::string name;
    u32 virtual_size = 0;
    u32 virtual_address = 0;  // RVA
    u32 raw_size = 0;
    u32 raw_offset = 0;
    u32 characteristics = 0;
};

struct Export {
    std::string name;  // empty for ordinal-only exports
    u32 ordinal = 0;
    u32 rva = 0;
    std::optional<std::string> forwarder;
};

struct Import {
    std::string dll;
    std::string name;  // empty for ordinal imports
    std::optional<u16> ordinal;
    u64 iat_va = 0;    // address of the IAT slot the code calls through
};

struct BaseRelocation {
    u32 rva = 0;
    u8 type = 0;  // IMAGE_REL_BASED_* (3 = HIGHLOW, 10 = DIR64)
};

struct RichEntry {
    u16 product_id = 0;
    u16 build = 0;
    u32 count = 0;
};

struct CodeViewInfo {
    std::string signature;  // "RSDS" (PDB 7) or "NB10" (PDB 2)
    std::array<u8, 16> guid{};
    u32 nb10_signature = 0;
    u32 age = 0;
    std::string pdb_path;

    std::string guid_string() const;  // {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
};

struct RuntimeFunction {
    u32 begin_rva = 0;
    u32 end_rva = 0;
    u32 unwind_rva = 0;
};

class Image final : public BinaryImage {
public:
    static Result<Image> load(const std::filesystem::path& path);
    static Result<Image> parse(std::vector<std::byte> data);

    // BinaryImage
    Arch arch() const override { return machine_ == machine::amd64 ? Arch::x64 : Arch::x86; }
    u64 image_base() const override { return image_base_; }
    u64 image_size() const override { return size_of_image_; }
    u64 entry_point() const override { return entry_rva_ ? image_base_ + entry_rva_ : 0; }
    const std::vector<ImageSection>& image_sections() const override { return image_sections_; }
    std::optional<ByteSpan> view(u64 va, usize size) const override;
    bool has_relocations() const override { return !relocs_stripped() && !base_relocations_.empty(); }
    bool is_relocated(u64 va) const override;

    u16 machine() const { return machine_; }
    bool is_pe32_plus() const { return pe32_plus_; }
    bool is_dll() const { return (characteristics_ & 0x2000) != 0; }
    bool relocs_stripped() const { return (characteristics_ & 0x0001) != 0; }
    u32 timestamp() const { return timestamp_; }
    u16 characteristics() const { return characteristics_; }
    u16 dll_characteristics() const { return dll_characteristics_; }
    u16 subsystem() const { return subsystem_; }
    u8 linker_major() const { return linker_major_; }
    u8 linker_minor() const { return linker_minor_; }
    u32 entry_rva() const { return entry_rva_; }

    const std::vector<SectionHeader>& sections() const { return sections_; }
    const SectionHeader* section_for_rva(u32 rva) const;
    std::optional<u32> rva_to_offset(u32 rva) const;
    // Copy of `size` bytes at `rva`; bytes past the file-backed part of a section read as zero.
    std::optional<std::vector<std::byte>> read_rva(u32 rva, usize size) const;

    const std::vector<Export>& exports() const { return exports_; }
    const std::vector<Import>& imports() const { return imports_; }
    const std::vector<BaseRelocation>& base_relocations() const { return base_relocations_; }
    const std::optional<CodeViewInfo>& codeview() const { return codeview_; }
    const std::vector<RichEntry>& rich_entries() const { return rich_entries_; }
    const std::vector<RuntimeFunction>& runtime_functions() const { return runtime_functions_; }
    ByteSpan data() const { return data_; }

private:
    Result<void> parse_headers();
    Result<void> parse_exports(u32 rva, u32 size);
    Result<void> parse_imports(u32 rva);
    Result<void> parse_base_relocations(u32 rva, u32 size);
    Result<void> parse_debug_directory(u32 rva, u32 size);
    Result<void> parse_pdata(u32 rva, u32 size);
    void parse_rich_header(u32 pe_offset);
    std::optional<std::string> read_cstring_rva(u32 rva) const;

    std::vector<std::byte> data_;
    u16 machine_ = 0;
    bool pe32_plus_ = false;
    u32 timestamp_ = 0;
    u16 characteristics_ = 0;
    u16 dll_characteristics_ = 0;
    u16 subsystem_ = 0;
    u8 linker_major_ = 0, linker_minor_ = 0;
    u32 entry_rva_ = 0;
    u64 image_base_ = 0;
    u32 size_of_image_ = 0;
    u32 size_of_headers_ = 0;
    std::vector<SectionHeader> sections_;
    std::vector<ImageSection> image_sections_;
    std::vector<Export> exports_;
    std::vector<Import> imports_;
    std::vector<BaseRelocation> base_relocations_;  // sorted by rva
    std::optional<CodeViewInfo> codeview_;
    std::vector<RichEntry> rich_entries_;
    std::vector<RuntimeFunction> runtime_functions_;
};

// Human-readable Visual Studio name for a Rich-header product id (e.g. "VS2008 C++ compiler"), if known.
std::optional<std::string> describe_rich_product(u16 product_id);

} // namespace decomp::pe
