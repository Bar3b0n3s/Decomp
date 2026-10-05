#pragma once

// Type layouts (docs/architecture.md#types): what a compiler made of a struct, class, union or
// enum, read from CodeView type records: the target's PDB, or the project's headers compiled with debug
// information. Layouts name the fields annotated listings show, get_type returns them, and
// `decomp types check` compares the two sources.

#include "core/json.hpp"
#include "core/result.hpp"
#include "core/types.hpp"
#include "formats/codeview.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace decomp {

enum class TypeKind : u8 { structure, class_, union_, enumeration };
std::string_view to_string(TypeKind kind);  // "struct", "class", "union", "enum"

struct FieldLayout {
    std::string name;
    u64 offset = 0;                           // bytes from the start of the type
    u64 size = 0;                             // bytes; a bitfield's storage unit
    std::optional<u8> bit_offset, bit_width;  // bitfields
    std::string type;                         // as C++ writes it: "int", "Player*", "char[16]"
    std::string udt;                          // the struct, class or union it is (or is an array of), else ""
    std::vector<u64> dimensions;              // arrays: each dimension's extent, outermost first ("int[2][3]": 2, 3)
    std::string pointee;                      // the struct, class or union it points to, else ""
    std::vector<std::string> named;           // the structs, classes, unions and enums its type names, at any depth

    bool is_bitfield() const { return bit_width.has_value(); }
};

struct BaseLayout {
    std::string name;
    u64 offset = 0;  // non-virtual bases (a virtual base's place depends on the most derived class)
    bool is_virtual = false;
};

struct VirtualMethod {
    std::string name;
    std::optional<u64> slot;  // the methods a class introduces: their vtable entry; overriders: none
    bool pure = false;
};

// A method a class declares (not the ones the compiler generates), with its signature.
struct MethodLayout {
    std::string name;
    std::string return_type;              // as C++ writes it ("void" for constructors and destructors)
    std::vector<std::string> parameters;  // their types; "..." for a variable argument list
    std::string calling_convention;       // when not the default (__thiscall on x86, __cdecl for static methods)
    bool is_static = false;
    bool is_const = false;
    bool is_virtual = false;
    bool pure = false;
    std::optional<u64> slot;          // a virtual method the class introduces: its vtable entry
    std::vector<std::string> named;   // the structs, classes, unions and enums its signature names
};

struct StaticMember {
    std::string name;
    std::string type;
    std::vector<std::string> named;
};

// A type declared in a class: a nested struct, class, union or enum, or a typedef.
struct NestedType {
    std::string name;         // as the class names it
    std::string type;         // a nested type's name in the catalog; a typedef's type as C++ writes it
    bool is_typedef = false;  // a typedef (or a nested type of another class)
};

struct Enumerator {
    std::string name;
    i64 value = 0;
};

struct TypeLayout {
    std::string name;
    TypeKind kind = TypeKind::structure;
    u64 size = 0;
    std::vector<BaseLayout> bases;        // direct bases, in declaration order
    std::optional<u64> vfptr;             // the virtual function table pointer it adds
    std::optional<u64> vbptr;             // the virtual base table pointer it adds
    u64 vtable_slots = 0;                 // entries in its (primary) virtual function table
    std::vector<VirtualMethod> virtuals;  // introduced (in slot order), then overriding
    std::vector<FieldLayout> fields;      // its own non-static data members, in declaration order
    std::string underlying;               // enums: the underlying type
    std::vector<Enumerator> enumerators;  // enums
    // What its declaration holds besides the layout (for declarations the compiler lays out the same).
    std::vector<MethodLayout> methods;  // in declaration order
    std::vector<StaticMember> statics;
    std::vector<NestedType> nested;

    // The field holding byte `offset` (of bitfields sharing a unit and of a union's members, the first),
    // or null.
    const FieldLayout* field_at(u64 offset) const;
};

// The layout of the struct, class, union or enum at `index` (its definition, for a forward reference).
Result<TypeLayout> layout_of(const codeview::TypeStream& types, codeview::TypeIndex index, usize pointer_size);

// Type layouts by name (codeview::TypeStream::key_of).
class TypeCatalog {
public:
    explicit TypeCatalog(usize pointer_size = 4) : pointer_size_(pointer_size) {}
    // Every struct, class, union and enum the stream defines. Types defined more than once (in several
    // objects of a PDB) keep their first definition.
    static TypeCatalog from(const codeview::TypeStream& types, usize pointer_size);

    void add(TypeLayout layout);  // a type of that name already there stays
    const TypeLayout* find(std::string_view name) const;
    const std::vector<TypeLayout>& types() const { return types_; }
    bool empty() const { return types_.empty(); }
    usize pointer_size() const { return pointer_size_; }

    // How C++ names byte `offset` of `type`: "hp", "pos.x", "items[2].count", "grid[1][2]" (through
    // nested structs and arrays), a base's field by its own name, "__vfptr" and "__vbptr" (a base's other
    // than the first: "Named::__vfptr"). `field` is the innermost field reached (null for the table
    // pointers; valid until the catalog changes), `exact` whether `offset` is where it starts. nullopt
    // when the type is unknown or nothing holds that byte.
    struct FieldRef {
        std::string path;
        const FieldLayout* field = nullptr;
        bool exact = true;
    };
    std::optional<FieldRef> field_ref(std::string_view type, u64 offset) const;

private:
    std::optional<FieldRef> resolve(const TypeLayout& layout, u64 offset, int depth) const;

    usize pointer_size_ = 4;
    std::vector<TypeLayout> types_;
    std::unordered_map<std::string, usize> by_name_;
};

// A target's types, from its PDB: the type records, the layouts of the types they define, and the type
// (LF_PROCEDURE, LF_MFUNCTION) of each function the PDB lists, by address.
struct ProgramTypes {
    codeview::TypeStream stream;
    TypeCatalog catalog;
    std::unordered_map<u64, codeview::TypeIndex> function_types;
    std::unordered_map<std::string, std::string> sources;  // type name -> the file it was defined in, where known
};

// Whether a path is a compiler's, an SDK's or a library's header (Visual Studio, the Windows SDKs, the
// Platform and DirectX SDKs, clang's): where the types a program only uses come from.
bool system_header(std::string_view path);

// The structs, classes and unions a function's PDB type names: its class (`this`), then what its
// parameters and return value are or point to, without repeats. Empty when the PDB does not type it.
std::vector<std::string> types_of_function(const ProgramTypes& types, u64 va);

// The differences of `actual` from `expected`, one line each ("size 12, expected 8", "field speed at
// +0x8, expected +0x4", "virtual slot 2: Extra, expected Update"). Empty when the layouts agree in kind,
// size, bases, table pointers, virtual methods, fields (names, offsets, sizes, bits and types) and
// enumerators.
std::vector<std::string> compare_layouts(const TypeLayout& actual, const TypeLayout& expected);

// A field as C++ declares it: "int hp", "char name[16]", "unsigned int alive : 1", "void (__cdecl* cb)(int)".
std::string field_declaration(const FieldLayout& field);

// The layout as text, one line per base, table pointer and field with its offset (get_type, `decomp types
// show`).
std::string to_text(const TypeLayout& layout);
Json to_json(const TypeLayout& layout);

} // namespace decomp
