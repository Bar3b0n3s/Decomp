#pragma once

// Which registers and stack slots hold pointers to known types while a function runs, so that annotated
// listings can name what the code touches: `[ecx+0Ch]` is `this->health`. `this` and the parameters come
// from the function's PDB type (or, without one, `this` from a member function's decorated name); moves,
// field loads (`mov eax, [ecx+4]` when the field is a pointer), `lea` of embedded structs and bases,
// spills to the stack and reloads carry them, over the control-flow graph. A register holding an
// object's vfptr names the virtual method a call through it reaches.

#include "analysis/cfg.hpp"
#include "analysis/program.hpp"
#include "analysis/types.hpp"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace decomp {

// Layouts by name: the project's headers first (the source of truth), then the target's PDB.
class TypeView {
public:
    TypeView(const TypeCatalog* first, const TypeCatalog* second) : first_(first), second_(second) {}

    const TypeLayout* find(std::string_view name) const;
    // TypeCatalog::field_ref in the catalog that has `type`.
    std::optional<TypeCatalog::FieldRef> field_ref(std::string_view type, u64 offset, bool innermost = true) const;
    bool empty() const { return (!first_ || first_->empty()) && (!second_ || second_->empty()); }

private:
    const TypeCatalog* first_;
    const TypeCatalog* second_;
};

// The stack pointer before each instruction, as an offset from its value at entry (where the return
// address is), and the frame pointer once `mov ebp, esp` sets it up: a straight-line approximation,
// good enough to name stack slots.
struct StackPoints {
    std::vector<i64> sp;
    std::vector<std::optional<i64>> frame;
};
StackPoints stack_points(std::span<const x86::Instruction> instructions, bool x64);

// A value a register or stack slot holds: a pointer to `type`, which C++ calls `expression`.
struct TypedValue {
    std::string type;        // the struct, class or union it points to
    std::string expression;  // "this", "arg_4", "this->link.next"
    bool address = false;    // it holds the address of `expression` (from lea): members follow with '.'
    bool vtable = false;     // it holds the vtable of the object `expression` points to

    bool operator==(const TypedValue&) const = default;
};

// What is known at a point of the function: registers by family (x86::gpr_family) and stack slots by
// their offset from the stack pointer at entry.
struct TypeState {
    std::map<std::string, TypedValue> registers;
    std::map<i64, TypedValue> stack;

    bool operator==(const TypeState&) const = default;
};

// The state at a function's entry: `this` and the pointer parameters, and how they were found.
struct EntryTypes {
    TypeState state;
    std::vector<std::string> description;  // "this = Player* (ecx)", "arg_4 = Node*"
};
EntryTypes entry_types(const Program& program, u64 function, const TypeView& types);

// A field (or virtual method) an instruction reaches through a pointer to `type`.
struct TypedAccess {
    usize instruction = 0;
    std::string type;
    std::string path;      // "hp", "pos.y", "items[eax]"; for a virtual call, the method with "()"
    bool address = false;  // lea: the field's address, not the field
};

// Per instruction, what its memory operands reach through typed pointers ("this->hp",
// "this->link.next->kind", "this->area() (virtual, slot 0)"); with `accesses`, the same as records.
std::vector<std::vector<std::string>> typed_operand_notes(const Program& program, std::span<const x86::Instruction> instructions, const Cfg& cfg,
                                                          const EntryTypes& entry, const TypeView& types,
                                                          std::vector<TypedAccess>* accesses = nullptr);

// Where the program's functions reach the fields of `type` through pointers they are given (their
// `this` and parameters, as entry_types() finds them): the Types view's uses of a type.
struct FieldUse {
    u64 function = 0;
    u64 address = 0;  // the instruction
    std::string path;
    bool address_of = false;
};
std::vector<FieldUse> find_field_uses(const Program& program, const TypeView& types, std::string_view type,
                                      const std::function<bool()>& cancelled = {});

} // namespace decomp
