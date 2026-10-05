#pragma once

// The project's translation units (docs/project-format.md#unitstxt): units.txt lists them in link order,
// and each symbol's obj= in symbols.txt names the one it belongs to.

#include "analysis/units.hpp"
#include "core/json.hpp"
#include "core/result.hpp"
#include "project/project.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::project {

inline constexpr const char* kUnitsFile = "units.txt";

// units.txt line codec: `<name> [kind=] [source=] [origin=]`. A line without origin= was written by hand.
std::string format_unit_line(const Unit& unit);
Result<Unit> parse_unit_line(std::string_view line);

// units.txt in link order; empty when the project has none.
Result<std::vector<Unit>> load_units(const Project& project);
// Replaces units.txt.
Result<void> save_units(const Project& project, const std::vector<Unit>& units);

// The units of `program`, from the best record it has: its PDB's modules, else the object files a link
// map gave its symbols, else the analysis (which keeps the objects library matches gave).
struct DerivedUnits {
    UnitLayout layout;
    UnitOrigin from = UnitOrigin::analysis;
};
Result<DerivedUnits> derive_units(const Program& program);

// Writes a layout into the project: units.txt, and each symbol's unit as obj= in symbols.txt. The units
// the user added or edited (origin user) stay, with their symbols; units are listed in the order of
// their first function.
struct UnitsApplied {
    usize units = 0;
    usize functions = 0;  // functions with a unit
    usize unassigned = 0; // functions without one
};
Result<UnitsApplied> apply_units(Project& project, const UnitLayout& layout);

// Derives the project's units (derive_units()) and applies them. Without `force` it refuses when the
// project has units already, unless the analysis made them all (a map or a PDB now knows better).
// Objects that earlier derivations wrote (origin analysis or pdb) are set aside first, so they do not
// count as a map's.
Result<std::pair<DerivedUnits, UnitsApplied>> derive_project_units(Project& project, bool force);

// The project's units with the members symbols.txt gives them.
UnitLayout project_layout(const std::vector<Unit>& units, const SymbolDb& symbols);

// Matching progress per unit, in units.txt order; functions in no listed unit come last, under a unit
// with an empty name.
struct UnitProgress {
    Unit unit;
    usize functions = 0, matched = 0;
    u64 bytes = 0, matched_bytes = 0;  // functions of unknown size count 0 bytes
    double cost_usd = 0;
    std::map<FunctionStatus, usize> statuses;

    double percent_functions() const { return functions ? 100.0 * static_cast<double>(matched) / static_cast<double>(functions) : 0.0; }
    double percent_bytes() const { return bytes ? 100.0 * static_cast<double>(matched_bytes) / static_cast<double>(bytes) : 0.0; }
};
std::vector<UnitProgress> compute_unit_progress(const std::vector<Unit>& units, const SymbolDb& symbols,
                                                const std::map<u64, FunctionInfo>& infos);
Json to_json(const UnitProgress& progress);

} // namespace decomp::project
