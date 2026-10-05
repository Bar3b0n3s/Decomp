#pragma once

// The project's translation units (docs/project-format.md#unitstxt): units.txt lists them in link order,
// and each symbol's obj= in symbols.txt names the one it belongs to.

#include "analysis/units.hpp"
#include "core/json.hpp"
#include "core/result.hpp"
#include "matching/unit_source.hpp"
#include "project/project.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::project {

inline constexpr const char* kUnitsFile = "units.txt";

// units.txt line codec: `<name> [kind=] [source=] [origin=]`. A line without origin= was written by hand.
// A source must be a valid_unit_source().
std::string format_unit_line(const Unit& unit);
Result<Unit> parse_unit_line(std::string_view line);
// Whether a path can be a unit source: relative with forward slashes, under src/ but not src/functions/,
// without `.` or `..`, and naming a C or C++ file (.c, .cc, .cpp, .cxx).
bool valid_unit_source(std::string_view path);

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

// Unit sources (docs/project-format.md#unit-sources). The code unit with a source file that holds `fn`
// (by its obj=), when there is one.
const Unit* source_unit(const std::vector<Unit>& units, const Symbol& fn);
// Whether a function's verified source is in the project: in its unit's source, or its own file under
// src/functions/.
bool has_matched_source(const Project& project, const Symbol& fn, const std::vector<Unit>& units);
// Where it is: its unit's source when that holds the function, else its own file when that exists.
std::optional<std::filesystem::path> matched_source_location(const Project& project, const Symbol& fn, const std::vector<Unit>& units);
// The unit whose source a new match of `fn` joins: its source unit, when the project trusts the unit (a
// PDB's or a map's, or one the user wrote) or the unit's source file exists. A unit only the analysis
// guessed gets a source when `decomp units emit` (or the user) starts it: until then its functions'
// matches go to their own files.
const Unit* matching_unit(const Project& project, const std::vector<Unit>& units, const Symbol& fn);

// A change to a unit source, composed and verified but not written yet.
struct UnitChange {
    Unit unit;
    std::string base;     // the unit source it starts from (empty when the file does not exist)
    std::string content;  // the unit source with the change
    std::vector<u64> functions;  // the functions the new content holds
    std::vector<std::pair<u64, std::string>> rejected;  // functions that could not be composed, and why
    matching::UnitVerification verification;
    bool ok() const { return verification.all_byte_exact(); }
};
// Composes each function's verified translation unit (in address order) into the unit's source as it is
// on disk, and compiles the result once with every function in it diffed. A function whose source has
// no usable definition is rejected (and left out); when every one is, nothing is compiled.
Result<UnitChange> prepare_unit_change(const Project& project, const Program& program, const matching::MatchSetup& setup, const Unit& unit,
                                       const std::vector<std::pair<const Symbol*, std::string>>& sources);
// Writes a prepared change, recorded in changes.jsonl. Fails with ErrorCode::conflict when the unit
// source changed since it was prepared.
Result<WriteReceipt> commit_unit_change(const Project& project, const UnitChange& change, const ChangeOrigin& origin,
                                        const ChangeSubject& subject);

// Compiles a candidate for `fn` the way sessions do: composed into its unit's source when `unit` is given
// (the whole unit compiled, `fn` diffed from it), else on its own. A source that cannot join the unit
// gets no compile: `result.diff_error` says why.
struct Candidate {
    matching::CandidateResult result;
    std::optional<UnitChange> unit;
};
Result<Candidate> compile_candidate(const Project& project, const Program& program, const matching::MatchSetup& setup, const Symbol& fn,
                                    const std::string& source, const Unit* unit);

// Saves a function's verified translation unit: composed into its matching_unit()'s source when it has
// one (written when the function is byte-exact there and no function of the unit that was byte-exact
// before is not, prepared again when another writer changed the unit source in between), else as its
// own file under src/functions/. Fails, writing nothing, when the source cannot join the unit, is not
// byte-exact in it, or breaks a function of it.
struct SavedSource {
    std::filesystem::path path;  // the file written, relative to the project
    WriteReceipt receipt;
    std::optional<UnitChange> unit;  // set when it went into a unit source
};
Result<SavedSource> save_verified_function(const Project& project, const Program& program, const matching::MatchSetup& setup, const Symbol& fn,
                                           const std::string& source, const ChangeOrigin& origin);

// The functions of a prepared change that are not byte-exact in it, other than `except`.
std::vector<const matching::UnitCheck*> failing_functions(const UnitChange& change, u64 except = 0);
// The failing functions (other than `except`) that were byte-exact in the unit source the change starts
// from: what the change breaks. The base is compiled (usually from the compile cache) only when a
// function fails.
Result<std::vector<const matching::UnitCheck*>> broken_functions(const Program& program, const matching::MatchSetup& setup,
                                                                 const UnitChange& change, u64 except = 0);
// "add (98.5% ...); 0x401200 (why)": the checks' functions with their state, for messages.
std::string describe_checks(const Program& program, std::span<const matching::UnitCheck* const> checks);

// Compiles the unit sources (of `names`, else every code unit that has one) and diffs every function
// each holds.
struct UnitVerificationReport {
    Unit unit;
    matching::UnitVerification verification;
};
Result<std::vector<UnitVerificationReport>> verify_unit_sources(const Project& project, const Program& program,
                                                               const matching::MatchSetup& setup, std::span<const std::string> names = {});

// Moves the verified sources of matched functions (src/functions/<fn>.cpp) into their units' sources: per
// unit, composes them in address order, verifies the result, and when every function in it is byte-exact
// writes the unit source and removes the files it took in (each write recorded). Functions that do not
// compose, or that are not byte-exact in the unit, keep their own files: the unit is tried again
// without them.
struct EmitReport {
    struct UnitResult {
        std::string name;
        std::vector<u64> emitted;
        std::vector<std::pair<u64, std::string>> kept;  // functions left in their own files, and why
    };
    std::vector<UnitResult> units;
};
Result<EmitReport> emit_unit_sources(const Project& project, const Program& program, const matching::MatchSetup& setup,
                                     const ChangeOrigin& origin, std::span<const std::string> names = {});

} // namespace decomp::project
