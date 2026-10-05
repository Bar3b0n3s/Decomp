#include "analysis/rtti.hpp"

#include "analysis/demangle.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace decomp {

namespace {

constexpr u32 kHasHierarchy = 0x40;  // BaseClassDescriptor attribute: a ClassHierarchyDescriptor pointer follows

struct Locator {
    u64 va = 0;
    u32 offset = 0, constructor_displacement = 0;
    u64 type_descriptor = 0, hierarchy = 0;
};

class Finder {
public:
    explicit Finder(const pe::Image& image)
        : image_(image), x64_(image.arch() == Arch::x64), ptr_(pointer_size(image.arch())) {}

    RttiInfo run() {
        find_type_descriptors();
        find_locators();
        find_vftable_slots();
        RttiInfo info;
        std::map<u64, usize> by_descriptor;
        for (const auto& [td, name] : type_descriptors_) {
            RttiClass c;
            c.type_descriptor = td;
            c.is_struct = name[3] == 'U';
            c.decorated = name.substr(4);
            c.name = class_display_name(c.decorated);
            by_descriptor.emplace(td, info.classes.size());
            info.classes.push_back(std::move(c));
        }
        for (const Locator& l : locators_) {
            auto it = by_descriptor.find(l.type_descriptor);
            if (it == by_descriptor.end()) continue;
            RttiClass& c = info.classes[it->second];
            if (!c.hierarchy) read_hierarchy(c, l.hierarchy);
            RttiVftable v;
            v.locator = l.va;
            v.offset = l.offset;
            v.constructor_displacement = l.constructor_displacement;
            if (auto slot = locator_slots_.find(l.va); slot != locator_slots_.end()) {
                v.va = slot->second + ptr_;
                read_slots(v);
            }
            c.vftables.push_back(std::move(v));
        }
        for (RttiClass& c : info.classes) {
            std::ranges::sort(c.vftables, {}, &RttiVftable::offset);
            if (c.vftables.size() > 1)
                for (RttiVftable& v : c.vftables) v.for_base = base_at(c, v.offset);
        }
        std::ranges::sort(info.classes, {}, &RttiClass::name);
        for (usize ci = 0; ci < info.classes.size(); ++ci)
            for (usize vi = 0; vi < info.classes[ci].vftables.size(); ++vi)
                for (usize si = 0; si < info.classes[ci].vftables[vi].slots.size(); ++si)
                    info.slots.emplace(info.classes[ci].vftables[vi].slots[si], VirtualSlot{ci, vi, si});
        return info;
    }

private:
    std::optional<u64> read_pointer(u64 va) const {
        if (ptr_ == 8) return image_.read<u64>(va);
        return image_.read<u32>(va).transform([](u32 v) { return u64{v}; });
    }
    // A reference inside an RTTI structure: an address on x86, an image-relative one on x64.
    std::optional<u64> read_ref(u64 va) const {
        auto v = image_.read<u32>(va);
        if (!v) return std::nullopt;
        return x64_ ? image_.image_base() + *v : u64{*v};
    }

    // ".?AV" and ".?AU" names, two pointers into a TypeDescriptor whose second pointer (the runtime's
    // cache) is null and whose first is type_info's vftable.
    void find_type_descriptors() {
        for (const auto& s : image_.image_sections()) {
            if (s.executable || s.file_size == 0) continue;
            auto bytes = image_.view(s.va, static_cast<usize>(s.file_size));
            if (!bytes) continue;
            const auto* p = reinterpret_cast<const char*>(bytes->data());
            const usize n = bytes->size();
            for (usize i = 2 * ptr_; i + 4 < n; ++i) {
                if (p[i] != '.' || p[i + 1] != '?' || p[i + 2] != 'A' || (p[i + 3] != 'V' && p[i + 3] != 'U')) continue;
                const u64 td = s.va + i - 2 * ptr_;
                if (td % ptr_ != 0) continue;
                usize j = i;
                while (j < n && j - i < 1024 && p[j] != 0) ++j;
                if (j >= n || p[j] != 0) continue;
                const std::string_view name(p + i, j - i);
                if (name.size() < 7 || !name.ends_with("@@")) continue;
                if (!std::ranges::all_of(name, [](char c) { return c > 0x20 && c < 0x7F; })) continue;
                const auto vftable = read_pointer(td), spare = read_pointer(td + ptr_);
                if (!vftable || !spare || *spare != 0 || !image_.contains(*vftable)) continue;
                type_descriptors_.emplace(td, std::string(name));
                i = j;
            }
        }
    }

    // CompleteObjectLocator: signature (0 on x86, 1 on x64), offset, constructor displacement, the
    // TypeDescriptor, the ClassHierarchyDescriptor and, on x64, its own RVA.
    void find_locators() {
        const u64 size = x64_ ? 24 : 20;
        for (const auto& s : image_.image_sections()) {
            if (s.executable || s.file_size < size) continue;
            for (u64 va = s.va; va + size <= s.va + s.file_size; va += 4) {
                const auto signature = image_.read<u32>(va);
                if (!signature || *signature != (x64_ ? 1u : 0u)) continue;
                const auto td = read_ref(va + 12);
                if (!td || !type_descriptors_.contains(*td)) continue;
                if (x64_) {
                    const auto self = image_.read<u32>(va + 20);
                    if (!self || *self != va - image_.image_base()) continue;
                }
                const auto hierarchy = read_ref(va + 16);
                if (!hierarchy || !image_.contains(*hierarchy)) continue;
                Locator l;
                l.va = va;
                l.offset = image_.read<u32>(va + 4).value_or(0);
                l.constructor_displacement = image_.read<u32>(va + 8).value_or(0);
                l.type_descriptor = *td;
                l.hierarchy = *hierarchy;
                locators_.push_back(l);
            }
        }
    }

    // The pointer-sized slots that hold a locator's address: each precedes a vftable.
    void find_vftable_slots() {
        std::unordered_set<u64> locators;
        for (const Locator& l : locators_) locators.insert(l.va);
        for (const auto& s : image_.image_sections()) {
            if (s.executable || s.file_size < ptr_) continue;
            for (u64 va = s.va; va + ptr_ <= s.va + s.file_size; va += ptr_)
                if (auto value = read_pointer(va); value && locators.contains(*value)) {
                    locator_slots_.emplace(*value, va);
                    slot_positions_.insert(va);
                }
        }
    }

    // Code pointers from the first slot on, up to the next vftable's locator pointer or the first
    // value that is not code.
    void read_slots(RttiVftable& v) const {
        const ImageSection* section = image_.section_at(v.va);
        if (!section) return;
        const u64 end = section->va + section->file_size;
        for (u64 at = v.va; at + ptr_ <= end; at += ptr_) {
            if (slot_positions_.contains(at)) break;
            const auto value = read_pointer(at);
            if (!value || !image_.is_code(*value)) break;
            v.slots.push_back(*value);
        }
    }

    void read_hierarchy(RttiClass& c, u64 chd) const {
        const auto attributes = image_.read<u32>(chd + 4);
        const auto count = image_.read<u32>(chd + 8);
        const auto array = read_ref(chd + 12);
        if (!attributes || !count || !array || *count == 0 || *count > 4096) return;
        c.hierarchy = chd;
        c.attributes = *attributes;
        c.base_array = *array;
        usize next_direct = 1;
        for (u32 k = 0; k < *count; ++k) {
            const auto bcd = read_ref(*array + 4 * k);
            if (!bcd) break;
            const auto td = read_ref(*bcd);
            auto name = td ? type_descriptors_.find(*td) : type_descriptors_.end();
            if (name == type_descriptors_.end()) break;
            RttiBase b;
            b.descriptor = *bcd;
            b.decorated = name->second.substr(4);
            b.name = class_display_name(b.decorated);
            b.contained = image_.read<u32>(*bcd + 4).value_or(0);
            b.mdisp = static_cast<i32>(image_.read<u32>(*bcd + 8).value_or(0));
            b.pdisp = static_cast<i32>(image_.read<u32>(*bcd + 12).value_or(0xFFFFFFFF));
            b.vdisp = static_cast<i32>(image_.read<u32>(*bcd + 16).value_or(0));
            b.attributes = image_.read<u32>(*bcd + 20).value_or(0);
            if (k == 0) {
                c.self = std::move(b);
                continue;
            }
            // The array lists each base followed by its own bases: the direct ones come every
            // `contained + 1` entries.
            b.direct = k == next_direct;
            if (b.direct) next_direct = k + b.contained + 1;
            c.bases.push_back(std::move(b));
        }
    }

    // The base whose subobject a vftable at `offset` serves: a direct, non-virtual base there.
    static std::string base_at(const RttiClass& c, u32 offset) {
        for (const auto& b : c.bases)
            if (b.direct && b.pdisp == -1 && static_cast<u32>(b.mdisp) == offset) return b.decorated;
        for (const auto& b : c.bases)
            if (b.direct && static_cast<u32>(b.mdisp) == offset) return b.decorated;
        return {};
    }

    const pe::Image& image_;
    bool x64_;
    unsigned ptr_;
    std::map<u64, std::string> type_descriptors_;  // address -> ".?AVFoo@@"
    std::vector<Locator> locators_;
    std::unordered_map<u64, u64> locator_slots_;  // locator -> the slot that holds its address
    std::unordered_set<u64> slot_positions_;
};

std::string for_part(const RttiVftable& v) { return v.for_base.empty() ? "6B@" : "6B" + v.for_base + "@"; }

} // namespace

std::string encode_ms_number(i64 value) {
    if (value < 0) return "?" + encode_ms_number(-value);
    if (value == 0) return "A@";
    if (value <= 10) return std::string(1, static_cast<char>('0' + value - 1));
    std::string hex;
    for (u64 v = static_cast<u64>(value); v; v >>= 4) hex.insert(hex.begin(), static_cast<char>('A' + (v & 0xF)));
    return hex + "@";
}

std::string class_display_name(std::string_view decorated) {
    // Templates and other special names: the demangler reads them as the class of a vftable.
    if (decorated.starts_with('?')) {
        std::string d = display_name("??_7" + std::string(decorated) + "6B@");
        if (d.starts_with("const ")) d.erase(0, 6);
        if (const auto at = d.rfind("::`vftable'"); at != std::string::npos) return d.substr(0, at);
        return std::string(decorated);
    }
    std::string_view rest = decorated;
    if (rest.ends_with("@@")) rest.remove_suffix(2);
    std::vector<std::string_view> parts;
    for (auto part : split(rest, '@'))
        if (!part.empty()) parts.push_back(part);
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) out += (out.empty() ? "" : "::") + std::string(*it);
    return out;
}

std::string RttiClass::type_descriptor_name() const { return std::format("??_R0?A{}{}@8", is_struct ? 'U' : 'V', decorated); }
std::string RttiClass::vftable_name(const RttiVftable& v) const { return "??_7" + decorated + for_part(v); }
std::string RttiClass::locator_name(const RttiVftable& v) const { return "??_R4" + decorated + for_part(v); }
std::string RttiClass::hierarchy_name() const { return "??_R3" + decorated + "8"; }
std::string RttiClass::base_array_name() const { return "??_R2" + decorated + "8"; }

std::string base_descriptor_name(const RttiBase& b) {
    return "??_R1" + encode_ms_number(b.mdisp) + encode_ms_number(b.pdisp) + encode_ms_number(b.vdisp) + encode_ms_number(b.attributes) +
           b.decorated + "8";
}

const RttiClass* RttiInfo::find(std::string_view name) const {
    for (const auto& c : classes)
        if (c.name == name || c.decorated == name) return &c;
    return nullptr;
}

std::vector<std::string> RttiInfo::describe_slots(u64 function) const {
    std::vector<std::string> out;
    for (auto [it, end] = slots.equal_range(function); it != end; ++it) {
        const RttiClass& c = classes[it->second.class_index];
        const RttiVftable& v = c.vftables[it->second.vftable_index];
        out.push_back(std::format("slot {} of {}'s vftable{}", it->second.slot, c.name,
                                  v.for_base.empty() ? std::string() : " for " + class_display_name(v.for_base)));
    }
    return out;
}

RttiInfo find_rtti(const pe::Image& image) { return Finder(image).run(); }

void add_rtti_symbols(SymbolDb& symbols, const RttiInfo& rtti, Arch arch) {
    const u32 ptr = pointer_size(arch);
    auto add = [&](u64 va, std::string name, u32 size) {
        if (!va) return;
        Symbol s;
        s.va = va;
        s.name = std::move(name);
        s.kind = SymbolKind::data;
        s.size = size;
        s.source = SymbolSource::analysis;
        symbols.add(std::move(s));
    };
    for (const RttiClass& c : rtti.classes) {
        add(c.type_descriptor, c.type_descriptor_name(), static_cast<u32>(2 * ptr + c.decorated.size() + 5));
        if (c.hierarchy) {
            add(c.hierarchy, c.hierarchy_name(), 16);
            add(c.base_array, c.base_array_name(), static_cast<u32>(4 * (c.bases.size() + 1)));
            add(c.self.descriptor, base_descriptor_name(c.self), c.self.attributes & kHasHierarchy ? 28 : 24);
            for (const RttiBase& b : c.bases) add(b.descriptor, base_descriptor_name(b), b.attributes & kHasHierarchy ? 28 : 24);
        }
        for (const RttiVftable& v : c.vftables) {
            add(v.locator, c.locator_name(v), arch == Arch::x64 ? 24 : 20);
            add(v.va, c.vftable_name(v), static_cast<u32>(ptr * v.slots.size()));
        }
    }
}

} // namespace decomp
