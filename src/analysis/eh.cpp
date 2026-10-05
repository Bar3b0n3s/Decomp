#include "analysis/eh.hpp"

#include "analysis/demangle.hpp"

#include <algorithm>
#include <format>

namespace decomp {

namespace {

bool code_at(const BinaryImage& image, u64 va) {
    const ImageSection* s = image.section_at(va);
    return s && s->executable && va < s->va + s->file_size;
}

// Tables are file-backed data.
bool data_at(const BinaryImage& image, u64 va, u64 size) {
    const ImageSection* s = image.section_at(va);
    return s && !s->executable && va >= s->va && va + size <= s->va + s->file_size;
}

u32 read32(const BinaryImage& image, u64 va) { return image.read<u32>(va).value_or(0); }

// An address a FuncInfo holds: absolute on x86, image-relative on x64.
u64 address_of(const BinaryImage& image, u32 raw) {
    if (image.arch() == Arch::x86) return raw;
    return raw ? image.image_base() + raw : 0;
}

void sort_unique(std::vector<u64>& v) {
    std::ranges::sort(v);
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

} // namespace

std::optional<CxxFuncInfo> read_cxx_funcinfo(const BinaryImage& image, u64 va) {
    if (!data_at(image, va, 20)) return std::nullopt;
    CxxFuncInfo fi;
    fi.va = va;
    fi.magic = read32(image, va);
    if (fi.magic != kFuncInfoMagic1 && fi.magic != kFuncInfoMagic2 && fi.magic != kFuncInfoMagic3) return std::nullopt;
    fi.max_state = static_cast<i32>(read32(image, va + 4));
    const u64 unwind_map = address_of(image, read32(image, va + 8));
    fi.try_blocks = read32(image, va + 12);
    const u64 try_map = address_of(image, read32(image, va + 16));
    if (fi.max_state < 0 || fi.max_state > 0x10000 || fi.try_blocks > 0x1000) return std::nullopt;
    if (fi.max_state > 0 && !data_at(image, unwind_map, static_cast<u64>(fi.max_state) * 8)) return std::nullopt;
    if (fi.try_blocks > 0 && !data_at(image, try_map, u64{fi.try_blocks} * 20)) return std::nullopt;

    // UnwindMapEntry: the state to go to, and the code that leaves this one (0: nothing to destroy).
    for (i32 s = 0; s < fi.max_state; ++s) {
        const u64 entry = unwind_map + 8 * static_cast<u64>(s);
        const i32 to = static_cast<i32>(read32(image, entry));
        const u64 action = address_of(image, read32(image, entry + 4));
        if (to < -1 || to >= fi.max_state) return std::nullopt;
        if (!action) continue;
        if (!code_at(image, action)) return std::nullopt;
        fi.unwind_actions.push_back(action);
    }
    // TryBlockMapEntry: the states the try block and its handlers span, and its HandlerType array
    // (adjectives, the caught type, where the object goes, the handler's code; x64 adds the frame).
    const u64 handler_size = image.arch() == Arch::x86 ? 16 : 20;
    for (u32 t = 0; t < fi.try_blocks; ++t) {
        const u64 entry = try_map + 20 * u64{t};
        const i32 low = static_cast<i32>(read32(image, entry));
        const i32 high = static_cast<i32>(read32(image, entry + 4));
        const i32 catch_high = static_cast<i32>(read32(image, entry + 8));
        const u32 catches = read32(image, entry + 12);
        const u64 handlers = address_of(image, read32(image, entry + 16));
        if (low < 0 || high < low || catch_high < high || catch_high >= fi.max_state || catches == 0 || catches > 256) return std::nullopt;
        if (!data_at(image, handlers, handler_size * catches)) return std::nullopt;
        for (u32 c = 0; c < catches; ++c) {
            const u64 h = handlers + handler_size * c;
            CatchHandler handler;
            handler.try_block = t;
            handler.adjectives = read32(image, h);
            handler.type = address_of(image, read32(image, h + 4));
            handler.code = address_of(image, read32(image, h + 12));
            if ((handler.type && !image.contains(handler.type)) || !code_at(image, handler.code)) return std::nullopt;
            fi.catch_blocks.push_back(handler.code);
            fi.handlers.push_back(handler);
        }
    }
    sort_unique(fi.unwind_actions);
    sort_unique(fi.catch_blocks);
    return fi;
}

std::optional<u64> cxx_stub_funcinfo(const BinaryImage& image, const x86::Decoder& decoder, u64 stub) {
    const ImageSection* s = image.section_at(stub);
    if (!s || !s->executable) return std::nullopt;
    const u64 end = s->va + s->file_size;
    u64 at = stub;
    for (int n = 0; n < 12 && at < end; ++n) {
        auto bytes = image.view(at, static_cast<usize>(std::min<u64>(15, end - at)));
        if (!bytes) return std::nullopt;
        auto ins = decoder.decode(*bytes, at);
        if (!ins) return std::nullopt;
        if (ins->mnemonic == "mov" && ins->operands.size() == 2 && ins->operands[0].kind == x86::OperandKind::reg &&
            x86::gpr_family(ins->operands[0].reg) == "rax" && ins->operands[1].kind == x86::OperandKind::imm) {
            const u64 info = static_cast<u32>(ins->operands[1].imm);
            if (read_cxx_funcinfo(image, info)) return info;
        }
        if (x86::ends_block(ins->flow)) return std::nullopt;
        at += ins->length;
    }
    return std::nullopt;
}

std::string catch_clause(const BinaryImage& image, const CatchHandler& handler) {
    if (!handler.type || (handler.adjectives & 0x40)) return "catch (...)";
    // The TypeDescriptor's name (".?AUFailure@@", ".H") is its symbol's (??_R0?AUFailure@@@8) without
    // the prefix and suffix.
    std::string type = "?";
    if (const auto name = image.read_cstring(handler.type + 2 * pointer_size(image.arch()), 512); name && name->starts_with('.')) {
        type = display_name("??_R0" + name->substr(1) + "@8");
        if (const auto tag = type.find(" `RTTI Type Descriptor'"); tag != std::string::npos) type.erase(tag);
        for (std::string_view kind : {"struct ", "class ", "union ", "enum "})
            if (type.starts_with(kind)) type.erase(0, kind.size());
    }
    if (type.ends_with(" *")) type.erase(type.size() - 2, 1);  // "char *" -> "char*"
    return std::format("catch ({}{}{}{})", handler.adjectives & 1 ? "const " : "", handler.adjectives & 2 ? "volatile " : "", type,
                       handler.adjectives & 8 ? "&" : "");
}

bool registers_seh_frame(std::span<const x86::Instruction> code, u64 table) {
    for (usize i = 0; i < code.size(); ++i) {
        for (const auto& op : code[i].operands)
            if (op.kind == x86::OperandKind::mem && op.mem.segment == "fs" && op.mem.base.empty() && op.mem.index.empty() && op.mem.disp == 0)
                return true;
        if (code[i].mnemonic == "push" && !code[i].operands.empty() && code[i].operands[0].kind == x86::OperandKind::imm &&
            static_cast<u32>(code[i].operands[0].imm) == table && i + 1 < code.size() && code[i + 1].flow == x86::Flow::call)
            return true;
    }
    return false;
}

FunctionEh function_eh(const BinaryImage& image, const x86::Decoder& decoder, std::span<const x86::Instruction> code) {
    FunctionEh out;
    if (image.arch() != Arch::x86) return out;
    for (const auto& ins : code)
        for (const auto& f : ins.fields) {
            if (f.kind != x86::FieldKind::imm || f.size != 4) continue;
            const u64 value = static_cast<u32>(f.raw);
            if (!out.cxx && image.is_code(value)) {
                if (const auto info = cxx_stub_funcinfo(image, decoder, value)) {
                    out.cxx = read_cxx_funcinfo(image, *info);
                    if (out.cxx) out.stub = value;
                }
            } else if (!out.seh && image.contains(value) && !image.is_code(value) && registers_seh_frame(code, value)) {
                out.seh = read_scope_table(image, value);
            }
        }
    return out;
}

std::optional<ScopeTable> read_scope_table(const BinaryImage& image, u64 va) {
    if (image.arch() != Arch::x86) return std::nullopt;
    // {enclosing level, filter, handler}: a nested __try names an earlier entry.
    auto read_entries = [&](u64 at, i32 none) {
        std::vector<ScopeEntry> out;
        for (i32 i = 0; i < 64 && data_at(image, at, 12); ++i, at += 12) {
            ScopeEntry e;
            e.enclosing = static_cast<i32>(read32(image, at));
            e.filter = read32(image, at + 4);
            e.handler = read32(image, at + 8);
            if (e.enclosing != none && (e.enclosing < 0 || e.enclosing >= i)) break;
            if ((e.filter && !code_at(image, e.filter)) || !code_at(image, e.handler)) break;
            e.finally = e.filter == 0;
            out.push_back(e);
        }
        return out;
    };
    ScopeTable table;
    table.va = va;
    table.entries = read_entries(va, -1);
    if (!table.entries.empty()) return table;
    // _except_handler4: GSCookieOffset (-2 without a /GS cookie), GSCookieXOROffset, EHCookieOffset and
    // EHCookieXOROffset, all frame offsets, then entries whose outermost level is -2.
    if (!data_at(image, va, 16)) return std::nullopt;
    const i32 gs = static_cast<i32>(read32(image, va));
    const i32 eh = static_cast<i32>(read32(image, va + 8));
    if (gs != -2 && (gs >= 0 || gs < -0x10000)) return std::nullopt;
    if (eh >= 0 || eh < -0x10000) return std::nullopt;
    table.cookies = true;
    table.entries = read_entries(va + 16, -2);
    if (table.entries.empty()) return std::nullopt;
    return table;
}

FunctionEh function_eh_x64(const pe::Image& image, u64 start) {
    FunctionEh out;
    if (image.arch() != Arch::x64 || start < image.image_base()) return out;
    const u64 rva = start - image.image_base();
    const auto& functions = image.runtime_functions();
    auto f = std::ranges::find(functions, rva, &pe::RuntimeFunction::begin_rva);
    if (f == functions.end() || !f->handler_data_rva) return out;
    const u64 data = image.image_base() + f->handler_data_rva;
    const u32 first = read32(image, data);
    if (first > 0x1000) {  // an RVA: __CxxFrameHandler3's FuncInfo
        out.cxx = read_cxx_funcinfo(image, image.image_base() + first);
        return out;
    }
    // __C_specific_handler: a count, then {BeginAddress, EndAddress, HandlerAddress, JumpTarget}.
    if (first == 0 || first > 64 || !data_at(image, data + 4, 16 * u64{first})) return out;
    ScopeTable table;
    table.va = data;
    for (u32 i = 0; i < first; ++i) {
        const u64 r = data + 4 + 16 * u64{i};
        const u32 begin = read32(image, r), end = read32(image, r + 4), handler = read32(image, r + 8), target = read32(image, r + 12);
        if (begin >= end || begin < f->begin_rva || end > f->end_rva) return out;
        ScopeEntry e;
        e.finally = target == 0;
        e.handler = image.image_base() + (e.finally ? handler : target);
        e.filter = !e.finally && handler != 1 ? image.image_base() + handler : 0;  // 1: EXCEPTION_EXECUTE_HANDLER
        if (!code_at(image, e.handler) || (e.filter && !code_at(image, e.filter))) return out;
        table.entries.push_back(e);
    }
    out.seh = std::move(table);
    return out;
}

} // namespace decomp
