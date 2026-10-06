#include "relink/compare.hpp"

#include "core/hash.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <format>

namespace decomp::relink {

namespace {

std::string hex_of(ByteSpan bytes) {
    std::string out;
    for (auto b : bytes) out += std::format("{:02x}", static_cast<unsigned>(b));
    return out;
}

std::string describe_symbol(const SymbolDb& symbols, u64 va) {
    const auto* s = symbols.containing(va);
    if (!s) s = symbols.at_or_before(va);  // data without sizes
    if (!s) return {};
    const std::string& name = s->display.empty() ? s->name : s->display;
    return va == s->va ? name : std::format("{}+{:#x}", name, va - s->va);
}

} // namespace

ImageComparison compare_images(const pe::Image& original, std::vector<std::byte>& relinked, const ImageLayout& layout,
                               const SymbolDb& symbols, usize max_differences) {
    ImageComparison out;
    ByteSpan orig = original.data();
    out.original_sha1 = sha1_hex(orig);
    out.relinked_unstamped_sha1 = sha1_hex(relinked);
    out.original_size = orig.size();
    out.relinked_size = relinked.size();

    auto parsed = pe::Image::parse(relinked);
    if (parsed) {
        const auto theirs = parsed->identity_fields();
        for (const auto& f : original.identity_fields()) {
            if (f.name == "optional header CheckSum") continue;  // computed again once the rest is stamped
            auto it = std::ranges::find(theirs, f.name, &pe::IdentityField::name);
            if (it == theirs.end() || it->size != f.size || u64(f.offset) + f.size > orig.size() || u64(it->offset) + it->size > relinked.size())
                continue;
            StampedField s{f.name, it->offset, hex_of(orig.subspan(f.offset, f.size)),
                           hex_of(ByteSpan(relinked).subspan(it->offset, it->size))};
            std::copy_n(orig.begin() + f.offset, f.size, relinked.begin() + it->offset);
            if (s.original != s.relinked) out.stamped.push_back(std::move(s));
        }
        // The checksum covers the stamped fields: the linker's would be of its own timestamps.
        if (original.optional_header().checksum != 0) {
            const u32 at = parsed->optional_header().checksum_offset;
            const u32 before = read_le<u32>(relinked, at).value_or(0);
            const u32 sum = pe::compute_checksum(relinked, at);
            write_le<u32>(relinked, at, sum);
            if (before != sum)
                out.stamped.push_back({"optional header CheckSum", at, std::format("{:08x}", original.optional_header().checksum),
                                       std::format("{:08x}", before)});
        }
    }
    out.relinked_sha1 = sha1_hex(relinked);
    out.identical = out.relinked_sha1 == out.original_sha1;
    if (out.identical) return out;

    // Runs of differing bytes: the headers by file offset, the sections by RVA.
    auto add_run = [&](ByteDifference d, ByteSpan a, ByteSpan b) {
        const bool listed = out.differences.size() < max_differences;
        const bool first = d.rva && (!out.first || *d.rva < *out.first->rva);
        if (!listed && !first) return;
        d.original_bytes = hex_of(a.subspan(0, std::min<usize>(a.size(), 16)));
        d.relinked_bytes = hex_of(b.subspan(0, std::min<usize>(b.size(), 16)));
        if (d.rva) d.symbol = describe_symbol(symbols, original.image_base() + *d.rva);
        if (listed) out.differences.push_back(d);
        if (first) out.first = d;
    };
    const u32 header_end = std::min<u32>(original.header_size(), static_cast<u32>(std::min(orig.size(), relinked.size())));
    for (u32 i = 0; i < header_end;) {
        if (orig[i] == relinked[i]) {
            ++i;
            continue;
        }
        const std::string where = original.describe_header_offset(i);
        u32 j = i;
        while (j < header_end && orig[j] != relinked[j] && original.describe_header_offset(j) == where) ++j;
        out.differing_bytes += j - i;
        ByteDifference d;
        d.offset = i;
        d.size = j - i;
        d.where = where;
        add_run(d, orig.subspan(i, j - i), ByteSpan(relinked).subspan(i, j - i));
        i = j;
    }
    if (!parsed) {
        out.sections.push_back({"(the relinked image does not parse)", 0, std::nullopt, {}, true});
        return out;
    }
    for (const auto& s : original.sections()) {
        SectionDifference sd;
        sd.name = s.name;
        const pe::SectionHeader* other = nullptr;
        for (const auto& t : parsed->sections())
            if (t.name == s.name && t.virtual_address == s.virtual_address) other = &t;
        if (!other) {
            for (const auto& t : parsed->sections())
                if (t.name == s.name) other = &t;
        }
        if (!other) {
            sd.size_differs = true;
            out.sections.push_back(std::move(sd));
            continue;
        }
        sd.size_differs = other->virtual_size != s.virtual_size || other->raw_size != s.raw_size || other->virtual_address != s.virtual_address;
        const u32 size = std::max({s.raw_size, s.virtual_size, other->raw_size, other->virtual_size});
        auto a = original.read_rva(s.virtual_address, std::max(s.raw_size, s.virtual_size)).value_or(std::vector<std::byte>{});
        auto b = parsed->read_rva(other->virtual_address, std::max(other->raw_size, other->virtual_size)).value_or(std::vector<std::byte>{});
        // The stamped fields live in the buffer, not in `parsed`: read the relinked bytes from the buffer.
        for (u32 k = 0; k < b.size() && k < other->raw_size; ++k)
            if (u64(other->raw_offset) + k < relinked.size()) b[k] = relinked[other->raw_offset + k];
        a.resize(size);
        b.resize(size);
        for (u32 i = 0; i < size;) {
            if (a[i] == b[i]) {
                ++i;
                continue;
            }
            const u32 rva = s.virtual_address + i;
            const auto* c = layout.at(rva);
            u32 j = i;
            while (j < size && a[j] != b[j] && layout.at(s.virtual_address + j) == c) ++j;
            sd.differing_bytes += j - i;
            out.differing_bytes += j - i;
            ByteDifference d;
            d.offset = i < s.raw_size ? s.raw_offset + i : 0;
            d.size = j - i;
            d.rva = rva;
            d.where = std::format("{}+{:#x}", s.name, i);
            if (c) d.unit = c->unit;
            if (!sd.first_rva) {
                sd.first_rva = rva;
                sd.first_unit = d.unit;
            }
            add_run(d, ByteSpan(a).subspan(i, j - i), ByteSpan(b).subspan(i, j - i));
            i = j;
        }
        if (sd.differing_bytes || sd.size_differs) out.sections.push_back(std::move(sd));
    }
    for (const auto& t : parsed->sections()) {
        if (std::ranges::find(original.sections(), t.name, &pe::SectionHeader::name) == original.sections().end())
            out.sections.push_back({t.name, 0, std::nullopt, {}, true});
    }
    // Data past the last section (an overlay, or a file of another size).
    if (orig.size() != relinked.size()) {
        ByteDifference d;
        d.offset = static_cast<u32>(std::min(orig.size(), relinked.size()));
        d.size = static_cast<u32>(std::max(orig.size(), relinked.size()) - d.offset);
        d.where = std::format("file size: {} bytes, the original's {}", relinked.size(), orig.size());
        if (out.differences.size() < max_differences) out.differences.push_back(d);
    }
    return out;
}

Json to_json(const ImageComparison& c) {
    Json stamped = Json::array();
    for (const auto& s : c.stamped)
        stamped.push_back({{"name", s.name}, {"offset", s.offset}, {"original", s.original}, {"relinked", s.relinked}});
    auto difference = [](const ByteDifference& d) {
        Json j{{"offset", d.offset}, {"size", d.size}, {"where", d.where}, {"original", d.original_bytes}, {"relinked", d.relinked_bytes}};
        if (d.rva) j["rva"] = *d.rva;
        if (!d.unit.empty()) j["unit"] = d.unit;
        if (!d.symbol.empty()) j["symbol"] = d.symbol;
        return j;
    };
    Json differences = Json::array();
    for (const auto& d : c.differences) differences.push_back(difference(d));
    Json sections = Json::array();
    for (const auto& s : c.sections) {
        Json j{{"name", s.name}, {"differing_bytes", s.differing_bytes}, {"size_differs", s.size_differs}};
        if (s.first_rva) j["first_rva"] = *s.first_rva;
        if (!s.first_unit.empty()) j["first_unit"] = s.first_unit;
        sections.push_back(std::move(j));
    }
    Json out{{"identical", c.identical},
             {"original_sha1", c.original_sha1},
             {"relinked_sha1", c.relinked_sha1},
             {"relinked_unstamped_sha1", c.relinked_unstamped_sha1},
             {"original_size", c.original_size},
             {"relinked_size", c.relinked_size},
             {"stamped", std::move(stamped)},
             {"differing_bytes", c.differing_bytes},
             {"differences", std::move(differences)},
             {"sections", std::move(sections)}};
    if (c.first) out["first"] = difference(*c.first);
    return out;
}

} // namespace decomp::relink
