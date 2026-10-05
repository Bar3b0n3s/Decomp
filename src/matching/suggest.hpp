#pragma once

// Which compiler to match a target with, from what its Rich header says about the build
// (docs/matching.md#choosing-the-toolchain), and how well a configured toolchain fits it: a toolchain's
// probe object carries the same compiler id (@comp.id) the target's objects left in the header.

#include "core/json.hpp"
#include "formats/rich.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::matching {

struct ToolchainSuggestion {
    std::string name;           // the toolchain name the docs use for the release: "vc6", "vs2019"
    std::string visual_studio;  // "Visual C++ 6.0", "Visual Studio 2019 16.11"
    std::string compiler;       // "C++ compiler 12.00.8804"
    u16 product_id = 0;         // of that compiler's Rich entry
    u16 build = 0;
    std::vector<std::string> notes;  // what else matters for matching: edition, LTCG, PGO, other compilers
};

// nullopt when the header names no compiler.
std::optional<ToolchainSuggestion> suggest_toolchain(const pe::BuildInfo& build);

// How a toolchain's compiler compares with the one the target's own code came from.
enum class ToolchainFit : u8 {
    unknown,        // the target names no compiler, or the toolchain's objects carry no compiler id (clang-cl)
    same_build,     // the same compiler version and build
    same_release,   // the same Visual Studio release, another build (service pack or update)
    other_release,
};
std::string_view to_string(ToolchainFit fit);

struct FitReport {
    ToolchainFit fit = ToolchainFit::unknown;
    std::string toolchain_compiler;  // "C++ compiler 12.00.8168 (Visual C++ 6.0)", empty when unknown
    std::string text;                // one line for people
};
// `comp_id`: (product id << 16) | build, the @comp.id of an object the toolchain compiled.
FitReport toolchain_fit(const pe::BuildInfo& build, std::optional<u32> comp_id);

Json to_json(const pe::BuildTool& tool);
Json to_json(const pe::BuildInfo& build);  // with "main_compiler" when there is one
Json to_json(const ToolchainSuggestion& suggestion);
Json to_json(const FitReport& fit);

} // namespace decomp::matching
