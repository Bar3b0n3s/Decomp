#include "matching/unit_check.hpp"

#include "core/strings.hpp"
#include "formats/coff_writer.hpp"
#include "matching/diff.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <unordered_map>

namespace decomp::matching {

std::string_view to_string(PlacementState state) {
    switch (state) {
    case PlacementState::equal: return "equal";
    case PlacementState::differs: return "differs";
    case PlacementState::unplaced: return "unplaced";
    case PlacementState::discarded: return "discarded";
    }
    return "?";
}

bool UnitCheckResult::ok() const {
    if (!missing.empty() || !problems.empty()) return false;
    for (const auto& s : sections) {
        if (s.state == PlacementState::differs) return false;
        if (s.state == PlacementState::unplaced && !s.comdat) return false;
    }
    return true;
}

usize UnitCheckResult::count(PlacementState state) const {
    return static_cast<usize>(std::ranges::count(sections, state, &PlacedSection::state));
}

std::string UnitCheckResult::summary() const {
    std::vector<std::string> parts;
    parts.push_back(std::format("{} of {} sections equal", count(PlacementState::equal), sections.size()));
    for (const auto& s : sections) {
        if (s.state == PlacementState::differs)
            parts.push_back(std::format("{} {} differs at +{:#x}", s.name, s.symbol, s.first_difference.value_or(0)));
        else if (s.state == PlacementState::unplaced && !s.comdat)
            parts.push_back(std::format("{} {} not placed", s.name, s.symbol));
    }
    for (const auto& c : missing) parts.push_back(std::format("{} at {:#x} ({} bytes) missing", c.name, c.rva, c.size));
    for (const auto& p : problems) parts.push_back(p);
    return join(parts, "; ");
}

namespace {

bool goes_into_image(const coff::Section& s) {
    if (s.size == 0) return false;
    if (s.characteristics & (coff::scn_flags::lnk_remove | coff::scn_flags::lnk_info)) return false;
    if (s.name.starts_with(".debug")) return false;
    return (s.characteristics & (pe::scn::cnt_code | pe::scn::cnt_initialized_data | pe::scn::cnt_uninitialized_data)) != 0;
}

// The program's symbol of exactly this (decorated) name or alias.
const Symbol* exact_symbol(const SymbolDb& symbols, std::string_view name) {
    const auto* s = symbols.find(name);
    if (!s) return nullptr;
    if (s->name == name || std::ranges::find(s->aliases, name) != s->aliases.end()) return s;
    return nullptr;
}

// The COMDAT symbol of a section: the first symbol after its section symbol, defined in it.
const coff::Symbol* naming_symbol(const coff::Object& object, const coff::Section& section) {
    const coff::Symbol* first = nullptr;
    for (const auto& s : object.symbols()) {
        if (s.section_number != static_cast<i32>(section.number) || s.is_section_symbol() || s.storage_class == coff::storage::file) continue;
        if (!first) first = &s;
        if (section.comdat) return &s;  // the COMDAT symbol comes first
    }
    return first;
}

struct Field {
    unsigned size = 0;
    i64 addend = 0;
};

Field field_of(const coff::Object& object, const coff::Section& section, const coff::Relocation& r) {
    Field f;
    f.size = object.relocation_size(r.type);
    if (f.size == 8) f.addend = static_cast<i64>(read_le<u64>(section.data, r.offset).value_or(0));
    else if (f.size == 4) f.addend = read_le<i32>(section.data, r.offset).value_or(0);
    else if (f.size == 2) f.addend = read_le<i16>(section.data, r.offset).value_or(0);
    return f;
}

bool x64(const coff::Object& object) { return object.machine() == pe::machine::amd64; }

// The extra distance from the end of the field to the next instruction an x64 REL32_<n> relocation counts.
std::optional<u32> rel32_extra(const coff::Object& object, u16 type) {
    if (x64(object)) {
        if (type >= coff::reloc_amd64::rel32 && type <= coff::reloc_amd64::rel32_5) return type - coff::reloc_amd64::rel32;
        return std::nullopt;
    }
    if (type == coff::reloc_i386::rel32) return 0;
    return std::nullopt;
}

bool absolute32(const coff::Object& object, u16 type) {
    return x64(object) ? type == coff::reloc_amd64::addr32 : type == coff::reloc_i386::dir32;
}
bool absolute64(const coff::Object& object, u16 type) { return x64(object) && type == coff::reloc_amd64::addr64; }
bool image_relative(const coff::Object& object, u16 type) {
    return x64(object) ? type == coff::reloc_amd64::addr32nb : type == coff::reloc_i386::dir32nb;
}

// Where the image's field at `p` (an RVA) says the relocation's target is.
std::optional<u32> image_target(const pe::Image& image, const coff::Object& object, const coff::Relocation& r, const Field& f, u32 p) {
    const u64 base = image.image_base();
    if (absolute64(object, r.type)) {
        auto v = image.read_rva(p, 8);
        if (!v) return std::nullopt;
        const u64 value = read_le<u64>(*v, 0).value_or(0);
        return static_cast<u32>(value - base - static_cast<u64>(f.addend));
    }
    auto v = image.read_rva(p, 4);
    if (!v || f.size != 4) return std::nullopt;
    const u32 value = read_le<u32>(*v, 0).value_or(0);
    if (absolute32(object, r.type)) return static_cast<u32>(value - static_cast<u32>(base) - static_cast<u32>(f.addend));
    if (image_relative(object, r.type)) return static_cast<u32>(value - static_cast<u32>(f.addend));
    if (auto extra = rel32_extra(object, r.type)) return static_cast<u32>(value + p + 4 + *extra - static_cast<u32>(f.addend));
    return std::nullopt;
}

// The value the linker writes for the relocation when its target is at RVA `t`; nullopt for a type this
// check does not compute.
std::optional<u64> expected_field(const pe::Image& image, const coff::Object& object, const coff::Relocation& r, const Field& f, u32 p, u32 t) {
    const u64 base = image.image_base();
    if (absolute64(object, r.type)) return base + t + static_cast<u64>(f.addend);
    if (absolute32(object, r.type)) return static_cast<u32>(base + t + static_cast<u64>(f.addend));
    if (image_relative(object, r.type)) return static_cast<u32>(t + static_cast<u64>(f.addend));
    if (auto extra = rel32_extra(object, r.type)) return static_cast<u32>(t + static_cast<u64>(f.addend) - (p + 4 + *extra));
    const bool secrel = x64(object) ? r.type == coff::reloc_amd64::secrel : r.type == coff::reloc_i386::secrel;
    if (secrel) {
        if (const auto* s = image.section_for_rva(t)) return static_cast<u32>(t - s->virtual_address + static_cast<u64>(f.addend));
        return std::nullopt;
    }
    const bool section_index = x64(object) ? r.type == coff::reloc_amd64::section : r.type == coff::reloc_i386::section;
    if (section_index) {
        const auto& sections = image.sections();
        for (usize i = 0; i < sections.size(); ++i)
            if (&sections[i] == image.section_for_rva(t)) return static_cast<u16>(i + 1);
        return std::nullopt;
    }
    return std::nullopt;
}

u32 align_up(u32 value, u32 alignment) { return alignment > 1 ? (value + alignment - 1) / alignment * alignment : value; }

} // namespace

UnitCheckResult check_unit(const Program& program, const ImageLayout& layout, const coff::Object& object, std::string_view unit,
                           const std::vector<u64>& functions) {
    UnitCheckResult out;
    out.unit = std::string(unit);
    const auto& image = program.image();
    const u64 base = image.image_base();

    // The sections that go into the image, and each name's sections in object order.
    std::vector<const coff::Section*> kept;
    std::map<u32, usize> index_of;  // section number -> index in kept / out.sections
    for (const auto& s : object.sections()) {
        if (!goes_into_image(s)) continue;
        index_of[s.number] = kept.size();
        kept.push_back(&s);
        PlacedSection p;
        p.section = s.number;
        p.name = s.name;
        p.size = s.size;
        p.alignment = coff::alignment_of(s.characteristics);
        p.uninitialized = s.is_bss();
        p.code = s.is_code();
        p.comdat = s.comdat.has_value();
        p.selection = s.comdat ? s.comdat->selection : 0;
        if (const auto* sym = naming_symbol(object, s)) p.symbol = sym->name;
        out.sections.push_back(std::move(p));
    }
    std::map<std::string, std::vector<usize>> blocks;
    for (usize i = 0; i < kept.size(); ++i) blocks[kept[i]->name].push_back(i);
    auto sec_alignment = [&](usize i) { return std::max<u32>(out.sections[i].alignment, 1); };

    // Anchors: an RVA for a section, from one of its symbols or from a reference to it. The unit's own
    // functions pin their sections.
    struct Anchor {
        std::string why;
        bool function = false;
    };
    std::map<usize, std::map<u32, Anchor>> anchors;  // section index -> rva -> what said so
    auto anchor = [&](usize i, u32 rva, std::string why, bool function = false) {
        auto [it, inserted] = anchors[i].emplace(rva, Anchor{std::move(why), function});
        if (!inserted && function) it->second.function = true;
        return inserted;
    };
    for (u64 va : functions) {
        const auto* target = program.symbols().at(va);
        if (!target) continue;
        const auto* cs = find_candidate_symbol(object, *target);
        if (!cs || !index_of.contains(static_cast<u32>(cs->section_number))) continue;
        anchor(index_of[static_cast<u32>(cs->section_number)], static_cast<u32>(va - base - cs->value), std::format("function {}", target->name), true);
    }
    for (const auto& s : object.symbols()) {
        if (!s.is_defined() || !s.is_external() || !index_of.contains(static_cast<u32>(s.section_number))) continue;
        const auto* known = exact_symbol(program.symbols(), s.name);
        if (!known || known->va < base) continue;
        anchor(index_of[static_cast<u32>(s.section_number)], static_cast<u32>(known->va - base - s.value), std::format("symbol {}", s.name));
    }

    std::map<usize, u32> placed;  // section index -> rva
    std::set<usize> discarded;
    std::map<usize, std::string> discarded_unit;
    std::map<usize, std::string> folded;  // section index -> the section it is folded into
    auto other_unit_at = [&](u32 rva) -> std::optional<std::string> {
        const auto* c = layout.at(rva);
        if (!c || c->unit == unit || c->linker || c->unit.empty()) return std::nullopt;
        return c->unit;
    };

    // A COMDAT that only references put in another unit's contribution is that unit's copy, which the
    // linker keeps: a string or constant pooled across units. Then each name's sections go in a row:
    // a section follows the one before it unless its anchors put it elsewhere (a gap: a function of the
    // unit is not in the source, or the object differs), and sections before the first anchored one
    // end where the next begins.
    std::vector<std::string> gaps;
    const bool sorted_table = image.machine() == pe::machine::amd64;
    auto place_blocks = [&] {
        placed.clear();
        gaps.clear();
        for (usize i = 0; i < kept.size(); ++i) {
            if (!out.sections[i].comdat || discarded.contains(i)) continue;
            const auto& here = anchors[i];
            if (here.empty() || std::ranges::any_of(here, [](const auto& a) { return a.second.function; })) continue;
            std::optional<std::string> other;
            for (const auto& [rva, a] : here) {
                other = other_unit_at(rva);
                if (!other) break;
            }
            if (other) {
                discarded.insert(i);
                discarded_unit[i] = *other;
            }
        }
        // A COMDAT anchored where a section of this object with the same bytes is pinned is folded into it
        // (/OPT:ICF): two functions the compiler made alike, one address.
        for (usize i = 0; i < kept.size(); ++i) {
            if (!out.sections[i].comdat || discarded.contains(i)) continue;
            const auto& here = anchors[i];
            if (here.empty() || std::ranges::any_of(here, [](const auto& a) { return a.second.function; })) continue;
            for (usize j = 0; j < kept.size(); ++j) {
                if (j == i || discarded.contains(j) || kept[j]->size != kept[i]->size || kept[j]->data != kept[i]->data) continue;
                const auto& theirs = anchors[j];
                const bool pinned_there = std::ranges::any_of(theirs, [&](const auto& a) { return a.second.function && here.contains(a.first); });
                if (!pinned_there) continue;
                discarded.insert(i);
                discarded_unit[i] = std::string(unit);
                folded[i] = out.sections[j].symbol;
                break;
            }
        }
        for (auto& [name, members] : blocks) {
            // x64 .pdata: the linker sorts the exception table by address, so each section is where its
            // function's entries are, whatever came before it in the object.
            if (sorted_table && name == ".pdata") {
                for (usize i : members) {
                    if (discarded.contains(i) || anchors[i].empty()) continue;
                    const auto& sec = out.sections[i];
                    placed[i] = anchors[i].begin()->first;
                    for (const auto& [rva, a] : anchors[i])
                        if (rva != placed[i])
                            gaps.push_back(std::format("{} {} is placed at {:#x}, but {} puts it at {:#x}", sec.name, sec.symbol, placed[i], a.why, rva));
                }
                continue;
            }
            std::optional<u32> cursor;
            std::string previous;
            std::vector<usize> pending;
            for (usize i : members) {
                if (discarded.contains(i)) continue;
                const auto& sec = out.sections[i];
                const auto& here = anchors[i];
                std::optional<u32> at;
                const std::optional<u32> next = cursor ? std::optional<u32>(align_up(*cursor, sec.alignment)) : std::nullopt;
                if (next && (here.empty() || here.contains(*next))) {
                    at = next;
                } else if (!here.empty()) {
                    auto f = std::ranges::find_if(here, [](const auto& a) { return a.second.function; });
                    at = f != here.end() ? f->first : here.begin()->first;
                    if (next && *at > *next)
                        gaps.push_back(std::format("{} bytes between {} and {} {} at {:#x}, which the linker would close", *at - *next, previous,
                                                   sec.name, sec.symbol, *at));
                    else if (next)
                        gaps.push_back(std::format("{} {} at {:#x} overlaps {}", sec.name, sec.symbol, *at, previous));
                }
                if (!at) {
                    pending.push_back(i);
                    continue;
                }
                u32 end = *at;
                for (auto p = pending.rbegin(); p != pending.rend(); ++p) {
                    const auto& ps = out.sections[*p];
                    const u32 align = std::max<u32>(ps.alignment, 1);
                    const u32 start = end >= ps.size ? (end - ps.size) / align * align : 0;
                    placed[*p] = start;
                    end = start;
                }
                pending.clear();
                placed[i] = *at;
                cursor = *at + sec.size;
                previous = std::format("{} {}", sec.name, sec.symbol);
                for (const auto& [rva, a] : here)
                    if (rva != *at)
                        gaps.push_back(std::format("{} {} is placed at {:#x}, but {} puts it at {:#x}", sec.name, sec.symbol, *at, a.why, rva));
            }
        }
    };

    // A section nothing refers to (x64 .pdata) is found by its relocations: where the image holds, at the
    // same offsets, the values its placed targets give. Image sections are indexed by their aligned values.
    std::map<std::string, std::unordered_multimap<u32, u32>> value_index;  // image section -> value -> rva
    auto values_of = [&](const pe::SectionHeader& s) -> const std::unordered_multimap<u32, u32>& {
        auto [it, inserted] = value_index.try_emplace(s.name);
        if (inserted) {
            const u32 size = std::min(s.virtual_size ? s.virtual_size : s.raw_size, s.raw_size);
            if (auto bytes = image.read_rva(s.virtual_address, size))
                for (u32 at = 0; at + 4 <= size; at += 4) it->second.emplace(read_le<u32>(*bytes, at).value_or(0), s.virtual_address + at);
        }
        return it->second;
    };
    auto reverse_anchor = [&](usize i) -> bool {
        const auto* sec = kept[i];
        struct Want {
            u32 offset = 0;
            unsigned size = 0;
            u64 value = 0;
        };
        std::vector<Want> wants;
        for (const auto& r : sec->relocations) {
            if (!absolute32(object, r.type) && !absolute64(object, r.type) && !image_relative(object, r.type)) continue;
            const auto* target = object.symbol_at_index(r.symbol_index);
            if (!target || !target->is_defined() || !index_of.contains(static_cast<u32>(target->section_number))) continue;
            auto tp = placed.find(index_of[static_cast<u32>(target->section_number)]);
            if (tp == placed.end()) continue;
            const Field f = field_of(object, *sec, r);
            if (auto v = expected_field(image, object, r, f, 0, tp->second + target->value)) wants.push_back({r.offset, f.size, *v});
        }
        if (wants.empty() || wants[0].offset % 4 != 0) return false;
        std::set<std::string> outputs;
        for (const auto& c : layout.contributions)
            if (c.name == sec->name) outputs.insert(c.section);
        if (outputs.empty()) outputs.insert(sec->name.substr(0, sec->name.find('$')));
        for (const auto& s : image.sections()) {
            if (!outputs.contains(s.name)) continue;
            const auto& values = values_of(s);
            auto [lo, hi] = values.equal_range(static_cast<u32>(wants[0].value));
            for (auto it = lo; it != hi; ++it) {
                const u32 start = it->second - wants[0].offset;
                if (start % sec_alignment(i) != 0) continue;
                bool all = true;
                for (const auto& w : wants) {
                    auto v = image.read_rva(start + w.offset, w.size);
                    u64 value = 0;
                    if (v)
                        for (unsigned b = 0; b < w.size; ++b) value |= static_cast<u64>((*v)[b]) << (8 * b);
                    if (!v || value != w.value) {
                        all = false;
                        break;
                    }
                }
                if (all) {
                    anchor(i, start, "the values its relocations give");
                    return true;
                }
            }
        }
        return false;
    };

    // Relocations of placed sections anchor their targets; repeat until nothing new is placed.
    for (int round = 0; round < 64; ++round) {
        place_blocks();
        bool added = false;
        for (const auto& [i, rva] : placed) {
            const auto* sec = kept[i];
            for (const auto& r : sec->relocations) {
                const auto* target = object.symbol_at_index(r.symbol_index);
                if (!target || !target->is_defined() || !index_of.contains(static_cast<u32>(target->section_number))) continue;
                const usize ti = index_of[static_cast<u32>(target->section_number)];
                const Field f = field_of(object, *sec, r);
                auto t = image_target(image, object, r, f, rva + r.offset);
                if (!t) continue;
                const u32 section_rva = *t - target->value;
                if (anchor(ti, section_rva, std::format("the reference at {}+{:#x}", out.sections[i].symbol, r.offset))) added = true;
            }
        }
        if (!added)
            for (usize i = 0; i < kept.size(); ++i)
                if (!placed.contains(i) && !discarded.contains(i) && anchors[i].empty() && reverse_anchor(i)) added = true;
        if (!added) break;
    }
    place_blocks();
    // Associative sections go with their COMDAT.
    for (usize i = 0; i < kept.size(); ++i) {
        const auto& c = kept[i]->comdat;
        if (!c || c->selection != coff::comdat_select::associative) continue;
        if (auto it = index_of.find(c->associated_section); it != index_of.end() && discarded.contains(it->second)) {
            discarded.insert(i);
            discarded_unit[i] = discarded_unit[it->second];
            placed.erase(i);
        }
    }

    // External symbols this object references, bound to what the image holds there: one name, one place.
    std::map<std::string, u32> bindings;
    for (usize i = 0; i < kept.size(); ++i) {
        auto& p = out.sections[i];
        if (discarded.contains(i)) {
            p.state = PlacementState::discarded;
            p.unit = discarded_unit[i];
            if (auto f = folded.find(i); f != folded.end()) {
                p.folded_into = f->second;
                p.note = std::format("folded into {} (identical COMDAT folding)", f->second);
            }
            if (auto a = anchors.find(i); a != anchors.end() && !a->second.empty()) p.rva = a->second.begin()->first;
            continue;
        }
        auto at = placed.find(i);
        if (at == placed.end()) {
            p.state = PlacementState::unplaced;
            p.note = p.comdat ? "nothing in the image refers to it (the linker drops an unreferenced COMDAT with /OPT:REF)"
                              : "no symbol or reference says where it goes";
            continue;
        }
        p.rva = at->second;
        const auto* sec = kept[i];
        auto actual = image.read_rva(at->second, sec->size);
        if (!actual) {
            p.state = PlacementState::differs;
            p.note = std::format("{:#x} (+{} bytes) is not in the image", at->second, sec->size);
            continue;
        }
        std::vector<bool> checked(sec->size, false);
        auto mark = [&](u32 offset) {
            if (!p.first_difference || offset < *p.first_difference) p.first_difference = offset;
        };
        for (const auto& r : sec->relocations) {
            const Field f = field_of(object, *sec, r);
            if (f.size == 0 || r.offset + f.size > sec->size) continue;
            for (u32 b = 0; b < f.size; ++b) checked[r.offset + b] = true;
            const u32 pos = at->second + r.offset;
            const auto* target = object.symbol_at_index(r.symbol_index);
            std::optional<u32> t;
            if (target && target->is_defined() && index_of.contains(static_cast<u32>(target->section_number))) {
                const usize ti = index_of[static_cast<u32>(target->section_number)];
                if (auto tp = placed.find(ti); tp != placed.end()) t = tp->second + target->value;
                else if (auto a = anchors.find(ti); a != anchors.end() && !a->second.empty()) t = a->second.begin()->first + target->value;
            } else if (target && !target->is_defined()) {
                if (const auto* known = exact_symbol(program.symbols(), target->name); known && known->va >= base) {
                    t = static_cast<u32>(known->va - base);
                } else if (auto seen = image_target(image, object, r, f, pos)) {
                    auto [it, inserted] = bindings.emplace(target->name, *seen);
                    t = it->second;
                }
                if (t) out.externals.emplace(target->name, *t);
            }
            u64 actual_value = 0;
            for (u32 b = 0; b < f.size; ++b) actual_value |= static_cast<u64>((*actual)[r.offset + b]) << (8 * b);
            std::optional<u64> want = t ? expected_field(image, object, r, f, pos, *t) : std::nullopt;
            if (!want) {
                // Not computable (an unplaced target, an unusual type): the bytes must be the object's.
                u64 compiled = 0;
                for (u32 b = 0; b < f.size; ++b) compiled |= static_cast<u64>(sec->data[r.offset + b]) << (8 * b);
                if (!t) continue;
                want = compiled;
            }
            const u64 mask = f.size >= 8 ? ~u64{0} : ((u64{1} << (8 * f.size)) - 1);
            if ((actual_value & mask) != (*want & mask)) {
                p.differing_bytes += f.size;
                mark(r.offset);
                if (p.note.empty())
                    p.note = std::format("the {} at +{:#x} points to {:#x}, the image's to {:#x}", object.relocation_type_name(r.type), r.offset,
                                         *want & mask, actual_value & mask);
            }
        }
        if (sec->is_bss()) {
            for (u32 b = 0; b < sec->size; ++b)
                if ((*actual)[b] != std::byte{0}) {
                    ++p.differing_bytes;
                    mark(b);
                }
        } else {
            for (u32 b = 0; b < sec->size && b < sec->data.size(); ++b)
                if (!checked[b] && (*actual)[b] != sec->data[b]) {
                    ++p.differing_bytes;
                    mark(b);
                }
        }
        p.state = p.differing_bytes ? PlacementState::differs : PlacementState::equal;
        if (p.state == PlacementState::differs && p.note.empty())
            p.note = std::format("{} bytes differ, the first at +{:#x}", p.differing_bytes, p.first_difference.value_or(0));
    }

    for (const auto& s : object.symbols()) {
        if (!s.is_defined() || !s.is_external() || !index_of.contains(static_cast<u32>(s.section_number))) continue;
        if (auto it = placed.find(index_of[static_cast<u32>(s.section_number)]); it != placed.end())
            out.definitions.emplace(s.name, it->second + s.value);
    }

    // Against the PDB: the unit's contributions are exactly the placed sections.
    if (layout.source == LayoutSource::pdb) {
        std::map<u32, const PlacedSection*> by_rva;
        for (const auto& p : out.sections)
            if (p.rva && p.state != PlacementState::discarded && p.state != PlacementState::unplaced) by_rva[*p.rva] = &p;
        for (const auto* c : layout.of_unit(unit)) {
            if (c->linker || c->size == 0) continue;  // an empty section: the compiler makes it, nothing to fill
            auto it = by_rva.find(c->rva);
            if (it == by_rva.end()) {
                out.missing.push_back(*c);
            } else if (it->second->size != c->size) {
                out.problems.push_back(std::format("{} {} at {:#x} is {} bytes; the original's was {}", it->second->name, it->second->symbol, c->rva,
                                               it->second->size, c->size));
            }
        }
        for (const auto& p : out.sections) {
            if (!p.rva || p.state == PlacementState::discarded || p.state == PlacementState::unplaced) continue;
            const auto* c = layout.at(*p.rva);
            if (!c || c->rva != *p.rva || c->unit != unit)
                out.problems.push_back(std::format("{} {} goes to {:#x}, {}", p.name, p.symbol, *p.rva,
                                               c ? std::format("inside {}'s {} at {:#x}", c->unit, c->name, c->rva)
                                                 : std::string("where the original has no contribution")));
        }
    }
    out.problems.insert(out.problems.begin(), gaps.begin(), gaps.end());
    return out;
}

Json to_json(const UnitCheckResult& result) {
    Json sections = Json::array();
    for (const auto& s : result.sections) {
        Json j{{"section", s.section},
               {"name", s.name},
               {"symbol", s.symbol},
               {"size", s.size},
               {"alignment", s.alignment},
               {"comdat", s.comdat},
               {"state", std::string(to_string(s.state))}};
        if (s.rva) j["rva"] = *s.rva;
        if (!s.unit.empty()) j["unit"] = s.unit;
        if (!s.folded_into.empty()) j["folded_into"] = s.folded_into;
        if (s.first_difference) j["first_difference"] = *s.first_difference;
        if (s.differing_bytes) j["differing_bytes"] = s.differing_bytes;
        if (!s.note.empty()) j["note"] = s.note;
        sections.push_back(std::move(j));
    }
    Json missing = Json::array();
    for (const auto& c : result.missing) missing.push_back({{"rva", c.rva}, {"size", c.size}, {"name", c.name}, {"section", c.section}});
    return {{"unit", result.unit}, {"ok", result.ok()}, {"sections", std::move(sections)}, {"missing", std::move(missing)},
            {"problems", result.problems}};
}

} // namespace decomp::matching
