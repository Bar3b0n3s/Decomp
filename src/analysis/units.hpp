#pragma once

// Translation units (docs/project-format.md#unitstxt): the object files a program was linked from, in
// link order, and the one each function and global came from. A PDB lists them: its modules, and the
// section contributions that place their code and data. A link map names each symbol's object file.
// Without either, the analysis guesses them from the layout of the code (units_by_analysis()).

#include "analysis/symbols.hpp"
#include "core/json.hpp"
#include "formats/pdb.hpp"
#include "formats/pe.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp {

class Program;

enum class UnitKind : u8 {
    code,     // the program's own code, decompiled into a source file of its own
    library,  // a member of a static library (the C runtime, an SDK library)
    import,   // an import library's stubs and import descriptors
    linker,   // what the linker made itself (`* Linker *`)
};
std::string_view to_string(UnitKind kind);
std::optional<UnitKind> unit_kind_from_string(std::string_view text);

// Where a unit came from, in increasing order of trust.
enum class UnitOrigin : u8 { analysis, map, pdb, user };
std::string_view to_string(UnitOrigin origin);
std::optional<UnitOrigin> unit_origin_from_string(std::string_view text);

struct Unit {
    std::string name;  // the object as a link map names it: "player.obj", "LIBCMT:printf.obj", "* Linker *"
    UnitKind kind = UnitKind::code;
    std::string source;  // a code unit's source file, relative to the project: "src/player.cpp"
    UnitOrigin origin = UnitOrigin::analysis;
};

// The units of a program and the unit of each symbol.
struct UnitLayout {
    std::vector<Unit> units;             // link order
    std::map<u64, std::string> members;  // symbol address -> unit name (functions and data)

    const Unit* find(std::string_view name) const;
    // The unit holding the symbol at `va`; empty when none does.
    std::string_view unit_of(u64 va) const;
    // The functions of `unit` among `symbols`, in address order.
    std::vector<const Symbol*> functions(std::string_view unit, const SymbolDb& symbols) const;
};

// The unit name of a PDB module, as a link map names the object: "basic.obj" for C:\src\basic.obj,
// "LIBCMT:printf.obj" for a member of LIBCMT.lib, "kernel32:KERNEL32.dll" for an import library's
// member, and `* Linker *` and `Import:KERNEL32.dll` as they are.
std::string unit_name(const pdb::Module& module);
// A unit name as link maps write it: a library member's library without its extension
// ("LIBC.LIB:printf.obj" from a library match becomes "LIBC:printf.obj").
std::string normalize_unit_name(std::string_view name);
// What kind of unit an object is, from its name: a library member is `library`, a DLL member or an
// `Import:` descriptor `import`, the linker's own `linker`, anything else `code`.
UnitKind unit_kind_of(std::string_view name);

// One unit per module of the PDB, in module (link) order. Each function and data symbol goes to the
// module whose section contribution holds its address. Code units get sources (assign_sources()) from
// the source files the modules name.
UnitLayout units_from_pdb(const pdb::Reader& pdb, const pe::Image& image, const SymbolDb& symbols);

// The units the symbols' object files name (`Symbol::object`: link maps, library matches), ordered by
// their lowest address. A function without an object file goes to the unit of the functions around
// it, or of the function before it when they differ; data without one stays unassigned.
UnitLayout units_from_objects(const SymbolDb& symbols, const BinaryImage& image);

// A guess from the code alone, for a program with neither a PDB nor a map: functions that share a
// symbol's object file keep it (library matches), linker and import thunks go to `* Linker *` and the
// import units, and the rest is cut into units of consecutive functions where nothing ties the
// functions on one side of a cut to those on the other: calls, data both use, and data placed close
// together or out of order (a unit's own data sits together, in the order of its source). Units are
// named after their first function's address ("unit_00401000.obj").
UnitLayout units_by_analysis(const Program& program);

// Gives each code unit without a source one under `dir` ("src"): the file the PDB names for the module
// when it knows it (`sources`, by unit name), else the object's stem with ".c" when all of its functions
// have C names and ".cpp" otherwise. Directories from the original path are added where two units would
// share a file name.
void assign_sources(UnitLayout& layout, const SymbolDb& symbols, const std::map<std::string, std::string>& sources = {},
                    std::string_view dir = "src");

// A derived layout against the truth (a PDB's modules, a map's objects), over the functions the truth
// assigns, in address order. A boundary is a place where the next function is in another unit.
struct UnitComparison {
    usize truth_units = 0, found_units = 0;  // units holding functions
    usize functions = 0;                     // functions the truth assigns
    usize exact_units = 0;                   // truth units the layout finds with exactly the same functions
    usize functions_in_exact_units = 0;
    usize truth_boundaries = 0, found_boundaries = 0, common_boundaries = 0;
    bool same_names = false;  // every function in a unit of the same name (layouts from the same records)
    std::vector<std::string> differences;  // the first few disagreements, for display

    double exact_rate() const {
        return functions ? static_cast<double>(functions_in_exact_units) / static_cast<double>(functions) : 1.0;
    }
    double boundary_precision() const {
        return found_boundaries ? static_cast<double>(common_boundaries) / static_cast<double>(found_boundaries) : 1.0;
    }
    double boundary_recall() const {
        return truth_boundaries ? static_cast<double>(common_boundaries) / static_cast<double>(truth_boundaries) : 1.0;
    }
};
UnitComparison compare_units(const UnitLayout& truth, const UnitLayout& found, const SymbolDb& symbols);
Json to_json(const UnitComparison& comparison);

} // namespace decomp
