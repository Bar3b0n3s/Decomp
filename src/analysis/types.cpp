#include "analysis/types.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace decomp {

using codeview::Member;
using codeview::TypeIndex;
using codeview::TypeStream;
namespace leaf = codeview::leaf;

std::string_view to_string(TypeKind kind) {
    switch (kind) {
    case TypeKind::structure: return "struct";
    case TypeKind::class_: return "class";
    case TypeKind::union_: return "union";
    case TypeKind::enumeration: return "enum";
    }
    return "struct";
}

const FieldLayout* TypeLayout::field_at(u64 offset) const {
    for (const FieldLayout& f : fields)
        if (f.size > 0 && offset >= f.offset && offset - f.offset < f.size) return &f;
    return nullptr;
}

namespace {

// An array's dimensions, outermost first; empty for other types.
std::vector<u64> dimensions_of(const TypeStream& types, TypeIndex type, usize pointer_size) {
    std::vector<u64> out;
    for (int guard = 0; guard < 16; ++guard) {
        const auto array = types.array(types.unmodified(type));
        if (!array) break;
        const u64 element = types.size_of(array->element, pointer_size);
        out.push_back(element > 0 ? array->size / element : 0);
        type = array->element;
    }
    return out;
}

// The structs, classes, unions and enums a type names, through modifiers, pointers, arrays, bitfields and
// function types; each once, in the order met.
void collect_named(const TypeStream& types, TypeIndex index, std::vector<std::string>& out, int depth) {
    if (depth > 16 || index < codeview::kFirstTypeIndex) return;
    const auto record = types.record(index);
    if (!record) return;
    const auto add = [&](std::string name) {
        if (!name.empty() && std::ranges::find(out, name) == out.end()) out.push_back(std::move(name));
    };
    if (types.udt(index)) {
        add(types.key_of(index));
        return;
    }
    switch (record->leaf) {
    case leaf::modifier: collect_named(types, types.unmodified(index), out, depth + 1); break;
    case leaf::pointer:
        if (const auto p = types.pointer(index)) collect_named(types, p->referent, out, depth + 1);
        break;
    case leaf::array:
    case leaf::array_st:
        if (const auto a = types.array(index)) collect_named(types, a->element, out, depth + 1);
        break;
    case leaf::bitfield:
        if (const auto b = types.bitfield(index)) collect_named(types, b->type, out, depth + 1);
        break;
    case leaf::procedure:
    case leaf::mfunction:
        if (const auto f = types.function(index)) {
            collect_named(types, f->return_type, out, depth + 1);
            for (const TypeIndex t : f->parameters) collect_named(types, t, out, depth + 1);
        }
        break;
    default: break;
    }
}

std::vector<std::string> named_types(const TypeStream& types, TypeIndex index) {
    std::vector<std::string> out;
    collect_named(types, index, out, 0);
    return out;
}

FieldLayout field_layout(const TypeStream& types, const Member& m, usize pointer_size) {
    FieldLayout f;
    f.name = m.name;
    f.offset = static_cast<u64>(m.offset);
    TypeIndex type = m.type;
    if (const auto bits = types.bitfield(type)) {
        f.bit_offset = bits->position;
        f.bit_width = bits->length;
        type = bits->type;
    }
    f.size = types.size_of(type, pointer_size);
    f.type = types.name_of(type);
    f.udt = types.udt_name(type);
    f.dimensions = dimensions_of(types, type, pointer_size);
    f.pointee = types.pointee_udt(type);
    f.named = named_types(types, type);
    return f;
}

// A method from its attributes and type; nullopt for what the compiler generated (and pseudo methods).
std::optional<MethodLayout> method_layout(const TypeStream& types, const std::string& name, u16 attribute, TypeIndex type, u32 vtable_offset,
                                          usize pointer_size) {
    if ((attribute & 0x0120) != 0) return std::nullopt;  // pseudo (0x20), compiler-generated (0x100)
    const u8 property = static_cast<u8>((attribute >> 2) & 7);
    MethodLayout m;
    m.name = name;
    m.is_static = property == 2;
    m.is_virtual = property == 1 || property == 4 || property == 5 || property == 6;
    m.pure = property == 5 || property == 6;
    if (property == 4 || property == 6) m.slot = vtable_offset / std::max<usize>(pointer_size, 1);
    if (const auto f = types.function(type)) {
        m.return_type = types.name_of(f->return_type);
        for (const TypeIndex p : f->parameters) m.parameters.push_back(p == 0 ? "..." : types.name_of(p));
        if (f->this_type != 0)
            if (const auto self = types.pointer(f->this_type)) m.is_const = types.name_of(self->referent).starts_with("const ");
        // x86 has conventions to choose from: __thiscall is a method's default, __cdecl a static one's.
        const u8 standard = m.is_static ? 0x00 : 0x0b;
        if (pointer_size == 4 && f->calling_convention != standard) m.calling_convention = codeview::calling_convention_name(f->calling_convention);
        collect_named(types, f->return_type, m.named, 0);
        for (const TypeIndex p : f->parameters) collect_named(types, p, m.named, 0);
    }
    return m;
}

std::string base_name(const TypeStream& types, TypeIndex type) {
    std::string name = types.udt_name(type);
    return name.empty() ? types.name_of(type) : name;
}

// Hex digits enough for every offset of a type of `size` bytes (at least two).
int offset_width(u64 size) {
    int width = 2;
    for (u64 limit = 0x100; size > limit && width < 16; limit <<= 4) ++width;
    return width;
}

std::string virtual_text(const VirtualMethod& v) { return v.pure ? v.name + " = 0" : v.name; }

std::string bytes_text(u64 size) { return std::format("{} byte{}", size, size == 1 ? "" : "s"); }

} // namespace

Result<TypeLayout> layout_of(const TypeStream& types, TypeIndex index, usize pointer_size) {
    const auto declared = types.udt(index);
    if (!declared) return make_error(ErrorCode::invalid_argument, "type {:#x} is not a struct, class, union or enum", index);
    const auto definition = types.definition(index);
    if (!definition) return make_error(ErrorCode::not_found, "{} is declared but not defined in the type records", declared->name);
    const codeview::Udt u = *types.udt(*definition);

    TypeLayout layout;
    layout.name = types.key_of(*definition);
    switch (u.leaf) {
    case leaf::class_: layout.kind = TypeKind::class_; break;
    case leaf::union_: layout.kind = TypeKind::union_; break;
    case leaf::enum_: layout.kind = TypeKind::enumeration; break;
    default: layout.kind = TypeKind::structure; break;
    }
    std::vector<Member> members;
    if (u.field_list != 0) {
        auto list = types.field_list(u.field_list);
        if (!list) return std::unexpected(std::move(list.error()).with_context(layout.name));
        members = std::move(*list);
    }

    if (u.is_enum()) {
        layout.size = types.size_of(u.underlying, pointer_size);
        layout.underlying = types.name_of(u.underlying);
        // Compilers write a value in the numeric leaf they like (clang -2 as 0xfffffffe); it is a value
        // of the underlying type.
        const TypeIndex underlying = types.unmodified(u.underlying);
        const u64 bits = std::min<u64>(layout.size, 8) * 8;
        for (const Member& m : members) {
            if (m.leaf != leaf::enumerate) continue;
            i64 value = m.offset;
            if (bits > 0 && bits < 64) {
                const u64 raw = static_cast<u64>(value) & ((u64{1} << bits) - 1);
                const bool negative = codeview::simple_type_signed(underlying) && (raw >> (bits - 1)) != 0;
                value = negative ? static_cast<i64>(raw | ~((u64{1} << bits) - 1)) : static_cast<i64>(raw);
            }
            layout.enumerators.push_back({m.name, value});
        }
        return layout;
    }

    layout.size = u.size;
    if (u.vshape != 0)
        if (const auto slots = types.vtshape_count(u.vshape)) layout.vtable_slots = *slots;
    for (const Member& m : members) {
        switch (m.leaf) {
        case leaf::bclass: layout.bases.push_back({base_name(types, m.type), static_cast<u64>(m.offset), false}); break;
        case leaf::vbclass:
            layout.bases.push_back({base_name(types, m.type), 0, true});
            if (!layout.vbptr) layout.vbptr = static_cast<u64>(m.offset);
            break;
        case leaf::vfunctab:
        case leaf::vfuncoff:
            if (!layout.vfptr) layout.vfptr = static_cast<u64>(m.offset);
            if (layout.vtable_slots == 0)
                if (const auto pointer = types.pointer(m.type))
                    if (const auto slots = types.vtshape_count(pointer->referent)) layout.vtable_slots = *slots;
            break;
        case leaf::member: layout.fields.push_back(field_layout(types, m, pointer_size)); break;
        case leaf::onemethod:
            if (auto method = method_layout(types, m.name, m.attribute, m.type, m.vtable_offset, pointer_size)) layout.methods.push_back(std::move(*method));
            break;
        case leaf::method: {
            auto overloads = types.method_list(m.type, m.method_count);
            if (!overloads) return std::unexpected(std::move(overloads.error()).with_context(layout.name));
            for (const codeview::MethodEntry& e : *overloads)
                if (auto method = method_layout(types, m.name, e.attribute, e.type, e.vtable_offset, pointer_size)) layout.methods.push_back(std::move(*method));
            break;
        }
        case leaf::stmember: layout.statics.push_back({m.name, types.name_of(m.type), named_types(types, m.type)}); break;
        case leaf::nesttype:
        case leaf::nesttypeex: {
            // A type declared in the class is named after it; another name for a type is a typedef.
            NestedType nested{m.name, types.name_of(m.type), true};
            if (types.udt(m.type)) {
                const std::string key = types.key_of(types.definition(m.type).value_or(m.type));
                nested.type = key;
                nested.is_typedef = key != layout.name + "::" + m.name && !key.starts_with(layout.name + "::<");
            }
            layout.nested.push_back(std::move(nested));
            break;
        }
        default: break;  // friends, the indirect virtual bases
        }
    }
    for (const MethodLayout& method : layout.methods)
        if (method.is_virtual) layout.virtuals.push_back({method.name, method.slot, method.pure});
    std::ranges::stable_sort(layout.virtuals, {}, [](const VirtualMethod& v) { return v.slot.value_or(std::numeric_limits<u64>::max()); });
    return layout;
}

TypeCatalog TypeCatalog::from(const TypeStream& types, usize pointer_size) {
    TypeCatalog catalog(pointer_size);
    for (const TypeIndex index : types.definitions())
        if (auto layout = layout_of(types, index, pointer_size)) catalog.add(std::move(*layout));
    return catalog;
}

void TypeCatalog::add(TypeLayout layout) {
    if (by_name_.contains(layout.name)) return;
    by_name_.emplace(layout.name, types_.size());
    types_.push_back(std::move(layout));
}

const TypeLayout* TypeCatalog::find(std::string_view name) const {
    const auto it = by_name_.find(std::string(name));
    return it == by_name_.end() ? nullptr : &types_[it->second];
}

std::optional<TypeCatalog::FieldRef> TypeCatalog::field_ref(std::string_view type, u64 offset, bool innermost) const {
    const TypeLayout* layout = find(type);
    if (!layout) return std::nullopt;
    return resolve(*layout, offset, 0, innermost);
}

std::optional<TypeCatalog::FieldRef> TypeCatalog::resolve(const TypeLayout& layout, u64 offset, int depth, bool innermost) const {
    if (depth > 16 || offset >= layout.size) return std::nullopt;
    for (const FieldLayout& f : layout.fields) {
        if (f.size == 0 || offset < f.offset || offset - f.offset >= f.size) continue;
        u64 within = offset - f.offset;
        std::string path = f.name;
        if (!f.dimensions.empty() && !f.is_bitfield()) {
            u64 elements = 1;
            for (const u64 d : f.dimensions) elements *= std::max<u64>(d, 1);
            if (const u64 stride = f.size / elements; stride > 0) {
                u64 index = within / stride;
                within %= stride;
                std::vector<u64> indices(f.dimensions.size());
                for (usize i = f.dimensions.size(); i-- > 0;) {
                    const u64 extent = std::max<u64>(f.dimensions[i], 1);
                    indices[i] = index % extent;
                    index /= extent;
                }
                for (const u64 i : indices) path += std::format("[{}]", i);
            }
        }
        if (!innermost && within == 0) return FieldRef{std::move(path), &f, true, 0};
        if (!f.udt.empty() && !f.is_bitfield())
            if (const TypeLayout* inner = find(f.udt))
                if (auto ref = resolve(*inner, within, depth + 1, innermost)) {
                    ref->path = path + "." + ref->path;
                    return ref;
                }
        return FieldRef{std::move(path), &f, within == 0, within};
    }
    for (const BaseLayout& b : layout.bases) {
        if (b.is_virtual || offset < b.offset) continue;
        const TypeLayout* base = find(b.name);
        if (!base) continue;
        auto ref = resolve(*base, offset - b.offset, depth + 1, innermost);
        if (!ref) continue;
        // A table pointer of a base other than the first is that base's.
        if (ref->field == nullptr && b.offset != 0 && !ref->path.contains("::")) ref->path = b.name + "::" + ref->path;
        return ref;
    }
    for (const auto& [at, name] : {std::pair(layout.vfptr, "__vfptr"), std::pair(layout.vbptr, "__vbptr")})
        if (at && offset >= *at && offset - *at < pointer_size_) return FieldRef{name, nullptr, offset == *at, offset - *at};
    return std::nullopt;
}

bool system_header(std::string_view path) {
    std::string p = to_lower(path);
    std::ranges::replace(p, '\\', '/');
    static constexpr std::string_view kMarkers[] = {
        "/microsoft visual studio", "/windows kits/", "/microsoft sdks/", "/microsoft platform sdk", "/platformsdk/", "/platform sdk/",
        "/vc98/include", "/vc98/mfc", "/vc98/atl", "/vc/include", "/vc/atlmfc", "/vc/platformsdk", "/vc7/include", "/vc7/atlmfc",
        "/vc7/platformsdk", "/dxsdk", "/directx sdk", "/directx9", "/ucrt/", "/include/um/", "/include/shared/", "/lib/clang/", "/stlport",
    };
    return std::ranges::any_of(kMarkers, [&](std::string_view m) { return p.find(m) != std::string::npos; });
}

std::vector<std::string> types_of_function(const ProgramTypes& types, u64 va) {
    std::vector<std::string> out;
    const auto it = types.function_types.find(va);
    if (it == types.function_types.end()) return out;
    const auto function = types.stream.function(it->second);
    if (!function) return out;
    const auto add = [&](std::string name) {
        if (!name.empty() && std::ranges::find(out, name) == out.end()) out.push_back(std::move(name));
    };
    if (function->class_type != 0) add(types.stream.udt_name(function->class_type));
    for (const TypeIndex t : function->parameters) {
        add(types.stream.udt_name(t));
        add(types.stream.pointee_udt(t));
    }
    add(types.stream.udt_name(function->return_type));
    add(types.stream.pointee_udt(function->return_type));
    return out;
}

std::vector<std::string> compare_layouts(const TypeLayout& actual, const TypeLayout& expected) {
    std::vector<std::string> out;
    if (actual.kind != expected.kind) out.push_back(std::format("{}, expected {}", to_string(actual.kind), to_string(expected.kind)));
    if (actual.size != expected.size) out.push_back(std::format("size {}, expected {}", actual.size, expected.size));

    const auto base_text = [](const BaseLayout& b) {
        return b.is_virtual ? std::format("virtual base {}", b.name) : std::format("base {} at +{:#x}", b.name, b.offset);
    };
    for (usize i = 0; i < std::max(actual.bases.size(), expected.bases.size()); ++i) {
        if (i >= actual.bases.size()) out.push_back(base_text(expected.bases[i]) + " missing");
        else if (i >= expected.bases.size()) out.push_back(base_text(actual.bases[i]) + " not expected");
        else if (const std::string a = base_text(actual.bases[i]), e = base_text(expected.bases[i]); a != e)
            out.push_back(std::format("{}, expected {}", a, e));
    }

    const auto pointer_text = [](std::optional<u64> at) { return at ? std::format("at +{:#x}", *at) : std::string("none"); };
    if (actual.vfptr != expected.vfptr) out.push_back(std::format("vfptr {}, expected {}", pointer_text(actual.vfptr), pointer_text(expected.vfptr)));
    if (actual.vbptr != expected.vbptr) out.push_back(std::format("vbptr {}, expected {}", pointer_text(actual.vbptr), pointer_text(expected.vbptr)));
    if (actual.vtable_slots != expected.vtable_slots)
        out.push_back(std::format("{} vtable slots, expected {}", actual.vtable_slots, expected.vtable_slots));

    std::map<u64, std::string> actual_slots, expected_slots;
    std::vector<std::string> actual_overrides, expected_overrides;
    for (const auto& [layout, slots, overrides] : {std::tuple(&actual, &actual_slots, &actual_overrides), std::tuple(&expected, &expected_slots, &expected_overrides)})
        for (const VirtualMethod& v : layout->virtuals) {
            if (v.slot) slots->emplace(*v.slot, virtual_text(v));
            else overrides->push_back(virtual_text(v));
        }
    std::map<u64, std::pair<std::string, std::string>> slots;
    for (const auto& [slot, name] : actual_slots) slots[slot].first = name;
    for (const auto& [slot, name] : expected_slots) slots[slot].second = name;
    for (const auto& [slot, names] : slots)
        if (names.first != names.second)
            out.push_back(std::format("virtual slot {}: {}, expected {}", slot, names.first.empty() ? "none" : names.first,
                                      names.second.empty() ? "none" : names.second));
    for (const std::string& name : actual_overrides)
        if (std::ranges::find(expected_overrides, name) == expected_overrides.end()) out.push_back(std::format("override {} not expected", name));
    for (const std::string& name : expected_overrides)
        if (std::ranges::find(actual_overrides, name) == actual_overrides.end()) out.push_back(std::format("override {} missing", name));

    // Fields by name; several of one name (anonymous members) in order.
    const auto bits_text = [](const FieldLayout& f) {
        return f.is_bitfield() ? std::format("{} bits at bit {}", *f.bit_width, f.bit_offset.value_or(0)) : std::string("no bits");
    };
    std::vector<bool> matched(actual.fields.size(), false);
    for (const FieldLayout& e : expected.fields) {
        usize i = 0;
        while (i < actual.fields.size() && (matched[i] || actual.fields[i].name != e.name)) ++i;
        if (i == actual.fields.size()) {
            out.push_back(std::format("field {} missing (expected at +{:#x})", e.name, e.offset));
            continue;
        }
        matched[i] = true;
        const FieldLayout& a = actual.fields[i];
        if (a.offset != e.offset) out.push_back(std::format("field {} at +{:#x}, expected +{:#x}", e.name, a.offset, e.offset));
        if (a.size != e.size) out.push_back(std::format("field {} is {} bytes, expected {}", e.name, a.size, e.size));
        if (a.bit_offset != e.bit_offset || a.bit_width != e.bit_width)
            out.push_back(std::format("field {}: {}, expected {}", e.name, bits_text(a), bits_text(e)));
        if (a.type != e.type) out.push_back(std::format("field {}: {}, expected {}", e.name, a.type, e.type));
    }
    for (usize i = 0; i < actual.fields.size(); ++i)
        if (!matched[i]) out.push_back(std::format("field {} at +{:#x} not expected", actual.fields[i].name, actual.fields[i].offset));

    if (actual.underlying != expected.underlying)
        out.push_back(std::format("underlying type {}, expected {}", actual.underlying.empty() ? "none" : actual.underlying,
                                  expected.underlying.empty() ? "none" : expected.underlying));
    for (const Enumerator& e : expected.enumerators) {
        const auto it = std::ranges::find(actual.enumerators, e.name, &Enumerator::name);
        if (it == actual.enumerators.end()) out.push_back(std::format("enumerator {} missing (expected {})", e.name, e.value));
        else if (it->value != e.value) out.push_back(std::format("enumerator {} = {}, expected {}", e.name, it->value, e.value));
    }
    for (const Enumerator& a : actual.enumerators)
        if (std::ranges::find(expected.enumerators, a.name, &Enumerator::name) == expected.enumerators.end())
            out.push_back(std::format("enumerator {} = {} not expected", a.name, a.value));
    return out;
}

std::string field_declaration(const FieldLayout& field) {
    const std::string& type = field.type;
    std::string out;
    // A pointer to a function: the name goes in the parentheses, "void (__cdecl* cb)(int)", and so do an
    // array's dimensions, "void (__cdecl* table[4])(int)".
    const auto group = type.find("*)");
    const auto bracket = type.find('[');
    if (group != std::string::npos && type.find('(') < group) {
        std::string tail = type.substr(group + 1), dims;
        if (const auto close = tail.rfind(')'); close != std::string::npos && close + 1 < tail.size() && tail[close + 1] == '[') {
            dims = tail.substr(close + 1);
            tail.resize(close + 1);
        }
        out = type.substr(0, group + 1) + " " + field.name + dims + tail;
    } else if (bracket != std::string::npos) {
        out = type.substr(0, bracket) + " " + field.name + type.substr(bracket);
    } else {
        out = type + " " + field.name;
    }
    if (field.bit_width) out += std::format(" : {}", *field.bit_width);
    return out;
}

std::string to_text(const TypeLayout& layout) {
    std::string out = std::format("{} {}", to_string(layout.kind), layout.name);
    if (layout.kind == TypeKind::enumeration) {
        out += std::format(" : {}  // {}\n", layout.underlying, bytes_text(layout.size));
        for (const Enumerator& e : layout.enumerators) out += std::format("  {} = {}\n", e.name, e.value);
        return out;
    }
    for (usize i = 0; i < layout.bases.size(); ++i)
        out += std::format("{}{}{}", i == 0 ? " : " : ", ", layout.bases[i].is_virtual ? "virtual " : "", layout.bases[i].name);
    out += "  // " + bytes_text(layout.size);
    if (layout.vtable_slots > 0) out += std::format(", vtable of {}", layout.vtable_slots);
    out += "\n";

    // Bases, table pointers and fields, in offset order.
    const int width = offset_width(layout.size);
    std::vector<std::pair<u64, std::string>> lines;
    for (const BaseLayout& b : layout.bases)
        if (!b.is_virtual) lines.emplace_back(b.offset, std::format("{} (base)", b.name));
    if (layout.vfptr) lines.emplace_back(*layout.vfptr, "vfptr");
    if (layout.vbptr) lines.emplace_back(*layout.vbptr, "vbptr");
    for (const FieldLayout& f : layout.fields)
        lines.emplace_back(f.offset, f.is_bitfield() ? std::format("{}  // bit {}", field_declaration(f), *f.bit_offset) : field_declaration(f));
    std::ranges::stable_sort(lines, {}, &std::pair<u64, std::string>::first);
    for (const auto& [offset, text] : lines) out += std::format("  +0x{:0{}x}  {}\n", offset, width, text);
    for (const VirtualMethod& v : layout.virtuals)
        out += v.slot ? std::format("  virtual {}  // slot {}\n", virtual_text(v), *v.slot) : std::format("  virtual {}  // override\n", virtual_text(v));
    return out;
}

Json to_json(const TypeLayout& layout) {
    Json j = {{"name", layout.name}, {"kind", to_string(layout.kind)}, {"size", layout.size}};
    if (layout.kind == TypeKind::enumeration) {
        j["underlying"] = layout.underlying;
        Json enumerators = Json::array();
        for (const Enumerator& e : layout.enumerators) enumerators.push_back({{"name", e.name}, {"value", e.value}});
        j["enumerators"] = std::move(enumerators);
        return j;
    }
    if (!layout.bases.empty()) {
        Json bases = Json::array();
        for (const BaseLayout& b : layout.bases) {
            Json base = {{"name", b.name}};
            if (b.is_virtual) base["virtual"] = true;
            else base["offset"] = b.offset;
            bases.push_back(std::move(base));
        }
        j["bases"] = std::move(bases);
    }
    if (layout.vfptr) j["vfptr"] = *layout.vfptr;
    if (layout.vbptr) j["vbptr"] = *layout.vbptr;
    if (layout.vtable_slots > 0) j["vtable_slots"] = layout.vtable_slots;
    if (!layout.virtuals.empty()) {
        Json virtuals = Json::array();
        for (const VirtualMethod& v : layout.virtuals) {
            Json method = {{"name", v.name}};
            if (v.slot) method["slot"] = *v.slot;
            else method["override"] = true;
            if (v.pure) method["pure"] = true;
            virtuals.push_back(std::move(method));
        }
        j["virtuals"] = std::move(virtuals);
    }
    Json fields = Json::array();
    for (const FieldLayout& f : layout.fields) {
        Json field = {{"name", f.name}, {"offset", f.offset}, {"size", f.size}, {"type", f.type}};
        if (f.bit_width) {
            field["bit_offset"] = *f.bit_offset;
            field["bit_width"] = *f.bit_width;
        }
        if (!f.dimensions.empty()) field["dimensions"] = f.dimensions;
        if (!f.udt.empty()) field["udt"] = f.udt;
        if (!f.pointee.empty()) field["pointee"] = f.pointee;
        fields.push_back(std::move(field));
    }
    j["fields"] = std::move(fields);
    return j;
}

} // namespace decomp
