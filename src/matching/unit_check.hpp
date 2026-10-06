#pragma once

// A unit's compiled object placed where the linker put the unit's code and data in the target, and
// compared with it (docs/matching.md#units): the order of its functions, its data's contents and
// placement, pooled strings and constants, and its exception-handling and unwind tables. What a relink
// links from source must pass this check first.

#include "analysis/layout.hpp"
#include "analysis/program.hpp"
#include "core/json.hpp"
#include "formats/coff.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::matching {

enum class PlacementState : u8 {
    equal,      // placed, and the image holds the same bytes there
    differs,    // placed, and some bytes differ
    unplaced,   // nothing tells where the linker puts it
    discarded,  // a COMDAT whose copy another unit gives the image (a pooled string or constant)
};
std::string_view to_string(PlacementState state);

struct PlacedSection {
    u32 section = 0;          // its number in the object
    std::string name;         // ".text$mn"
    std::string symbol;       // what names it: its COMDAT symbol, else its first symbol
    u32 size = 0;
    u32 alignment = 1;
    bool uninitialized = false;
    bool comdat = false;
    std::optional<u32> rva;   // where it goes in the image
    PlacementState state = PlacementState::unplaced;
    std::string unit;                     // a discarded COMDAT: the unit whose copy the image has
    std::optional<u32> first_difference;  // offset of the first differing byte in the section
    usize differing_bytes = 0;
    std::string note;  // how it differs, or why it could not be placed
};

struct UnitCheckResult {
    std::string unit;
    std::vector<PlacedSection> sections;      // what the object puts in the image, in object order
    std::vector<Contribution> missing;        // the unit's contributions (from the PDB) no compiled section fills
    std::vector<std::string> problems;        // placements that contradict each other or the PDB
    // Where the object's external references resolve in the image, by the program's symbols or the
    // image's bytes (name -> RVA), and where its external definitions land: what a relink must provide
    // to it and what it provides.
    std::map<std::string, u32> externals;
    std::map<std::string, u32> definitions;

    // Every section placed and equal (or discarded, or an unreferenced COMDAT the linker drops), nothing
    // missing and no problems: the object can replace the unit's original in a relink.
    bool ok() const;
    usize count(PlacementState state) const;
    // "3 sections equal, .data differs at +0x8", for messages.
    std::string summary() const;
};

// Places and compares the sections of `object`, `unit`'s source compiled:
// - Sections are placed by their symbols: the unit's functions (`functions`, the unit source's) at their
//   addresses, external names the program knows, and the targets of the relocations of sections already
//   placed (the image's bytes at a relocation say where its target is). The linker puts an object's
//   sections of one name together in object order, so placing one of them places the others.
// - A COMDAT placed in another unit's contribution is a copy the linker discards in favor of that unit's
//   (a string or constant pooled across units).
// - Each placed section is compared with the image, its relocations by where their targets are.
// With a layout from the PDB, each of the unit's contributions must be filled by a placed section of the
// same address and size, and no placed section may sit in another unit's.
UnitCheckResult check_unit(const Program& program, const ImageLayout& layout, const coff::Object& object, std::string_view unit,
                           const std::vector<u64>& functions);

Json to_json(const UnitCheckResult& result);

} // namespace decomp::matching
