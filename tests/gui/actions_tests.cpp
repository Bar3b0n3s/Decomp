#include "gui/actions.hpp"
#include "gui/palette.hpp"
#include "harness.hpp"

#include <doctest/doctest.h>

#include <map>

using namespace decomp;
using namespace decomp::gui;
using namespace decomp::gui::test;

TEST_CASE("actions: add, replace, remove, enable and run") {
    Actions actions;
    int runs = 0;
    bool enabled = false;
    actions.add({.id = "a", .label = "First", .run = [&] { ++runs; }});
    actions.add({.id = "b", .label = "Second", .enabled = [&] { return enabled; }, .run = [&] { runs += 10; }});
    actions.add({.id = "c", .label = "No run function"});
    REQUIRE(actions.all().size() == 3);

    CHECK(actions.run("a"));
    CHECK(runs == 1);
    CHECK_FALSE(actions.run("b"));  // disabled
    CHECK_FALSE(actions.is_enabled("b"));
    enabled = true;
    CHECK(actions.run("b"));
    CHECK(runs == 11);
    CHECK_FALSE(actions.is_enabled("c"));
    CHECK_FALSE(actions.run("missing"));

    actions.add({.id = "a", .label = "First, replaced", .run = [&] { runs = -1; }});
    CHECK(actions.all().size() == 3);
    CHECK(actions.all().front().label == "First, replaced");  // keeps its position
    CHECK(actions.run("a"));
    CHECK(runs == -1);

    CHECK(actions.remove("b"));
    CHECK_FALSE(actions.remove("b"));
    CHECK(actions.find("b") == nullptr);
}

TEST_CASE("shortcut labels") {
    CHECK(shortcut_label(0).empty());
    CHECK(shortcut_label(ImGuiMod_Ctrl | ImGuiKey_P) == "Ctrl+P");
    CHECK(shortcut_label(ImGuiKey_F5) == "F5");
    CHECK(shortcut_label(ImGuiMod_Shift | ImGuiKey_F5) == "Shift+F5");
    CHECK(shortcut_label(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F5) == "Ctrl+Shift+F5");
    CHECK(shortcut_label(ImGuiMod_Alt | ImGuiKey_LeftArrow) == "Alt+Left");
    CHECK(shortcut_label(ImGuiMod_Ctrl | ImGuiKey_Equal) == "Ctrl+=");
    CHECK(shortcut_label(ImGuiMod_Ctrl | ImGuiKey_Minus) == "Ctrl+-");
    CHECK(shortcut_label(ImGuiMod_Ctrl | ImGuiKey_0) == "Ctrl+0");
}

TEST_CASE("fuzzy matching") {
    CHECK(fuzzy_score("", "anything") == 0);
    CHECK(fuzzy_score("pause", "Pause run").has_value());
    CHECK(fuzzy_score("PAUSE", "pause run").has_value());  // case-insensitive
    CHECK(fuzzy_score("prn", "Pause run").has_value());    // subsequence
    CHECK_FALSE(fuzzy_score("nur", "Pause run").has_value());  // order matters
    CHECK_FALSE(fuzzy_score("x", "Pause run").has_value());
    CHECK(fuzzy_score("run mon", "Show Run monitor").has_value());  // spaces in the pattern are ignored

    // A word-start jump must not hide a plain subsequence ("xb" in "axbXc").
    CHECK(fuzzy_score("xb", "axbXc").has_value());

    // Prefixes and word starts beat scattered matches; consecutive runs beat gaps.
    CHECK(*fuzzy_score("dash", "Dashboard") > *fuzzy_score("dash", "Toggle raw bytes: dash"));
    CHECK(*fuzzy_score("rm", "Run monitor") > *fuzzy_score("rm", "Larger font, more"));
    CHECK(*fuzzy_score("theme", "Theme: Dark") > *fuzzy_score("theme", "The more you see"));
    // Shorter texts win ties.
    CHECK(*fuzzy_score("stop", "Stop") > *fuzzy_score("stop", "Stop run (finish the current turns)"));
}

TEST_CASE("palette action matching") {
    Actions actions;
    actions.add({.id = "run.pause", .label = "Pause run", .category = "Run", .run = [] {}});
    actions.add({.id = "view.run_monitor", .label = "Show Run monitor", .category = "View", .run = [] {}});
    actions.add({.id = "font.larger", .label = "Larger font", .category = "View", .run = [] {}});
    actions.add({.id = "hidden", .label = "Hidden pause", .run = [] {}, .in_palette = false});

    auto all = match_actions(actions, "");
    REQUIRE(all.size() == 3);  // hidden ones are left out; registration order
    CHECK(all[0].action->id == "run.pause");

    auto pause = match_actions(actions, "pause");
    REQUIRE(pause.size() == 1);
    CHECK(pause[0].action->id == "run.pause");

    auto monitor = match_actions(actions, "run mon");
    REQUIRE_FALSE(monitor.empty());
    CHECK(monitor[0].action->id == "view.run_monitor");

    // The category takes part: "view larger" matches "View: Larger font".
    auto larger = match_actions(actions, "view larger");
    REQUIRE_FALSE(larger.empty());
    CHECK(larger[0].action->id == "font.larger");
}

TEST_CASE("palette queries") {
    auto q = parse_palette_query("  >  pause  ");
    CHECK(q.kind == PaletteQuery::Kind::actions);
    CHECK(q.text == "pause");
    CHECK_FALSE(q.address);

    q = parse_palette_query("\"Hello, world\"");
    CHECK(q.kind == PaletteQuery::Kind::strings);
    CHECK(q.text == "Hello, world");

    q = parse_palette_query("0x401000");
    CHECK(q.kind == PaletteQuery::Kind::all);
    REQUIRE(q.address);
    CHECK(*q.address == 0x401000u);

    q = parse_palette_query("0xZZ");
    CHECK_FALSE(q.address);
    q = parse_palette_query("add");
    CHECK(q.kind == PaletteQuery::Kind::all);
    CHECK_FALSE(q.address);
}

TEST_CASE("the shell's actions have unique ids and shortcuts") {
    HeadlessContext gui;
    Settings settings;
    App app(make_services(nullptr), settings);
    std::map<std::string, int> ids;
    std::map<ImGuiKeyChord, std::vector<std::string>> chords;
    for (const Action& a : app.actions().all()) {
        ++ids[a.id];
        CHECK_MESSAGE(!a.label.empty(), a.id);
        CHECK_MESSAGE(static_cast<bool>(a.run), a.id);
        if (a.shortcut) chords[a.shortcut].push_back(a.id);
        for (ImGuiKeyChord alt : a.alternates) chords[alt].push_back(a.id);
    }
    for (const auto& [id, count] : ids) CHECK_MESSAGE(count == 1, id);
    for (const auto& [chord, owners] : chords) {
        // Start and Resume share F5: they are never enabled together.
        if (owners == std::vector<std::string>{"run.start", "run.resume"}) continue;
        CHECK_MESSAGE(owners.size() == 1, shortcut_label(chord) << " is bound to " << owners.size() << " actions");
    }
    // docs/ui.md#keyboard-shortcuts
    auto shortcut_of = [&](const char* id) {
        const Action* a = app.actions().find(id);
        return a ? a->shortcut : 0;
    };
    CHECK(shortcut_of("palette.open") == (ImGuiMod_Ctrl | ImGuiKey_P));
    CHECK(shortcut_of("run.start") == ImGuiKey_F5);
    CHECK(shortcut_of("run.resume") == ImGuiKey_F5);
    CHECK(shortcut_of("run.pause") == ImGuiKey_F6);
    CHECK(shortcut_of("run.stop") == (ImGuiMod_Shift | ImGuiKey_F5));
    CHECK(shortcut_of("run.abort") == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F5));
    CHECK(shortcut_of("nav.back") == (ImGuiMod_Alt | ImGuiKey_LeftArrow));
    CHECK(shortcut_of("nav.forward") == (ImGuiMod_Alt | ImGuiKey_RightArrow));
    CHECK(shortcut_of("font.larger") == (ImGuiMod_Ctrl | ImGuiKey_Equal));
    CHECK(shortcut_of("font.smaller") == (ImGuiMod_Ctrl | ImGuiKey_Minus));
    CHECK(shortcut_of("font.reset") == (ImGuiMod_Ctrl | ImGuiKey_0));
    CHECK(shortcut_of("view.dashboard") == (ImGuiMod_Ctrl | ImGuiKey_1));
    CHECK(shortcut_of("view.changes") == (ImGuiMod_Ctrl | ImGuiKey_9));
    CHECK(shortcut_of("view.units") == 0);  // the tenth view has no Ctrl+digit
    CHECK(shortcut_of("view.types") == 0);
    CHECK(shortcut_of("view.cost") == 0);
}
