#pragma once

// Comparing a relinked image with the original (docs/architecture.md#relinking). The fields that only say
// when and how an image was built (timestamps, the PDB's GUID and age, the checksum: pe identity fields)
// are taken over from the original first; every other byte must be the same for the SHA-1s to be equal.
// Differences are reported by header field, or by section and RVA with the unit whose contribution holds
// them.

#include "analysis/layout.hpp"
#include "analysis/symbols.hpp"
#include "core/json.hpp"
#include "formats/pe.hpp"

#include <optional>
#include <string>
#include <vector>

namespace decomp::relink {

// A field taken over from the original.
struct StampedField {
    std::string name;
    u32 offset = 0;  // in the relinked file
    std::string original, relinked;  // the values, hex
};

// A run of differing bytes.
struct ByteDifference {
    u32 offset = 0;  // in the original file
    u32 size = 0;
    std::string where;          // "COFF header TimeDateStamp", ".text+0x123"
    std::optional<u32> rva;     // for section contents
    std::string unit;           // the unit whose contribution holds it
    std::string symbol;         // "dispatch+0x12"
    std::string original_bytes, relinked_bytes;  // up to 16 bytes from where it starts, hex
};

struct SectionDifference {
    std::string name;
    usize differing_bytes = 0;
    std::optional<u32> first_rva;
    std::string first_unit;
    bool size_differs = false;
};

struct ImageComparison {
    std::string original_sha1;
    std::string relinked_sha1;            // after the identity fields were taken over
    std::string relinked_unstamped_sha1;  // as the linker wrote it
    u64 original_size = 0, relinked_size = 0;
    bool identical = false;
    std::vector<StampedField> stamped;
    usize differing_bytes = 0;
    std::vector<ByteDifference> differences;  // the first runs, in file order
    std::vector<SectionDifference> sections;  // every section with a difference
    // The first difference in section contents (address order), and the unit holding it: the place to look.
    std::optional<ByteDifference> first;
};

// Stamps `relinked` with the original's identity fields (in place) and compares the two. At most
// `max_differences` runs are listed.
ImageComparison compare_images(const pe::Image& original, std::vector<std::byte>& relinked, const ImageLayout& layout,
                               const SymbolDb& symbols, usize max_differences = 64);

Json to_json(const ImageComparison& comparison);

} // namespace decomp::relink
