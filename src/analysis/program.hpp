#pragma once

#include "analysis/jump_tables.hpp"
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
// `pointer`: an address stored in data (a vtable slot, a callback table, a string table).
enum class XrefKind : u8 { call, jump, read, address, write, pointer };
std::string_view to_string(XrefKind kind);

struct Xref {
    u64 from = 0;      // instruction address, or the data address of a pointer
    u64 function = 0;  // start of the function containing `from` (0 if unknown, and for pointers in data)
    XrefKind kind = XrefKind::read;
    u64 to = 0;        // the referenced address
    u64 via = 0;       // the linker thunk (incremental linking, import) the reference reached `to` through
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

struct OpenOptions {
    std::optional<std::filesystem::path> pdb = {};  // this PDB instead of the one found next to the image
    std::optional<std::filesystem::path> map = {};  // the build's link map: names, object files, function starts
    bool use_pdb = true;  // false: open as if the image had no PDB (to measure the analysis against it)
    // Find the functions by analysis (discover_functions) when no usable PDB describes them. A project
    // opens without it: its symbols.txt lists the functions found when it was created.
    bool discover = true;
};

// A loaded target binary: image, symbols and lazily computed analysis results. Copies made with
// with_symbols() share the image and decoder, so a new symbol generation is cheap.
class Program {
public:
    // Loads a PE image. The PDB is taken from `options.pdb`, or found next to the image via its CodeView
    // record or "<stem>.pdb"; a PDB whose GUID/age does not match is ignored with a warning.
    static Result<Program> open(const std::filesystem::path& binary, const OpenOptions& options);
    static Result<Program> open(const std::filesystem::path& binary, const std::optional<std::filesystem::path>& pdb_path = {}) {
        return open(binary, OpenOptions{.pdb = pdb_path});
    }

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

    // Adds the symbols of the build's link map (SymbolDb::add_map) once it is known to describe this
    // image. Returns the number of symbols it added or renamed. Call before the analysis is queried.
    Result<usize> add_map(const std::filesystem::path& map_path);
    // Functions found by discover_functions() from the current symbols become symbols (named sub_<va>
    // unless already named; import thunks after their import). Call before the analysis is queried.
    void add_discovered_functions();
    Arch arch() const { return image_->arch(); }

    // "0x401000", "401000h", a decorated name, a readable name or a PDB name -> address.
    std::optional<u64> resolve(std::string_view name_or_address) const;

    Result<FunctionExtent> function_extent(u64 start) const;
    // Instructions of the function in address order (data ranges skipped).
    Result<std::vector<x86::Instruction>> function_instructions(u64 start) const;
    Result<std::vector<x86::Instruction>> function_instructions(const FunctionExtent& extent) const;

    // Cross references to `target`, built on first use: what every sized function references (see
    // xrefs_from()), the pointers stored in data (at base relocations outside the code; without
    // relocations, at aligned values that hold an address in the image), and for references that land
    // on a linker thunk, the same reference to the function or import behind it (with `via` set).
    std::vector<Xref> xrefs_to(u64 target) const;
    // The pointers stored in data, as (where, value): see xrefs_to().
    std::vector<std::pair<u64, u64>> data_pointers() const;
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
    // The function or import slot behind a linker thunk at `va`: an import thunk (`jmp [IAT slot]`), or
    // an unnamed incremental-linking thunk (see thunk_destination()). nullopt for anything else,
    // including a function of its own whose body is a jump.
    std::optional<u64> linker_thunk_target(u64 va) const;
    // A switch table dispatched by `jmp`; `before` holds the instructions that run before it, in order.
    std::optional<JumpTable> jump_table(std::span<const x86::Instruction> before, const x86::Instruction& jmp, u64 fn_start, u64 fn_limit) const;

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
