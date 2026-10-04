#include "gui/search_index.hpp"

#include "analysis/strings.hpp"
#include "core/strings.hpp"
#include "gui/view.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace decomp::gui {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

constexpr usize kMaxHits = 40;

} // namespace

std::vector<SearchEntry> build_search_entries(const Program& program, bool strings) {
    std::vector<SearchEntry> out;
    for (const auto& [va, s] : program.symbols()) {
        SearchEntry e;
        e.va = va;
        e.function = s.kind == SymbolKind::function;
        e.label = s.display.empty() ? s.name : s.display;
        // Both names are searchable: "Player::Hit" and "?Hit@Player@@QAEXH@Z".
        e.key = lower(s.display == s.name || s.display.empty() ? s.name : s.display + "\n" + s.name);
        e.detail = std::format("{} {}", to_string(s.kind), hex(va, 8));
        out.push_back(std::move(e));
    }
    if (strings)
        for (auto& str : scan_strings(program.image(), {}, &program.symbols())) {
            SearchEntry e;
            e.va = str.va;
            e.string = true;
            e.key = lower(str.text);
            e.label = std::format("\"{}\"", str.text.size() > 80 ? str.text.substr(0, 77) + "..." : str.text);
            e.detail = std::format("string {}", hex(str.va, 8));
            out.push_back(std::move(e));
        }
    return out;
}

std::vector<SearchHit> search_entries(const std::vector<SearchEntry>& entries, std::string_view query, bool strings, usize limit) {
    std::vector<SearchHit> hits;
    const std::string q = lower(trim(query));
    if (q.empty()) return hits;
    for (usize i = 0; i < entries.size(); ++i) {
        const SearchEntry& e = entries[i];
        if (e.string != strings) continue;
        const auto at = e.key.find(q);
        if (at == std::string::npos) continue;
        int score = 500;
        if (at == 0 || e.key[at - 1] == '\n' || e.key[at - 1] == ':' || e.key[at - 1] == ' ') score += 300;  // a name or word starts here
        if (e.key.size() == q.size()) score += 200;
        if (e.function) score += 100;
        score -= static_cast<int>(std::min<usize>(e.key.size(), 200));
        hits.push_back({static_cast<u32>(i), score});
    }
    const usize keep = std::min(limit, hits.size());
    std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(keep), hits.end(),
                      [](const SearchHit& a, const SearchHit& b) { return a.score > b.score; });
    hits.resize(keep);
    return hits;
}

void SearchIndex::poll(JobQueue& jobs, const std::shared_ptr<const Program>& program) {
    jobs_ = &jobs;
    if (program.get() != program_) {
        program_ = program.get();
        building_.cancel();
        search_.cancel();
        entries_.reset();
        shown_ = {};
        requested_.clear();
        if (program) building_ = jobs.submit([program] { return build_search_entries(*program, true); });
    }
    if (building_.ready())
        if (auto built = building_.take()) {
            entries_ = std::make_shared<const std::vector<SearchEntry>>(std::move(*built));
            requested_.clear();  // search again with the index
        }
    if (auto r = search_.poll()) shown_ = std::move(*r);
}

void SearchIndex::provide(const PaletteQuery& query, ViewContext& ctx, std::vector<PaletteItem>& out) {
    if (query.address) return;  // the palette offers "Go to" itself
    const bool strings = query.kind == PaletteQuery::Kind::strings;
    if (trim(query.text).empty()) return;
    if (!entries_) {
        out.push_back({program_ ? "Indexing the program's names and strings..." : "Open a project to search its functions and strings", "", -1, false, {}});
        return;
    }
    if ((query.text != requested_ || strings != requested_strings_) && jobs_) {
        requested_ = query.text;
        requested_strings_ = strings;
        search_.submit(*jobs_, [entries = entries_, text = query.text, strings] {
            return Result{text, strings, search_entries(*entries, text, strings, kMaxHits)};
        });
    }
    // The newest finished results while the next search runs (they rarely differ by much).
    if (shown_.strings != strings) return;
    for (const SearchHit& hit : shown_.hits) {
        const SearchEntry& e = (*entries_)[hit.entry];
        const std::string view = e.function ? "inspector" : "binary_explorer";
        // Scores below the actions' fuzzy matches unless the name starts with the query.
        out.push_back({e.label, e.detail, hit.score / 10, true, [&ctx, view, va = e.va] { ctx.open(view, NavTarget{.va = va}); }});
    }
}

} // namespace decomp::gui
