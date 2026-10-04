#include "arch/x86/decoder.hpp"

#include "core/strings.hpp"

#include <Zydis/Zydis.h>

#include <format>

namespace decomp::x86 {

std::string_view to_string(Flow flow) {
    switch (flow) {
    case Flow::none: return "none";
    case Flow::jump: return "jump";
    case Flow::cond_jump: return "cond_jump";
    case Flow::call: return "call";
    case Flow::ret: return "ret";
    case Flow::indirect_jump: return "indirect_jump";
    case Flow::indirect_call: return "indirect_call";
    case Flow::trap: return "trap";
    case Flow::halt: return "halt";
    }
    return "?";
}

const Field* Instruction::field_at(u8 offset) const {
    for (const auto& f : fields)
        if (f.offset == offset) return &f;
    return nullptr;
}

struct Decoder::Impl {
    ZydisDecoder decoder;
};

Decoder::Decoder(Arch arch) : arch_(arch), impl_(std::make_unique<Impl>()) {
    if (arch == Arch::x64) ZydisDecoderInit(&impl_->decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    else ZydisDecoderInit(&impl_->decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
}

Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

namespace {

std::string reg_name(ZydisRegister reg) {
    if (reg == ZYDIS_REGISTER_NONE) return {};
    const char* s = ZydisRegisterGetString(reg);
    return s ? s : "?";
}

Flow classify(const ZydisDecodedInstruction& ins, const ZydisDecodedOperand* ops) {
    bool direct = ins.operand_count_visible > 0 && ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[0].imm.is_relative;
    switch (ins.meta.category) {
    case ZYDIS_CATEGORY_COND_BR: return Flow::cond_jump;
    case ZYDIS_CATEGORY_UNCOND_BR: return direct ? Flow::jump : Flow::indirect_jump;
    case ZYDIS_CATEGORY_CALL: return direct ? Flow::call : Flow::indirect_call;
    case ZYDIS_CATEGORY_RET: return Flow::ret;
    default: break;
    }
    switch (ins.mnemonic) {
    case ZYDIS_MNEMONIC_INT3:
    case ZYDIS_MNEMONIC_UD2:
    case ZYDIS_MNEMONIC_UD0:
    case ZYDIS_MNEMONIC_UD1: return Flow::trap;
    case ZYDIS_MNEMONIC_HLT: return Flow::halt;
    case ZYDIS_MNEMONIC_IRET:
    case ZYDIS_MNEMONIC_IRETD:
    case ZYDIS_MNEMONIC_IRETQ: return Flow::ret;
    default: return Flow::none;
    }
}

bool segment_is_explicit(const ZydisDecodedInstruction& ins, ZydisRegister seg) {
    if (seg == ZYDIS_REGISTER_FS || seg == ZYDIS_REGISTER_GS) return true;
    return (ins.attributes & ZYDIS_ATTRIB_HAS_SEGMENT) != 0;
}

} // namespace

std::optional<Instruction> Decoder::decode(ByteSpan bytes, u64 address) const {
    ZydisDecodedInstruction zi;
    ZydisDecodedOperand zops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&impl_->decoder, bytes.data(), bytes.size(), &zi, zops))) return std::nullopt;

    Instruction ins;
    ins.address = address;
    ins.length = zi.length;
    for (u8 i = 0; i < zi.length; ++i) ins.bytes[i] = static_cast<u8>(bytes[i]);
    ins.mnemonic = ZydisMnemonicGetString(zi.mnemonic);
    if (zi.attributes & ZYDIS_ATTRIB_HAS_LOCK) ins.prefix = "lock ";
    else if (zi.attributes & ZYDIS_ATTRIB_HAS_REPNE) ins.prefix = "repne ";
    else if (zi.attributes & ZYDIS_ATTRIB_HAS_REPE) ins.prefix = (zi.mnemonic == ZYDIS_MNEMONIC_CMPSB || zi.mnemonic == ZYDIS_MNEMONIC_CMPSD || zi.mnemonic == ZYDIS_MNEMONIC_CMPSW || zi.mnemonic == ZYDIS_MNEMONIC_SCASB || zi.mnemonic == ZYDIS_MNEMONIC_SCASD || zi.mnemonic == ZYDIS_MNEMONIC_SCASW) ? "repe " : "rep ";
    else if (zi.attributes & ZYDIS_ATTRIB_HAS_REP) ins.prefix = "rep ";
    ins.flow = classify(zi, zops);

    int imm_index = 0;
    for (u8 i = 0; i < zi.operand_count_visible; ++i) {
        const auto& zo = zops[i];
        Operand op;
        op.size_bits = zo.size;
        switch (zo.type) {
        case ZYDIS_OPERAND_TYPE_REGISTER:
            op.kind = OperandKind::reg;
            op.reg = reg_name(zo.reg.value);
            break;
        case ZYDIS_OPERAND_TYPE_MEMORY: {
            op.kind = OperandKind::mem;
            op.mem.base = reg_name(zo.mem.base);
            op.mem.index = reg_name(zo.mem.index);
            op.mem.scale = zo.mem.scale;
            op.mem.has_disp = zo.mem.disp.has_displacement;
            op.mem.disp = zo.mem.disp.value;
            if (segment_is_explicit(zi, zo.mem.segment)) op.mem.segment = reg_name(zo.mem.segment);
            if (zo.mem.disp.has_displacement && zi.raw.disp.size > 0) {
                Field f;
                f.offset = zi.raw.disp.offset;
                f.size = static_cast<u8>(zi.raw.disp.size / 8);
                f.kind = FieldKind::disp;
                f.operand = static_cast<i8>(i);
                f.raw = zi.raw.disp.value;
                bool rip = zo.mem.base == ZYDIS_REGISTER_RIP || zo.mem.base == ZYDIS_REGISTER_EIP;
                f.rip_relative = rip;
                if (rip) f.absolute = address + zi.length + static_cast<u64>(zi.raw.disp.value);
                else f.absolute = static_cast<u64>(zi.raw.disp.value) & (arch_ == Arch::x86 ? 0xFFFFFFFFull : ~0ull);
                op.mem.field = static_cast<i8>(ins.fields.size());
                ins.fields.push_back(f);
                bool absolute_mem = rip || (zo.mem.base == ZYDIS_REGISTER_NONE && zo.mem.index == ZYDIS_REGISTER_NONE);
                if (absolute_mem || ins.flow == Flow::indirect_jump || ins.flow == Flow::indirect_call)
                    ins.memory_target = f.absolute;
            }
            break;
        }
        case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
            op.kind = OperandKind::imm;
            op.imm_signed = zo.imm.is_signed;
            op.imm_relative = zo.imm.is_relative;
            op.imm = zo.imm.is_signed ? zo.imm.value.s : static_cast<i64>(zo.imm.value.u);
            if (imm_index < 2 && zi.raw.imm[imm_index].size > 0) {
                const auto& raw = zi.raw.imm[imm_index];
                Field f;
                f.offset = raw.offset;
                f.size = static_cast<u8>(raw.size / 8);
                f.kind = raw.is_relative ? FieldKind::rel : FieldKind::imm;
                f.operand = static_cast<i8>(i);
                f.raw = raw.is_signed ? raw.value.s : static_cast<i64>(raw.value.u);
                if (raw.is_relative) {
                    ZyanU64 target = 0;
                    if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&zi, &zo, address, &target))) f.absolute = target;
                    ins.branch_target = f.absolute;
                } else {
                    u64 mask = zo.size >= 64 ? ~0ull : ((1ull << zo.size) - 1);
                    f.absolute = static_cast<u64>(op.imm) & mask;
                }
                op.field = static_cast<i8>(ins.fields.size());
                ins.fields.push_back(f);
            }
            ++imm_index;
            break;
        }
        case ZYDIS_OPERAND_TYPE_POINTER:
            op.kind = OperandKind::pointer;
            op.ptr_segment = zo.ptr.segment;
            op.ptr_offset = zo.ptr.offset;
            break;
        default: continue;
        }
        ins.operands.push_back(std::move(op));
    }
    return ins;
}

std::vector<Instruction> Decoder::decode_all(ByteSpan bytes, u64 address) const {
    std::vector<Instruction> out;
    usize pos = 0;
    while (pos < bytes.size()) {
        auto ins = decode(bytes.subspan(pos), address + pos);
        if (!ins) {
            Instruction bad;
            bad.address = address + pos;
            bad.length = 1;
            bad.bytes[0] = static_cast<u8>(bytes[pos]);
            bad.mnemonic = "(bad)";
            out.push_back(bad);
            pos += 1;
            continue;
        }
        pos += ins->length;
        out.push_back(std::move(*ins));
    }
    return out;
}

std::string format_hex_signed(i64 value) {
    if (value < 0) return std::format("-0x{:x}", static_cast<u64>(-(value + 1)) + 1);
    return std::format("0x{:x}", static_cast<u64>(value));
}

namespace {

std::string size_keyword(u16 bits) {
    switch (bits) {
    case 8: return "byte";
    case 16: return "word";
    case 32: return "dword";
    case 48: return "fword";
    case 64: return "qword";
    case 80: return "tbyte";
    case 128: return "xmmword";
    case 256: return "ymmword";
    case 512: return "zmmword";
    default: return bits ? std::format("m{}", bits) : std::string();
    }
}

std::optional<std::string> field_text(const Instruction& ins, i8 field, const FieldRenderer& fields) {
    if (field < 0 || !fields) return std::nullopt;
    return fields(ins, ins.fields[static_cast<usize>(field)]);
}

} // namespace

std::string render_operand(const Instruction& ins, const Operand& op, const FieldRenderer& fields) {
    switch (op.kind) {
    case OperandKind::reg: return op.reg;
    case OperandKind::imm: {
        if (auto text = field_text(ins, op.field, fields)) return *text;
        if (op.imm_relative && ins.branch_target) return std::format("0x{:x}", *ins.branch_target);
        if (op.imm_signed) return format_hex_signed(op.imm);
        u64 mask = op.size_bits >= 64 ? ~0ull : ((1ull << op.size_bits) - 1);
        return std::format("0x{:x}", static_cast<u64>(op.imm) & mask);
    }
    case OperandKind::pointer: return std::format("0x{:x}:0x{:x}", op.ptr_segment, op.ptr_offset);
    case OperandKind::mem: {
        std::string out;
        auto kw = size_keyword(op.size_bits);
        if (!kw.empty() && ins.mnemonic != "lea") out = kw + " ptr ";
        if (!op.mem.segment.empty()) out += op.mem.segment + ":";
        out += "[";
        auto disp_text = field_text(ins, op.mem.field, fields);
        const Field* f = op.mem.field >= 0 ? &ins.fields[static_cast<usize>(op.mem.field)] : nullptr;
        bool rip = f && f->rip_relative;
        if (rip) {
            out += disp_text ? *disp_text : std::format("0x{:x}", f->absolute);
        } else {
            bool any = false;
            if (!op.mem.base.empty()) {
                out += op.mem.base;
                any = true;
            }
            if (!op.mem.index.empty()) {
                if (any) out += "+";
                out += op.mem.index;
                if (op.mem.scale > 1) out += std::format("*{}", op.mem.scale);
                any = true;
            }
            if (disp_text) {
                if (any) out += "+";
                out += *disp_text;
            } else if (op.mem.has_disp && (op.mem.disp != 0 || !any)) {
                if (!any) out += std::format("0x{:x}", static_cast<u64>(op.mem.disp) & (f && f->size == 8 ? ~0ull : 0xFFFFFFFFull));
                else if (op.mem.disp < 0) out += format_hex_signed(op.mem.disp);
                else out += "+" + format_hex_signed(op.mem.disp);
            }
        }
        out += "]";
        return out;
    }
    }
    return "?";
}

std::vector<std::string> render_operands(const Instruction& ins, const FieldRenderer& fields) {
    std::vector<std::string> out;
    for (const auto& op : ins.operands) out.push_back(render_operand(ins, op, fields));
    return out;
}

std::string render(const Instruction& ins, const FieldRenderer& fields) {
    std::string out = ins.prefix + ins.mnemonic;
    auto ops = render_operands(ins, fields);
    for (usize i = 0; i < ops.size(); ++i) {
        out += i == 0 ? " " : ", ";
        out += ops[i];
    }
    return out;
}

} // namespace decomp::x86
