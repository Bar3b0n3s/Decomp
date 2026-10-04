#pragma once

// Global search (docs/ui.md#search-and-command-palette): functions and other symbols by name, and the
// strings in the image (with the `"` prefix), for the command palette. The index is built in the
// background for each program generation, and every query runs as a latest-wins job, so typing never
// waits for a scan of 100,000 names.

#include "analysis/program.hpp"
#include "gui/jobs.hpp"
#include "gui/palette.hpp"

#include <memory>
#include <string>
#include <vector>

namespace decomp::gui {

struct SearchEntry {
    std::string key;     // lowercased text that is searched
    std::string label;   // shown
    std::string detail;  // right-aligned: kind and address
    u64 va = 0;
    bool function = false;
    bool string = false;
};

// Entries for a program: every symbol (readable and decorated names) and, when `strings` is set, the
// image's strings. Pure; run it in the background.
std::vector<SearchEntry> build_search_entries(const Program& program, bool strings);

struct SearchHit {
    u32 entry = 0;
    int score = 0;
};
// Case-insensitive substring matches of `query` among `entries` (strings only when `strings`, symbols
// otherwise), best first: a match at the start beats one inside, functions beat other symbols, shorter
// beats longer. At most `limit`.
std::vector<SearchHit> search_entries(const std::vector<SearchEntry>& entries, std::string_view query, bool strings, usize limit);

class SearchIndex {
public:
    // Once per frame on the UI thread: rebuilds the index when the program changes and takes finished
    // results.
    void poll(JobQueue& jobs, const std::shared_ptr<const Program>& program);
    // The palette provider: items from the latest finished search, and a new search when the query
    // changed.
    void provide(const PaletteQuery& query, ViewContext& ctx, std::vector<PaletteItem>& out);
    bool ready() const { return entries_ != nullptr; }

private:
    using Entries = std::shared_ptr<const std::vector<SearchEntry>>;
    struct Result {
        std::string query;
        bool strings = false;
        std::vector<SearchHit> hits;
    };

    JobQueue* jobs_ = nullptr;
    const Program* program_ = nullptr;
    JobHandle<std::vector<SearchEntry>> building_;
    Entries entries_;
    LatestWins<Result> search_;
    std::string requested_;
    bool requested_strings_ = false;
    Result shown_;
};

} // namespace decomp::gui
