#include "formats/codeview.hpp"

#include <cstring>
#include <format>
#include <set>

namespace decomp::codeview {

namespace {

// A cursor over a record's bytes. Reading past the end leaves it failed (and reads zeros).
class Cursor {
public:
    explicit Cursor(std::span<const std::byte> data) : data_(data) {}

    bool ok() const { return ok_; }
    usize remaining() const { return data_.size() - pos_; }

    template <class T>
    T read() {
        T value{};
        if (sizeof(T) > remaining()) {
            ok_ = false;
            pos_ = data_.size();
            return value;
        }
        std::memcpy(&value, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return value;
    }

    // A numeric leaf: a value below 0x8000 stands for itself, else a typed value follows its kind.
    i64 numeric() {
        const u16 kind = read<u16>();
        if (kind < 0x8000) return kind;
        switch (kind) {
        case 0x8000: return read<i8>();
        case 0x8001: return read<i16>();
        case 0x8002: return read<u16>();
        case 0x8003: return read<i32>();
        case 0x8004: return read<u32>();
        case 0x8009: return read<i64>();
        case 0x800a: return static_cast<i64>(read<u64>());
        default: ok_ = false; return 0;
        }
    }

    // A zero-terminated name, or (the _st records) one prefixed with its length.
    std::string name(bool length_prefixed = false) {
        if (length_prefixed) {
            const usize length = read<u8>();
            if (length > remaining()) {
                ok_ = false;
                pos_ = data_.size();
                return {};
            }
            std::string out(reinterpret_cast<const char*>(data_.data() + pos_), length);
            pos_ += length;
            return out;
        }
        usize end = pos_;
        while (end < data_.size() && data_[end] != std::byte{0}) ++end;
        std::string out(reinterpret_cast<const char*>(data_.data() + pos_), end - pos_);
        if (end >= data_.size()) ok_ = false;
        pos_ = std::min(end + 1, data_.size());
        return out;
    }

    // The padding between field list entries: LF_PAD0..LF_PAD15 (0xf0..0xff), the low nibble counting
    // the bytes to the next entry.
    void skip_padding() {
        while (pos_ < data_.size() && static_cast<u8>(data_[pos_]) >= 0xf0) pos_ += std::max<usize>(1, static_cast<u8>(data_[pos_]) & 0x0f);
        pos_ = std::min(pos_, data_.size());
    }

private:
    std::span<const std::byte> data_;
    usize pos_ = 0;
    bool ok_ = true;
};

// The struct, class, interface, union and enum leaves, the _st forms normalized; 0 for other records.
u16 udt_leaf(u16 kind) {
    switch (kind) {
    case leaf::class_: case leaf::class_st: return leaf::class_;
    case leaf::structure: case leaf::structure_st: return leaf::structure;
    case leaf::interface: return leaf::interface;
    case leaf::union_: case leaf::union_st: return leaf::union_;
    case leaf::enum_: case leaf::enum_st: return leaf::enum_;
    default: return 0;
    }
}

bool length_prefixed_udt(u16 kind) { return kind >= leaf::class_st && kind <= leaf::enum_st; }

bool is_array(u16 kind) { return kind == leaf::array || kind == leaf::array_st; }

std::string calling_convention(u8 cc) {
    switch (cc) {
    case 0x00: return "__cdecl";
    case 0x04: return "__fastcall";
    case 0x07: return "__stdcall";
    case 0x0b: return "__thiscall";
    case 0x18: return "__vectorcall";
    default: return "";
    }
}

} // namespace

std::string simple_type_name(TypeIndex index) {
    std::string base;
    switch (index & 0xff) {
    case 0x03: base = "void"; break;
    case 0x08: base = "HRESULT"; break;
    case 0x10: base = "signed char"; break;
    case 0x20: base = "unsigned char"; break;
    case 0x70: base = "char"; break;
    case 0x71: base = "wchar_t"; break;
    case 0x7a: base = "char16_t"; break;
    case 0x7b: base = "char32_t"; break;
    case 0x7c: base = "char8_t"; break;
    case 0x68: base = "__int8"; break;
    case 0x69: base = "unsigned __int8"; break;
    case 0x11: case 0x72: base = "short"; break;
    case 0x21: case 0x73: base = "unsigned short"; break;
    case 0x12: base = "long"; break;
    case 0x22: base = "unsigned long"; break;
    case 0x74: base = "int"; break;
    case 0x75: base = "unsigned int"; break;
    case 0x13: case 0x76: base = "__int64"; break;
    case 0x23: case 0x77: base = "unsigned __int64"; break;
    case 0x14: case 0x78: base = "__int128"; break;
    case 0x24: case 0x79: base = "unsigned __int128"; break;
    case 0x30: base = "bool"; break;
    case 0x31: base = "__bool16"; break;
    case 0x32: base = "__bool32"; break;
    case 0x33: base = "__bool64"; break;
    case 0x40: base = "float"; break;
    case 0x41: base = "double"; break;
    case 0x42: base = "long double"; break;
    case 0x46: base = "_Float16"; break;
    default: base = std::format("<simple {:#x}>", index & 0xff); break;
    }
    // Bits 8-11 give the pointer mode: 0 the type itself, 1-6 near/far/huge, 32 and 64-bit pointers.
    return (index & 0xf00) != 0 ? base + "*" : base;
}

u64 simple_type_size(TypeIndex index, usize pointer_size) {
    switch ((index >> 8) & 0xf) {
    case 0: break;
    case 1: return 2;
    case 2: case 3: case 4: case 5: return 4;
    case 6: return 8;
    default: return pointer_size;
    }
    switch (index & 0xff) {
    case 0x03: return 0;
    case 0x10: case 0x20: case 0x70: case 0x68: case 0x69: case 0x30: case 0x7c: return 1;
    case 0x11: case 0x21: case 0x72: case 0x73: case 0x71: case 0x7a: case 0x31: case 0x46: return 2;
    case 0x08: case 0x12: case 0x22: case 0x74: case 0x75: case 0x7b: case 0x32: case 0x40: return 4;
    case 0x13: case 0x23: case 0x76: case 0x77: case 0x33: case 0x41: return 8;
    case 0x42: return 10;
    case 0x14: case 0x24: case 0x78: case 0x79: return 16;
    default: return 0;
    }
}

bool simple_type_signed(TypeIndex index) {
    if ((index & 0xf00) != 0) return false;
    switch (index & 0xff) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:  // signed char, short, long, __int64, __int128
    case 0x68: case 0x70: case 0x72: case 0x74: case 0x76: case 0x78:  // __int8, char, the sized ints
    case 0x40: case 0x41: case 0x42: case 0x46: return true;
    default: return false;
    }
}

Result<TypeStream> TypeStream::parse(std::vector<std::byte> records, TypeIndex first) {
    TypeStream s;
    s.bytes_ = std::move(records);
    s.first_ = first;
    for (usize pos = 0; pos + 4 <= s.bytes_.size();) {
        u16 length = 0;
        std::memcpy(&length, s.bytes_.data() + pos, 2);
        if (length < 2 || pos + 2 + length > s.bytes_.size())
            return make_error(ErrorCode::parse, "type record {:#x} at offset {:#x} runs past the end of the stream", first + s.records_.size(), pos);
        s.records_.emplace_back(static_cast<u32>(pos + 2), static_cast<u32>(length));
        pos += 2 + static_cast<usize>(length);
    }
    for (TypeIndex i = s.first_; i < s.end(); ++i) {
        const auto u = s.udt(i);
        if (!u || u->forward_ref()) continue;
        if (!u->unique_name.empty()) s.by_unique_name_.try_emplace(u->unique_name, i);
        if (!u->name.empty()) s.by_name_.try_emplace(u->name, i);
    }
    return s;
}

Result<TypeStream> TypeStream::from_debug_t(std::span<const std::byte> section) {
    if (section.size() < 4) return make_error(ErrorCode::parse, ".debug$T section too small");
    u32 signature = 0;
    std::memcpy(&signature, section.data(), 4);
    if (signature != 4) return make_error(ErrorCode::unsupported, ".debug$T signature {} (only CodeView C13, signature 4, is read)", signature);
    return parse(std::vector<std::byte>(section.begin() + 4, section.end()));
}

std::optional<TypeStream::Record> TypeStream::record(TypeIndex index) const {
    if (index < first_ || index >= end()) return std::nullopt;
    const auto [offset, length] = records_[index - first_];
    Record r;
    std::memcpy(&r.leaf, bytes_.data() + offset, 2);
    r.data = std::span<const std::byte>(bytes_.data() + offset + 2, length - 2);
    return r;
}

std::optional<Udt> TypeStream::udt(TypeIndex index) const {
    const auto r = record(index);
    if (!r) return std::nullopt;
    Cursor c(r->data);
    Udt u;
    u.leaf = udt_leaf(r->leaf);
    switch (u.leaf) {
    case leaf::class_:
    case leaf::structure:
    case leaf::interface:
        u.count = c.read<u16>();
        u.property = c.read<u16>();
        u.field_list = c.read<u32>();
        u.derived = c.read<u32>();
        u.vshape = c.read<u32>();
        u.size = static_cast<u64>(c.numeric());
        break;
    case leaf::union_:
        u.count = c.read<u16>();
        u.property = c.read<u16>();
        u.field_list = c.read<u32>();
        u.size = static_cast<u64>(c.numeric());
        break;
    case leaf::enum_:
        u.count = c.read<u16>();
        u.property = c.read<u16>();
        u.underlying = c.read<u32>();
        u.field_list = c.read<u32>();
        break;
    default: return std::nullopt;
    }
    const bool st = length_prefixed_udt(r->leaf);
    u.name = c.name(st);
    if ((u.property & 0x200) != 0 && c.remaining() > 0) u.unique_name = c.name(st);
    if (!c.ok()) return std::nullopt;
    return u;
}

std::optional<Pointer> TypeStream::pointer(TypeIndex index) const {
    const auto r = record(index);
    if (!r || r->leaf != leaf::pointer) return std::nullopt;
    Cursor c(r->data);
    Pointer p;
    p.referent = c.read<u32>();
    const u32 attributes = c.read<u32>();
    if (!c.ok()) return std::nullopt;
    p.mode = static_cast<u8>((attributes >> 5) & 7);
    p.is_volatile = (attributes & 0x200) != 0;
    p.is_const = (attributes & 0x400) != 0;
    p.size = static_cast<u8>((attributes >> 13) & 0x3f);
    return p;
}

std::optional<Function> TypeStream::function(TypeIndex index) const {
    const auto r = record(index);
    if (!r || (r->leaf != leaf::procedure && r->leaf != leaf::mfunction)) return std::nullopt;
    Cursor c(r->data);
    Function f;
    f.return_type = c.read<u32>();
    if (r->leaf == leaf::mfunction) {
        f.class_type = c.read<u32>();
        f.this_type = c.read<u32>();
    }
    f.calling_convention = c.read<u8>();
    c.read<u8>();   // function attributes
    c.read<u16>();  // parameter count
    const TypeIndex arglist = c.read<u32>();
    if (!c.ok()) return std::nullopt;
    if (const auto args = record(arglist); args && args->leaf == leaf::arglist) {
        Cursor a(args->data);
        const u32 count = a.read<u32>();
        for (u32 i = 0; i < count && a.ok(); ++i) f.parameters.push_back(a.read<u32>());
        if (!a.ok()) return std::nullopt;
    }
    return f;
}

std::optional<Array> TypeStream::array(TypeIndex index) const {
    const auto r = record(index);
    if (!r || !is_array(r->leaf)) return std::nullopt;
    Cursor c(r->data);
    Array a;
    a.element = c.read<u32>();
    c.read<u32>();  // the index type
    a.size = static_cast<u64>(c.numeric());
    return c.ok() ? std::optional(a) : std::nullopt;
}

std::optional<Bitfield> TypeStream::bitfield(TypeIndex index) const {
    const auto r = record(index);
    if (!r || r->leaf != leaf::bitfield) return std::nullopt;
    Cursor c(r->data);
    Bitfield b;
    b.type = c.read<u32>();
    b.length = c.read<u8>();
    b.position = c.read<u8>();
    return c.ok() ? std::optional(b) : std::nullopt;
}

Result<std::vector<Member>> TypeStream::field_list(TypeIndex index) const {
    std::vector<Member> out;
    std::set<TypeIndex> seen;
    for (TypeIndex current = index; current != 0;) {
        if (!seen.insert(current).second) break;
        const auto r = record(current);
        if (!r || r->leaf != leaf::fieldlist) return make_error(ErrorCode::parse, "type {:#x} is not a field list", current);
        Cursor c(r->data);
        TypeIndex next = 0;
        while (c.remaining() >= 2) {
            Member m;
            m.leaf = c.read<u16>();
            bool st = true;
            switch (m.leaf) {
            case leaf::enumerate_st: m.leaf = leaf::enumerate; break;
            case leaf::friendfcn_st: m.leaf = leaf::friendfcn; break;
            case leaf::member_st: m.leaf = leaf::member; break;
            case leaf::stmember_st: m.leaf = leaf::stmember; break;
            case leaf::method_st: m.leaf = leaf::method; break;
            case leaf::nesttype_st: m.leaf = leaf::nesttype; break;
            case leaf::onemethod_st: m.leaf = leaf::onemethod; break;
            case leaf::nesttypeex_st: m.leaf = leaf::nesttypeex; break;
            default: st = false; break;
            }
            switch (m.leaf) {
            case leaf::bclass:
                m.attribute = c.read<u16>();
                m.type = c.read<u32>();
                m.offset = c.numeric();
                break;
            case leaf::vbclass:
            case leaf::ivbclass:
                m.attribute = c.read<u16>();
                m.type = c.read<u32>();
                c.read<u32>();  // the vbptr's type
                m.offset = c.numeric();  // the vbptr's offset
                c.numeric();             // the base's index in the virtual base table
                break;
            case leaf::index:
                c.read<u16>();
                next = c.read<u32>();
                break;
            case leaf::vfunctab:
            case leaf::friendcls:
                c.read<u16>();
                m.type = c.read<u32>();
                break;
            case leaf::vfuncoff:
                c.read<u16>();
                m.type = c.read<u32>();
                m.offset = c.read<i32>();
                break;
            case leaf::friendfcn:
            case leaf::nesttype:
                c.read<u16>();
                m.type = c.read<u32>();
                m.name = c.name(st);
                break;
            case leaf::nesttypeex:
            case leaf::stmember:
                m.attribute = c.read<u16>();
                m.type = c.read<u32>();
                m.name = c.name(st);
                break;
            case leaf::enumerate:
                m.attribute = c.read<u16>();
                m.offset = c.numeric();
                m.name = c.name(st);
                break;
            case leaf::member:
                m.attribute = c.read<u16>();
                m.type = c.read<u32>();
                m.offset = c.numeric();
                m.name = c.name(st);
                break;
            case leaf::method:
                m.method_count = c.read<u16>();
                m.type = c.read<u32>();
                m.name = c.name(st);
                break;
            case leaf::onemethod:
                m.attribute = c.read<u16>();
                m.type = c.read<u32>();
                if (m.introducing_virtual()) m.vtable_offset = c.read<u32>();
                m.name = c.name(st);
                break;
            default: return make_error(ErrorCode::unsupported, "field list {:#x}: unknown entry kind {:#x}", current, m.leaf);
            }
            if (!c.ok()) return make_error(ErrorCode::parse, "field list {:#x}: truncated entry {:#x}", current, m.leaf);
            if (m.leaf != leaf::index) out.push_back(std::move(m));
            c.skip_padding();
        }
        current = next;
    }
    return out;
}

Result<std::vector<MethodEntry>> TypeStream::method_list(TypeIndex index, u16 count) const {
    const auto r = record(index);
    if (!r || r->leaf != leaf::methodlist) return make_error(ErrorCode::parse, "type {:#x} is not a method list", index);
    Cursor c(r->data);
    std::vector<MethodEntry> out;
    while (c.remaining() >= 8 && out.size() < count) {
        MethodEntry e;
        e.attribute = c.read<u16>();
        c.read<u16>();
        e.type = c.read<u32>();
        if (e.introducing_virtual()) e.vtable_offset = c.read<u32>();
        if (!c.ok()) return make_error(ErrorCode::parse, "method list {:#x}: truncated", index);
        out.push_back(e);
    }
    return out;
}

std::optional<u16> TypeStream::vtshape_count(TypeIndex index) const {
    const auto r = record(index);
    if (!r || r->leaf != leaf::vtshape) return std::nullopt;
    Cursor c(r->data);
    const u16 count = c.read<u16>();
    return c.ok() ? std::optional(count) : std::nullopt;
}

std::optional<TypeIndex> TypeStream::function_type_of_id(TypeIndex index) const {
    const auto r = record(index);
    if (!r || (r->leaf != leaf::func_id && r->leaf != leaf::mfunc_id)) return std::nullopt;
    Cursor c(r->data);
    c.read<u32>();  // the scope, or the class
    const TypeIndex type = c.read<u32>();
    return c.ok() ? std::optional(type) : std::nullopt;
}

std::optional<TypeIndex> TypeStream::definition(TypeIndex index) const {
    const auto u = udt(index);
    if (!u) return std::nullopt;
    if (!u->forward_ref()) return index;
    if (!u->unique_name.empty())
        if (auto it = by_unique_name_.find(u->unique_name); it != by_unique_name_.end()) return it->second;
    if (auto it = by_name_.find(u->name); it != by_name_.end()) return it->second;
    return std::nullopt;
}

std::optional<TypeIndex> TypeStream::find_definition(std::string_view name) const {
    if (auto it = by_name_.find(std::string(name)); it != by_name_.end()) return it->second;
    return std::nullopt;
}

std::vector<TypeIndex> TypeStream::definitions() const {
    std::vector<TypeIndex> out;
    for (TypeIndex i = first_; i < end(); ++i)
        if (const auto u = udt(i); u && !u->forward_ref()) out.push_back(i);
    return out;
}

TypeIndex TypeStream::unmodified(TypeIndex index) const {
    for (int guard = 0; guard < 16; ++guard) {
        const auto r = record(index);
        if (!r || r->leaf != leaf::modifier) break;
        Cursor c(r->data);
        const TypeIndex base = c.read<u32>();
        if (!c.ok()) break;
        index = base;
    }
    return index;
}

u64 TypeStream::size_of(TypeIndex index, usize pointer_size) const {
    if (index < kFirstTypeIndex) return simple_type_size(index, pointer_size);
    const auto r = record(index);
    if (!r) return 0;
    Cursor c(r->data);
    switch (r->leaf) {
    case leaf::modifier: return size_of(c.read<u32>(), pointer_size);
    case leaf::pointer: {
        const auto p = pointer(index);
        return p && p->size ? p->size : pointer_size;
    }
    case leaf::array:
    case leaf::array_st: {
        c.read<u32>();
        c.read<u32>();
        return static_cast<u64>(c.numeric());
    }
    case leaf::bitfield: return size_of(c.read<u32>(), pointer_size);
    case leaf::class_:
    case leaf::structure:
    case leaf::interface:
    case leaf::union_:
    case leaf::enum_:
    case leaf::class_st:
    case leaf::structure_st:
    case leaf::union_st:
    case leaf::enum_st: {
        const auto def = definition(index);
        const auto u = def ? udt(*def) : udt(index);
        if (!u) return 0;
        return u->is_enum() ? size_of(u->underlying, pointer_size) : u->size;
    }
    default: return 0;
    }
}

std::string TypeStream::name_of(TypeIndex index) const {
    if (index < kFirstTypeIndex) return simple_type_name(index);
    const auto r = record(index);
    if (!r) return std::format("<type {:#x}>", index);
    Cursor c(r->data);
    switch (r->leaf) {
    case leaf::modifier: {
        const TypeIndex base = c.read<u32>();
        const u16 attributes = c.read<u16>();
        std::string prefix;
        if (attributes & 1) prefix += "const ";
        if (attributes & 2) prefix += "volatile ";
        return prefix + name_of(base);
    }
    case leaf::pointer: {
        const auto p = pointer(index);
        if (!p) break;
        std::string suffix = p->mode == 1 ? "&" : p->mode == 4 ? "&&" : "*";
        if (p->is_const) suffix += " const";
        if (const auto f = function(p->referent)) {
            std::vector<std::string> params;
            for (TypeIndex t : f->parameters) params.push_back(name_of(t));
            return std::format("{} ({}{})({})", name_of(f->return_type), calling_convention(f->calling_convention), suffix, [&] {
                std::string out;
                for (usize i = 0; i < params.size(); ++i) out += (i ? ", " : "") + params[i];
                return out;
            }());
        }
        return name_of(p->referent) + suffix;
    }
    case leaf::array:
    case leaf::array_st: {
        // Nested arrays read outside in: int[2][3] is an array of two int[3].
        std::string dims;
        TypeIndex current = index;
        for (int guard = 0; guard < 16; ++guard) {
            const auto a = record(current);
            if (!a || !is_array(a->leaf)) break;
            Cursor ac(a->data);
            const TypeIndex element = ac.read<u32>();
            ac.read<u32>();
            const u64 size = static_cast<u64>(ac.numeric());
            const u64 element_size = size_of(element, 4);
            dims += element_size ? std::format("[{}]", size / element_size) : std::string("[]");
            current = element;
        }
        return name_of(current) + dims;
    }
    case leaf::bitfield: return name_of(c.read<u32>());
    case leaf::procedure:
    case leaf::mfunction: {
        const auto f = function(index);
        if (!f) break;
        std::string out = name_of(f->return_type) + " (";
        for (usize i = 0; i < f->parameters.size(); ++i) out += (i ? ", " : "") + name_of(f->parameters[i]);
        return out + ")";
    }
    case leaf::class_:
    case leaf::structure:
    case leaf::interface:
    case leaf::union_:
    case leaf::enum_:
    case leaf::class_st:
    case leaf::structure_st:
    case leaf::union_st:
    case leaf::enum_st: {
        const auto u = udt(index);
        return u ? u->name : std::format("<type {:#x}>", index);
    }
    default: break;
    }
    return std::format("<type {:#x}>", index);
}

std::string TypeStream::key_of(TypeIndex index) const {
    const auto u = udt(index);
    if (!u) return {};
    if (!u->name.starts_with('<')) return u->name;
    if (!u->unique_name.empty()) return u->unique_name;
    return std::format("{}@{:x}", u->name, definition(index).value_or(index));
}

std::string TypeStream::udt_name(TypeIndex index) const {
    for (int guard = 0; guard < 16 && index >= kFirstTypeIndex; ++guard) {
        const auto r = record(index);
        if (!r) return {};
        if (r->leaf == leaf::modifier || is_array(r->leaf)) {
            Cursor c(r->data);
            index = c.read<u32>();
            continue;
        }
        if (const u16 kind = udt_leaf(r->leaf); kind != 0 && kind != leaf::enum_) return key_of(index);
        return {};
    }
    return {};
}

std::string TypeStream::pointee_udt(TypeIndex index) const {
    for (int guard = 0; guard < 16 && index >= kFirstTypeIndex; ++guard) {
        const auto r = record(index);
        if (!r) return {};
        if (r->leaf == leaf::modifier) {
            Cursor c(r->data);
            index = c.read<u32>();
            continue;
        }
        if (r->leaf == leaf::pointer) {
            const auto p = pointer(index);
            if (!p || p->mode == 2 || p->mode == 3) return {};
            // A pointer to an array of structs points to the struct.
            return udt_name(p->referent);
        }
        return {};
    }
    return {};
}

} // namespace decomp::codeview
