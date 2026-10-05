#include "viewmodel/search.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

using namespace decomp;
using namespace decomp::vm;

namespace {

// A stand-in for the palette's matcher: a case-sensitive substring, scored higher the earlier it starts.
std::optional<int> substring_score(std::string_view pattern, std::string_view text) {
    const usize at = text.find(pattern);
    if (at == std::string_view::npos) return std::nullopt;
    return 1000 - static_cast<int>(at);
}

} // namespace

TEST_CASE("search: symbols by demangled or decorated name, best first") {
    test::FixtureProject fx;
    const auto entries = search_entries(fx.program.symbols());
    REQUIRE(entries.size() == fx.program.symbols().size());
    CHECK(std::ranges::is_sorted(entries, {}, &SearchEntry::va));
    const auto& helper = *std::ranges::find(entries, fx.va("helper"), &SearchEntry::va);
    CHECK(helper.display == "helper");

    auto names = [&](const std::vector<SearchHit>& hits) {
        std::vector<std::string> out;
        for (const auto& h : hits) out.push_back(entries[h.index].display);
        return out;
    };
    // "add" starts the decorated name ?add@@ (score 999) but sits deeper in the demangled one.
    const auto add = search_symbols(entries, "add", 10, substring_score);
    REQUIRE_FALSE(add.empty());
    CHECK(names(add).front() == "int __cdecl add(int, int)");
    CHECK(add.front().score == 999);
    // Equal scores (both decorated names start with it): by address.
    const auto g = search_symbols(entries, "g_", 10, substring_score);
    REQUIRE(g.size() == 2);
    CHECK(entries[g[0].index].name == "?g_counter@@3HA");
    CHECK(entries[g[1].index].name == "?g_table@@3PAHA");
    // Functions come before other symbols with the same score.
    std::vector<SearchEntry> tie(2);
    tie[0] = {0x10, "x_data", "x_data", SymbolKind::data};
    tie[1] = {0x20, "x_code", "x_code", SymbolKind::function};
    CHECK(search_symbols(tie, "x_", 10, substring_score).front().index == 1);
    CHECK(search_symbols(entries, "int", 3, substring_score).size() == 3);
    CHECK(search_symbols(entries, "  ", 10, substring_score).empty());
    CHECK(search_symbols(entries, "zzz", 10, substring_score).empty());
    CHECK(search_symbols(entries, "add", 0, substring_score).empty());
    int polls = 0;
    CHECK(search_symbols(entries, "a", 10, substring_score, [&] { return ++polls > 0; }).empty());

    // Many matches are trimmed as they come, and the result is still the best ones.
    std::vector<SearchEntry> many(5000);
    for (usize i = 0; i < many.size(); ++i) {
        many[i].va = i;
        many[i].display = many[i].name = std::string(i % 100, ' ') + "fn";
        many[i].kind = SymbolKind::function;
    }
    const auto best = search_symbols(many, "fn", 5, substring_score);
    REQUIRE(best.size() == 5);
    for (const auto& h : best) CHECK(many[h.index].display == "fn");
    CHECK(best.front().index == 0);
}

TEST_CASE("search: strings by substring, shortest first") {
    std::vector<ImageString> strings(4);
    const char* texts[] = {"Hello world", "say hello", "hello", "goodbye"};
    for (usize i = 0; i < strings.size(); ++i) {
        strings[i].va = 0x1000 + i * 0x20;
        strings[i].text = texts[i];
    }
    const auto hits = search_strings(strings, "HELLO", 10);
    REQUIRE(hits.size() == 3);
    CHECK(strings[hits[0].index].text == "hello");
    CHECK(strings[hits[1].index].text == "say hello");
    CHECK(strings[hits[2].index].text == "Hello world");
    CHECK(search_strings(strings, "hello", 1).size() == 1);
    CHECK(search_strings(strings, "", 10).empty());
    CHECK(search_strings(strings, "xyz", 10).empty());
}
