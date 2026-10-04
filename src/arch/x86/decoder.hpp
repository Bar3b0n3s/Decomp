#pragma once

#include "core/bytes.hpp"
#include "formats/image.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace decomp::x86 {

enum class Flow : u8 { none, jump, cond_jump, call, ret, indirect_jump, indirect_call, trap, halt };

std::string_view to_string(Flow flow);
inline bool ends_block(Flow f) { return f == Flow::jump || f == Flow::ret || f == Flow::indirect_jump || f == Flow::trap || f == Flow::halt; }

enum class OperandKind : u8 { reg, mem, imm, pointer };

// A byte range of the instruction encoding that holds a displacement or an immediate.
enum class FieldKind : u8 { disp, imm, rel };

struct Field {
    u8 offset = 0;  // byte offset within the instruction
    u8 size = 0;    // in bytes
    FieldKind kind = FieldKind::disp;
    i8 operand = -1;  // index into Instruction::operands
    i64 raw = 0;      // value as encoded (sign-extended for signed fields)
    // disp/imm: the raw value as an address; rel: absolute branch target; rip-relative disp: absolute target.
    u64 absolute = 0;
    bool rip_relative = false;
};

struct MemOperand {
    std::string segment;  // empty unless an override or fs/gs
    std::string base, index;
    u8 scale = 0;
    i64 disp = 0;
    bool has_disp = false;
    i8 field = -1;  // index into Instruction::fields for the displacement
};

struct Operand {
    OperandKind kind = OperandKind::reg;
    u16 size_bits = 0;
    std::string reg;  // reg operands
    MemOperand mem;   // mem operands
    i64 imm = 0;      // imm operands
    bool imm_signed = false;
    bool imm_relative = false;
    i8 field = -1;  // imm operands
    u16 ptr_segment = 0;
    u32 ptr_offset = 0;
};

struct Instruction {
    u64 address = 0;
    u8 length = 0;
    std::array<u8, 15> bytes{};
    std::string mnemonic;  // e.g. "mov", "jz"; prefixes (lock/rep) are separate
    std::string prefix;    // "lock ", "rep ", "repne " ...
    std::vector<Operand> operands;  // visible operands only
    std::vector<Field> fields;
    Flow flow = Flow::none;
    std::optional<u64> branch_target;  // direct jumps/calls
    std::optional<u64> memory_target;  // absolute memory operand / rip-relative / table base for indirect jumps

    // Field whose byte range starts at `offset`, if any.
    const Field* field_at(u8 offset) const;
    u64 end() const { return address + length; }
};

// Supplies replacement text for an address-bearing field while rendering; nullopt keeps the number.
using FieldRenderer = std::function<std::optional<std::string>(const Instruction&, const Field&)>;

class Decoder {
public:
    explicit Decoder(Arch arch);
    ~Decoder();
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;

    Arch arch() const { return arch_; }

    // Decodes one instruction at `address` from `bytes` (nullopt on invalid encoding).
    std::optional<Instruction> decode(ByteSpan bytes, u64 address) const;

    // Linear sweep over [address, address + bytes.size()); invalid bytes become 1-byte "(bad)" entries.
    std::vector<Instruction> decode_all(ByteSpan bytes, u64 address) const;

private:
    struct Impl;
    Arch arch_;
    std::unique_ptr<Impl> impl_;
};

// Intel-syntax rendering. Memory operands always carry a size ("dword ptr"); numbers are hex.
std::string render_operand(const Instruction& ins, const Operand& op, const FieldRenderer& fields = {});
std::string render(const Instruction& ins, const FieldRenderer& fields = {});
std::vector<std::string> render_operands(const Instruction& ins, const FieldRenderer& fields = {});

std::string format_hex_signed(i64 value);

} // namespace decomp::x86
