#include "gui/actions.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>

namespace decomp::gui {

void Actions::add(Action action) {
    auto it = std::ranges::find(actions_, action.id, &Action::id);
    if (it != actions_.end()) *it = std::move(action);
    else actions_.push_back(std::move(action));
}

bool Actions::remove(std::string_view id) {
    return std::erase_if(actions_, [&](const Action& a) { return a.id == id; }) > 0;
}

const Action* Actions::find(std::string_view id) const {
    auto it = std::ranges::find(actions_, id, &Action::id);
    return it == actions_.end() ? nullptr : &*it;
}

bool Actions::is_enabled(const Action& action) { return action.run && (!action.enabled || action.enabled()); }

bool Actions::is_enabled(std::string_view id) const {
    const Action* a = find(id);
    return a && is_enabled(*a);
}

bool Actions::run(std::string_view id) const {
    const Action* a = find(id);
    if (!a || !is_enabled(*a)) return false;
    a->run();
    return true;
}

std::vector<std::string> Actions::handle_shortcuts(bool text_input) const {
    std::vector<std::string> ran;
    // Collect first: running an action may add or remove actions.
    std::vector<std::string> pressed;
    for (const Action& a : actions_) {
        if (text_input && !a.in_text_input) continue;
        bool hit = a.shortcut != 0 && ImGui::IsKeyChordPressed(a.shortcut);
        for (ImGuiKeyChord alt : a.alternates) hit = hit || (alt != 0 && ImGui::IsKeyChordPressed(alt));
        if (hit) pressed.push_back(a.id);
    }
    for (const auto& id : pressed)
        if (run(id)) ran.push_back(id);
    return ran;
}

bool Actions::menu_item(std::string_view id, bool selected) const {
    const Action* a = find(id);
    return a && menu_item(id, a->label.c_str(), selected);
}

bool Actions::menu_item(std::string_view id, const char* label, bool selected) const {
    const Action* a = find(id);
    if (!a) return false;
    const std::string shortcut = shortcut_label(a->shortcut);
    if (!ImGui::MenuItem(label, shortcut.empty() ? nullptr : shortcut.c_str(), selected, is_enabled(*a))) return false;
    a->run();
    return true;
}

std::string shortcut_label(ImGuiKeyChord chord) {
    if (chord == 0) return {};
    // ImGui names keys after the US layout ("Equal", "Minus"); the palette and menus show the glyphs.
    std::string out;
    if (chord & ImGuiMod_Ctrl) out += "Ctrl+";
    if (chord & ImGuiMod_Shift) out += "Shift+";
    if (chord & ImGuiMod_Alt) out += "Alt+";
    if (chord & ImGuiMod_Super) out += "Super+";
    const auto key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    switch (key) {
    case ImGuiKey_Equal: out += "="; break;
    case ImGuiKey_Minus: out += "-"; break;
    case ImGuiKey_LeftArrow: out += "Left"; break;
    case ImGuiKey_RightArrow: out += "Right"; break;
    case ImGuiKey_UpArrow: out += "Up"; break;
    case ImGuiKey_DownArrow: out += "Down"; break;
    default: out += ImGui::GetKeyName(key); break;
    }
    return out;
}

namespace {

char fold(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool word_start(std::string_view text, usize i) {
    if (i == 0) return true;
    const auto prev = static_cast<unsigned char>(text[i - 1]);
    const auto cur = static_cast<unsigned char>(text[i]);
    if (!std::isalnum(prev)) return true;
    return std::islower(prev) && std::isupper(cur);  // camelCase boundary
}

} // namespace

std::optional<int> fuzzy_score(std::string_view pattern, std::string_view text) {
    std::string pat;
    for (char c : pattern)
        if (c != ' ') pat += fold(c);
    if (pat.empty()) return 0;

    // Greedy left-to-right match. The first pass jumps ahead to word-start occurrences ("rm" in "Run
    // monitor"); when that leaves a later character unmatched, a plain first-occurrence pass decides.
    auto attempt = [&](bool prefer_word_starts) -> std::optional<int> {
        int score = 0;
        usize ti = 0;
        int run = 0;
        std::optional<usize> prev;
        for (const char want : pat) {
            usize found = text.size();
            for (usize j = ti; j < text.size(); ++j) {
                if (fold(text[j]) != want) continue;
                if (found == text.size()) found = j;
                if (!prefer_word_starts || j == ti || word_start(text, j)) {
                    found = j;
                    break;
                }
            }
            if (found == text.size()) return std::nullopt;
            int s = 1;
            if (found == 0) s += 8;
            if (word_start(text, found)) s += 6;
            if (prev && found == *prev + 1) {
                ++run;
                s += 4 * run;
            } else {
                run = 0;
            }
            if (prev) s -= static_cast<int>(std::min<usize>(found - *prev - 1, 5));  // gaps cost a little
            score += s;
            prev = found;
            ti = found + 1;
        }
        return score - static_cast<int>(std::min<usize>(text.size(), 60) / 4);  // shorter texts first
    };
    if (auto s = attempt(true)) return s;
    return attempt(false);
}

std::vector<ActionMatch> match_actions(const Actions& actions, std::string_view query) {
    std::vector<ActionMatch> out;
    for (const Action& a : actions.all()) {
        if (!a.in_palette) continue;
        if (query.empty()) {
            out.push_back({&a, 0});
            continue;
        }
        std::optional<int> best = fuzzy_score(query, a.label);
        if (!a.category.empty()) {
            if (auto s = fuzzy_score(query, a.category + ": " + a.label); s && (!best || *s > *best)) best = s;
        }
        if (best) out.push_back({&a, *best});
    }
    if (!query.empty()) std::ranges::stable_sort(out, [](const ActionMatch& x, const ActionMatch& y) { return x.score > y.score; });
    return out;
}

} // namespace decomp::gui
