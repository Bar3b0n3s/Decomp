#pragma once

// Relinking the project's target (docs/architecture.md#relinking). A code unit whose source compiles to
// the unit's code and data (matching/unit_check.hpp) is linked from that source; every other unit's
// code and data goes in as a split object of its original bytes (relink/split.hpp), the imports through
// import libraries made from the image's import table, and the original linker links it all with the
// flags the image's headers call for. The result is compared with the target (relink/compare.hpp): the
// whole program verified, not just its functions.

#include "analysis/layout.hpp"
#include "core/json.hpp"
#include "matching/match.hpp"
#include "matching/unit_check.hpp"
#include "project/project.hpp"
#include "relink/compare.hpp"
#include "relink/linker.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace decomp::project {

// The image layout relinks and unit checks use: the PDB's section contributions when the project's target
// has its PDB, else the project's units cut from its symbols.
Result<ImageLayout> project_image_layout(const Program& program, const std::vector<Unit>& units);

// A unit's source compiled and checked against the image.
struct UnitSourceCheck {
    Unit unit;
    std::vector<u64> functions;          // the unit's functions (their obj=)
    std::vector<u64> missing_functions;  // those the compiled source does not put in place (all of them without an object)
    matching::CompileResult compile;
    std::string error;                   // why there is no check: no source file, the compile failed
    std::optional<matching::UnitCheckResult> check;

    // Every function in the source, and the compiled object fills the unit's place in the image exactly.
    bool complete() const { return error.empty() && missing_functions.empty() && check && check->ok(); }
    // "complete", "4 of 12 functions in its source", the check's summary.
    std::string summary() const;
};
Result<UnitSourceCheck> check_unit_source(const Project& project, const Program& program, const matching::MatchSetup& setup,
                                          const ImageLayout& layout, const Unit& unit);
// The units with sources (of `names`, else every code unit with a source file), checked.
Result<std::vector<UnitSourceCheck>> check_unit_sources(const Project& project, const Program& program, const matching::MatchSetup& setup,
                                                        std::span<const std::string> names = {});
Json to_json(const UnitSourceCheck& check);

// A unit source made from a whole translation unit (an original source file, one written by hand): every
// function of the unit composed from it (matching::compose_function) after its marker, in the order the
// translation unit defines them, the rest of it the prelude; checked like the unit's source, not written.
struct ComposedUnit {
    Unit unit;
    std::string content;
    std::vector<std::pair<u64, std::string>> rejected;  // the unit's functions it does not define, and why
    UnitSourceCheck check;
};
Result<ComposedUnit> compose_unit_source(const Project& project, const Program& program, const matching::MatchSetup& setup,
                                         const std::string& unit, std::string_view source);

struct RelinkOptions {
    std::vector<std::string> source;  // link these units from their sources even when their checks fail
    std::vector<std::string> split;   // carry these units in split objects even when their sources check
    bool all_split = false;           // every unit from its original bytes: tests the relink itself
    std::function<bool()> cancelled;
    std::function<void(const std::string&)> progress;  // one line per step
};

enum class LinkMode : u8 {
    source,  // the unit's object compiled from its source
    split,   // a split object of the unit's original bytes
    linker,  // what the linker makes itself (its own unit, import libraries' members)
};
std::string_view to_string(LinkMode mode);

struct UnitLink {
    Unit unit;
    LinkMode mode = LinkMode::split;
    std::string reason;  // why: "complete", "4 of 12 functions in its source", "no source"
    std::optional<UnitSourceCheck> check;
    std::string object;  // what was linked, relative to the relink directory (empty: nothing)
    u64 bytes = 0;       // the size of its contributions
};

struct RelinkResult {
    std::string time;
    std::vector<UnitLink> units;           // link order
    std::vector<std::string> libraries;    // the import libraries made, relative to the relink directory
    std::vector<std::string> notes;        // what the relink had to work around or could not do
    relink::LinkerFit linker;              // whether the linker is the one that made the image
    relink::LinkResult link;
    std::string image;                     // the relinked image, relative to the project
    std::optional<relink::ImageComparison> comparison;
    std::string error;                     // why nothing was compared

    bool identical() const { return comparison && comparison->identical; }
    usize count(LinkMode mode) const;
};

// .decomp/relink: objects/, libs/, out/ and result.json.
std::filesystem::path relink_dir(const Project& project);

// Relinks the target and compares the result with it; the result is also written to result.json.
Result<RelinkResult> relink_project(const Project& project, const Program& program, const matching::MatchSetup& setup,
                                    const RelinkOptions& options = {});
Json to_json(const RelinkResult& result);
// The last relink's result.json, when there is one.
std::optional<Json> last_relink(const Project& project);

} // namespace decomp::project
