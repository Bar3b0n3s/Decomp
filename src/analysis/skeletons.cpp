#include "analysis/skeletons.hpp"

#include "analysis/demangle.hpp"
#include "analysis/rtti.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <functional>
#include <map>

namespace decomp {

namespace {

// A parameter list's parameters, split at its top-level commas.
std::vector<std::string> split_parameters(std::string_view list) {
    std::vector<std::string> out;
    int depth = 0;
    usize start = 0;
    for (usize i = 0; i <= list.size(); ++i) {
        if (i == list.size() || (list[i] == ',' && depth == 0)) {
            if (const std::string_view part = trim(list.substr(start, i - start)); !part.empty()) out.emplace_back(part);
            start = i + 1;
            continue;
        }
        if (list[i] == '<' || list[i] == '(' || list[i] == '[') ++depth;
        else if (list[i] == '>' || list[i] == ')' || list[i] == ']') --depth;
    }
    return out;
}

std::string unqualified(std::string_view name) {
    const auto scope = name.rfind("::");
    return std::string(scope == std::string_view::npos ? name : name.substr(scope + 2));
}

bool same_method(const MethodLayout& a, const MethodLayout& b) {
    const bool destructors = a.name.starts_with('~') && b.name.starts_with('~');
    return destructors || (a.name == b.name && a.parameters == b.parameters && a.is_const == b.is_const);
}

// A deleting destructor (??_G scalar, ??_E vector) stands in a vftable for the class's destructor.
bool deleting_destructor(std::string_view name) { return name.starts_with("??_G") || name.starts_with("??_E"); }

u64 round_up(u64 value, u64 alignment) { return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment; }

} // namespace

std::optional<MethodLayout> method_from_decorated(std::string_view decorated, std::string_view class_name, Arch arch) {
    const auto text = demangle(decorated);
    if (!text) return std::nullopt;
    const std::string qualified = qualified_name(decorated);
    if (!qualified.starts_with(std::string(class_name) + "::")) return std::nullopt;
    const std::string method = qualified.substr(class_name.size() + 2);
    if (method.empty() || method.find("::") != std::string::npos || method.find('`') != std::string::npos) return std::nullopt;
    // "<access>: [virtual |static ]<return type> <calling convention> <class>::<method>(<parameters>)[ const]"
    const std::string marker = qualified + "(";
    const auto at = text->find(marker);
    if (at == std::string::npos) return std::nullopt;
    usize close = std::string::npos;
    for (usize i = at + marker.size(), depth = 1; i < text->size(); ++i) {
        if ((*text)[i] == '(') ++depth;
        else if ((*text)[i] == ')' && --depth == 0) {
            close = i;
            break;
        }
    }
    if (close == std::string::npos) return std::nullopt;
    std::string prefix(trim(std::string_view(*text).substr(0, at)));
    const std::string parameters = text->substr(at + marker.size(), close - at - marker.size());
    const std::string suffix(trim(std::string_view(*text).substr(close + 1)));

    MethodLayout m;
    m.name = method;
    for (const char* access : {"public: ", "protected: ", "private: "})
        if (prefix.starts_with(access)) prefix = prefix.substr(std::strlen(access));
    if (prefix.starts_with("virtual ")) {
        m.is_virtual = true;
        prefix = prefix.substr(8);
    }
    if (prefix.starts_with("static ")) {
        m.is_static = true;
        prefix = prefix.substr(7);
    }
    std::string convention;
    for (const char* cc : {"__thiscall", "__cdecl", "__stdcall", "__fastcall", "__vectorcall"})
        if (prefix == cc || prefix.ends_with(std::string(" ") + cc)) {
            convention = cc;
            prefix = std::string(trim(std::string_view(prefix).substr(0, prefix.size() - std::strlen(cc))));
            break;
        }
    m.return_type = prefix;  // "" for constructors and destructors
    if (arch == Arch::x86 && !convention.empty() && convention != (m.is_static ? "__cdecl" : "__thiscall")) m.calling_convention = convention;
    if (parameters != "void") m.parameters = split_parameters(parameters);
    m.is_const = suffix.starts_with("const");
    return m;
}

TypeCatalog rtti_skeletons(const Program& program) {
    const RttiInfo& rtti = program.rtti();
    const Arch arch = program.arch();
    const u64 pointer = arch == Arch::x64 ? 8 : 4;
    const auto function_at = [&](u64 va) { return program.symbols().at(program.thunk_destination(va).value_or(va)); };

    // A base that another base follows fills the space up to it.
    std::map<std::string, u64> required;
    for (const RttiClass& c : rtti.classes) {
        std::vector<const RttiBase*> direct;
        for (const RttiBase& b : c.bases)
            if (b.direct && b.pdisp < 0) direct.push_back(&b);
        std::ranges::stable_sort(direct, {}, &RttiBase::mdisp);
        for (usize i = 0; i + 1 < direct.size(); ++i)
            if (direct[i + 1]->mdisp > direct[i]->mdisp)
                required[direct[i]->name] = std::max<u64>(required[direct[i]->name], static_cast<u64>(direct[i + 1]->mdisp - direct[i]->mdisp));
    }
    // The member functions the symbols name, by class.
    std::map<std::string, std::vector<const Symbol*>> members;
    for (const Symbol* s : program.symbols().functions()) {
        if (!s->name.starts_with('?') || deleting_destructor(s->name)) continue;
        const std::string qualified = qualified_name(s->name);
        if (const auto scope = qualified.rfind("::"); scope != std::string::npos) members[qualified.substr(0, scope)].push_back(s);
    }

    std::map<std::string, TypeLayout, std::less<>> built;
    // The method in a slot of a type's primary vtable: its own, or its primary base's.
    const auto method_in_slot = [&](std::string type, u64 slot) -> const MethodLayout* {
        for (int depth = 0; depth < 32 && !type.empty(); ++depth) {
            const auto it = built.find(type);
            if (it == built.end()) return nullptr;
            for (const MethodLayout& m : it->second.methods)
                if (m.slot == slot) return &m;
            std::string next;
            for (const BaseLayout& b : it->second.bases)
                if (!b.is_virtual && b.offset == 0) next = b.name;
            type = next;
        }
        return nullptr;
    };
    std::function<const TypeLayout*(const std::string&, int)> build = [&](const std::string& name, int depth) -> const TypeLayout* {
        if (const auto it = built.find(name); it != built.end()) return &it->second;
        if (depth > 32) return nullptr;
        const RttiClass* c = rtti.find(name);
        TypeLayout layout;
        layout.name = name;
        layout.kind = c && c->is_struct ? TypeKind::structure : TypeKind::class_;
        if (!c) {
            layout.size = 1;  // a base RTTI knows only by name
            return &built.emplace(name, std::move(layout)).first->second;
        }

        // Bases: the non-virtual ones in offset order, then the virtual ones.
        std::vector<const RttiBase*> direct;
        for (const RttiBase& b : c->bases)
            if (b.direct) direct.push_back(&b);
        std::ranges::stable_sort(direct, [](const RttiBase* a, const RttiBase* b) {
            return std::pair(a->pdisp >= 0, a->pdisp >= 0 ? 0 : a->mdisp) < std::pair(b->pdisp >= 0, b->pdisp >= 0 ? 0 : b->mdisp);
        });
        const TypeLayout* primary = nullptr;
        u64 end = 0;
        bool polymorphic = false, virtual_bases = false;
        for (const RttiBase* b : direct) {
            const TypeLayout* base = build(b->name, depth + 1);
            const bool is_virtual = b->pdisp >= 0;
            layout.bases.push_back({b->name, is_virtual ? 0 : static_cast<u64>(b->mdisp), is_virtual});
            virtual_bases = virtual_bases || is_virtual;
            if (!base || is_virtual) continue;
            if (b->mdisp == 0) primary = base;
            end = std::max(end, static_cast<u64>(b->mdisp) + base->size);
            polymorphic = polymorphic || base->vtable_slots > 0 || base->vfptr;
        }

        // The primary vftable: its slots past the primary base's are the ones the class introduces.
        const auto vftable = std::ranges::find(c->vftables, 0u, &RttiVftable::offset);
        if (vftable != c->vftables.end()) {
            layout.vtable_slots = vftable->slots.size();
            if (!primary) {
                layout.vfptr = 0;
                end = std::max(end, pointer);
            }
            polymorphic = true;
            const usize inherited = primary ? std::min<usize>(primary->vtable_slots, vftable->slots.size()) : 0;
            for (usize slot = 0; slot < vftable->slots.size(); ++slot) {
                const Symbol* fn = function_at(vftable->slots[slot]);
                std::optional<MethodLayout> method;
                if (fn && deleting_destructor(fn->name)) {
                    method = MethodLayout{};
                    method->name = "~" + unqualified(name);
                    method->is_virtual = true;
                } else if (fn && fn->name != "_purecall" && qualified_name(fn->name) != "purecall") {
                    method = method_from_decorated(fn->name, name, arch);
                    if (method && !method->is_virtual) method.reset();
                }
                if (slot < inherited) {
                    // An override, where the base's slot has the same method.
                    const MethodLayout* base_method = method_in_slot(primary->name, slot);
                    if (method && base_method && same_method(*method, *base_method)) layout.methods.push_back(*method);
                    continue;
                }
                const bool taken = method && std::ranges::any_of(layout.methods, [&](const MethodLayout& m) { return same_method(m, *method); });
                if (!method || taken) {
                    method = MethodLayout{};
                    method->name = std::format("vf{}", slot);
                    method->return_type = "void";
                    method->is_virtual = true;
                    method->pure = fn && (fn->name == "_purecall" || qualified_name(fn->name) == "purecall");
                }
                method->slot = slot;
                layout.methods.push_back(*method);
            }
        }
        // The vftables it has for its other bases (and its virtual bases) hold its overrides of their methods.
        std::vector<std::string> polymorphic_virtual_bases;
        for (const RttiBase* b : direct)
            if (const auto it = built.find(b->name); b->pdisp >= 0 && it != built.end() && (it->second.vtable_slots > 0 || it->second.vfptr))
                polymorphic_virtual_bases.push_back(b->name);
        for (const RttiVftable& v : c->vftables) {
            if (v.offset == 0) continue;
            std::string base = v.for_base.empty() ? std::string() : class_display_name(v.for_base);
            if (base.empty() && polymorphic_virtual_bases.size() == 1) base = polymorphic_virtual_bases.front();
            if (base.empty()) continue;
            for (usize slot = 0; slot < v.slots.size(); ++slot) {
                const Symbol* fn = function_at(v.slots[slot]);
                if (!fn || deleting_destructor(fn->name)) continue;
                const auto method = method_from_decorated(fn->name, name, arch);
                const MethodLayout* base_method = method_in_slot(base, slot);
                if (!method || !method->is_virtual || !base_method || !same_method(*method, *base_method)) continue;
                if (std::ranges::none_of(layout.methods, [&](const MethodLayout& m) { return same_method(m, *method); })) layout.methods.push_back(*method);
            }
        }
        for (const MethodLayout& m : layout.methods)
            if (m.is_virtual) layout.virtuals.push_back({m.name, m.slot, m.pure});

        // The other member functions the symbols name.
        if (const auto it = members.find(name); it != members.end())
            for (const Symbol* s : it->second) {
                const auto method = method_from_decorated(s->name, name, arch);
                if (!method || method->is_virtual) continue;  // virtual ones are where the vftable says
                if (std::ranges::any_of(layout.methods, [&](const MethodLayout& m) {
                        return m.name == method->name && m.parameters == method->parameters && m.is_const == method->is_const;
                    }))
                    continue;
                if (method->name.starts_with('~') && std::ranges::any_of(layout.methods, [](const MethodLayout& m) { return m.name.starts_with('~'); }))
                    continue;
                layout.methods.push_back(*method);
            }

        // A fill up to where the next base of a derived class starts.
        if (const auto it = required.find(name); it != required.end() && it->second > end) {
            FieldLayout fill;
            fill.offset = end;
            fill.size = it->second - end;
            fill.name = std::format("unknown_{:x}", end);
            fill.type = std::format("char[{}]", fill.size);
            fill.dimensions = {fill.size};
            layout.fields.push_back(std::move(fill));
            end = it->second;
        }
        if (virtual_bases) polymorphic = true;
        layout.size = end == 0 ? 1 : round_up(end, polymorphic ? pointer : 1);
        return &built.emplace(name, std::move(layout)).first->second;
    };

    TypeCatalog out(pointer);
    for (const RttiClass& c : rtti.classes) build(c.name, 0);
    for (const RttiClass& c : rtti.classes) out.add(built.at(c.name));
    for (auto& [name, layout] : built) out.add(layout);  // bases RTTI knows only by name
    return out;
}

std::vector<std::string> compare_with_rtti(const RttiClass& rtti, const TypeCatalog& compiled) {
    std::vector<std::string> out;
    const TypeLayout* layout = compiled.find(rtti.name);
    if (!layout) return {"no layout was compiled"};
    for (const RttiVftable& v : rtti.vftables) {
        if (v.offset == 0) {
            if (layout->vtable_slots != v.slots.size())
                out.push_back(std::format("{} vtable slots, RTTI says {}", layout->vtable_slots, v.slots.size()));
        } else if (!v.for_base.empty()) {
            const std::string base = class_display_name(v.for_base);
            const TypeLayout* b = compiled.find(base);
            if (!b) out.push_back(std::format("no layout of {}, whose vtable is at +{:#x}", base, v.offset));
            else if (b->vtable_slots != v.slots.size())
                out.push_back(std::format("the vtable for {} has {} slots, RTTI says {}", base, b->vtable_slots, v.slots.size()));
        }
    }
    for (const RttiBase& b : rtti.bases) {
        if (!b.direct) continue;
        const auto it = std::ranges::find(layout->bases, b.name, &BaseLayout::name);
        if (it == layout->bases.end()) out.push_back(std::format("base {} missing", b.name));
        else if ((b.pdisp >= 0) != it->is_virtual) out.push_back(std::format("base {} {}virtual, RTTI says {}", b.name, it->is_virtual ? "" : "not ", b.pdisp >= 0 ? "virtual" : "not"));
        else if (b.pdisp < 0 && it->offset != static_cast<u64>(b.mdisp))
            out.push_back(std::format("base {} at +{:#x}, RTTI says +{:#x}", b.name, it->offset, b.mdisp));
    }
    return out;
}

} // namespace decomp
