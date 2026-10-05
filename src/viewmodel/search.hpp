#pragma once

// The palette's searches over a project (docs/ui.md#search-and-command-palette): symbols by decorated
// or demangled name, and the image's strings. The index is built once per program generation; each
// search scores every entry, so the GUI runs it as a background job. Pure.

#include "analysis/strings.hpp"
#include "analysis/symbols.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::vm {

struct SearchEntry {
    u64 va = 0;
    std::string name;     // decorated
    std::string display;  // demangled (the name when there is none)
    SymbolKind kind = SymbolKind::unknown;
};
// Every symbol, in address order.
std::vector<SearchEntry> search_entries(const SymbolDb& symbols);

struct SearchHit {
    usize index = 0;  // into the entries (or strings) searched
    int score = 0;    // higher is better
};

// A fuzzy matcher: nullopt when `text` does not match `pattern`, else a score (higher is better). The
// GUI passes its palette's matcher (gui::fuzzy_score) so every palette result is ranked alike.
using Scorer = std::function<std::optional<int>(std::string_view pattern, std::string_view text)>;

// Entries whose demangled or decorated name matches `query`, best first (ties: functions first, then by
// address), at most `limit`. An empty query matches nothing. `cancelled` is polled every 1024 entries; a
// cancelled search returns what it found so far.
std::vector<SearchHit> search_symbols(std::span<const SearchEntry> entries, std::string_view query, usize limit, const Scorer& score,
                                      const std::function<bool()>& cancelled = {});

// Strings containing `query` (ignoring ASCII case), the shortest first (ties by address), at most `limit`.
std::vector<SearchHit> search_strings(std::span<const ImageString> strings, std::string_view query, usize limit,
                                      const std::function<bool()>& cancelled = {});

} // namespace decomp::vm
