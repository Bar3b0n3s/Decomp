#pragma once

// Every user-visible command is an Action: menus, the command palette and keyboard shortcuts all run
// the same registry entry (docs/ui.md#search-and-command-palette, #keyboard-shortcuts).

#include "core/types.hpp"

#include <imgui.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::gui {

// Every member has a default, so actions are written with designated initializers.
struct Action {
    std::string id = {};                         // stable: "run.pause", "view.dashboard"
    std::string label = {};                      // menus and the palette: "Pause run"
    std::string category = {};                   // palette detail: "Run", "View", "Layout"
    ImGuiKeyChord shortcut = 0;                  // e.g. ImGuiMod_Ctrl | ImGuiKey_P; 0 = none
    std::vector<ImGuiKeyChord> alternates = {};  // also trigger the action, not displayed
    std::function<bool()> enabled = {};          // null: always enabled
    std::function<void()> run = {};
    bool in_text_input = false;  // the shortcut also fires while a text field or editor has focus
    bool in_palette = true;      // listed by the command palette
};

class Actions {
public:
    // Adds an action, or replaces the one with the same id (keeping its position).
    void add(Action action);
    bool remove(std::string_view id);
    const Action* find(std::string_view id) const;
    const std::vector<Action>& all() const { return actions_; }

    static bool is_enabled(const Action& action);
    bool is_enabled(std::string_view id) const;
    // Runs the action if it exists and is enabled; returns whether it ran.
    bool run(std::string_view id) const;

    // Runs every enabled action whose shortcut was pressed this frame (ImGui::IsKeyChordPressed). While
    // `text_input` is true, only actions marked in_text_input respond. Returns the ids that ran.
    std::vector<std::string> handle_shortcuts(bool text_input) const;

    // ImGui::MenuItem for an action (label, shortcut text, enabled state); runs it when clicked.
    bool menu_item(std::string_view id, bool selected = false) const;
    // The same, with a different menu label.
    bool menu_item(std::string_view id, const char* label, bool selected) const;

private:
    std::vector<Action> actions_;
};

// "Ctrl+Shift+F5"; empty for 0.
std::string shortcut_label(ImGuiKeyChord chord);

// Fuzzy subsequence matching for the palette. nullopt unless every character of `pattern` appears in
// `text` in order (case-insensitive, spaces in the pattern ignored); otherwise a score where higher is
// better: matches at the start and at word starts, consecutive runs and shorter texts score higher.
std::optional<int> fuzzy_score(std::string_view pattern, std::string_view text);

struct ActionMatch {
    const Action* action = nullptr;
    int score = 0;
};

// Palette actions matching `query` (against "category: label" and the label), best first; an empty query
// lists them all in registration order.
std::vector<ActionMatch> match_actions(const Actions& actions, std::string_view query);

} // namespace decomp::gui
