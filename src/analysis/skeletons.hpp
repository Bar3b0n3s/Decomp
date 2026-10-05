#pragma once

// Class skeletons from MSVC run-time type information (analysis/rtti.hpp), for targets without a PDB:
// what RTTI and the symbols say of a class, as layouts that analysis/declarations.hpp writes as C++.
//
// - Bases at their offsets (direct ones, in offset order; virtual ones), and the vfptr a class adds:
//   one when it has a vftable at offset 0 and no polymorphic base there.
// - The virtual methods it introduces, the slots of its primary vftable past its primary base's: named
//   after the functions in them, with the signatures their decorated names give (`virtual int area()
//   const;`), a destructor for a deleting destructor, `vf<slot>` for a function without a name, `= 0`
//   for _purecall. Overrides are declared where the base's slot has the same method.
// - The other member functions the symbols name (constructors, the destructor, methods).
// - Fields are unknown; a class that another base follows gets a `char` array that fills it to the next
//   base's offset, so that the bases land where RTTI says.
//
// Compiled, a skeleton's vtables and base offsets can be compared with the RTTI's.

#include "analysis/program.hpp"
#include "analysis/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp {

// The skeletons of every class the program's RTTI names, by name.
TypeCatalog rtti_skeletons(const Program& program);

// How a compiled class differs from what RTTI says of it: its primary vtable's entry count, its direct
// bases and their offsets, the entry counts of the vtables it has for its other bases.
std::vector<std::string> compare_with_rtti(const RttiClass& rtti, const TypeCatalog& compiled);

// A member function's declaration from its decorated name: "?area@Shape@game@@UBEHXZ" -> area, int,
// (), const, virtual. nullopt when the name does not demangle to a member function of `class_name`.
std::optional<MethodLayout> method_from_decorated(std::string_view decorated, std::string_view class_name, Arch arch);

} // namespace decomp
