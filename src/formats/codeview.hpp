#pragma once

// CodeView type records (https://llvm.org/docs/PDB/CodeViewTypes.html). A PDB's TPI stream and an object's
// .debug$T section (compiled with /Z7) hold the same records: what the compiler made of every type the
// code uses. TypeStream indexes them and decodes the records that describe layouts; analysis/types.hpp
// turns those into TypeLayouts.

#include "core/result.hpp"
#include "core/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace decomp::codeview {

using TypeIndex = u32;
inline constexpr TypeIndex kFirstTypeIndex = 0x1000;

// The leaf kinds read here (the 32-bit type index forms). Visual C++ 7.0 and 7.1 wrote the _st forms
// (length-prefixed names); decoding normalizes them to the forms with zero-terminated names.
namespace leaf {
inline constexpr u16 vtshape = 0x000a;
inline constexpr u16 modifier = 0x1001;
inline constexpr u16 pointer = 0x1002;
inline constexpr u16 procedure = 0x1008;
inline constexpr u16 mfunction = 0x1009;
inline constexpr u16 arglist = 0x1201;
inline constexpr u16 fieldlist = 0x1203;
inline constexpr u16 bitfield = 0x1205;
inline constexpr u16 methodlist = 0x1206;
inline constexpr u16 bclass = 0x1400;
inline constexpr u16 vbclass = 0x1401;
inline constexpr u16 ivbclass = 0x1402;
inline constexpr u16 index = 0x1404;
inline constexpr u16 vfunctab = 0x1409;
inline constexpr u16 friendcls = 0x140a;
inline constexpr u16 vfuncoff = 0x140c;
inline constexpr u16 enumerate = 0x1502;
inline constexpr u16 array = 0x1503;
inline constexpr u16 class_ = 0x1504;
inline constexpr u16 structure = 0x1505;
inline constexpr u16 union_ = 0x1506;
inline constexpr u16 enum_ = 0x1507;
inline constexpr u16 friendfcn = 0x150c;
inline constexpr u16 member = 0x150d;
inline constexpr u16 stmember = 0x150e;
inline constexpr u16 method = 0x150f;
inline constexpr u16 nesttype = 0x1510;
inline constexpr u16 onemethod = 0x1511;
inline constexpr u16 nesttypeex = 0x1512;
inline constexpr u16 interface = 0x1519;

inline constexpr u16 enumerate_st = 0x0403;
inline constexpr u16 array_st = 0x1003;
inline constexpr u16 class_st = 0x1004;
inline constexpr u16 structure_st = 0x1005;
inline constexpr u16 union_st = 0x1006;
inline constexpr u16 enum_st = 0x1007;
inline constexpr u16 friendfcn_st = 0x1403;
inline constexpr u16 member_st = 0x1405;
inline constexpr u16 stmember_st = 0x1406;
inline constexpr u16 method_st = 0x1407;
inline constexpr u16 nesttype_st = 0x1408;
inline constexpr u16 onemethod_st = 0x140b;
inline constexpr u16 nesttypeex_st = 0x140d;

// Item (IPI) records: what S_GPROC32_ID and S_LPROC32_ID name.
inline constexpr u16 func_id = 0x1601;
inline constexpr u16 mfunc_id = 0x1602;
} // namespace leaf

// A struct, class, interface, union or enum record.
struct Udt {
    u16 leaf = 0;  // normalized: leaf::class_, structure, interface, union_ or enum_
    u16 count = 0;     // members in the field list
    u16 property = 0;  // CV_prop_t
    TypeIndex field_list = 0;
    TypeIndex derived = 0;
    TypeIndex vshape = 0;      // classes: the LF_VTSHAPE of their virtual function table
    TypeIndex underlying = 0;  // enums
    u64 size = 0;              // bytes; enums: 0 (the underlying type's)
    std::string name, unique_name;

    bool forward_ref() const { return (property & 0x80) != 0; }
    bool is_enum() const { return leaf == leaf::enum_; }
    bool is_union() const { return leaf == leaf::union_; }
};

// One entry of a field list.
struct Member {
    u16 leaf = 0;     // leaf::member, bclass, vbclass, ivbclass, vfunctab, stmember, onemethod, method, enumerate, nesttype, ...
    u16 attribute = 0;  // CV_fldattr_t: access in bits 0-1, the method property in bits 2-4
    TypeIndex type = 0;  // the member's type; bases: the base class; vfunctab: the vfptr's type; method: the method list
    i64 offset = 0;      // members and bases: byte offset; enumerators: the value; virtual bases: the vbptr offset
    u32 vtable_offset = 0;  // introducing virtual methods: their byte offset in the vtable
    u16 method_count = 0;   // overloaded methods (leaf::method)
    std::string name;

    // The method property (CV_methodprop_e): 1 virtual, 4 introducing virtual, 5 pure virtual, 6 pure introducing.
    u8 method_property() const { return static_cast<u8>((attribute >> 2) & 7); }
    bool introducing_virtual() const { return method_property() == 4 || method_property() == 6; }
    bool is_virtual() const { return method_property() == 1 || method_property() == 5 || introducing_virtual(); }
};

// An LF_METHODLIST entry: one overload.
struct MethodEntry {
    u16 attribute = 0;
    TypeIndex type = 0;
    u32 vtable_offset = 0;
    bool introducing_virtual() const { return ((attribute >> 2) & 7) == 4 || ((attribute >> 2) & 7) == 6; }
    bool is_virtual() const { const u8 p = (attribute >> 2) & 7; return p == 1 || p == 4 || p == 5 || p == 6; }
};

struct Pointer {
    TypeIndex referent = 0;
    u8 mode = 0;  // 0 pointer, 1 lvalue reference, 2 pointer to data member, 3 pointer to member function, 4 rvalue reference
    u8 size = 0;  // bytes
    bool is_const = false, is_volatile = false;
};

struct Array {
    TypeIndex element = 0;
    u64 size = 0;  // bytes, all elements
};

struct Bitfield {
    TypeIndex type = 0;  // the storage unit's type
    u8 length = 0, position = 0;
};

struct Function {
    TypeIndex return_type = 0;
    TypeIndex class_type = 0;  // member functions
    TypeIndex this_type = 0;   // member functions that are not static
    u8 calling_convention = 0;
    std::vector<TypeIndex> parameters;  // without `this`
};

class TypeStream {
public:
    struct Record {
        u16 leaf = 0;
        std::span<const std::byte> data;  // after the leaf kind
    };

    // `records`: a type stream's records, the first one with index `first` (a PDB's TPI stream after its
    // header).
    static Result<TypeStream> parse(std::vector<std::byte> records, TypeIndex first = kFirstTypeIndex);
    // An object's .debug$T section: the signature (4, C13), then the records.
    static Result<TypeStream> from_debug_t(std::span<const std::byte> section);

    TypeIndex first() const { return first_; }
    TypeIndex end() const { return first_ + static_cast<TypeIndex>(records_.size()); }
    std::optional<Record> record(TypeIndex index) const;

    // Decoded records; nullopt when `index` is not a record of that kind (or is malformed).
    std::optional<Udt> udt(TypeIndex index) const;
    std::optional<Pointer> pointer(TypeIndex index) const;
    std::optional<Function> function(TypeIndex index) const;
    std::optional<Array> array(TypeIndex index) const;
    std::optional<Bitfield> bitfield(TypeIndex index) const;
    // A field list with its continuations (LF_INDEX) followed.
    Result<std::vector<Member>> field_list(TypeIndex index) const;
    Result<std::vector<MethodEntry>> method_list(TypeIndex index, u16 count) const;
    // The number of entries in an LF_VTSHAPE.
    std::optional<u16> vtshape_count(TypeIndex index) const;
    // An item stream's LF_FUNC_ID or LF_MFUNC_ID: the function type it names (in the type stream).
    std::optional<TypeIndex> function_type_of_id(TypeIndex index) const;

    // The definition of a struct, class, union or enum: `index` itself, or the record a forward reference
    // stands for (by unique name, else by name). nullopt when none is defined in the stream.
    std::optional<TypeIndex> definition(TypeIndex index) const;
    std::optional<TypeIndex> find_definition(std::string_view name) const;  // by name
    // Every defined struct, class, union and enum, in index order (forward references left out).
    std::vector<TypeIndex> definitions() const;

    // The type an LF_MODIFIER (const, volatile) modifies, through all of them; other types themselves.
    TypeIndex unmodified(TypeIndex index) const;
    // A type's size in bytes for a target with `pointer_size`-byte pointers (0 when unknown).
    u64 size_of(TypeIndex index, usize pointer_size) const;
    // A type as C++ writes it: "int", "const char*", "Player*", "char[16]", "void (__cdecl*)(int)".
    std::string name_of(TypeIndex index) const;
    // The name a struct, class, union or enum goes by in a TypeCatalog: its name; an anonymous one
    // ("<unnamed-tag>") its unique name, or without one "<unnamed-tag>@<index of its definition>".
    std::string key_of(TypeIndex index) const;
    // The struct, class or union a type is (through modifiers and arrays), or points to (one level), by
    // key_of; else "".
    std::string udt_name(TypeIndex index) const;
    std::string pointee_udt(TypeIndex index) const;

private:
    std::vector<std::byte> bytes_;
    std::vector<std::pair<u32, u32>> records_;  // (offset of the leaf kind in bytes_, record length after its length field)
    TypeIndex first_ = kFirstTypeIndex;
    std::unordered_map<std::string, TypeIndex> by_unique_name_, by_name_;  // definitions
};

// A simple (built-in) type's name, size and signedness: indices below 0x1000.
std::string simple_type_name(TypeIndex index);
u64 simple_type_size(TypeIndex index, usize pointer_size);
bool simple_type_signed(TypeIndex index);

} // namespace decomp::codeview
