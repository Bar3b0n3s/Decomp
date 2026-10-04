#pragma once

#include "analysis/symbols.hpp"
#include "arch/x86/decoder.hpp"
#include "core/result.hpp"
#include "formats/pe.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace decomp {

enum class TableEncoding : u8 {
    absolute,  // entries are absolute addresses (x86: jmp [reg*4+table])
    relative,  // entries are int32 offsets from the table start (clang x64)
    rva,       // entries are 32-bit RVAs from the image base (MSVC x64)
};

struct JumpTable {
    u64 jump_va = 0;   // the indirect jump instruction
    u64 table_va = 0;  // first entry
    unsigned entry_size = 4;
    TableEncoding encoding = TableEncoding::absolute;
    u64 load_va = 0;   // x64: the instruction that loads the entry (its displacement is the table RVA for MSVC)
    std::vector<u64> targets;  // one per entry
    bool inside_code = false;  // table sits within the function's byte range (MSVC x86)
};

struct FunctionExtent {
    u64 start = 0;
    u64 end = 0;  // exclusive
    bool from_symbol = false;
    std::vector<JumpTable> jump_tables;
    // Byte ranges inside [start, end) that are data, not code (in-code jump tables).
    std::vector<std::pair<u64, u64>> data_ranges;

    u64 size() const { return end - start; }
    bool contains(u64 va) const { return va >= start && va < end; }
};

enum class XrefKind : u8 { call, jump, read, address };
std::string_view to_string(XrefKind kind);

struct Xref {
    u64 from = 0;      // instruction address
    u64 function = 0;  // start of the function containing `from` (0 if unknown)
    XrefKind kind = XrefKind::read;
};

// A loaded target binary: image, symbols and lazily computed analysis results.
class Program {
public:
    // Loads a PE image. The PDB is taken from `pdb_path`, or found next to the image via its CodeView
    // record or "<stem>.pdb"; a PDB whose GUID/age does not match is ignored with a warning.
    static Result<Program> open(const std::filesystem::path& binary, const std::optional<std::filesystem::path>& pdb_path = {});

    const pe::Image& image() const { return *image_; }
    const x86::Decoder& decoder() const { return *decoder_; }
    const SymbolDb& symbols() const { return symbols_; }
    SymbolDb& symbols() { return symbols_; }
    const std::filesystem::path& path() const { return path_; }
    const std::optional<std::filesystem::path>& pdb_path() const { return pdb_path_; }
    Arch arch() const { return image_->arch(); }

    // "0x401000", "401000h", a decorated name, a readable name or a PDB name -> address.
    std::optional<u64> resolve(std::string_view name_or_address) const;

    Result<FunctionExtent> function_extent(u64 start) const;
    // Instructions of the function in address order (data ranges skipped).
    Result<std::vector<x86::Instruction>> function_instructions(u64 start) const;
    Result<std::vector<x86::Instruction>> function_instructions(const FunctionExtent& extent) const;

    // Cross references to `target` (built on first use by scanning every sized function).
    std::vector<Xref> xrefs_to(u64 target) const;
    // Functions that call `target`.
    std::vector<u64> callers_of(u64 target) const;

    // Display name for an address: symbol (+offset), import, or hex.
    std::string describe_address(u64 va) const;

private:
    void build_xrefs() const;
    std::optional<JumpTable> read_jump_table(const x86::Instruction& jmp, u64 fn_start, u64 fn_limit) const;
    // x64 tables are reached through registers; `before` holds the instructions preceding the jump.
    std::optional<JumpTable> read_x64_jump_table(const std::vector<x86::Instruction>& before, const x86::Instruction& jmp,
                                                 u64 fn_start, u64 fn_limit) const;
    std::vector<u64> read_table_entries(const JumpTable& table, u64 fn_start, u64 fn_limit) const;

    std::filesystem::path path_;
    std::optional<std::filesystem::path> pdb_path_;
    std::unique_ptr<pe::Image> image_;
    std::unique_ptr<x86::Decoder> decoder_;
    SymbolDb symbols_;

    mutable std::unique_ptr<std::once_flag> xref_once_ = std::make_unique<std::once_flag>();
    mutable std::unordered_map<u64, std::vector<Xref>> xrefs_;
};

} // namespace decomp
