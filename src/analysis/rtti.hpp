#pragma once

// MSVC run-time type information (/GR) in an image: the classes it names, their bases and their
// vftables (docs/architecture.md#rtti-and-vftables).
//
// A TypeDescriptor holds a class's decorated name (".?AVFoo@@" for a class, ".?AUFoo@@" for a struct).
// A CompleteObjectLocator (COL) ties one vftable to its class and to the class's hierarchy: a
// ClassHierarchyDescriptor, its BaseClassArray and a BaseClassDescriptor per base. The pointer-sized
// slot right before a vftable points at its COL. On x86 the references are addresses; on x64 they are
// image-relative, and a COL also holds its own RVA.

#include "analysis/symbols.hpp"
#include "formats/pe.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace decomp {

struct RttiBase {
    std::string decorated;  // the class part of decorated names: "Square@game@@"
    std::string name;       // "game::Square"
    u64 descriptor = 0;     // the BaseClassDescriptor
    u32 contained = 0;      // bases below it in the array (its own bases)
    i32 mdisp = 0;          // offset of the base in the class, or in the virtual base
    i32 pdisp = -1;         // offset of the vbtable pointer (-1: not a virtual base)
    i32 vdisp = 0;          // offset of the base's entry in the vbtable
    u32 attributes = 0;
    bool direct = false;    // a direct base of the class
};

struct RttiVftable {
    u64 va = 0;               // the first slot
    u64 locator = 0;          // its COL
    u32 offset = 0;           // of the vftable pointer in the complete object
    u32 constructor_displacement = 0;
    std::string for_base;     // the base it serves when the class has several ("Named@@"); else empty
    std::vector<u64> slots;   // the virtual functions, in slot order
};

struct RttiClass {
    std::string decorated;  // "Unit@@", "Square@game@@"
    std::string name;       // "Unit", "game::Square"
    bool is_struct = false;
    u64 type_descriptor = 0;
    u64 hierarchy = 0;  // the ClassHierarchyDescriptor (0 when no vftable of the class refers to it)
    u32 attributes = 0; // 1: multiple inheritance, 2: virtual inheritance
    u64 base_array = 0;
    RttiBase self;                // the class's own BaseClassDescriptor (first in the array)
    std::vector<RttiBase> bases;  // in hierarchy order, the class itself excluded
    std::vector<RttiVftable> vftables;

    // Decorated names of the RTTI structures and vftables, as MSVC writes them.
    std::string type_descriptor_name() const;            // ??_R0?AVUnit@@@8
    std::string vftable_name(const RttiVftable& v) const;  // ??_7Unit@@6B@, ??_7Unit@@6BNamed@@@
    std::string locator_name(const RttiVftable& v) const;  // ??_R4Unit@@6B@
    std::string hierarchy_name() const;                   // ??_R3Unit@@8
    std::string base_array_name() const;                  // ??_R2Unit@@8
};

// ??_R1A@?0A@EA@Unit@@8: a BaseClassDescriptor's name encodes its displacements and attributes.
std::string base_descriptor_name(const RttiBase& base);

struct VirtualSlot {
    usize class_index = 0;
    usize vftable_index = 0;
    usize slot = 0;
};

struct RttiInfo {
    std::vector<RttiClass> classes;  // by name
    std::multimap<u64, VirtualSlot> slots;  // function -> where vftables hold it

    const RttiClass* find(std::string_view name) const;  // by readable or decorated name
    // "slot 1 of Unit's vftable for game::Square" for every vftable slot that holds `function`.
    std::vector<std::string> describe_slots(u64 function) const;
};

RttiInfo find_rtti(const pe::Image& image);

// Names the RTTI structures and vftables (source analysis, so a PDB's or a map's names win): ??_R0
// TypeDescriptors, ??_R4 locators, ??_R3 hierarchies, ??_R2 base arrays, ??_R1 base descriptors and
// ??_7 vftables.
void add_rtti_symbols(SymbolDb& symbols, const RttiInfo& rtti, Arch arch);

// The class part of a decorated name to a readable one: "Square@game@@" -> "game::Square".
std::string class_display_name(std::string_view decorated);
// MSVC's encoding of a number in decorated names: 0 -> "A@", 4 -> "3", -1 -> "?0", 0x40 -> "EA@".
std::string encode_ms_number(i64 value);

} // namespace decomp
