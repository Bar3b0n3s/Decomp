#pragma once

// Library function signatures (docs/architecture.md#library-functions): the functions of a static
// library's objects as bytes, with the fields their relocations fill in masked out, matched against the
// target's functions to find the ones the linker copied from the library (the C runtime, SDK and engine
// libraries). Those are not the target's own code: they are named after the library and marked
// `library`, so no agent works on them.

#include "analysis/program.hpp"
#include "core/result.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace decomp {

struct SignatureReference {
    u32 offset = 0;      // of the relocated field in the function
    u8 size = 0;         // of the field
    u16 type = 0;        // the relocation type
    bool pc_relative = false;
    std::string symbol;  // what it refers to; empty for the function's own labels and sections
};

struct FunctionSignature {
    std::string name;     // decorated: "_strlen", "?lib@@YAHH@Z"
    std::string member;   // the archive member (object) it came from
    std::string library;  // the library's file name
    std::vector<u8> bytes;    // trailing padding trimmed
    std::vector<bool> mask;   // false inside relocated fields
    std::vector<SignatureReference> references;
    usize significant = 0;    // bytes compared (not masked)
};

struct SignatureOptions {
    usize min_significant = 12;  // shorter functions are too common to identify anything
};

// The function signatures of the objects in a static library (import objects carry no code).
Result<std::vector<FunctionSignature>> library_signatures(const std::filesystem::path& library, Arch arch, const SignatureOptions& options = {});

struct LibraryMatch {
    u64 va = 0;  // the target function
    std::vector<const FunctionSignature*> candidates;  // signatures whose bytes and references match
    const FunctionSignature* chosen = nullptr;  // the function, when one candidate is left; else ambiguous
};

// Tries every function of the program against the signatures. A candidate's bytes must match where
// they are not masked, and the fields its relocations fill must lead where the target's names say
// (a name from the image, a map or a PDB, or another function matched here). A name that two target
// functions both fit is given to neither. In address order; functions no signature fits are left out.
std::vector<LibraryMatch> match_library_functions(const Program& program, std::span<const FunctionSignature> signatures);

} // namespace decomp
