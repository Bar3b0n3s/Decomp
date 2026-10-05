#pragma once

// C++ declarations of types from their layouts (analysis/types.hpp): what `decomp types import` writes
// into a project header so that the compiler, reading it, lays the types out as the target's PDB says.
// The declarations are checked by compiling them and comparing the layouts read back
// (project/types.hpp), so what they cannot express shows as a difference rather than a silent change.

#include "analysis/types.hpp"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace decomp {

struct TypeDeclaration {
    std::string name;  // the type's name ("game::Shape")
    // The declaration as a header holds it: in its namespaces, with #pragma pack around it when its
    // fields are packed tighter than their alignment.
    std::string text;
    bool definition = true;  // false: a forward declaration ("struct Entity;")
};

struct DeclarationPlan {
    std::vector<TypeDeclaration> declarations;  // forward declarations first, then definitions in dependency order
    std::vector<std::string> skipped;           // what could not be declared, and why
    std::set<std::string> existing_used;        // the types of `existing` the declarations use
};

// The declarations that reproduce the layouts of `names` from `catalog`: each type after the types it
// needs defined first (its bases, the types of its fields by value, the enums its fields and methods
// use), and forward declarations of the structs, classes and unions it only points to. A type nested in
// a class is declared in that class's definition. Types in `existing` (what the project's headers
// declare already) are neither defined nor declared again. Templates are skipped.
DeclarationPlan declare_types(const TypeCatalog& catalog, const std::vector<std::string>& names, const std::set<std::string>& existing);

// One type's definition: fields (flattened anonymous unions and structs rebuilt from their offsets,
// anonymous member types inline, bitfields with unnamed padding where bits are skipped), static members,
// methods with their signatures (virtual ones in slot order), the types nested in it; for an enum, its
// enumerators with their values. Without namespaces or #pragma pack.
std::string definition_of(const TypeLayout& layout, const TypeCatalog& catalog);

// The type's natural alignment: the largest of its fields', bases' and table pointers' (1 when empty).
u64 alignment_of(const TypeLayout& layout, const TypeCatalog& catalog);

// The #pragma pack value the type's field offsets need (0 when natural alignment produces them).
u64 packing_of(const TypeLayout& layout, const TypeCatalog& catalog);

// Whether a name is an anonymous type's ("<unnamed-tag>", "Entity::<unnamed-type-pair>", ".?AU<unnamed-...").
bool anonymous_type_name(std::string_view name);

} // namespace decomp
