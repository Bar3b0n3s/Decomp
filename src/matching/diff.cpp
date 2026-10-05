#include "matching/diff.hpp"

#include "analysis/annotate.hpp"
#include "analysis/demangle.hpp"
#include "analysis/eh.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <unordered_map>

namespace decomp::matching {

std::string_view to_string(RefKind kind) {
    switch (kind) {
    case RefKind::symbol: return "symbol";
    case RefKind::label: return "label";
    case RefKind::string: return "string";
    case RefKind::wide_string: return "wide_string";
    case RefKind::float32: return "float32";
    case RefKind::float64: return "float64";
    case RefKind::vector: return "vector";
    case RefKind::table: return "table";
    case RefKind::unknown: return "unknown";
    }
    return "?";
}

std::string_view to_string(RowKind kind) {
    switch (kind) {
    case RowKind::equal: return "equal";
    case RowKind::encoding: return "encoding";
    case RowKind::operand: return "operand";
    case RowKind::opcode: return "opcode";
    case RowKind::insert: return "insert";
    case RowKind::del: return "delete";
    }
    return "?";
}

std::string_view to_string(OperandDiff kind) {
    switch (kind) {
    case OperandDiff::reg: return "register";
    case OperandDiff::imm: return "immediate";
    case OperandDiff::mem: return "memory";
    case OperandDiff::stack: return "stack";
    case OperandDiff::symbol: return "symbol";
    }
    return "?";
}

namespace {

std::string bytes_hex(const std::byte* p, usize n) {
    std::string out;
    for (usize i = 0; i < n; ++i) out += std::format("{:02x}", static_cast<u8>(p[i]));
    return out;
}

std::string float_display(RefKind kind, const std::string& hex_bytes_le) {
    auto byte_at = [&](usize i) { return static_cast<u8>(std::stoul(hex_bytes_le.substr(i * 2, 2), nullptr, 16)); };
    if (kind == RefKind::float32 && hex_bytes_le.size() == 8) {
        u32 v = 0;
        for (usize i = 0; i < 4; ++i) v |= u32(byte_at(i)) << (8 * i);
        return std::format("{}f", std::bit_cast<float>(v));
    }
    if (kind == RefKind::float64 && hex_bytes_le.size() == 16) {
        u64 v = 0;
        for (usize i = 0; i < 8; ++i) v |= u64(byte_at(i)) << (8 * i);
        return std::format("{}", std::bit_cast<double>(v));
    }
    return "const:" + hex_bytes_le;
}

std::string short_symbol(const std::string& name, i64 offset) {
    auto q = qualified_name(name);
    if (q.empty()) q = name;
    return offset ? std::format("{}+{:#x}", q, offset) : q;
}

Ref label_ref(const std::map<u64, usize>& index_of, u64 target, u64 function_base) {
    Ref r;
    r.kind = RefKind::label;
    if (auto it = index_of.find(target); it != index_of.end()) r.key = std::format("L{}", it->second);
    else r.key = std::format("off+{:x}", target - function_base);
    r.display = std::format("loc_{:x}", target - function_base);
    return r;
}

usize field_index(const x86::Instruction& ins, const x86::Field& f) {
    return static_cast<usize>(&f - ins.fields.data());
}

std::string render_side(const SideInstruction& si) {
    return x86::render(si.ins, [&](const x86::Instruction& in, const x86::Field& f) -> std::optional<std::string> {
        const auto& ref = si.refs[field_index(in, f)];
        if (!ref) return std::nullopt;
        return ref->display;
    });
}

// Operand templates: refs replaced by a placeholder so shapes can be compared independently of names.
std::vector<std::string> operand_templates(const SideInstruction& si) {
    return x86::render_operands(si.ins, [&](const x86::Instruction& in, const x86::Field& f) -> std::optional<std::string> {
        if (!si.refs[field_index(in, f)]) return std::nullopt;
        return std::string("{ref}");
    });
}

// MSVC names wide string literals ??_C@_1...; they are compared over all their UTF-16 units.
bool is_wide_literal_symbol(std::string_view name) { return name.starts_with("??_C@_1"); }

// UTF-16 units up to the terminator, read through `unit(i)`; the comparison key is their hex bytes.
template <class ReadUnit>
std::optional<Ref> wide_string_ref(ReadUnit unit) {
    std::string bytes, text;
    for (usize i = 0; i < (1u << 15); ++i) {
        std::optional<u16> u = unit(i);
        if (!u) return std::nullopt;
        if (*u == 0) {
            Ref r;
            r.kind = RefKind::wide_string;
            r.key = bytes_hex(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
            r.display = "L\"" + truncate_utf8(text, 48) + "\"";
            return r;
        }
        bytes.push_back(static_cast<char>(*u & 0xFF));
        bytes.push_back(static_cast<char>(*u >> 8));
        if (*u >= 0x20 && *u < 0x7F && *u != '"' && *u != '\\') text.push_back(static_cast<char>(*u));
        else text += std::format("\\u{:04x}", *u);
    }
    return std::nullopt;
}

std::optional<Ref> image_wide_string(const BinaryImage& image, u64 va) {
    return wide_string_ref([&](usize i) { return image.read<u16>(va + 2 * i); });
}

} // namespace

// A target name is the linker's own when it is C++-decorated or differs from the symbol's readable PDB
// name (a public symbol); then the candidate must use exactly that name, so a match also proves the
// declaration (signature, calling convention, linkage). Otherwise only readable names are known.
bool symbol_names_match(std::string_view target, std::string_view target_alt, std::string_view candidate) {
    auto strip = [](std::string_view n) { return n.starts_with("__imp_") ? n.substr(6) : n; };
    const std::string_view t = strip(target), c = strip(candidate);
    if (t == c || (!target_alt.empty() && target_alt == c)) return true;
    const bool linker_name = t.starts_with('?') || (!target_alt.empty() && target_alt != target);
    if (linker_name) return false;
    return names_equivalent(t, c) || (!target_alt.empty() && names_equivalent(target_alt, c));
}

namespace {

Ref target_ref(const Program& program, u64 va) {
    Ref r;
    r.target_va = va;
    const auto& image = program.image();
    const Symbol* s = program.symbols().at(va);
    if (!s) {
        // A call through an incremental-linking or import thunk compares as a call to its destination
        // (the object file names the function itself).
        if (auto dest = program.thunk_destination(va)) {
            r = target_ref(program, *dest);
            r.target_va = va;
            return r;
        }
    }
    i64 off = 0;
    if (!s) {
        s = program.symbols().containing(va);
        if (s) off = static_cast<i64>(va - s->va);
    }
    if (s && off == 0 && s->kind == SymbolKind::string && is_wide_literal_symbol(s->name)) {
        if (auto w = image_wide_string(image, va)) {
            w->target_va = va;
            return *w;
        }
    }
    if (s && off == 0 && s->kind == SymbolKind::string) {
        if (auto str = image.read_cstring(va)) {
            r.kind = RefKind::string;
            r.key = *str;
            r.display = escape_c_string(truncate_utf8(*str, 48));
            return r;
        }
    }
    if (s && off == 0 && s->kind == SymbolKind::float_const) {
        usize n = s->size ? s->size : 4;
        if (auto bytes = image.view(va, n)) {
            r.kind = n == 4 ? RefKind::float32 : n == 8 ? RefKind::float64 : RefKind::vector;
            r.key = bytes_hex(bytes->data(), n);
            r.display = float_display(r.kind, r.key);
            return r;
        }
    }
    if (s) {
        r.kind = RefKind::symbol;
        r.key = s->name;
        r.alt = s->pdb_name;
        r.offset = off;
        r.display = short_symbol(s->kind == SymbolKind::import ? s->name : s->name, off);
        if (s->kind != SymbolKind::import && !s->pdb_name.empty() && qualified_name(s->name) == s->name)
            r.display = off ? std::format("{}+{:#x}", s->pdb_name, off) : s->pdb_name;
        return r;
    }
    r.kind = RefKind::unknown;
    r.key = std::format("unk_{:x}", va);
    r.display = std::format("{:#x}", va);
    return r;
}

const JumpTable* table_at(const FunctionExtent& ext, u64 va) {
    for (const auto& t : ext.jump_tables)
        if (t.table_va == va) return &t;
    return nullptr;
}

} // namespace

namespace {

// Trailing int3 bytes are padding as far as the linked image goes: the trap compilers put after a final
// call that cannot return (Visual Studio 2015 and later; one byte or several) and the linker's fill
// look the same, so the comparison leaves them out on both sides.
void drop_trailing_int3(std::vector<x86::Instruction>& list) {
    while (list.size() > 1 && list.back().length == 1 && list.back().bytes[0] == 0xCC) list.pop_back();
}

// x86 exception-handling companions of the function being compared: the handler stub it registers and
// its scope table. MSVC names them after the function (__ehhandler$f, __sehtable$f); the target often
// has only their addresses, so both sides name them as the function's own, whatever it is called.
constexpr std::string_view kOwnEhHandler = "__ehhandler$<this function>";
constexpr std::string_view kOwnScopeTable = "__sehtable$<this function>";

Ref own_companion(std::string_view key, std::string display) {
    Ref r;
    r.kind = RefKind::symbol;
    r.key = std::string(key);
    r.display = std::move(display);
    return r;
}

// A candidate's reference to the companions MSVC names after `function` (clang decorates the stub's
// name as a C symbol on x86: ___ehhandler$f).
Ref own_companions(Ref r, const std::string& function) {
    if (r.kind != RefKind::symbol || r.offset != 0) return r;
    std::string_view key = r.key;
    if (key.starts_with("___ehhandler$")) key.remove_prefix(1);
    if (key.starts_with("__ehhandler$") && key.substr(12) == function) return own_companion(kOwnEhHandler, r.display);
    if (key.starts_with("__sehtable$") && key.substr(11) == function) return own_companion(kOwnScopeTable, r.display);
    return r;
}

} // namespace

Result<Side> build_target_side(const Program& program, u64 va) {
    TRY_ASSIGN(auto ext, program.function_extent(va));
    TRY_ASSIGN(auto list, program.function_instructions(ext));
    drop_trailing_int3(list);
    Side side;
    const Symbol* sym = program.symbols().at(va);
    side.name = sym ? sym->name : std::format("sub_{:x}", va);
    if (sym && sym->pdb_name != sym->name) side.alt = sym->pdb_name;
    side.address = va;
    side.size = ext.end - ext.start;
    std::map<u64, usize> index_of;
    for (usize i = 0; i < list.size(); ++i) index_of[list[i].address] = i;
    const auto rva_fields = image_relative_fields(program.image(), list);
    const FunctionEh eh = program.arch() == Arch::x86 ? function_eh(program.image(), program.decoder(), list) : FunctionEh{};

    for (auto& ins : list) {
        SideInstruction si;
        si.refs.resize(ins.fields.size());
        for (usize f = 0; f < ins.fields.size(); ++f) {
            const auto& field = ins.fields[f];
            u64 target = field.absolute;
            // MSVC x64 tables are addressed by RVA in an unrelocated displacement.
            bool rva_table = false;
            for (const auto& t : ext.jump_tables)
                if (t.encoding == TableEncoding::rva && t.load_va == ins.address && field.kind == x86::FieldKind::disp) {
                    target = t.table_va;
                    rva_table = true;
                }
            // MSVC x64 addresses globals as [image base register + index + RVA].
            const bool rva_field = !rva_table && rva_fields.contains({ins.address, f});
            if (rva_field) target = program.image().image_base() + static_cast<u64>(field.raw);
            if (!rva_table && !rva_field && !is_address_field(program, ins, field)) continue;
            if (eh.cxx && target == eh.stub) {
                si.refs[f] = own_companion(kOwnEhHandler, "__ehhandler$" + side.name);
                continue;
            }
            if (eh.seh && target == eh.seh->va) {
                si.refs[f] = own_companion(kOwnScopeTable, "__sehtable$" + side.name);
                continue;
            }
            if (const JumpTable* t = table_at(ext, target)) {
                Ref r;
                r.kind = RefKind::table;
                std::vector<std::string> labels;
                for (u64 entry : t->targets) labels.push_back(label_ref(index_of, entry, va).key);
                r.key = join(labels, ",");
                r.display = "switch_table";
                r.target_va = target;
                si.refs[f] = r;
            } else if (ext.contains(target)) {
                si.refs[f] = label_ref(index_of, target, va);
            } else {
                si.refs[f] = target_ref(program, target);
            }
            // Without .reloc, an in-image constant is only guessed to be an address.
            if (!rva_table && !rva_field && !program.image().has_relocations() && field.kind != x86::FieldKind::rel &&
                !field.rip_relative)
                si.refs[f]->heuristic = true;
        }
        si.ins = std::move(ins);
        si.text = render_side(si);
        side.instructions.push_back(std::move(si));
    }
    return side;
}

const coff::Symbol* find_candidate_symbol(const coff::Object& obj, const Symbol& target) {
    if (auto s = obj.find_defined(target.name)) return s;
    for (const auto* s : obj.function_symbols()) {
        if (names_equivalent(s->name, target.name)) return s;
        if (!target.pdb_name.empty() && names_equivalent(s->name, target.pdb_name)) return s;
    }
    return nullptr;
}

namespace {

struct CandidateContext {
    const coff::Object& obj;
    const coff::Section& section;
    u32 start = 0;
    u32 code_end = 0;
    std::map<u64, usize> index_of;  // section offset -> instruction index
    std::string function;           // its name, which MSVC's names for its EH companions carry
};

const coff::Relocation* reloc_at(const coff::Section& sec, u32 offset) {
    auto it = std::ranges::lower_bound(sec.relocations, offset, {}, &coff::Relocation::offset);
    if (it == sec.relocations.end() || it->offset != offset) return nullptr;
    return &*it;
}

std::optional<i64> read_field(const coff::Section& sec, u32 offset, unsigned size) {
    ByteSpan d = sec.data;
    switch (size) {
    case 1: return read_le<i8>(d, offset);
    case 2: return read_le<i16>(d, offset);
    case 4: return read_le<i32>(d, offset);
    case 8: return read_le<i64>(d, offset);
    default: return std::nullopt;
    }
}

const coff::Symbol* defined_symbol_at(const coff::Object& obj, i32 section_number, u32 offset) {
    const coff::Symbol* best = nullptr;
    for (const auto* s : obj.section_symbols(section_number)) {
        if (s->value != offset || s->storage_class == coff::storage::label) continue;
        if (!best || (s->is_external() && !best->is_external())) best = s;
    }
    return best;
}

Ref candidate_reloc_ref(const CandidateContext& ctx, const coff::Relocation& reloc, i64 addend) {
    const auto& obj = ctx.obj;
    Ref r;
    const coff::Symbol* sym = obj.symbol_at_index(reloc.symbol_index);
    if (!sym) {
        r.key = std::format("bad_symbol_{}", reloc.symbol_index);
        r.display = r.key;
        return r;
    }
    if (!sym->is_defined()) {
        r.kind = RefKind::symbol;
        r.key = sym->name;
        r.offset = addend;
        r.display = short_symbol(sym->name, addend);
        return r;
    }
    const coff::Section* tsec = obj.section(sym->section_number);
    if (!tsec) {
        r.key = sym->name;
        r.display = sym->name;
        return r;
    }
    i64 off = static_cast<i64>(sym->value) + addend;

    if (tsec == &ctx.section) {
        if (off >= ctx.start && off < ctx.code_end) return label_ref(ctx.index_of, static_cast<u64>(off), ctx.start);
        // In-section data after the code (MSVC x86 jump tables): handled like any anonymous table below.
    }
    if (tsec->is_code() && tsec != &ctx.section) {
        if (const coff::Symbol* named = defined_symbol_at(obj, sym->section_number, static_cast<u32>(off))) {
            r.kind = RefKind::symbol;
            r.key = named->name;
            r.display = short_symbol(named->name, 0);
            return r;
        }
        r.kind = RefKind::symbol;
        r.key = sym->name;
        r.offset = addend;
        r.display = short_symbol(sym->name, addend);
        return r;
    }
    std::string literal = is_string_literal_symbol(sym->name) ? sym->name : std::string();
    if (literal.empty() && tsec->comdat && sym->is_section_symbol() && !tsec->is_bss())
        if (auto n = defined_symbol_at(obj, sym->section_number, 0); n && is_string_literal_symbol(n->name)) literal = n->name;
    if (!literal.empty() && is_wide_literal_symbol(literal) && off >= 0) {
        auto unit = [&](usize i) -> std::optional<u16> {
            const usize at = static_cast<usize>(off) + 2 * i;
            if (at + 2 > tsec->data.size()) return std::nullopt;
            return read_le<u16>(ByteSpan(tsec->data), at);
        };
        if (auto w = wide_string_ref(unit)) return *w;
    }
    if (!literal.empty()) {
        if (auto str = read_cstring_at(tsec->data, static_cast<usize>(off), 1 << 16)) {
            r.kind = RefKind::string;
            r.key = *str;
            r.display = escape_c_string(truncate_utf8(*str, 48));
            return r;
        }
    }
    std::string const_name = sym->name;
    if (sym->is_section_symbol())
        if (auto n = defined_symbol_at(obj, sym->section_number, static_cast<u32>(off))) const_name = n->name;
    if (is_float_constant_symbol(const_name)) {
        usize n = (const_name.size() - const_name.find('@') - 1) / 2;
        if (off >= 0 && u64(off) + n <= tsec->data.size()) {
            r.kind = n == 4 ? RefKind::float32 : n == 8 ? RefKind::float64 : RefKind::vector;
            r.key = bytes_hex(tsec->data.data() + off, n);
            r.display = float_display(r.kind, r.key);
            return r;
        }
    }
    // Named symbols compare by name, except labels on data inside the function's own section (MSVC
    // x86 names in-section jump tables `$LN<n>`), which are read like anonymous tables below.
    if (!sym->is_section_symbol() && sym->storage_class != coff::storage::label && tsec != &ctx.section) {
        r.kind = RefKind::symbol;
        r.key = sym->name;
        r.offset = addend;
        r.display = short_symbol(sym->name, addend);
        return r;
    }
    // Anonymous data via a section symbol or label: a jump table if its entries point back into the function.
    unsigned ps = pointer_size(obj.arch());
    std::vector<std::string> labels;
    u32 stride = 0;
    for (u32 entry = static_cast<u32>(off); entry < tsec->size; entry += stride) {
        const coff::Relocation* er = reloc_at(*tsec, entry);
        if (!er) break;
        unsigned size = obj.relocation_size(er->type);
        if (size == 0 || (stride && size != stride)) break;
        stride = size;
        const coff::Symbol* es = obj.symbol_at_index(er->symbol_index);
        if (!es || es->section_number != static_cast<i32>(ctx.section.number)) break;
        auto ea = read_field(*tsec, entry, size);
        if (!ea) break;
        i64 label = static_cast<i64>(es->value) + *ea;
        // PC-relative entries (clang x64: REL32 against .text) carry addend = label + (entry - table) + 4 + k.
        if (obj.relocation_is_pc_relative(er->type)) {
            i64 extra = obj.machine() == 0x8664 && er->type > coff::reloc_amd64::rel32 ? er->type - coff::reloc_amd64::rel32 : 0;
            label -= static_cast<i64>(entry - static_cast<u32>(off)) + 4 + extra;
        }
        labels.push_back(label_ref(ctx.index_of, static_cast<u64>(label), ctx.start).key);
    }
    (void)ps;
    if (!labels.empty()) {
        r.kind = RefKind::table;
        r.key = join(labels, ",");
        r.display = "switch_table";
        return r;
    }
    if (!tsec->is_bss())
        if (auto str = read_cstring_at(tsec->data, static_cast<usize>(off), 4096); str && str->size() >= 2) {
            r.kind = RefKind::string;
            r.key = *str;
            r.display = escape_c_string(truncate_utf8(*str, 48));
            return r;
        }
    if (auto named = defined_symbol_at(obj, sym->section_number, static_cast<u32>(off))) {
        r.kind = RefKind::symbol;
        r.key = named->name;
        r.display = short_symbol(named->name, 0);
        return r;
    }
    r.kind = RefKind::unknown;
    r.key = std::format("{}+{:#x}", tsec->name, off);
    r.display = r.key;
    return r;
}

} // namespace

Result<Side> build_candidate_side(const coff::Object& obj, const coff::Symbol& function) {
    const coff::Section* sec = obj.section(function.section_number);
    if (!sec || !sec->is_code()) return make_error(ErrorCode::invalid_argument, "'{}' is not defined in a code section", function.name);
    u32 start = function.value;
    u32 end = start + obj.symbol_size(function);
    if (end > sec->data.size()) return make_error(ErrorCode::parse, "'{}' extends past its section", function.name);

    x86::Decoder decoder(obj.arch());
    auto list = decoder.decode_all(ByteSpan(sec->data).subspan(start, end - start), start);

    // In-section data (MSVC x86 puts jump tables right after the code in the same COMDAT): the first
    // relocated reference from an indirect jump into this section past the jump marks the end of code.
    u32 code_end = end;
    for (const auto& ins : list) {
        if (ins.flow != x86::Flow::indirect_jump) continue;
        for (const auto& f : ins.fields) {
            const coff::Relocation* rel = reloc_at(*sec, static_cast<u32>(ins.address + f.offset));
            if (!rel) continue;
            const coff::Symbol* s = obj.symbol_at_index(rel->symbol_index);
            if (!s || s->section_number != function.section_number) continue;
            u64 off = s->value + static_cast<u64>(f.raw);
            if (off > ins.address && off < code_end) code_end = static_cast<u32>(off);
        }
    }
    std::erase_if(list, [&](const x86::Instruction& i) { return i.address >= code_end; });
    drop_trailing_int3(list);

    CandidateContext ctx{obj, *sec, start, code_end, {}, function.name};
    for (usize i = 0; i < list.size(); ++i) ctx.index_of[list[i].address] = i;

    Side side;
    side.name = function.name;
    side.address = start;
    side.size = code_end - start;
    for (auto& ins : list) {
        SideInstruction si;
        si.refs.resize(ins.fields.size());
        for (usize f = 0; f < ins.fields.size(); ++f) {
            const auto& field = ins.fields[f];
            if (const coff::Relocation* rel = reloc_at(*sec, static_cast<u32>(ins.address + field.offset))) {
                si.refs[f] = own_companions(candidate_reloc_ref(ctx, *rel, field.raw), ctx.function);
            } else if (field.kind == x86::FieldKind::rel) {
                si.refs[f] = label_ref(ctx.index_of, field.absolute, start);
            }
        }
        si.ins = std::move(ins);
        si.text = render_side(si);
        side.instructions.push_back(std::move(si));
    }
    return side;
}

namespace {

// Target instruction index -> paired candidate instruction index (from the alignment).
using IndexMap = std::unordered_map<usize, usize>;

bool labels_equal(const std::string& tkey, const std::string& ckey, const IndexMap& map) {
    if (tkey.starts_with('L') && ckey.starts_with('L')) {
        usize ti = std::stoul(tkey.substr(1)), ci = std::stoul(ckey.substr(1));
        auto it = map.find(ti);
        return it != map.end() && it->second == ci;
    }
    return tkey == ckey;
}

bool refs_equal(const Ref& t, const Ref& c, const Program& program, const IndexMap& map) {
    const auto& image = program.image();
    if (t.kind == RefKind::label || c.kind == RefKind::label)
        return t.kind == c.kind && labels_equal(t.key, c.key, map);
    if (c.kind == RefKind::table || t.kind == RefKind::table) {
        if (t.kind != c.kind) return false;
        auto tl = split(t.key, ','), cl = split(c.key, ',');
        if (tl.size() != cl.size()) return false;
        for (usize i = 0; i < tl.size(); ++i)
            if (!labels_equal(std::string(tl[i]), std::string(cl[i]), map)) return false;
        return true;
    }
    if (c.kind == RefKind::string) {
        if (t.kind == RefKind::string) return t.key == c.key;
        if (t.target_va) {
            auto s = image.read_cstring(t.target_va, c.key.size() + 1);
            return s && *s == c.key;
        }
        return false;
    }
    if (c.kind == RefKind::wide_string) {
        if (t.kind == RefKind::wide_string) return t.key == c.key;
        if (t.target_va) {
            auto w = image_wide_string(image, t.target_va);
            return w && w->key == c.key;
        }
        return false;
    }
    if (c.kind == RefKind::float32 || c.kind == RefKind::float64 || c.kind == RefKind::vector) {
        if (t.kind == c.kind) return t.key == c.key;
        if (t.target_va) {
            auto bytes = image.view(t.target_va, c.key.size() / 2);
            return bytes && bytes_hex(bytes->data(), bytes->size()) == c.key;
        }
        return false;
    }
    if (c.kind == RefKind::symbol) {
        if (t.kind != RefKind::symbol || t.offset != c.offset) return false;
        return symbol_names_match(t.key, t.alt, c.key);
    }
    return false;
}

bool is_stack_register(const std::string& r) { return r == "esp" || r == "ebp" || r == "rsp" || r == "rbp"; }

struct PairResult {
    bool equal = false;
    std::vector<std::pair<usize, OperandDiff>> operands;
    bool bytes_equal = false;
};

PairResult compare_pair(const SideInstruction& target, const SideInstruction& c, const Program& program,
                        const IndexMap& map, std::vector<Binding>& bindings) {
    // An address that was only guessed (image without .reloc) where the candidate has a plain constant
    // is compared as that constant.
    std::optional<SideInstruction> adjusted;
    for (usize f = 0; f < target.refs.size() && f < c.refs.size() && f < c.ins.fields.size(); ++f) {
        if (target.refs[f] && target.refs[f]->heuristic && !c.refs[f] && target.ins.fields[f].raw == c.ins.fields[f].raw) {
            if (!adjusted) adjusted = target;
            adjusted->refs[f].reset();
        }
    }
    const SideInstruction& t = adjusted ? *adjusted : target;
    PairResult pr;
    auto tt = operand_templates(t), ct = operand_templates(c);
    for (usize i = 0; i < std::min(tt.size(), ct.size()); ++i) {
        const auto& to = t.ins.operands[i];
        const auto& co = c.ins.operands[i];
        if (tt[i] != ct[i]) {
            OperandDiff kind = OperandDiff::mem;
            if (to.kind == x86::OperandKind::reg && co.kind == x86::OperandKind::reg) kind = OperandDiff::reg;
            else if (to.kind == x86::OperandKind::imm && co.kind == x86::OperandKind::imm) kind = OperandDiff::imm;
            else if (to.kind == x86::OperandKind::mem && co.kind == x86::OperandKind::mem && is_stack_register(to.mem.base) &&
                     to.mem.base == co.mem.base && to.mem.index == co.mem.index)
                kind = OperandDiff::stack;
            pr.operands.emplace_back(i, kind);
            continue;
        }
        // Same shape: compare the references carried by this operand.
        for (usize f = 0; f < t.ins.fields.size() && f < c.ins.fields.size(); ++f) {
            if (t.ins.fields[f].operand != static_cast<i8>(i)) continue;
            const auto& tr = t.refs[f];
            const auto& cr = c.refs[f];
            if (!tr && !cr) continue;
            if (tr && cr && refs_equal(*tr, *cr, program, map)) continue;
            pr.operands.emplace_back(i, OperandDiff::symbol);
            if (tr && cr && tr->kind == RefKind::unknown && cr->kind == RefKind::symbol && tr->target_va)
                bindings.push_back({tr->target_va, cr->key});
            break;
        }
    }
    if (tt.size() != ct.size()) pr.operands.emplace_back(std::min(tt.size(), ct.size()), OperandDiff::mem);
    pr.equal = pr.operands.empty();
    if (pr.equal && t.ins.length == c.ins.length) {
        // Compare bytes outside the reference-carrying fields.
        std::vector<bool> masked(t.ins.length, false);
        for (usize f = 0; f < t.ins.fields.size(); ++f) {
            bool has_ref = t.refs[f].has_value() || (f < c.refs.size() && c.refs[f].has_value());
            if (!has_ref) continue;
            const auto& field = t.ins.fields[f];
            for (u8 b = 0; b < field.size && field.offset + b < t.ins.length; ++b) masked[field.offset + b] = true;
        }
        pr.bytes_equal = true;
        for (u8 b = 0; b < t.ins.length; ++b)
            if (!masked[b] && t.ins.bytes[b] != c.ins.bytes[b]) pr.bytes_equal = false;
    }
    return pr;
}

// Alignment over template keys: Needleman-Wunsch for normal sizes, sequential pairing beyond a cap.
std::vector<std::pair<std::optional<usize>, std::optional<usize>>> align(const std::vector<std::string>& a,
                                                                          const std::vector<std::string>& b,
                                                                          const std::vector<std::string>& am,
                                                                          const std::vector<std::string>& bm) {
    std::vector<std::pair<std::optional<usize>, std::optional<usize>>> out;
    const usize n = a.size(), m = b.size();
    if (n * m > 25'000'000) {
        for (usize i = 0; i < std::max(n, m); ++i)
            out.emplace_back(i < n ? std::optional<usize>(i) : std::nullopt, i < m ? std::optional<usize>(i) : std::nullopt);
        return out;
    }
    constexpr int gap = 2;
    auto sub = [&](usize i, usize j) { return a[i] == b[j] ? 0 : am[i] == bm[j] ? 1 : 3; };
    std::vector<int> prev(m + 1), cur(m + 1);
    std::vector<u8> dir((n + 1) * (m + 1), 0);  // 0 diag, 1 up (delete), 2 left (insert)
    for (usize j = 0; j <= m; ++j) {
        prev[j] = static_cast<int>(j) * gap;
        dir[j] = 2;
    }
    for (usize i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i) * gap;
        dir[i * (m + 1)] = 1;
        for (usize j = 1; j <= m; ++j) {
            int d = prev[j - 1] + sub(i - 1, j - 1), u = prev[j] + gap, l = cur[j - 1] + gap;
            int best = d;
            u8 choice = 0;
            if (u < best) { best = u; choice = 1; }
            if (l < best) { best = l; choice = 2; }
            cur[j] = best;
            dir[i * (m + 1) + j] = choice;
        }
        std::swap(prev, cur);
    }
    usize i = n, j = m;
    while (i > 0 || j > 0) {
        u8 d = (i > 0 && j > 0) ? dir[i * (m + 1) + j] : (i > 0 ? 1 : 2);
        if (d == 0) {
            out.emplace_back(i - 1, j - 1);
            --i;
            --j;
        } else if (d == 1) {
            out.emplace_back(i - 1, std::nullopt);
            --i;
        } else {
            out.emplace_back(std::nullopt, j - 1);
            --j;
        }
    }
    std::ranges::reverse(out);
    return out;
}

std::string inverse_condition(const std::string& m) {
    static const std::map<std::string, std::string> inv = {
        {"jz", "jnz"}, {"jnz", "jz"}, {"jb", "jnb"}, {"jnb", "jb"}, {"jbe", "jnbe"}, {"jnbe", "jbe"},
        {"jl", "jnl"}, {"jnl", "jl"}, {"jle", "jnle"}, {"jnle", "jle"}, {"js", "jns"}, {"jns", "js"},
        {"jo", "jno"}, {"jno", "jo"}, {"jp", "jnp"}, {"jnp", "jp"}};
    auto it = inv.find(m);
    return it == inv.end() ? std::string() : it->second;
}

void add_hints(FunctionDiff& d) {
    if (d.byte_exact) return;
    auto& h = d.hints;
    if (!d.candidate.name.empty() && !symbol_names_match(d.target.name, d.target.alt, d.candidate.name))
        h.push_back(std::format("The candidate defines `{}` ({}) but the target function is `{}` ({}): the declaration differs "
                                "(parameter types, const, calling convention or extern \"C\").",
                                d.candidate.name, display_name(d.candidate.name), d.target.name, display_name(d.target.name)));
    usize diffs = d.encoding + d.operand + d.opcode + d.inserted + d.deleted;
    if (diffs == 0 && !d.exact) h.push_back("All instructions align, but some references could not be verified.");

    bool only_reg = d.operand > 0, only_stack = d.operand > 0;
    for (const auto& r : d.rows) {
        if (r.kind == RowKind::equal) continue;
        if (r.kind != RowKind::operand) {
            only_reg = only_stack = false;
            continue;
        }
        for (auto [i, k] : r.operands) {
            if (k != OperandDiff::reg) only_reg = false;
            if (k != OperandDiff::stack) only_stack = false;
        }
    }
    if (only_reg)
        h.push_back("Only register allocation differs. Try reordering declarations or statements, changing variable "
                    "lifetimes, introducing or removing temporaries, or changing an expression's evaluation order.");
    if (only_stack)
        h.push_back("Only stack offsets differ: local variable order, sizes or types differ (MSVC lays out locals by "
                    "declaration order and size).");
    if (d.encoding > 0)
        h.push_back(std::format("{} instruction(s) are identical in text but encoded differently (e.g. operand form or "
                                "immediate size); often a different operand type, signedness, or compiler flag.", d.encoding));

    for (const auto& r : d.rows) {
        if (r.kind != RowKind::opcode || !r.target || !r.candidate) continue;
        const auto& tm = d.target.instructions[*r.target].ins.mnemonic;
        const auto& cm = d.candidate.instructions[*r.candidate].ins.mnemonic;
        if (!inverse_condition(tm).empty() && inverse_condition(tm) == cm) {
            h.push_back(std::format("Branch condition inverted at target #{} ({} vs {}): swap the if/else bodies or negate "
                                    "the condition.", *r.target, tm, cm));
            break;
        }
    }

    // Same instructions in a different order.
    if (d.target.instructions.size() == d.candidate.instructions.size() && d.opcode + d.operand > 0) {
        std::vector<std::string> a, b;
        for (const auto& i : d.target.instructions) a.push_back(i.text);
        for (const auto& i : d.candidate.instructions) b.push_back(i.text);
        std::ranges::sort(a);
        std::ranges::sort(b);
        if (a == b)
            h.push_back("Same instructions in a different order: statement order or the evaluation order of an "
                        "expression differs.");
    }

    // An import reached through the import table on one side and through the linker's thunk on the other:
    // the declaration has __declspec(dllimport) on one side only.
    {
        struct ImportCalls {
            std::vector<std::pair<std::string, usize>> table, direct;  // (name without __imp_, instruction)
        };
        // Reading a slot (`call [__imp_X]`, or `mov esi, [__imp_X]` then `call esi`) is a call through the
        // table; a direct call or jump to the import's name goes through the thunk.
        auto import_calls = [](const Side& s) {
            ImportCalls out;
            for (usize i = 0; i < s.instructions.size(); ++i) {
                const auto& si = s.instructions[i];
                const bool direct = si.ins.flow == x86::Flow::call || si.ins.flow == x86::Flow::jump;
                for (const auto& ref : si.refs) {
                    if (!ref || ref->kind != RefKind::symbol) continue;
                    std::string_view key = ref->key;
                    const bool slot = key.starts_with("__imp_");
                    if (slot) key.remove_prefix(6);
                    if (direct) out.direct.emplace_back(std::string(key), i);
                    else if (slot) out.table.emplace_back(std::string(key), i);
                    break;
                }
            }
            return out;
        };
        auto has = [](const std::vector<std::pair<std::string, usize>>& list, std::string_view name) {
            return std::ranges::any_of(list, [&](const auto& e) { return names_equivalent(e.first, name); });
        };
        const ImportCalls t = import_calls(d.target), c = import_calls(d.candidate);
        for (const auto& [name, i] : t.table)
            if (has(c.direct, name) && !has(c.table, name)) {
                h.push_back(std::format("Target #{} calls `{}` through the import table (`call [__imp_...]`), the candidate through "
                                        "the linker's import thunk: declare it `__declspec(dllimport)`.", i, name));
                break;
            }
        for (const auto& [name, i] : t.direct)
            if (has(c.table, name) && !has(c.direct, name) && !has(t.table, name)) {
                h.push_back(std::format("Target #{} calls `{}` through the linker's import thunk, the candidate through the import "
                                        "table: declare it without `__declspec(dllimport)`.", i, name));
                break;
            }
    }

    long delta = static_cast<long>(d.candidate.instructions.size()) - static_cast<long>(d.target.instructions.size());
    if (delta != 0)
        h.push_back(std::format("Candidate has {} {} instruction(s) than the target.", std::abs(delta), delta > 0 ? "more" : "fewer"));

    // Reference differences: strings, constants, callees.
    for (const auto& r : d.rows) {
        if (r.kind != RowKind::operand || !r.target || !r.candidate) continue;
        const auto& t = d.target.instructions[*r.target];
        const auto& c = d.candidate.instructions[*r.candidate];
        for (auto [op, kind] : r.operands) {
            if (kind != OperandDiff::symbol) continue;
            for (usize f = 0; f < t.refs.size() && f < c.refs.size(); ++f) {
                if (t.ins.fields[f].operand != static_cast<i8>(op) || !t.refs[f] || !c.refs[f]) continue;
                const auto& tr = *t.refs[f];
                const auto& cr = *c.refs[f];
                if (tr.kind == RefKind::label || cr.kind == RefKind::label) {
                    if (tr.display == cr.display)
                        h.push_back(std::format("Branch at target #{} lands on code that differs between the versions "
                                                "(around {}).", *r.target, tr.display));
                    else
                        h.push_back(std::format("Branch at target #{} goes to a different place ({} vs {}): the control "
                                                "flow around it differs.", *r.target, tr.display, cr.display));
                    continue;
                }
                std::string what = cr.kind == RefKind::string ? "String literal" : (cr.kind == RefKind::float32 || cr.kind == RefKind::float64) ? "Constant"
                                   : t.ins.flow == x86::Flow::call ? "Callee" : "Reference";
                if (tr.kind == RefKind::symbol && cr.kind == RefKind::symbol && tr.offset == cr.offset &&
                    names_equivalent(tr.alt.empty() ? tr.key : tr.alt, cr.key))
                    h.push_back(std::format("Declaration differs at target #{}: the target uses `{}` ({}) but the candidate's "
                                            "declaration produces `{}` ({}). Match the signature, calling convention, const "
                                            "and linkage (extern \"C\").",
                                            *r.target, tr.key, display_name(tr.key), cr.key, display_name(cr.key)));
                else if (tr.kind == RefKind::unknown && cr.kind == RefKind::symbol)
                    h.push_back(std::format("Target #{} references {:#x}, which has no symbol; the candidate uses `{}` there. "
                                            "If that is the same object, name the address `{}`.", *r.target, tr.target_va, cr.key, cr.key));
                else
                    h.push_back(std::format("{} differs at target #{}: target {} vs candidate {}.", what, *r.target, tr.display, cr.display));
            }
        }
        if (h.size() > 12) break;
    }
}

} // namespace

FunctionDiff diff_sides(Side target, Side candidate, const Program& program) {
    FunctionDiff d;
    d.target = std::move(target);
    d.candidate = std::move(candidate);

    std::vector<std::string> ak, bk, am, bm;
    auto key_of = [](const SideInstruction& si) {
        return si.ins.prefix + si.ins.mnemonic + " " + join(operand_templates(si), ", ");
    };
    for (const auto& i : d.target.instructions) {
        ak.push_back(key_of(i));
        am.push_back(i.ins.prefix + i.ins.mnemonic);
    }
    for (const auto& i : d.candidate.instructions) {
        bk.push_back(key_of(i));
        bm.push_back(i.ins.prefix + i.ins.mnemonic);
    }

    bool refs_all_ok = true, bytes_all_ok = true;
    auto alignment = align(ak, bk, am, bm);
    IndexMap map;
    for (auto [ti, ci] : alignment)
        if (ti && ci) map[*ti] = *ci;
    for (auto [ti, ci] : alignment) {
        Row row;
        row.target = ti;
        row.candidate = ci;
        if (ti && ci) {
            const auto& t = d.target.instructions[*ti];
            const auto& c = d.candidate.instructions[*ci];
            if (am[*ti] != bm[*ci]) {
                row.kind = RowKind::opcode;
                ++d.opcode;
            } else {
                auto pr = compare_pair(t, c, program, map, d.bindings);
                if (!pr.equal) {
                    row.kind = RowKind::operand;
                    row.operands = std::move(pr.operands);
                    ++d.operand;
                    for (auto [i, k] : row.operands)
                        if (k == OperandDiff::symbol) refs_all_ok = false;
                } else if (!pr.bytes_equal) {
                    row.kind = RowKind::encoding;
                    ++d.encoding;
                    bytes_all_ok = false;
                } else {
                    row.kind = RowKind::equal;
                    ++d.equal;
                }
            }
        } else if (ti) {
            row.kind = RowKind::del;
            ++d.deleted;
        } else {
            row.kind = RowKind::insert;
            ++d.inserted;
        }
        d.rows.push_back(std::move(row));
    }

    usize n = std::max(d.target.instructions.size(), d.candidate.instructions.size());
    double credit = 0;
    for (const auto& r : d.rows) {
        switch (r.kind) {
        case RowKind::equal: credit += 1.0; break;
        case RowKind::encoding: credit += 0.9; break;
        case RowKind::operand: {
            bool soft = std::ranges::all_of(r.operands, [](auto& p) { return p.second == OperandDiff::reg || p.second == OperandDiff::stack; });
            credit += soft ? 0.75 : 0.5;
            break;
        }
        default: break;
        }
    }
    // The candidate must define the function under the target's decorated name: same declaration.
    const bool name_ok = d.candidate.name.empty() || symbol_names_match(d.target.name, d.target.alt, d.candidate.name);
    d.exact = d.opcode == 0 && d.operand == 0 && d.inserted == 0 && d.deleted == 0 && refs_all_ok && name_ok && n > 0;
    d.byte_exact = d.exact && bytes_all_ok && d.encoding == 0;
    d.match_percent = d.byte_exact ? 100.0 : (n ? std::min(99.9, 100.0 * credit / static_cast<double>(n)) : 0.0);
    // Deduplicate bindings.
    std::ranges::sort(d.bindings, {}, &Binding::target_va);
    d.bindings.erase(std::unique(d.bindings.begin(), d.bindings.end(),
                                 [](const Binding& x, const Binding& y) { return x.target_va == y.target_va && x.candidate_symbol == y.candidate_symbol; }),
                     d.bindings.end());
    add_hints(d);
    return d;
}

Result<FunctionDiff> diff_function(const Program& program, u64 va, const coff::Object& obj, const std::string& candidate_symbol) {
    TRY_ASSIGN(auto target, build_target_side(program, va));
    const coff::Symbol* cs = nullptr;
    if (!candidate_symbol.empty()) {
        cs = obj.find_defined(candidate_symbol);
        if (!cs)
            for (const auto* s : obj.function_symbols())
                if (names_equivalent(s->name, candidate_symbol)) cs = s;
    } else if (const Symbol* ts = program.symbols().at(va)) {
        cs = find_candidate_symbol(obj, *ts);
    }
    if (!cs) {
        std::vector<std::string> names;
        for (const auto* s : obj.function_symbols()) names.push_back(s->name);
        return make_error(ErrorCode::not_found, "the candidate object does not define '{}' (it defines: {})",
                          candidate_symbol.empty() ? target.name : candidate_symbol, names.empty() ? "no functions" : join(names, ", "));
    }
    TRY_ASSIGN(auto candidate, build_candidate_side(obj, *cs));
    return diff_sides(std::move(target), std::move(candidate), program);
}

std::string summary_line(const FunctionDiff& d) {
    std::vector<std::string> parts;
    if (d.encoding) parts.push_back(std::format("{} encoding", d.encoding));
    if (d.operand) parts.push_back(std::format("{} operand", d.operand));
    if (d.opcode) parts.push_back(std::format("{} opcode", d.opcode));
    if (d.inserted) parts.push_back(std::format("{} extra", d.inserted));
    if (d.deleted) parts.push_back(std::format("{} missing", d.deleted));
    usize n = std::max(d.target.instructions.size(), d.candidate.instructions.size());
    return std::format("match {:.1f}% ({}/{} equal{}{}) - {}", d.match_percent, d.equal, n, parts.empty() ? "" : "; ",
                       join(parts, ", "), d.byte_exact ? "MATCHING (byte-exact)" : d.exact ? "equivalent but bytes differ" : "not matching");
}

namespace {

char marker(RowKind k) {
    switch (k) {
    case RowKind::equal: return ' ';
    case RowKind::encoding: return 'e';
    case RowKind::operand: return '~';
    case RowKind::opcode: return '!';
    case RowKind::insert: return '+';
    case RowKind::del: return '-';
    }
    return '?';
}

std::vector<bool> visible_rows(const FunctionDiff& d, const ReportOptions& o) {
    std::vector<bool> show(d.rows.size(), !o.compact);
    if (o.compact)
        for (usize i = 0; i < d.rows.size(); ++i)
            if (d.rows[i].kind != RowKind::equal)
                for (usize j = i >= o.context ? i - o.context : 0; j <= std::min(d.rows.size() - 1, i + o.context); ++j) show[j] = true;
    return show;
}

} // namespace

std::string to_text(const FunctionDiff& d, const ReportOptions& o) {
    std::string out = summary_line(d) + "\n";
    out += std::format("target {} ({} bytes) | candidate {} ({} bytes)\n", d.target.name, d.target.size, d.candidate.name, d.candidate.size);
    auto show = visible_rows(d, o);
    usize printed = 0;
    bool gap = false;
    const char* reset = o.color ? "\x1b[0m" : "";
    for (usize i = 0; i < d.rows.size(); ++i) {
        if (!show[i]) {
            gap = true;
            continue;
        }
        if (gap) {
            out += "   ...\n";
            gap = false;
        }
        if (++printed > o.max_rows) {
            out += std::format("   ... {} more rows\n", d.rows.size() - i);
            break;
        }
        const auto& r = d.rows[i];
        auto cell = [&](const Side& s, std::optional<usize> idx) -> std::string {
            if (!idx) return {};
            const auto& si = s.instructions[*idx];
            std::string t = std::format("{:4x}: ", si.ins.address - s.address);
            if (o.bytes) t += std::format("{:<20} ", hex_bytes(si.ins.bytes.data(), si.ins.length, ""));
            return t + si.text;
        };
        std::string color;
        if (o.color) {
            switch (r.kind) {
            case RowKind::equal: break;
            case RowKind::encoding: color = "\x1b[35m"; break;
            case RowKind::operand: color = "\x1b[33m"; break;
            case RowKind::opcode: color = "\x1b[31m"; break;
            case RowKind::insert: color = "\x1b[32m"; break;
            case RowKind::del: color = "\x1b[31m"; break;
            }
        }
        std::string left = cell(d.target, r.target), right = cell(d.candidate, r.candidate);
        std::string note;
        if (r.kind == RowKind::operand) {
            std::vector<std::string> parts;
            for (auto [op, kind] : r.operands) parts.push_back(std::format("op{} {}", op, to_string(kind)));
            note = "  (" + join(parts, ", ") + ")";
        } else if (r.kind == RowKind::encoding) {
            note = "  (encoding)";
        }
        out += std::format("{}{} {:<52}| {}{}{}\n", color, marker(r.kind), left, right, note, color.empty() ? "" : reset);
    }
    if (!d.hints.empty()) {
        out += "hints:\n";
        for (const auto& h : d.hints) out += "- " + h + "\n";
    }
    return out;
}

Json to_json(const FunctionDiff& d, const ReportOptions& o) {
    Json j;
    j["match_percent"] = std::round(d.match_percent * 10) / 10;
    j["exact"] = d.exact;
    j["byte_exact"] = d.byte_exact;
    j["summary"] = summary_line(d);
    j["target"] = {{"name", d.target.name}, {"address", d.target.address}, {"size", d.target.size},
                   {"instructions", d.target.instructions.size()}};
    j["candidate"] = {{"name", d.candidate.name}, {"size", d.candidate.size}, {"instructions", d.candidate.instructions.size()}};
    j["counts"] = {{"equal", d.equal}, {"encoding", d.encoding}, {"operand", d.operand}, {"opcode", d.opcode},
                   {"extra", d.inserted}, {"missing", d.deleted}};
    auto show = visible_rows(d, o);
    Json rows = Json::array();
    for (usize i = 0; i < d.rows.size() && rows.size() < o.max_rows; ++i) {
        if (!show[i]) continue;
        const auto& r = d.rows[i];
        Json row{{"kind", std::string(to_string(r.kind))}};
        if (r.target) row["t"] = d.target.instructions[*r.target].text, row["ti"] = *r.target;
        if (r.candidate) row["c"] = d.candidate.instructions[*r.candidate].text, row["ci"] = *r.candidate;
        if (!r.operands.empty()) {
            Json ops = Json::array();
            for (auto [op, kind] : r.operands) ops.push_back({{"operand", op}, {"diff", std::string(to_string(kind))}});
            row["operands"] = ops;
        }
        rows.push_back(std::move(row));
    }
    j["rows"] = rows;
    j["hints"] = d.hints;
    Json binds = Json::array();
    for (const auto& b : d.bindings) binds.push_back({{"target_va", b.target_va}, {"candidate_symbol", b.candidate_symbol}});
    j["bindings"] = binds;
    return j;
}

} // namespace decomp::matching
