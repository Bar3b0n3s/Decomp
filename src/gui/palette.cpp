#include "gui/palette.hpp"

#include "core/strings.hpp"
#include "gui/actions.hpp"
#include "gui/view.hpp"
#include "gui/widgets.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <format>

namespace decomp::gui {

PaletteQuery parse_palette_query(std::string_view input) {
    PaletteQuery q;
    std::string_view s = trim(input);
    if (s.starts_with('>')) {
        q.kind = PaletteQuery::Kind::actions;
        s = trim(s.substr(1));
    } else if (s.starts_with('"')) {
        q.kind = PaletteQuery::Kind::strings;
        s.remove_prefix(1);
        if (s.ends_with('"')) s.remove_suffix(1);
    } else if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        q.address = parse_u64(s);
    }
    q.text = std::string(s);
    return q;
}

void CommandPalette::add_provider(std::string name, PaletteProvider provider) {
    auto it = std::ranges::find(providers_, name, &std::pair<std::string, PaletteProvider>::first);
    if (it != providers_.end()) it->second = std::move(provider);
    else providers_.emplace_back(std::move(name), std::move(provider));
}

void CommandPalette::open(std::string_view initial) {
    input_ = std::string(initial);
    selected_ = 0;
    open_request_ = true;
}

std::vector<PaletteItem> CommandPalette::items(std::string_view input, ViewContext& ctx) const {
    const PaletteQuery q = parse_palette_query(input);
    std::vector<PaletteItem> out;
    if (q.address) {
        const u64 va = *q.address;
        out.push_back({std::format("Go to {}", hex(va, 8)), "Address", 1000, true, [&ctx, va] { ctx.open("inspector", {.va = va}); }});
    }
    if (q.kind != PaletteQuery::Kind::strings) {
        for (const ActionMatch& m : match_actions(ctx.actions, q.text)) {
            const Action& a = *m.action;
            std::string detail = a.category;
            if (a.shortcut) detail += (detail.empty() ? "" : "   ") + shortcut_label(a.shortcut);
            out.push_back({a.label, std::move(detail), m.score, Actions::is_enabled(a), [&ctx, id = a.id] { ctx.actions.run(id); }});
        }
    }
    const usize before_providers = out.size();
    if (q.kind != PaletteQuery::Kind::actions)
        for (const auto& [name, provider] : providers_) provider(q, ctx, out);
    if (q.kind == PaletteQuery::Kind::strings && out.size() == before_providers)
        out.push_back({std::format("Search strings for \"{}\"", q.text), "needs a loaded project", 0, false, {}});
    if (!q.text.empty()) std::ranges::stable_sort(out, [](const PaletteItem& a, const PaletteItem& b) { return a.score > b.score; });
    if (out.size() > kMaxItems) out.resize(kMaxItems);
    return out;
}

namespace {

struct InputState {
    int* selected;
    int count;
    bool* scroll;
};

int on_input(ImGuiInputTextCallbackData* data) {
    auto* st = static_cast<InputState*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory && st->count > 0) {
        if (data->EventKey == ImGuiKey_UpArrow) *st->selected = (*st->selected + st->count - 1) % st->count;
        if (data->EventKey == ImGuiKey_DownArrow) *st->selected = (*st->selected + 1) % st->count;
        *st->scroll = true;
    }
    return 0;
}

} // namespace

void CommandPalette::draw(ViewContext& ctx) {
    constexpr const char* kPopup = "##command_palette";
    if (open_request_) {
        ImGui::OpenPopup(kPopup);
        open_request_ = false;
        focus_input_ = true;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float font = ImGui::GetFontSize();
    const float width = std::min(viewport->WorkSize.x * 0.9f, font * 42.0f);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + font * 1.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    if (!ImGui::BeginPopup(kPopup, ImGuiWindowFlags_NoMove)) {
        open_ = false;
        close_request_ = false;
        return;
    }
    open_ = true;

    std::vector<PaletteItem> list = items(input_, ctx);
    const int count = static_cast<int>(list.size());
    selected_ = count == 0 ? 0 : std::clamp(selected_, 0, count - 1);

    if (focus_input_) {
        ImGui::SetKeyboardFocusHere();
        focus_input_ = false;
    }
    InputState state{&selected_, count, &scroll_to_selected_};
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::string before = input_;
    const bool enter = ImGui::InputTextWithHint("##query", "Search: actions (>), addresses (0x...), strings (\"...)", &input_,
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, on_input,
                                                &state);
    if (input_ != before) selected_ = 0;

    std::function<void()> chosen;
    const float rows = static_cast<float>(std::clamp(count, 1, 12));
    if (ImGui::BeginChild("##results", ImVec2(0, rows * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoNav)) {
        if (count == 0) ImGui::TextDisabled("No matches");
        for (int i = 0; i < count; ++i) {
            const PaletteItem& item = list[static_cast<usize>(i)];
            ImGui::PushID(i);
            const bool is_selected = i == selected_;
            const ImVec2 row = ImGui::GetCursorScreenPos();
            const float right = row.x + ImGui::GetContentRegionAvail().x;
            ImGui::BeginDisabled(!item.enabled);
            if (ImGui::Selectable(item.label.c_str(), is_selected, ImGuiSelectableFlags_None) && item.enabled) chosen = item.run;
            ImGui::EndDisabled();
            if (!item.detail.empty()) {
                const float w = ImGui::CalcTextSize(item.detail.c_str()).x;
                ImGui::GetWindowDrawList()->AddText(ImVec2(right - w, row.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), item.detail.c_str());
            }
            if (is_selected && scroll_to_selected_) {
                ImGui::SetScrollHereY();
                scroll_to_selected_ = false;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (enter && count > 0) {
        const PaletteItem& item = list[static_cast<usize>(selected_)];
        if (item.enabled) chosen = item.run;
    }
    if (chosen || close_request_ || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
        close_request_ = false;
        open_ = false;
    }
    ImGui::EndPopup();
    if (chosen) chosen();  // after EndPopup: actions may open popups of their own
}

} // namespace decomp::gui
