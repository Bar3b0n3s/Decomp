#include "viewmodel/search.hpp"

#include "core/strings.hpp"

#include <algorithm>

namespace decomp::vm {

std::vector<SearchEntry> search_entries(const SymbolDb& symbols) {
    std::vector<SearchEntry> out;
    out.reserve(symbols.size());
    for (const auto& [va, s] : symbols) out.push_back({va, s.name, s.display.empty() ? s.name : s.display, s.kind});
    return out;
}

std::vector<SearchHit> search_symbols(std::span<const SearchEntry> entries, std::string_view query, usize limit, const Scorer& score,
                                      const std::function<bool()>& cancelled) {
    std::vector<SearchHit> hits;
    query = trim(query);
    if (query.empty() || limit == 0 || !score) return hits;
    // Best score first; then functions; then by address.
    auto better = [&](const SearchHit& a, const SearchHit& b) {
        if (a.score != b.score) return a.score > b.score;
        const bool fa = entries[a.index].kind == SymbolKind::function, fb = entries[b.index].kind == SymbolKind::function;
        if (fa != fb) return fa;
        return entries[a.index].va < entries[b.index].va;
    };
    for (usize i = 0; i < entries.size(); ++i) {
        if (i % 1024 == 0 && cancelled && cancelled()) break;
        const SearchEntry& e = entries[i];
        std::optional<int> best = score(query, e.display);
        if (e.name != e.display)
            if (auto s = score(query, e.name); s && (!best || *s > *best)) best = s;
        if (best) hits.push_back({i, *best});
        // Trimmed now and then, so a vague query over many symbols stays small.
        if (hits.size() >= limit * 8) {
            std::ranges::sort(hits, better);
            hits.resize(limit);
        }
    }
    std::ranges::sort(hits, better);
    if (hits.size() > limit) hits.resize(limit);
    return hits;
}

std::vector<SearchHit> search_strings(std::span<const ImageString> strings, std::string_view query, usize limit,
                                      const std::function<bool()>& cancelled) {
    std::vector<SearchHit> hits;
    const std::string needle = to_lower(trim(query));
    if (needle.empty() || limit == 0) return hits;
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    for (usize i = 0; i < strings.size(); ++i) {
        if (i % 1024 == 0 && cancelled && cancelled()) break;
        const std::string& text = strings[i].text;
        const bool found = std::search(text.begin(), text.end(), needle.begin(), needle.end(), [&](char h, char n) { return lower(h) == n; }) != text.end();
        if (found) hits.push_back({i, -static_cast<int>(std::min<usize>(text.size(), 1u << 30))});  // shorter strings rank higher
    }
    std::ranges::stable_sort(hits, [](const SearchHit& a, const SearchHit& b) { return a.score > b.score; });
    if (hits.size() > limit) hits.resize(limit);
    return hits;
}

} // namespace decomp::vm
