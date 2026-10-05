#include "analysis/declarations.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <optional>

namespace decomp {

namespace {

constexpr int kMaxDepth = 16;

// A qualified name's parts: "game::Shape" -> game, Shape (not inside template arguments).
std::vector<std::string> name_parts(std::string_view name) {
    std::vector<std::string> out;
    usize start = 0;
    int depth = 0;
    for (usize i = 0; i < name.size(); ++i) {
        if (name[i] == '<') ++depth;
        else if (name[i] == '>') --depth;
        else if (depth == 0 && name.substr(i, 2) == "::") {
            out.emplace_back(name.substr(start, i - start));
            start = i + 2;
            ++i;
        }
    }
    out.emplace_back(name.substr(start));
    return out;
}

std::string unqualified(std::string_view name) { return name_parts(name).back(); }

bool template_name(std::string_view name) { return name.find('<') != std::string_view::npos && !anonymous_type_name(name); }

bool is_record(const TypeLayout& layout) { return layout.kind != TypeKind::enumeration; }

// The class a nested type is declared in ("Outer" for "Outer::Inner"), when the catalog has it.
std::optional<std::string> enclosing_class(std::string_view name, const TypeCatalog& catalog) {
    const auto parts = name_parts(name);
    if (parts.size() < 2) return std::nullopt;
    std::string outer = parts[0];
    for (usize i = 1; i + 1 < parts.size(); ++i) outer += "::" + parts[i];
    const TypeLayout* layout = catalog.find(outer);
    if (!layout || !is_record(*layout)) return std::nullopt;
    return outer;
}

// The namespaces a type is declared in: its qualifiers, unless it is nested in a class.
std::vector<std::string> namespaces_of(std::string_view name, const TypeCatalog& catalog) {
    if (enclosing_class(name, catalog)) return {};
    auto parts = name_parts(name);
    parts.pop_back();
    return parts;
}

u64 round_up(u64 value, u64 alignment) { return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment; }

u64 elements_of(const FieldLayout& f) {
    u64 n = 1;
    for (const u64 d : f.dimensions) n *= std::max<u64>(d, 1);
    return n;
}

// The alignment intrinsic types declare (__declspec(align)), which their fields do not show.
std::optional<u64> intrinsic_alignment(std::string_view name) {
    if (name == "__m64") return 8;
    if (name.starts_with("__m128")) return 16;
    if (name.starts_with("__m256")) return 32;
    if (name.starts_with("__m512")) return 64;
    return std::nullopt;
}

u64 type_alignment(const TypeLayout& layout, const TypeCatalog& catalog, int depth);

u64 field_alignment(const FieldLayout& f, const TypeCatalog& catalog, int depth) {
    if (f.is_bitfield()) return std::max<u64>(f.size, 1);
    if (!f.udt.empty())
        if (const TypeLayout* inner = catalog.find(f.udt)) return type_alignment(*inner, catalog, depth + 1);
    const u64 element = f.size / elements_of(f);
    if (element == 0) return 1;
    return std::min<u64>(element & (~element + 1), 16);  // a scalar's size (its lowest set bit for odd sizes)
}

// The end of the type's last byte that a member occupies (bases, table pointers, fields).
u64 members_end(const TypeLayout& layout, const TypeCatalog& catalog) {
    u64 end = 0;
    if (layout.vfptr) end = std::max(end, *layout.vfptr + catalog.pointer_size());
    if (layout.vbptr) end = std::max(end, *layout.vbptr + catalog.pointer_size());
    for (const BaseLayout& b : layout.bases)
        if (!b.is_virtual)
            if (const TypeLayout* base = catalog.find(b.name); base && base->size > 1) end = std::max(end, b.offset + base->size);
    for (const FieldLayout& f : layout.fields) end = std::max(end, f.offset + f.size);
    return end;
}

u64 natural_alignment(const TypeLayout& layout, const TypeCatalog& catalog, int depth) {
    if (depth > kMaxDepth) return 1;
    if (!is_record(layout)) return std::max<u64>(layout.size, 1);
    if (const auto intrinsic = intrinsic_alignment(layout.name)) return *intrinsic;
    u64 alignment = 1;
    if (layout.vfptr || layout.vbptr || std::ranges::any_of(layout.bases, &BaseLayout::is_virtual)) alignment = catalog.pointer_size();
    for (const BaseLayout& b : layout.bases)
        if (const TypeLayout* base = catalog.find(b.name)) alignment = std::max(alignment, type_alignment(*base, catalog, depth + 1));
    for (const FieldLayout& f : layout.fields) alignment = std::max(alignment, field_alignment(f, catalog, depth + 1));
    return alignment;
}

// The packing (#pragma pack) the offsets need, and the alignment (__declspec(align)) the size needs; 0 for none.
struct Packing {
    u64 pack = 0;
    u64 align = 0;
};

Packing packing(const TypeLayout& layout, const TypeCatalog& catalog, int depth) {
    Packing out;
    if (!is_record(layout) || depth > kMaxDepth || intrinsic_alignment(layout.name)) return out;
    const u64 natural = natural_alignment(layout, catalog, depth);
    const u64 end = members_end(layout, catalog);
    // The largest packing every offset agrees with.
    u64 pack = 16;
    for (; pack > 1; pack /= 2) {
        bool fits = true;
        for (const FieldLayout& f : layout.fields)
            if (f.offset % std::min(field_alignment(f, catalog, depth + 1), pack) != 0) fits = false;
        for (const BaseLayout& b : layout.bases)
            if (const TypeLayout* base = catalog.find(b.name); base && !b.is_virtual && b.offset % std::min(type_alignment(*base, catalog, depth + 1), pack) != 0)
                fits = false;
        if (fits) break;
    }
    // The size: smaller packing when the type ends before its alignment would round it, a declared
    // alignment when it ends after. Virtual bases (and their vtordisps) follow a class's own members,
    // so its size says nothing of either.
    if (std::ranges::any_of(layout.bases, &BaseLayout::is_virtual)) {
        if (pack < natural) out.pack = pack;
        return out;
    }
    const auto size_with = [&](u64 alignment) { return end == 0 ? 1 : round_up(end, alignment); };
    while (pack > 1 && size_with(std::min(natural, pack)) > layout.size) pack /= 2;
    if (pack < natural) out.pack = pack;
    const u64 effective = std::min(natural, pack);
    if (size_with(effective) < layout.size) {
        // Declared alignment: the largest power of two the size is a multiple of (__declspec(align(16))
        // is the usual reason), unless where the type is a field says less.
        u64 limit = layout.size & (~layout.size + 1);
        for (const TypeLayout& other : catalog.types())
            for (const FieldLayout& f : other.fields)
                if (f.udt == layout.name && f.offset != 0) limit = std::min(limit, f.offset & (~f.offset + 1));
        for (u64 a = std::min<u64>(limit, 8192); a > effective; a /= 2)
            if (size_with(a) == layout.size) {
                out.align = a;
                break;
            }
    }
    return out;
}

u64 type_alignment(const TypeLayout& layout, const TypeCatalog& catalog, int depth) {
    if (depth > kMaxDepth) return 1;
    const Packing p = packing(layout, catalog, depth);
    u64 alignment = natural_alignment(layout, catalog, depth);
    if (p.pack != 0) alignment = std::min(alignment, p.pack);
    return std::max(alignment, p.align);
}

// Fields as a source declares them: flattened members of anonymous unions and structs regrouped.
struct Member {
    const FieldLayout* field = nullptr;
    bool is_union = false;  // for groups
    std::vector<Member> members;

    static Member of(const FieldLayout& f) {
        Member m;
        m.field = &f;
        return m;
    }
    static Member group(bool is_union, std::vector<Member> members) {
        Member m;
        m.is_union = is_union;
        m.members = std::move(members);
        return m;
    }
};

u64 field_end(const FieldLayout& f) { return f.offset + std::max<u64>(f.size, 1); }

bool continues_bits(const FieldLayout& previous, const FieldLayout& f) {
    return previous.is_bitfield() && f.is_bitfield() && previous.offset == f.offset && *f.bit_offset > *previous.bit_offset;
}

// Whether fields[k] starts inside the previous field's bytes: a member of a flattened anonymous union.
bool starts_back(const std::vector<FieldLayout>& fields, usize k) {
    return k > 0 && fields[k].offset < field_end(fields[k - 1]) && !continues_bits(fields[k - 1], fields[k]);
}

std::vector<Member> struct_members(const std::vector<FieldLayout>& fields, usize begin, usize end, int depth) {
    std::vector<Member> out;
    usize i = begin;
    while (i < end) {
        usize k = i + 1;
        while (k < end && !starts_back(fields, k)) ++k;
        if (k >= end || depth > kMaxDepth) {
            for (; i < end; ++i) out.push_back(Member::of(fields[i]));
            break;
        }
        // fields[k] starts back at an earlier field of this run: an anonymous union starts there.
        const u64 at = fields[k].offset;
        usize start = i;
        while (start < k && fields[start].offset != at) ++start;
        if (start == k) {  // it starts inside a field, which no union explains: leave them as they are
            for (; i <= k; ++i) out.push_back(Member::of(fields[i]));
            continue;
        }
        for (; i < start; ++i) out.push_back(Member::of(fields[i]));
        // Its alternatives start at `start` and at each later field that starts back at the same offset;
        // it ends at the first field past its largest alternative.
        std::vector<usize> alternatives = {start};
        u64 union_end = 0;
        for (usize m = start; m < k; ++m) union_end = std::max(union_end, field_end(fields[m]));
        usize m = k;
        for (; m < end; ++m) {
            if (fields[m].offset == at && starts_back(fields, m)) alternatives.push_back(m);
            else if (fields[m].offset >= union_end) break;
            union_end = std::max(union_end, field_end(fields[m]));
        }
        Member group = Member::group(true, {});
        for (usize a = 0; a < alternatives.size(); ++a) {
            const usize from = alternatives[a], to = a + 1 < alternatives.size() ? alternatives[a + 1] : m;
            if (to - from == 1) group.members.push_back(Member::of(fields[from]));
            else group.members.push_back(Member::group(false, struct_members(fields, from, to, depth + 1)));
        }
        out.push_back(std::move(group));
        i = m;
    }
    return out;
}

// A union's own fields: each member starts at 0; members after it at higher offsets were an anonymous
// struct's.
std::vector<Member> union_members(const std::vector<FieldLayout>& fields) {
    std::vector<Member> out;
    usize i = 0;
    while (i < fields.size()) {
        usize j = i + 1;
        while (j < fields.size() && (fields[j].offset != fields[i].offset || continues_bits(fields[j - 1], fields[j]))) ++j;
        if (j - i == 1) out.push_back(Member::of(fields[i]));
        else out.push_back(Member::group(false, struct_members(fields, i, j, 1)));
        i = j;
    }
    return out;
}

std::vector<Member> members_of(const TypeLayout& layout) {
    return layout.kind == TypeKind::union_ ? union_members(layout.fields) : struct_members(layout.fields, 0, layout.fields.size(), 0);
}

std::string dimensions_text(const FieldLayout& f) {
    std::string out;
    for (const u64 d : f.dimensions) out += std::format("[{}]", d);
    return out;
}

bool conversion_operator(std::string_view name) {
    if (!name.starts_with("operator ")) return false;
    const std::string_view rest = trim(name.substr(9));
    return rest != "new" && rest != "delete" && rest != "new[]" && rest != "delete[]" && rest != "co_await";
}

std::string method_declaration(const MethodLayout& m, std::string_view class_name) {
    const bool special = m.name == class_name || m.name.starts_with('~') || conversion_operator(m.name);
    std::string out;
    if (m.is_static) out += "static ";
    if (m.is_virtual) out += "virtual ";
    if (!special) out += m.return_type + " ";
    if (!m.calling_convention.empty()) out += m.calling_convention + " ";
    out += std::format("{}({})", m.name, join(m.parameters, ", "));
    if (m.is_const) out += " const";
    if (m.pure) out += " = 0";
    return out + ";";
}

class Writer {
public:
    explicit Writer(const TypeCatalog& catalog) : catalog_(catalog) {}

    void definition(const TypeLayout& layout, int indent, int depth) {
        const std::string name = unqualified(layout.name);
        if (!is_record(layout)) {
            line(indent, layout.underlying.empty() || layout.underlying == "int" ? std::format("enum {} {{", name)
                                                                                : std::format("enum {} : {} {{", name, layout.underlying));
            for (const Enumerator& e : layout.enumerators) line(indent + 1, std::format("{} = {},", e.name, e.value));
            line(indent, "};");
            return;
        }
        const Packing p = packing(layout, catalog_, 0);
        std::string head = std::format("{} {}{}", to_string(layout.kind), p.align ? std::format("__declspec(align({})) ", p.align) : "", name);
        for (usize i = 0; i < layout.bases.size(); ++i)
            head += std::format("{}public {}{}", i == 0 ? " : " : ", ", layout.bases[i].is_virtual ? "virtual " : "", layout.bases[i].name);
        line(indent, head + " {");
        if (layout.kind == TypeKind::class_) line(indent, "public:");
        // Nested types first: the fields and methods may use them.
        for (const NestedType& nested : layout.nested) {
            if (anonymous_type_name(nested.type)) continue;
            const TypeLayout* inner = nested.is_typedef ? nullptr : catalog_.find(nested.type);
            if (nested.is_typedef) {
                FieldLayout alias;
                alias.name = nested.name;
                alias.type = nested.type;
                line(indent + 1, "typedef " + field_declaration(alias) + ";");
            } else if (inner && depth < kMaxDepth && name_parts(inner->name).size() > 1 && unqualified(inner->name) == nested.name) {
                definition(*inner, indent + 1, depth + 1);
            } else if (inner) {
                line(indent + 1, std::format("{} {};", is_record(*inner) ? to_string(inner->kind) : "enum", nested.name));
            }
        }
        members(members_of(layout), indent + 1, depth);
        for (const StaticMember& s : layout.statics) {
            FieldLayout member;
            member.name = s.name;
            member.type = s.type;
            line(indent + 1, "static " + field_declaration(member) + ";");
        }
        for (const MethodLayout& m : layout.methods) line(indent + 1, method_declaration(m, name));
        line(indent, "};");
    }

    std::string take() { return std::move(out_); }

private:
    void line(int indent, std::string_view text) {
        out_.append(static_cast<usize>(indent) * 4, ' ');
        out_ += text;
        out_ += '\n';
    }

    void members(const std::vector<Member>& list, int indent, int depth) {
        const FieldLayout* previous = nullptr;
        for (const Member& m : list) {
            if (!m.field) {
                line(indent, m.is_union ? "union {" : "struct {");
                members(m.members, indent + 1, depth);
                line(indent, "};");
                previous = nullptr;
                continue;
            }
            field(*m.field, previous, indent, depth);
            previous = m.field;
        }
    }

    void field(const FieldLayout& f, const FieldLayout* previous, int indent, int depth) {
        if (f.is_bitfield()) {
            const u64 unit_bits = f.size * 8;
            u64 next_bit = 0;
            if (previous && previous->is_bitfield() && previous->offset == f.offset) {
                next_bit = u64{*previous->bit_offset} + *previous->bit_width;
            } else if (previous && previous->is_bitfield() && previous->type == f.type && f.offset == previous->offset + previous->size &&
                       u64{*previous->bit_offset} + *previous->bit_width + *f.bit_width <= unit_bits) {
                line(indent, f.type + " : 0;");  // it would have fit in the previous unit: the source started a new one
            }
            if (*f.bit_offset > next_bit) line(indent, std::format("{} : {};", f.type, *f.bit_offset - next_bit));
        }
        // An anonymous struct, union or enum as a member's type is defined where the member is.
        if (anonymous_type_name(f.type) && depth < kMaxDepth) {
            const TypeLayout* inner = !f.udt.empty() ? catalog_.find(f.udt) : nullptr;
            for (const std::string& n : f.named)
                if (!inner && anonymous_type_name(n)) inner = catalog_.find(n);
            if (inner && is_record(*inner)) {
                line(indent, std::format("{} {{", to_string(inner->kind)));
                members(members_of(*inner), indent + 1, depth + 1);
                line(indent, std::format("}} {}{};", f.name, dimensions_text(f)));
                return;
            }
            if (inner) {
                line(indent, "enum {");
                for (const Enumerator& e : inner->enumerators) line(indent + 1, std::format("{} = {},", e.name, e.value));
                line(indent, std::format("}} {}{};", f.name, dimensions_text(f)));
                return;
            }
        }
        line(indent, field_declaration(f) + ";");
    }

    const TypeCatalog& catalog_;
    std::string out_;
};

} // namespace

bool anonymous_type_name(std::string_view name) {
    return name.find("<unnamed") != std::string_view::npos || name.find("<anonymous") != std::string_view::npos || name.starts_with('<') ||
           name.find("::<") != std::string_view::npos;
}

std::string definition_of(const TypeLayout& layout, const TypeCatalog& catalog) {
    Writer writer(catalog);
    writer.definition(layout, 0, 0);
    return writer.take();
}

u64 alignment_of(const TypeLayout& layout, const TypeCatalog& catalog) { return type_alignment(layout, catalog, 0); }

u64 packing_of(const TypeLayout& layout, const TypeCatalog& catalog) { return packing(layout, catalog, 0).pack; }

DeclarationPlan declare_types(const TypeCatalog& catalog, const std::vector<std::string>& names, const std::set<std::string>& existing) {
    DeclarationPlan plan;
    std::set<std::string> done, in_progress, forward;
    std::vector<TypeDeclaration> definitions;

    const auto wrap = [&](const std::string& name, std::string text, u64 pack) {
        const auto namespaces = namespaces_of(name, catalog);
        std::string open, close;
        for (const std::string& ns : namespaces) {
            open += std::format("namespace {} {{\n", ns);
            close = "}\n" + close;
        }
        text = open + text + close;
        if (pack != 0) text = std::format("#pragma pack(push, {})\n{}#pragma pack(pop)\n", pack, text);
        return text;
    };

    std::function<void(const std::string&, int)> define = [&](const std::string& requested, int depth) {
        std::string name = requested;
        while (const auto outer = enclosing_class(name, catalog)) name = *outer;
        if (existing.contains(name)) plan.existing_used.insert(name);
        if (depth > 64 || done.contains(name) || existing.contains(name) || in_progress.contains(name) || anonymous_type_name(name)) return;
        const TypeLayout* layout = catalog.find(name);
        if (!layout) {
            plan.skipped.push_back(name + ": no definition in the type records");
            done.insert(name);
            return;
        }
        if (template_name(name)) {
            plan.skipped.push_back(name + ": a template instance (declare the template by hand)");
            done.insert(name);
            return;
        }
        in_progress.insert(name);
        // A type it names but does not hold: declared forward, unless it is nested in a class (then the
        // class comes first).
        const auto refer = [&](const std::string& n) {
            if (enclosing_class(n, catalog)) define(n, depth + 1);
            else if (existing.contains(n)) plan.existing_used.insert(n);
            else forward.insert(n);
        };
        // What it needs complete first, and what it only points to; nested and anonymous member types
        // bring theirs.
        std::vector<const TypeLayout*> parts = {layout};
        for (usize i = 0; i < parts.size() && parts.size() < 256; ++i) {
            const TypeLayout& part = *parts[i];
            for (const BaseLayout& b : part.bases) define(b.name, depth + 1);
            for (const FieldLayout& f : part.fields) {
                if (!f.udt.empty() && anonymous_type_name(f.udt)) {
                    if (const TypeLayout* inner = catalog.find(f.udt)) parts.push_back(inner);
                } else if (!f.udt.empty()) {
                    define(f.udt, depth + 1);
                }
                for (const std::string& n : f.named) refer(n);
            }
            for (const NestedType& nested : part.nested)
                if (!nested.is_typedef)
                    if (const TypeLayout* inner = catalog.find(nested.type); inner && enclosing_class(inner->name, catalog) == part.name)
                        parts.push_back(inner);
            for (const MethodLayout& m : part.methods)
                for (const std::string& n : m.named) refer(n);
            for (const StaticMember& s : part.statics)
                for (const std::string& n : s.named) refer(n);
        }
        // Enums cannot be declared forward (before C++11): define the ones it names.
        for (const std::string& n : std::set<std::string>(forward))
            if (const TypeLayout* e = catalog.find(n); e && !is_record(*e)) define(n, depth + 1);
        in_progress.erase(name);
        done.insert(name);
        definitions.push_back({name, wrap(name, definition_of(*layout, catalog), packing(*layout, catalog, 0).pack), true});
    };
    for (const std::string& name : names) define(name, 0);

    // What the definitions only point to, declared forward when nothing defines it.
    for (std::string name : forward) {
        while (const auto outer = enclosing_class(name, catalog)) name = *outer;
        if (done.contains(name) || existing.contains(name) || anonymous_type_name(name) || template_name(name)) continue;
        const TypeLayout* layout = catalog.find(name);
        if (layout && !is_record(*layout)) continue;  // enums are defined above
        if (enclosing_class(name, catalog)) continue;
        const std::string keyword = layout ? std::string(to_string(layout->kind)) : "struct";
        plan.declarations.push_back({name, wrap(name, std::format("{} {};\n", keyword, unqualified(name)), 0), false});
        done.insert(name);
    }
    for (auto& d : definitions) plan.declarations.push_back(std::move(d));
    return plan;
}

} // namespace decomp
