#pragma once

#include "analysis/symbols.hpp"
#include "arch/x86/decoder.hpp"
#include "core/result.hpp"
#include "formats/pe.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
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

// call/jump: a branch that leaves the function (an indirect call through memory is a call too);
// read/write: a memory operand the instruction reads or stores to; address: an address-valued
// immediate (`push offset`, `mov reg, offset`).
enum class XrefKind : u8 { call, jump, read, address, write };
std::string_view to_string(XrefKind kind);

struct Xref {
    u64 from = 0;      // instruction address
    u64 function = 0;  // start of the function containing `from` (0 if unknown)
    XrefKind kind = XrefKind::read;
    u64 to = 0;        // the referenced address
};

// Displacements that are RVAs because their base or index register holds the image base. MSVC x64
// materializes `lea r, [rip+__ImageBase]` and then reads `[r+index*scale+<rva>]` (global arrays, jump
// and byte tables). Registers are tracked in address order; a write or a call (volatile registers)
// forgets them. Returns (instruction address, field index) pairs.
std::set<std::pair<u64, usize>> image_relative_fields(const BinaryImage& image, std::span<const x86::Instruction> list);

// What happened to the target's PDB when the program was opened.
enum class PdbStatus : u8 {
    matched,      // loaded; its GUID and age match the image
    mismatch,     // found, but for a different build of the image (ignored)
    unsupported,  // found, but unreadable (for example a PDB 2.0 file from VC6) (ignored)
    absent,       // none found
};
std::string_view to_string(PdbStatus status);

// A loaded target binary: image, symbols and lazily computed analysis results. Copies made with
// with_symbols() share the image and decoder, so a new symbol generation is cheap.
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
    PdbStatus pdb_status() const { return pdb_status_; }
    const std::string& pdb_detail() const { return pdb_detail_; }  // why a PDB was ignored

    // The same image with a different symbol database (fresh analysis caches).
    Program with_symbols(SymbolDb symbols) const;
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
    // What the function at `function_va` references, by the rules of xrefs_to(): branches that leave
    // it, memory operands and address-valued immediates that hold an address (relocated, RIP-relative,
    // image-base-relative on x64, or an in-image value in an image without relocations). In
    // instruction order; empty when there is no function to decode. Decodes the function (no index).
    std::vector<Xref> xrefs_from(u64 function_va) const;
    // The same for a function that is already decoded.
    std::vector<Xref> xrefs_in(const FunctionExtent& extent, std::span<const x86::Instruction> instructions) const;

    // Display name for an address: symbol (+offset), import, or hex.
    std::string describe_address(u64 va) const;

    // Thunks the linker inserts between a call and its destination: incremental-linking (ILT)
    // entries `jmp rel32 <function>` (possibly chained) and import thunks `jmp [IAT slot]`. Returns the
    // function's address or the IAT slot's address; nullopt when `va` is not such a thunk.
    std::optional<u64> thunk_destination(u64 va) const;

private:
    void fold_linker_thunks();
    std::optional<x86::Instruction> decode_at(u64 va) const;
    void build_xrefs() const;
    std::optional<JumpTable> read_jump_table(const x86::Instruction& jmp, u64 fn_start, u64 fn_limit) const;
    // x64 tables are reached through registers; `before` holds the instructions preceding the jump.
    std::optional<JumpTable> read_x64_jump_table(std::span<const x86::Instruction> before, const x86::Instruction& jmp,
                                                 u64 fn_start, u64 fn_limit) const;
    std::vector<u64> read_table_entries(const JumpTable& table, u64 fn_start, u64 fn_limit) const;

    std::filesystem::path path_;
    std::optional<std::filesystem::path> pdb_path_;
    std::shared_ptr<const pe::Image> image_;
    std::shared_ptr<const x86::Decoder> decoder_;
    SymbolDb symbols_;
    PdbStatus pdb_status_ = PdbStatus::absent;
    std::string pdb_detail_;

    mutable std::unique_ptr<std::once_flag> xref_once_ = std::make_unique<std::once_flag>();
    mutable std::unordered_map<u64, std::vector<Xref>> xrefs_;
};

} // namespace decomp
