#pragma once

// What the Relink view shows (docs/ui.md#relink): a relink's result (.decomp/relink/result.json, which
// `decomp relink` and the view write) read into rows: the units with how each was linked and its source's
// check, section by section; the linker and its output; the comparison with the target, with the first
// differing bytes and the unit holding them. And the bytes around a difference in the target and in the
// relinked image, side by side. Pure: reading files is the caller's.

#include "core/json.hpp"
#include "core/result.hpp"
#include "formats/pe.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::vm {

// One section of a unit's compiled object, placed in the image (matching::PlacedSection).
struct RelinkPlacedSection {
    std::string section;  // ".text"
    std::string name;     // the object section's name
    std::string symbol;   // what it defines
    std::string state;    // "equal", "differs", "unplaced", "discarded"
    std::string unit;     // a discarded one: the unit whose copy the image has
    std::string folded_into;
    std::string note;
    u32 size = 0;
    std::optional<u32> rva;
    std::optional<u32> first_difference;  // offset in the section
    u32 differing_bytes = 0;
    bool comdat = false;
};

// A unit source's check (decomp units check).
struct RelinkUnitCheck {
    bool complete = false;
    std::string summary;  // "complete", "4 of 12 functions in its source", what does not match
    std::string error;    // why there is no check: no source file, the compile failed
    std::string compile_output;
    usize functions = 0;
    std::vector<u64> missing_functions;  // the unit's functions the source does not put in place
    std::vector<RelinkPlacedSection> sections;
    struct Missing {
        u32 rva = 0, size = 0;
        std::string name, section;
    };
    std::vector<Missing> missing;       // the unit's contributions no section of the object fills
    std::vector<std::string> problems;  // gaps, overlaps, references that disagree

    usize count(std::string_view state) const;
};
RelinkUnitCheck read_unit_check(const Json& check);

struct RelinkUnit {
    std::string unit, kind;
    std::string mode;    // "source", "split", "linker"
    std::string reason;  // why: "complete", "no source", what its check found
    std::string object;  // what was linked, relative to the relink directory
    u64 bytes = 0;       // the size of its contributions
    std::optional<RelinkUnitCheck> check;
};

struct RelinkDifference {
    u32 offset = 0;  // in the target's file
    u32 size = 0;
    std::string where;  // "COFF header TimeDateStamp", ".data+0x0"
    std::optional<u32> rva;
    std::string unit, symbol;
    std::string original, relinked;  // up to 16 bytes from where it starts, hex
};

struct RelinkStamped {
    std::string name;
    u32 offset = 0;  // in the relinked file
    std::string original, relinked;  // hex
};

struct RelinkSectionDifference {
    std::string name;
    u64 differing_bytes = 0;
    std::optional<u32> first_rva;
    std::string first_unit;
    bool size_differs = false;
};

struct RelinkReport {
    std::string time;
    bool identical = false;
    std::vector<RelinkUnit> units;  // link order
    std::vector<std::string> libraries, notes;

    // The linker, and whether it is the one that made the target.
    std::string linker_kind, linker_version, linker_original, linker_text;
    std::optional<bool> same_linker;

    bool link_ok = false;
    int exit_code = -1;
    std::string link_output;
    std::vector<std::string> command;
    i64 link_ms = 0;
    std::string image;  // the relinked image, relative to the project
    std::string error;  // why nothing was compared

    bool compared = false;
    std::string original_sha1, relinked_sha1, relinked_unstamped_sha1;
    u64 original_size = 0, relinked_size = 0, differing_bytes = 0;
    std::vector<RelinkStamped> stamped;
    std::vector<RelinkDifference> differences;  // the first runs, file order
    std::vector<RelinkSectionDifference> sections;
    std::optional<RelinkDifference> first;  // the first difference in section contents

    usize count(std::string_view mode) const;
    // One line: "identical to the target (1 unit from source, 1 split)", "differs from the target: 4
    // bytes, the first at .data+0x0 in basic.obj (g_counter)", "the link failed".
    std::string headline() const;
};
RelinkReport read_relink_report(const Json& result);

// 16 bytes of the target and of the relinked image at the same RVA.
struct HexCompareRow {
    u32 rva = 0;  // of the row's first byte
    std::array<std::optional<u8>, 16> original, relinked;  // nullopt: no data there in that image
    std::array<bool, 16> differs{};
    bool any_difference() const;
};
// `rows` rows of 16 bytes starting `rows_before` rows before the row holding `rva`, but for rows neither
// image has a byte of (before the image, between sections). Bytes are read by RVA in each image, so a
// section the relink placed elsewhere in the file still lines up.
std::vector<HexCompareRow> hex_compare(const pe::Image& original, const pe::Image& relinked, u32 rva, int rows_before = 1, int rows = 4);

// The relinked image as the comparison saw it: the file with the identity fields it took over from the
// target put back (`stamped`'s original values at their offsets).
Result<pe::Image> load_stamped_relink(const std::filesystem::path& file, const std::vector<RelinkStamped>& stamped);

} // namespace decomp::vm
