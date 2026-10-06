#pragma once

// What the linker put where (docs/architecture.md#relinking): the image's sections cut into contributions,
// the input sections of the objects it was linked from, each with its unit. A relink gives the linker the
// same input sections in the same order: compiled objects for the units that match, and split objects
// carrying the original bytes of the rest.

#include "analysis/units.hpp"
#include "formats/pdb.hpp"
#include "formats/pe.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp {

class Program;

struct Contribution {
    u32 rva = 0;
    u32 size = 0;              // in the image; the part past its section's file data is zero
    std::string section;       // the image section (".text")
    std::string name;          // the input section's name (".text$mn"), as far as it is known: name_contributions()
    u32 characteristics = 0;   // the input section's: alignment, COMDAT, contents and access
    std::string unit;          // the unit it came from
    bool linker = false;       // the linker made it (its own unit, an import library's): no object carries it

    u32 end() const { return rva + size; }
    bool uninitialized() const { return (characteristics & pe::scn::cnt_uninitialized_data) != 0; }
    bool contains(u32 address) const { return address >= rva && address < rva + size; }
};

enum class LayoutSource : u8 { pdb, symbols };
std::string_view to_string(LayoutSource source);

// What compiled a unit's object, as the PDB's compile record says.
struct ObjectOrigin {
    int language = -1;     // CV_CFL_*: 0 C, 1 C++, 3 MASM
    u16 build = 0;         // the compiler's build number
    std::string compiler;  // its version string: "clang version 18.1.3 (...)"
    bool security_checks = false, sdl = false;  // compiled with /GS, /sdl
};

struct ImageLayout {
    std::vector<Contribution> contributions;  // address order
    LayoutSource source = LayoutSource::pdb;
    std::map<std::string, ObjectOrigin> origins;  // by unit, from the PDB

    // The contribution holding `rva`, or nullptr (padding, headers).
    const Contribution* at(u32 rva) const;
    // A unit's contributions in address order.
    std::vector<const Contribution*> of_unit(std::string_view unit) const;
};

// An entry of the POGO debug record link.exe writes (Visual Studio 2013 and later): the range of the
// image that the input sections of one name fill.
struct PogoEntry {
    u32 rva = 0;
    u32 size = 0;
    std::string name;  // ".text$mn"
};
std::vector<PogoEntry> pogo_entries(const pe::Image& image);

// The PDB's section contributions, in address order, with each module's unit name (pdb_unit_names())
// and the input section names its COFF group records give. Contributions of the linker's own module and
// of import libraries are marked `linker`. Empty contributions are kept: their alignment moves what follows.
ImageLayout layout_from_pdb(const pdb::Reader& pdb, const pe::Image& image);

// Without a PDB: each section of the image cut where the unit of the symbols in it changes (functions and
// data that `units` assigns), the bytes before a section's first symbol going with that symbol and the
// bytes after a symbol with it. What the linker makes itself (import and export tables, the debug
// directory and its records, base relocations, import thunks) is cut out as `linker`.
ImageLayout layout_from_units(const Program& program, const UnitLayout& units);

// x64: the exception table's entries that the linker holds (link.exe's PDBs give the table to the linker
// or to no module) go back to the units of the functions they describe, one 12-byte .pdata contribution
// each, a COMDAT when its function's code is one, as compilers write them; lld-link's PDBs already give
// them to their units. layout_from_pdb() does this.
void attribute_exception_table(ImageLayout& layout, const pe::Image& image);

// Gives each contribution the name of its input section, as far as the image tells: the POGO record's
// names; else ".bss" for uninitialized data, ".xdata" for x64 unwind information (what .pdata points at),
// and the image section's name for the rest.
void name_contributions(ImageLayout& layout, const pe::Image& image);

// The order the linker took `units` in, as the image shows it: the input sections of one name (and
// kind) are placed in link order, so each such group orders the units it holds; the groups' orders are
// merged, with `units`' own order breaking ties (and placing units that hold nothing). link.exe's PDBs
// list the modules in another order than the link's.
std::vector<std::string> link_order(const ImageLayout& layout, const std::vector<std::string>& units);

} // namespace decomp
