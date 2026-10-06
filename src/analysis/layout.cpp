#include "analysis/layout.hpp"

#include "analysis/program.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace decomp {

std::string_view to_string(LayoutSource source) { return source == LayoutSource::pdb ? "pdb" : "symbols"; }

const Contribution* ImageLayout::at(u32 rva) const {
    auto it = std::ranges::upper_bound(contributions, rva, {}, &Contribution::rva);
    // Empty contributions hold nothing: step over them.
    while (it != contributions.begin()) {
        --it;
        if (it->size) return it->contains(rva) ? &*it : nullptr;
    }
    return nullptr;
}

std::vector<const Contribution*> ImageLayout::of_unit(std::string_view unit) const {
    std::vector<const Contribution*> out;
    for (const auto& c : contributions)
        if (c.unit == unit) out.push_back(&c);
    return out;
}

std::vector<PogoEntry> pogo_entries(const pe::Image& image) {
    std::vector<PogoEntry> out;
    ByteSpan d = image.data();
    for (const auto& e : image.debug_entries()) {
        if (e.type != pe::debug_type::pogo || e.size < 4 || u64(e.file_offset) + e.size > d.size()) continue;
        const u32 signature = read_le<u32>(d, e.file_offset).value_or(0);
        // PGU, PGI, LTCG, or none (link.exe of Visual Studio 2026 without profile-guided optimization).
        if (signature != 0x00554750u && signature != 0x00494750u && signature != 0x4C544347u && signature != 0) continue;
        u32 at = e.file_offset + 4;
        const u32 end = e.file_offset + e.size;
        while (at + 8 < end) {
            PogoEntry p;
            p.rva = read_le<u32>(d, at).value_or(0);
            p.size = read_le<u32>(d, at + 4).value_or(0);
            u32 n = at + 8;
            while (n < end && d[n] != std::byte{0}) p.name.push_back(static_cast<char>(d[n++]));
            at = (n + 1 + 3) & ~3u;
            if (!p.name.empty()) out.push_back(std::move(p));
        }
        break;
    }
    return out;
}

namespace {

std::string section_name_at(const pe::Image& image, u32 rva) {
    const auto* s = image.section_for_rva(rva);
    return s ? s->name : std::string();
}

} // namespace

std::vector<std::string> link_order(const ImageLayout& layout, const std::vector<std::string>& units) {
    std::map<std::string, usize> index;
    for (usize i = 0; i < units.size(); ++i) index.emplace(units[i], i);
    // Each group's units in address order: an edge from each to the next.
    std::vector<std::set<usize>> next(units.size());
    std::vector<usize> waiting(units.size(), 0);
    std::map<std::tuple<std::string, std::string, u32>, usize> last;
    for (const auto& c : layout.contributions) {
        if (c.linker || c.size == 0) continue;
        const auto it = index.find(c.unit);
        if (it == index.end()) continue;
        // Contents and access, not alignment: what the linker groups input sections of one name by.
        const auto key = std::make_tuple(c.section, c.name, c.characteristics & 0xFE0000E0u);
        if (auto prev = last.find(key); prev != last.end() && prev->second != it->second) {
            if (next[prev->second].insert(it->second).second) ++waiting[it->second];
        }
        last[key] = it->second;
    }
    // Kahn's algorithm, the earliest of `units` first; a cycle (groups that disagree) is broken there too.
    std::set<usize> ready;
    for (usize i = 0; i < units.size(); ++i)
        if (waiting[i] == 0) ready.insert(i);
    std::vector<bool> placed(units.size(), false);
    std::vector<std::string> out;
    while (out.size() < units.size()) {
        usize u = 0;
        if (!ready.empty()) {
            u = *ready.begin();
            ready.erase(ready.begin());
        } else {
            while (placed[u]) ++u;
        }
        if (placed[u]) continue;
        placed[u] = true;
        out.push_back(units[u]);
        for (const usize n : next[u])
            if (!placed[n] && --waiting[n] == 0) ready.insert(n);
    }
    return out;
}

void attribute_exception_table(ImageLayout& layout, const pe::Image& image) {
    if (image.machine() != pe::machine::amd64) return;
    const auto [table, size] = image.data_directory(3);
    if (!table || size < 12) return;
    const u32 end = table + size;
    auto held = [](const Contribution* c) { return c && !c->linker && !c->unit.empty(); };
    bool all_held = true;
    for (u32 at = table; at + 12 <= end && all_held; at += 12) all_held = held(layout.at(at));
    if (all_held) return;
    // What the linker or no unit holds of the table goes; what it holds past the table stays.
    std::vector<Contribution> entries;
    std::erase_if(layout.contributions, [&](const Contribution& c) {
        if (held(&c) || !c.size || c.end() <= table || c.rva >= end) return false;
        if (c.rva < table) {
            Contribution before = c;
            before.size = table - c.rva;
            entries.push_back(std::move(before));
        }
        if (c.end() > end) {
            Contribution after = c;
            after.rva = end;
            after.size = c.end() - end;
            entries.push_back(std::move(after));
        }
        return true;
    });
    for (u32 at = table; at + 12 <= end; at += 12) {
        if (layout.at(at)) continue;  // still a unit's
        const auto bytes = image.read_rva(at, 4);
        const u32 begin = bytes ? read_le<u32>(*bytes, 0).value_or(0) : 0;
        const Contribution* function = begin ? layout.at(begin) : nullptr;
        Contribution c;
        c.rva = at;
        c.size = 12;
        c.section = section_name_at(image, at);
        c.name = ".pdata";
        // Read-only data aligned to 4, a COMDAT with its function's code when that is one. The entry of a
        // function the linker made is the linker's; one of a function no unit holds stays without a unit.
        c.characteristics = 0x40300040u | (function ? function->characteristics & pe::scn::lnk_comdat : 0);
        c.unit = function ? function->unit : std::string();
        c.linker = !function || function->linker;
        entries.push_back(std::move(c));
    }
    layout.contributions.insert(layout.contributions.end(), entries.begin(), entries.end());
    std::ranges::stable_sort(layout.contributions, {}, &Contribution::rva);
}

ImageLayout layout_from_pdb(const pdb::Reader& pdb, const pe::Image& image) {
    ImageLayout layout;
    layout.source = LayoutSource::pdb;
    const auto names = pdb_unit_names(pdb);
    for (usize i = 0; i < pdb.modules().size() && i < names.size(); ++i)
        if (pdb.modules()[i].language >= 0)
            layout.origins[names[i]] = {pdb.modules()[i].language, pdb.modules()[i].backend_build, pdb.modules()[i].compiler,
                                        pdb.modules()[i].security_checks, pdb.modules()[i].sdl};
    // Empty contributions stay: an empty section still aligns what the linker puts after it.
    for (const auto& c : pdb.contributions()) {
        if (c.module >= names.size()) continue;
        Contribution out;
        out.rva = c.rva;
        out.size = c.size;
        out.section = section_name_at(image, c.rva);
        out.characteristics = c.characteristics;
        out.unit = names[c.module];
        const auto kind = unit_kind_of(out.unit);
        out.linker = kind == UnitKind::linker || kind == UnitKind::import;
        layout.contributions.push_back(std::move(out));
    }
    std::ranges::stable_sort(layout.contributions, {}, &Contribution::rva);
    // The linker's record of each input section name's range names the contributions in it.
    for (const auto& g : pdb.coff_groups())
        for (auto& c : layout.contributions)
            if (c.name.empty() && c.rva >= g.rva && c.rva < g.rva + std::max<u32>(g.size, 1)) c.name = g.name;
    name_contributions(layout, image);
    attribute_exception_table(layout, image);
    return layout;
}

namespace {

// Byte ranges the linker writes itself, as (rva, end).
std::vector<std::pair<u32, u32>> linker_ranges(const Program& program) {
    const auto& image = program.image();
    std::vector<std::pair<u32, u32>> out;
    auto add = [&](u32 rva, u32 size) {
        if (rva && size) out.emplace_back(rva, rva + size);
    };
    ByteSpan d = image.data();
    const u32 pointer = image.is_pe32_plus() ? 8 : 4;
    add(image.data_directory(5).first, image.data_directory(5).second);  // base relocations
    add(image.data_directory(0).first, image.data_directory(0).second);  // exports
    add(image.data_directory(6).first, image.data_directory(6).second);  // debug directory
    add(image.data_directory(12).first, image.data_directory(12).second);  // IAT
    for (const auto& e : image.debug_entries()) add(e.rva, e.size);
    // Import descriptors, lookup tables, hint/name entries and DLL names.
    if (const u32 imports = image.data_directory(1).first) {
        for (u32 desc = imports;; desc += 20) {
            auto off = image.rva_to_offset(desc);
            if (!off) break;
            const u32 ilt = read_le<u32>(d, *off).value_or(0);
            const u32 name = read_le<u32>(d, *off + 12).value_or(0);
            const u32 iat = read_le<u32>(d, *off + 16).value_or(0);
            add(desc, 20);
            if (!ilt && !name && !iat) break;
            if (auto name_off = image.rva_to_offset(name)) {
                const auto dll = read_cstring_at(d, *name_off, 256).value_or("");
                add(name, static_cast<u32>(dll.size() + 1 + ((dll.size() + 1) % 2)));
            }
            for (u32 table : {ilt, iat}) {
                if (!table) continue;
                for (u32 i = 0;; ++i) {
                    auto entry_off = image.rva_to_offset(table + i * pointer);
                    if (!entry_off) break;
                    const u64 entry = pointer == 8 ? read_le<u64>(d, *entry_off).value_or(0) : read_le<u32>(d, *entry_off).value_or(0);
                    add(table + i * pointer, pointer);
                    if (entry == 0) break;
                    const bool by_ordinal = pointer == 8 ? (entry >> 63) != 0 : (entry >> 31) != 0;
                    if (by_ordinal || table == iat) continue;
                    const u32 hint_name = static_cast<u32>(entry & 0x7FFFFFFF);
                    if (auto hn = image.rva_to_offset(hint_name)) {
                        const auto text = read_cstring_at(d, *hn + 2, 4096).value_or("");
                        const u32 size = static_cast<u32>(2 + text.size() + 1);
                        add(hint_name, size + (size % 2));
                    }
                }
            }
        }
    }
    // Import thunks (jmp [IAT slot]) in code.
    for (const auto& [va, s] : program.symbols()) {
        if (!image.is_code(va)) continue;
        if (auto to = program.thunk_destination(va)) {
            const auto* slot = program.symbols().at(*to);
            if (slot && slot->kind == SymbolKind::import) add(static_cast<u32>(va - image.image_base()), 6);
        }
    }
    std::ranges::sort(out);
    // Merge overlapping and adjacent ranges.
    std::vector<std::pair<u32, u32>> merged;
    for (const auto& r : out) {
        if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
        else merged.push_back(r);
    }
    return merged;
}

} // namespace

ImageLayout layout_from_units(const Program& program, const UnitLayout& units) {
    const auto& image = program.image();
    ImageLayout layout;
    layout.source = LayoutSource::symbols;
    const auto linker = linker_ranges(program);
    auto in_linker = [&](u32 rva) {
        auto it = std::ranges::upper_bound(linker, rva, {}, [](const auto& r) { return r.first; });
        if (it == linker.begin()) return false;
        --it;
        return rva < it->second;
    };
    // Unit boundaries: each symbol with a unit starts a stretch of that unit (an import library's unit
    // holds nothing an object carries: the linker makes its part). On x64 each function's unwind
    // information starts a stretch of .xdata of the function's unit.
    struct Start {
        std::string unit;
        std::string name;  // the input section's name, when known
    };
    std::map<u32, Start> starts;
    for (const auto& [va, unit] : units.members) {
        if (va < image.image_base()) continue;
        const auto* s = program.symbols().at(va);
        if (s && s->kind == SymbolKind::label) continue;
        const auto kind = unit_kind_of(unit);
        starts.emplace(static_cast<u32>(va - image.image_base()), Start{kind == UnitKind::import || kind == UnitKind::linker ? "" : unit, ""});
    }
    if (image.is_pe32_plus())
        for (const auto& f : image.runtime_functions())
            starts.insert_or_assign(f.unwind_rva & ~1u, Start{std::string(units.unit_of(image.image_base() + f.begin_rva)), ".xdata"});
    for (const auto& section : image.sections()) {
        if (section.name == ".reloc" || section.name == ".rsrc") continue;
        // The section's size in memory: the file's alignment padding past it is not the section's.
        const u32 begin = section.virtual_address;
        const u32 end = begin + (section.virtual_size ? section.virtual_size : section.raw_size);
        // The unit of the first symbol in the section owns what comes before it (in a section of its own name).
        auto it = starts.lower_bound(begin);
        Start current = it != starts.end() && it->first < end ? Start{it->second.unit, ""} : Start{};
        auto emit = [&](u32 from, u32 to, const Start& owner, bool made_by_linker) {
            if (from >= to) return;
            if (!layout.contributions.empty()) {
                auto& last = layout.contributions.back();
                if (last.end() == from && last.unit == owner.unit && last.linker == made_by_linker && last.section == section.name &&
                    last.name == owner.name) {
                    last.size = to - last.rva;
                    return;
                }
            }
            Contribution c;
            c.rva = from;
            c.size = to - from;
            c.section = section.name;
            c.name = owner.name;
            c.characteristics = section.characteristics & (pe::scn::cnt_code | pe::scn::cnt_initialized_data | pe::scn::cnt_uninitialized_data |
                                                           pe::scn::mem_execute | pe::scn::mem_read | pe::scn::mem_write);
            c.unit = owner.unit;
            c.linker = made_by_linker;
            layout.contributions.push_back(std::move(c));
        };
        u32 pos = begin;
        while (pos < end) {
            // The next point where the owner or the linker state changes.
            u32 next = end;
            if (auto s = starts.upper_bound(pos); s != starts.end() && s->first < next) next = s->first;
            const bool made_by_linker = in_linker(pos);
            for (const auto& r : linker) {
                if (r.first > pos && r.first < next) next = r.first;
                if (r.first <= pos && r.second > pos && r.second < next) next = r.second;
            }
            if (auto s = starts.find(pos); s != starts.end()) current = s->second;
            emit(pos, next, made_by_linker ? Start{} : current, made_by_linker);
            pos = next;
        }
    }
    // Zero padding right after the linker's own tables is the linker's too (it aligns what it adds).
    for (usize i = 1; i < layout.contributions.size(); ++i) {
        auto& c = layout.contributions[i];
        const auto& before = layout.contributions[i - 1];
        if (c.linker || !before.linker || before.end() != c.rva || c.size >= 16) continue;
        auto bytes = image.read_rva(c.rva, c.size);
        if (bytes && std::ranges::all_of(*bytes, [](std::byte b) { return b == std::byte{0}; })) c.linker = true;
    }
    // The uninitialized tail of a section (past its file data) is uninitialized data.
    for (auto& c : layout.contributions) {
        const auto* s = image.section_for_rva(c.rva);
        if (s && c.rva >= s->virtual_address + s->raw_size) c.characteristics = (c.characteristics & ~pe::scn::cnt_initialized_data) |
                                                                                pe::scn::cnt_uninitialized_data;
    }
    name_contributions(layout, image);
    attribute_exception_table(layout, image);
    return layout;
}

void name_contributions(ImageLayout& layout, const pe::Image& image) {
    const auto pogo = pogo_entries(image);
    std::set<u32> unwind;
    if (image.is_pe32_plus())
        for (const auto& f : image.runtime_functions()) unwind.insert(f.unwind_rva & ~1u);
    for (auto& c : layout.contributions) {
        if (!c.name.empty()) continue;
        for (const auto& p : pogo)
            if (c.rva >= p.rva && c.rva < p.rva + std::max<u32>(p.size, 1)) c.name = p.name;
        if (!c.name.empty()) continue;
        if (c.uninitialized()) {
            c.name = ".bss";
        } else if (c.section == ".rdata" && image.is_pe32_plus() && unwind.contains(c.rva)) {
            c.name = ".xdata";
        } else {
            c.name = c.section;
        }
    }
}

} // namespace decomp
