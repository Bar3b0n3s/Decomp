// Global search: the index of the program's names and strings, and the palette provider over it.

#include "gui/search_index.hpp"
#include "harness.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <thread>

using namespace decomp;
using namespace decomp::gui;
using namespace decomp::gui::test;
using namespace std::chrono_literals;

TEST_CASE("search: functions by readable or decorated name, strings with the quote prefix") {
    auto program = Program::open(decomp::test::fixture("x86/basic.exe"));
    REQUIRE(program);
    const auto entries = build_search_entries(*program, true);
    REQUIRE_FALSE(entries.empty());

    auto first = [&](std::string_view query, bool strings) -> const SearchEntry* {
        const auto hits = search_entries(entries, query, strings, 10);
        return hits.empty() ? nullptr : &entries[hits.front().entry];
    };
    const SearchEntry* add = first("add", false);
    REQUIRE(add);
    CHECK(add->function);
    CHECK(add->label.find("add") != std::string::npos);
    // The decorated name finds it too, and case does not matter.
    const SearchEntry* decorated = first("?ADD@@", false);
    REQUIRE(decorated);
    CHECK(decorated->va == add->va);
    // Strings only with the prefix (the palette's `"`), and only strings then.
    const SearchEntry* hello = first("hello wor", true);
    REQUIRE(hello);
    CHECK(hello->string);
    CHECK(hello->label == "\"hello world\"");
    // Without the prefix, only symbols: here the literal's own symbol from the PDB (kind "string").
    if (const SearchEntry* literal = first("hello wor", false)) {
        CHECK_FALSE(literal->string);
        CHECK(literal->detail.starts_with("string"));
    }
    CHECK(search_entries(entries, "   ", false, 10).empty());
    CHECK(search_entries(entries, "a", false, 3).size() == 3);  // the limit holds
}

TEST_CASE("search: the palette lists matches once the background index and query have run") {
    auto opened = Program::open(decomp::test::fixture("x86/basic.exe"));
    REQUIRE(opened);
    auto program = std::make_shared<const Program>(std::move(*opened));
    HeadlessContext gui;
    Settings settings;
    App app(make_services(nullptr), settings);
    SearchIndex index;
    auto items = [&](std::string_view input) {
        const PaletteQuery q = parse_palette_query(input);
        std::vector<PaletteItem> out;
        index.provide(q, app.context(), out);
        return out;
    };
    // Before the index exists, the palette says so.
    index.poll(app.jobs(), program);
    for (int i = 0; i < 400 && !index.ready(); ++i) {
        std::this_thread::sleep_for(5ms);
        index.poll(app.jobs(), program);
    }
    REQUIRE(index.ready());
    std::vector<PaletteItem> found;
    for (int i = 0; i < 400 && found.empty(); ++i) {
        found = items("sum_arr");
        std::this_thread::sleep_for(5ms);
        index.poll(app.jobs(), program);
    }
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().label.find("sum_array") != std::string::npos);
    CHECK(found.front().enabled);
    CHECK(items("0x401000").empty());  // addresses are the palette's own "Go to"
    index.poll(app.jobs(), nullptr);    // project closed
    CHECK_FALSE(index.ready());
}
