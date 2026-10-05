#pragma once

// Function bounds measured against ground truth (docs/roadmap.md, Phase 2): the functions an analysis
// found, compared with the build's own record of them. A PDB gives every procedure's start and size; a
// map file gives starts only, so there an end counts as exact when nothing but padding lies between it
// and the next symbol.

#include "analysis/symbols.hpp"
#include "arch/x86/decoder.hpp"
#include "core/json.hpp"
#include "core/result.hpp"
#include "formats/image.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decomp {

struct FunctionBounds {
    u64 start = 0;
    u64 end = 0;  // exclusive; 0 when unknown (a map file)
    std::string name;
};

// The functions of `symbols` that start in executable code and have a size, in address order.
std::vector<FunctionBounds> function_bounds(const SymbolDb& symbols, const BinaryImage& image);
// The procedures of a PDB for an image loaded at `image_base`, one per start (code the linker folded
// keeps one name), in address order.
Result<std::vector<FunctionBounds>> pdb_function_bounds(const std::filesystem::path& pdb, u64 image_base, const BinaryImage& image);
// The functions of a link.exe-style map file: entries flagged `f`, or (lld-link writes no flags) every
// entry in executable code. Ends are unknown.
Result<std::vector<FunctionBounds>> map_function_bounds(const std::filesystem::path& map, const BinaryImage& image);

struct BoundsMismatch {
    enum class Kind : u8 { missed, wrong_end, extra };
    Kind kind = Kind::missed;
    u64 start = 0;
    u64 truth_end = 0;  // 0: unknown
    u64 found_end = 0;  // 0: not found
    std::string name;
};
std::string_view to_string(BoundsMismatch::Kind kind);

struct BoundsComparison {
    usize truth = 0;       // functions in the ground truth
    usize found = 0;       // functions the analysis found
    usize exact = 0;       // same start and end
    usize start_only = 0;  // same start, another end
    usize missed = 0;      // no function found at a true start
    usize extra = 0;       // a function found where none starts
    bool truth_has_ends = true;
    std::vector<BoundsMismatch> mismatches;  // in address order

    double exact_rate() const { return truth ? static_cast<double>(exact) / static_cast<double>(truth) : 1.0; }
    double start_rate() const { return truth ? static_cast<double>(exact + start_only) / static_cast<double>(truth) : 1.0; }
};

// Without ends in the truth, a found end is exact when the bytes up to the next true start (or the end
// of the section) are padding.
BoundsComparison compare_bounds(std::span<const FunctionBounds> truth, std::span<const FunctionBounds> found, const BinaryImage& image,
                                const x86::Decoder& decoder);
Json to_json(const BoundsComparison& c, usize max_mismatches);

} // namespace decomp
