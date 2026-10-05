#include "analysis/bounds.hpp"

#include "analysis/demangle.hpp"
#include "analysis/discovery.hpp"
#include "formats/map.hpp"
#include "formats/pdb.hpp"

#include <algorithm>
#include <map>

namespace decomp {

std::vector<FunctionBounds> function_bounds(const SymbolDb& symbols, const BinaryImage& image) {
    std::vector<FunctionBounds> out;
    for (const Symbol* s : symbols.functions())
        if (s->size > 0 && image.is_code(s->va)) out.push_back({s->va, s->va + s->size, s->display.empty() ? s->name : s->display});
    return out;
}

Result<std::vector<FunctionBounds>> pdb_function_bounds(const std::filesystem::path& pdb, u64 image_base, const BinaryImage& image) {
    TRY_ASSIGN(auto reader, pdb::Reader::load(pdb));
    std::map<u64, FunctionBounds> by_start;
    for (const auto& p : reader.procedures()) {
        const u64 va = image_base + p.rva;
        if (p.size == 0 || !image.is_code(va)) continue;
        by_start.try_emplace(va, FunctionBounds{va, va + p.size, p.name});
    }
    std::vector<FunctionBounds> out;
    for (auto& [va, f] : by_start) out.push_back(std::move(f));
    return out;
}

Result<std::vector<FunctionBounds>> map_function_bounds(const std::filesystem::path& path, const BinaryImage& image) {
    TRY_ASSIGN(auto m, map::load(path));
    // The map's addresses are at its preferred load address; the image may have another base.
    const u64 delta = m.preferred_base ? image.image_base() - m.preferred_base : 0;
    std::map<u64, FunctionBounds> by_start;
    for (const auto& e : m.entries) {
        if (e.section == 0) continue;
        const u64 va = e.va + delta;
        if (!image.is_code(va)) continue;
        if ((m.has_function_flags && !e.function) || is_code_label_symbol(e.name, image.arch())) continue;
        by_start.try_emplace(va, FunctionBounds{va, 0, e.name});
    }
    std::vector<FunctionBounds> out;
    for (auto& [va, f] : by_start) out.push_back(std::move(f));
    return out;
}

std::string_view to_string(BoundsMismatch::Kind kind) {
    switch (kind) {
    case BoundsMismatch::Kind::missed: return "missed";
    case BoundsMismatch::Kind::wrong_end: return "wrong_end";
    case BoundsMismatch::Kind::extra: return "extra";
    }
    return "missed";
}

BoundsComparison compare_bounds(std::span<const FunctionBounds> truth, std::span<const FunctionBounds> found, const BinaryImage& image,
                                const x86::Decoder& decoder) {
    BoundsComparison c;
    c.truth = truth.size();
    c.found = found.size();
    c.truth_has_ends = std::ranges::all_of(truth, [](const FunctionBounds& f) { return f.end != 0; });
    std::map<u64, const FunctionBounds*> found_by_start, truth_by_start;
    for (const auto& f : found) found_by_start.emplace(f.start, &f);
    for (const auto& t : truth) truth_by_start.emplace(t.start, &t);
    for (auto it = truth_by_start.begin(); it != truth_by_start.end(); ++it) {
        const FunctionBounds& t = *it->second;
        auto f = found_by_start.find(t.start);
        if (f == found_by_start.end()) {
            ++c.missed;
            c.mismatches.push_back({BoundsMismatch::Kind::missed, t.start, t.end, 0, t.name});
            continue;
        }
        const u64 found_end = f->second->end;
        bool exact = false;
        if (t.end) {
            exact = found_end == t.end;
        } else {
            // Up to the next true start (or the end of the section) only padding may follow.
            const ImageSection* section = image.section_at(t.start);
            u64 next = section ? section->va + std::min(section->file_size, std::max(section->virtual_size, section->file_size)) : found_end;
            if (auto n = std::next(it); n != truth_by_start.end()) next = std::min(next, n->first);
            exact = found_end <= next && found_end > t.start && found_end + padding_length(image, decoder, found_end, next) >= next;
        }
        if (exact) {
            ++c.exact;
        } else {
            ++c.start_only;
            c.mismatches.push_back({BoundsMismatch::Kind::wrong_end, t.start, t.end, found_end, t.name});
        }
    }
    for (const auto& [start, f] : found_by_start)
        if (!truth_by_start.contains(start)) {
            ++c.extra;
            c.mismatches.push_back({BoundsMismatch::Kind::extra, start, 0, f->end, f->name});
        }
    // What the truth has and the analysis got wrong first, then what the analysis found besides.
    std::ranges::sort(c.mismatches, {}, [](const BoundsMismatch& m) { return std::pair{m.kind == BoundsMismatch::Kind::extra, m.start}; });
    return c;
}

Json to_json(const BoundsComparison& c, usize max_mismatches) {
    Json mismatches = Json::array();
    for (usize i = 0; i < c.mismatches.size() && i < max_mismatches; ++i) {
        const auto& m = c.mismatches[i];
        Json j = {{"kind", std::string(to_string(m.kind))}, {"start", m.start}, {"name", m.name}};
        if (m.truth_end) j["truth_end"] = m.truth_end;
        if (m.found_end) j["found_end"] = m.found_end;
        mismatches.push_back(std::move(j));
    }
    return Json{{"truth", c.truth},
                {"found", c.found},
                {"exact", c.exact},
                {"start_only", c.start_only},
                {"missed", c.missed},
                {"extra", c.extra},
                {"truth_has_ends", c.truth_has_ends},
                {"exact_rate", c.exact_rate()},
                {"start_rate", c.start_rate()},
                {"mismatches", std::move(mismatches)}};
}

} // namespace decomp
