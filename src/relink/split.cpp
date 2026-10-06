#include "relink/split.hpp"

#include "formats/coff_writer.hpp"

#include <algorithm>
#include <bit>
#include <format>

namespace decomp::relink {

std::string image_base_symbol(const pe::Image& image) { return c_symbol(image, "__ImageBase"); }

std::string c_symbol(const pe::Image& image, std::string_view name) {
    return image.machine() == pe::machine::i386 ? "_" + std::string(name) : std::string(name);
}

namespace {

constexpr u32 kKeptCharacteristics = pe::scn::cnt_code | pe::scn::cnt_initialized_data | pe::scn::cnt_uninitialized_data |
                                     pe::scn::mem_discardable | pe::scn::mem_execute | pe::scn::mem_read | pe::scn::mem_write;

// The contribution's alignment, or without one the largest power of two its address allows (the linker
// puts a split section where the original's was as long as it starts aligned there).
u32 alignment_flags_for(const Contribution& c) {
    if (c.characteristics & coff::scn_flags::align_mask) return c.characteristics & coff::scn_flags::align_mask;
    const u32 alignment = c.rva ? std::min<u32>(u32{1} << std::countr_zero(c.rva), 8192) : 8192;
    return coff::alignment_flags(alignment);
}

} // namespace

Result<std::vector<std::byte>> write_split_object(const pe::Image& image, const SplitObjectSpec& spec, SplitStats* stats) {
    const bool x64 = image.machine() == pe::machine::amd64;
    coff::ObjectWriter w(image.machine());
    SplitStats local;
    const auto& relocs = image.base_relocations();
    const u64 base = image.image_base();
    const std::string image_base = image_base_symbol(image);

    for (const auto* c : spec.contributions) {
        const u32 characteristics = (c->characteristics & kKeptCharacteristics) | alignment_flags_for(*c);
        u32 section = 0;
        if (c->uninitialized()) {
            section = w.add_bss_section(c->name, characteristics, c->size);
        } else {
            auto data = image.read_rva(c->rva, c->size);
            if (!data) return make_error(ErrorCode::invalid_argument, "{} at {:#x} ({} bytes) is not in the image", c->name, c->rva, c->size);
            section = w.add_section(c->name, characteristics, std::move(*data));
        }
        ++local.sections;
        local.bytes += c->size;
        const bool code = (characteristics & pe::scn::cnt_code) != 0;
        // The names defined here; the one at the start of a COMDAT is its COMDAT symbol, so a compiled
        // unit's copy of the same COMDAT (a pooled string) is the one the linker drops.
        std::optional<u32> at_start;
        for (auto it = spec.definitions.lower_bound(c->rva); it != spec.definitions.end() && it->first < c->end(); ++it) {
            for (const auto& name : it->second) {
                const u32 handle = w.add_symbol(name, it->first - c->rva, static_cast<i32>(section), coff::storage::external, code ? 0x20 : 0);
                ++local.symbols;
                if (it->first == c->rva && !at_start) at_start = handle;
            }
        }
        // What was a COMDAT stays one, named or not: linkers order an object's COMDATs and its other
        // sections differently (lld-link takes the others first).
        if (c->characteristics & pe::scn::lnk_comdat) {
            if (at_start) {
                w.set_comdat(section, coff::comdat_select::any, at_start);
            } else {
                const u32 local_name = w.add_symbol(std::format("$decomp_{:x}", c->rva), 0, static_cast<i32>(section), coff::storage::static_);
                w.set_comdat(section, coff::comdat_select::noduplicates, local_name);
            }
        }
        if (c->uninitialized()) continue;

        // Base relocations: the same values, and the linker writes their base relocations again.
        auto& data = w.data(section);
        auto it = std::ranges::lower_bound(relocs, c->rva, {}, &pe::BaseRelocation::rva);
        for (; it != relocs.end() && it->rva < c->end(); ++it) {
            const u32 offset = it->rva - c->rva;
            const bool wide = it->type == 10;  // IMAGE_REL_BASED_DIR64
            if (it->type != 3 && !wide) continue;
            const u32 size = wide ? 8 : 4;
            if (offset + size > data.size()) continue;
            const u64 value = wide ? read_le<u64>(data, offset).value_or(0) : read_le<u32>(data, offset).value_or(0);
            const u32 target = static_cast<u32>(value - base);
            const u16 type = wide ? coff::reloc_amd64::addr64 : x64 ? coff::reloc_amd64::addr32 : coff::reloc_i386::dir32;
            u32 symbol = 0;
            if (auto slot = spec.import_slots.find(target); slot != spec.import_slots.end()) {
                symbol = w.undefined(slot->second);
                if (wide) write_le<u64>(data, offset, 0);
                else write_le<u32>(data, offset, 0);
            } else {
                symbol = w.undefined(image_base);
                if (wide) write_le<u64>(data, offset, target);
                else write_le<u32>(data, offset, target);
            }
            w.add_relocation(section, offset, symbol, type);
            ++local.relocations;
        }
    }
    for (const auto& d : spec.directives) w.add_directive(d);
    for (const auto& name : spec.safe_seh_handlers) {
        auto handle = w.find_defined(name);
        w.add_safe_seh_handler(handle ? *handle : w.undefined(name));
    }
    if (spec.comp_id) w.add_absolute("@comp.id", *spec.comp_id);
    // x86: the objects of a /SAFESEH image declare their exception handlers (.sxdata).
    if (!x64) w.add_absolute("@feat.00", 1);
    if (stats) *stats = local;
    return w.write();
}

} // namespace decomp::relink
