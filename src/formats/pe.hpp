#pragma once

#include "core/result.hpp"
#include "formats/image.hpp"
#include "formats/rich.hpp"

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
    u16 hint = 0;      // the hint beside the name
};

struct BaseRelocation {
    u32 rva = 0;
    u8 type = 0;  // IMAGE_REL_BASED_* (3 = HIGHLOW, 10 = DIR64)
};

struct CodeViewInfo {
    std::string signature;  // "RSDS" (PDB 7) or "NB10" (PDB 2)
    std::array<u8, 16> guid{};
    u32 nb10_signature = 0;
    u32 age = 0;
    std::string pdb_path;

    std::string guid_string() const;  // {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
};

// IMAGE_DEBUG_TYPE_*.
namespace debug_type {
inline constexpr u32 codeview = 2, vc_feature = 12, pogo = 13, iltcg = 14, repro = 16;
} // namespace debug_type

// An entry of the debug directory.
struct DebugEntry {
    u32 type = 0;
    u32 timestamp = 0;
    u32 size = 0;          // of its data
    u32 rva = 0;           // of its data (0 when it is not mapped)
    u32 file_offset = 0;   // of its data
    u32 entry_offset = 0;  // file offset of the directory entry itself
};

// Optional header fields a relink reproduces.
struct OptionalHeader {
    u32 section_alignment = 0, file_alignment = 0;
    u16 os_major = 0, os_minor = 0, image_major = 0, image_minor = 0, subsystem_major = 0, subsystem_minor = 0;
    u64 stack_reserve = 0, stack_commit = 0, heap_reserve = 0, heap_commit = 0;
    u32 checksum = 0;
    u32 checksum_offset = 0;  // file offset of the CheckSum field
    u32 size_of_headers = 0;
};

// A field that records when or how a build was made rather than what the image holds: timestamps, the
// PDB's GUID and age, the checksum. A relink can only take these over from the original.
struct IdentityField {
    std::string name;  // "COFF header TimeDateStamp", "debug directory entry 0 TimeDateStamp", "CodeView GUID"
    u32 offset = 0;    // in the file
    u32 size = 0;
};

// The standard PE checksum of a file, with the CheckSum field at `checksum_offset` counted as zero.
u32 compute_checksum(ByteSpan file, u32 checksum_offset);

struct RuntimeFunction {
    u32 begin_rva = 0;
    u32 end_rva = 0;
    u32 unwind_rva = 0;
    // The unwind data continues another entry's (UNW_FLAG_CHAININFO): the begin RVA of the entry the
    // chain starts from, so this range is part of that function. 0 for a function's own entry.
    u32 chained_to = 0;
    // UNW_FLAG_EHANDLER / UHANDLER: the language-specific handler (__C_specific_handler,
    // __CxxFrameHandler3/4, __GSHandlerCheck...) and its data (a scope table, a FuncInfo's RVA...).
    u32 handler_rva = 0;
    u32 handler_data_rva = 0;
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
    const std::vector<RichEntry>& rich_entries() const;  // empty without a Rich header
    const std::optional<RichHeader>& rich_header() const { return rich_; }
    // How the image was built, from its Rich header (nullopt without one: not linked by link.exe).
    std::optional<BuildInfo> build_info() const;
    const std::vector<RuntimeFunction>& runtime_functions() const { return runtime_functions_; }
    ByteSpan data() const { return data_; }

    const OptionalHeader& optional_header() const { return optional_; }
    // Data directory `index` (IMAGE_DIRECTORY_ENTRY_*): RVA and size, zero when absent.
    std::pair<u32, u32> data_directory(u32 index) const { return index < 16 ? directories_[index] : std::pair<u32, u32>{}; }
    const std::vector<DebugEntry>& debug_entries() const { return debug_entries_; }
    // The DLL name the export directory records ("basic.exe"); empty without exports.
    const std::string& export_name() const { return export_name_; }
    // The fields relinking takes over from the original, in file order (identity_fields above).
    std::vector<IdentityField> identity_fields() const;
    // What the header byte at `offset` belongs to: "COFF header TimeDateStamp", "section header .text
    // VirtualSize"; empty past the headers.
    std::string describe_header_offset(u32 offset) const;
    u32 header_size() const { return size_of_headers_; }
    u32 pe_offset() const { return pe_offset_; }

private:
    Result<void> parse_headers();
    Result<void> parse_exports(u32 rva, u32 size);
    Result<void> parse_imports(u32 rva);
    Result<void> parse_base_relocations(u32 rva, u32 size);
    Result<void> parse_debug_directory(u32 rva, u32 size);
    Result<void> parse_pdata(u32 rva, u32 size);
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
    std::optional<RichHeader> rich_;
    std::vector<RuntimeFunction> runtime_functions_;
    OptionalHeader optional_;
    std::array<std::pair<u32, u32>, 16> directories_{};
    std::vector<DebugEntry> debug_entries_;
    std::string export_name_;
    u32 pe_offset_ = 0;
    u32 optional_offset_ = 0;
    u16 optional_size_ = 0;
};

} // namespace decomp::pe
