// The shell's chrome (docs/ui.md#layout-and-chrome): menu bar, top bar, status bar, dock space, the view
// windows, tool windows and dialogs.

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "core/version.hpp"
#include "gui/app.hpp"
#include "gui/fonts.hpp"
#include "gui/layout.hpp"
#include "gui/widgets.hpp"

#include <imgui_internal.h>
#include <implot.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cmath>
#include <format>

namespace decomp::gui {

namespace {

// Shortens `text` with an ellipsis so that it fits in `width` pixels.
std::string fit_text(std::string_view text, float width) {
    if (ImGui::CalcTextSize(text.data(), text.data() + text.size()).x <= width) return std::string(text);
    const std::string_view ellipsis = "...";
    std::string out(text);
    while (!out.empty()) {
        // Drop a whole UTF-8 sequence at a time.
        do out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80);
        std::string candidate = out + std::string(ellipsis);
        if (ImGui::CalcTextSize(candidate.c_str()).x <= width) return candidate;
    }
    return std::string(ellipsis);
}

void vertical_separator() {
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
}

const ImVec4& phase_color(RunPhase phase, const std::string& status, const ThemeColors& c) {
    switch (phase) {
    case RunPhase::none: return c.muted;
    case RunPhase::running: return c.ok;
    case RunPhase::paused:
    case RunPhase::stopping: return c.warn;
    case RunPhase::finished: break;
    }
    if (status == "completed") return c.ok;
    if (status == "error" || status == "aborted") return c.error;
    return c.warn;
}

} // namespace

void App::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        ImGui::MenuItem("Open project...", nullptr, false, false);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Opening projects is coming in step S6.");
        if (ImGui::BeginMenu("Recent projects", !settings_.recent_projects.empty())) {
            for (const auto& path : settings_.recent_projects) {
                ImGui::MenuItem(path.c_str(), nullptr, false, false);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Opening projects is coming in step S6.");
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        actions_.menu_item("app.quit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Run")) {
        for (const char* id : {"run.start", "run.resume", "run.pause", "run.stop", "run.abort"}) actions_.menu_item(id);
        if (!services_.commands->available()) {
            ImGui::Separator();
            ImGui::TextDisabled("No run controller is attached.");
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (usize i = 0; i < slots_.size(); ++i) {
            Slot& slot = slots_[i];
            const std::string shortcut = i < 9 ? std::format("Ctrl+{}", i + 1) : std::string();
            if (ImGui::MenuItem(std::string(slot.view->title()).c_str(), shortcut.empty() ? nullptr : shortcut.c_str(), slot.open)) {
                if (slot.open) set_open(slot.view->id(), false);
                else ctx_.open(std::string(slot.view->id()));
            }
        }
        ImGui::Separator();
        actions_.menu_item("palette.open");
        actions_.menu_item("nav.back");
        actions_.menu_item("nav.forward");
        const usize unread = notifications_.unread();
        actions_.menu_item("notifications.show", unread ? std::format("Notifications ({} new)", unread).c_str() : "Notifications", false);
        ImGui::Separator();
        if (ImGui::BeginMenu("Theme")) {
            for (Theme t : {Theme::dark, Theme::light, Theme::high_contrast})
                actions_.menu_item(std::format("theme.{}", to_string(t)), std::string(theme_label(t)).c_str(), settings_.theme == t);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Diff palette")) {
            for (DiffPaletteKind k : {DiffPaletteKind::standard, DiffPaletteKind::okabe_ito})
                actions_.menu_item(std::format("diff_palette.{}", to_string(k)), std::string(diff_palette_label(k)).c_str(),
                                   settings_.diff_palette == k);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Font size")) {
            ImGui::TextDisabled("%.0f px", settings_.font_size);
            actions_.menu_item("font.larger");
            actions_.menu_item("font.smaller");
            actions_.menu_item("font.reset");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Layout")) {
            actions_.menu_item("layout.reset");
            actions_.menu_item("layout.save");
            if (!settings_.layouts.empty()) {
                ImGui::Separator();
                for (const auto& saved : settings_.layouts)
                    actions_.menu_item("layout.load." + saved.name, saved.name.c_str(), false);
                ImGui::Separator();
                if (ImGui::BeginMenu("Delete")) {
                    std::optional<std::string> remove;
                    for (const auto& saved : settings_.layouts)
                        if (ImGui::MenuItem(saved.name.c_str())) remove = saved.name;
                    if (remove) {
                        settings_.remove_layout(*remove);
                        ctx_.mark_settings_dirty();
                        register_layout_actions();
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        actions_.menu_item("help.shortcuts");
        actions_.menu_item("help.about");
        ImGui::Separator();
        if (ImGui::BeginMenu("Developer tools")) {
            actions_.menu_item("dev.metrics", "Dear ImGui metrics and debugger", show_metrics_);
            actions_.menu_item("dev.id_stack", "ID stack tool", show_id_stack_);
            actions_.menu_item("dev.style", "Style editor", show_style_editor_);
            actions_.menu_item("dev.demo", "Dear ImGui demo", show_demo_);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::draw_top_bar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad_y = std::round(style.FramePadding.y);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, pad_y));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, style.Colors[ImGuiCol_MenuBarBg]);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;
    const bool visible = ImGui::BeginViewportSideBar("##top_bar", viewport, ImGuiDir_Up, ImGui::GetFrameHeight() + 2 * pad_y, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (visible) {
        const ThemeColors& c = ctx_.colors();

        // Project and target; clicking opens the Dashboard.
        std::string project = ctx_.project.root.empty() ? std::string("No project") : fs::to_utf8(ctx_.project.root.filename());
        if (project.empty()) project = fs::to_utf8(ctx_.project.root);
        if (!ctx_.project.target.empty()) project += "  " + ctx_.project.target;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::Button(std::format("{}##project", project).c_str())) ctx_.open("dashboard");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            if (ctx_.project.root.empty()) ImGui::SetTooltip("No project is open. Open the Dashboard.");
            else
                ImGui::SetTooltip("%s%s\nOpen the Dashboard.", fs::to_utf8(ctx_.project.root).c_str(), ctx_.project.open ? "" : " (not loaded)");
        }
        vertical_separator();

        // Run state, always as text.
        ImGui::AlignTextToFramePadding();
        status_label(label(run_), phase_color(run_.phase, run_.status, c));
        vertical_separator();

        // Run controls, each enabled only when its command is valid.
        auto control = [this](const char* id, const char* text) {
            const Action* action = actions_.find(id);
            const bool enabled = action && Actions::is_enabled(*action);
            ImGui::BeginDisabled(!enabled);
            if (ImGui::Button(text)) actions_.run(id);
            ImGui::EndDisabled();
            if (action && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s (%s)", action->label.c_str(), shortcut_label(action->shortcut).c_str());
            ImGui::SameLine();
        };
        control("run.start", "Start");
        if (run_.phase == RunPhase::paused) control("run.resume", "Resume");
        else control("run.pause", "Pause");
        control("run.stop", "Stop");
        control("run.abort", "Abort");
        vertical_separator();

        ImGui::AlignTextToFramePadding();
        ImGui::Text("Workers %d/%d", run_.workers_active, run_.workers_total);
        vertical_separator();
        ImGui::Text("Spend $%.2f", run_.cost_usd);
        vertical_separator();
        status_label("API: no requests yet", c.muted);

        // Search and command palette, right-aligned.
        const float width = ImGui::GetFontSize() * 18;
        ImGui::SameLine();
        align_right(width);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        // Drawn like a text field; clicking opens the palette, where the typing happens.
        ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style.Colors[ImGuiCol_FrameBgHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, style.Colors[ImGuiCol_FrameBgActive]);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        if (ImGui::Button("##search", ImVec2(width, 0))) palette_.open();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float text_y = pos.y + style.FramePadding.y;
        dl->AddText(ImVec2(pos.x + style.FramePadding.x, text_y), ImGui::GetColorU32(ImGuiCol_TextDisabled), "Search or run a command");
        const std::string shortcut = shortcut_label(ImGuiMod_Ctrl | ImGuiKey_P);
        const float sw = ImGui::CalcTextSize(shortcut.c_str()).x;
        dl->AddText(ImVec2(pos.x + width - style.FramePadding.x - sw, text_y), ImGui::GetColorU32(ImGuiCol_TextDisabled), shortcut.c_str());
    }
    ImGui::End();
}

void App::draw_status_bar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, style.FramePadding.y));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, style.Colors[ImGuiCol_MenuBarBg]);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;
    const bool visible = ImGui::BeginViewportSideBar("##status_bar", viewport, ImGuiDir_Down, ImGui::GetFrameHeight(), flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (visible) {
        const char* dash = "\xE2\x80\x94";  // em dash: not known yet
        ImGui::TextUnformatted("Bytes matched");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", dash);
        vertical_separator();
        if (run_.phase == RunPhase::none && run_.planned == 0) ImGui::Text("Functions %s", dash);
        else ImGui::Text("Functions %d/%d matched", run_.matched, run_.planned);
        vertical_separator();
        ImGui::Text("Queue %s", dash);
        vertical_separator();
        ImGui::Text("ETA %s", dash);
        vertical_separator();
        if (run_.phase == RunPhase::none) ImGui::Text("Cache hit %s", dash);
        else ImGui::Text("Cache hit %.0f%%", run_.cache_hit_rate * 100.0);
        vertical_separator();

        // Notifications at the right edge; the last event fills the space in between.
        const usize unread = notifications_.unread();
        const std::string bell = unread ? std::format("Notifications ({})", unread) : std::string("Notifications");
        const float bell_w = ImGui::CalcTextSize(bell.c_str()).x + style.FramePadding.x * 2;
        const float avail = ImGui::GetContentRegionAvail().x - bell_w - style.ItemSpacing.x * 2;
        if (!run_.last_activity.empty() && avail > 40) {
            if (ImGui::TextLink(fit_text(run_.last_activity, avail).c_str())) ctx_.open("logs");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s\nOpen Logs and errors.", run_.last_activity.c_str());
        } else {
            ImGui::TextDisabled("No events");
        }
        ImGui::SameLine();
        align_right(bell_w);
        if (ImGui::SmallButton(bell.c_str())) show_notifications_ = !show_notifications_;
    }
    ImGui::End();
}

void App::draw_dockspace() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID id = dockspace_id();
    // The default layout on first run (or without an imgui.ini), and when the user resets it.
    if (reset_layout_ || ImGui::DockBuilderGetNode(id) == nullptr) {
        std::vector<layout::DockedWindow> windows;
        for (const auto& slot : slots_)
            windows.push_back({slot.window, layout::default_slot(slot.view->id()), layout::default_selected(slot.view->id())});
        layout::build_default(id, viewport->WorkSize, windows);
        reset_layout_ = false;
    }
    ImGui::DockSpaceOverViewport(id, viewport, ImGuiDockNodeFlags_None);
}

void App::draw_views() {
    const float font = ImGui::GetFontSize();
    for (usize i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (!slot.open) continue;
        const bool focus = focus_ && focus_->slot == i;
        if (focus) ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowSize(ImVec2(font * 40, font * 28), ImGuiCond_FirstUseEver);  // when floating
        bool open = true;
        // Views take focus when navigated to (focus_), not merely by appearing: that would also pick the
        // visible tab of every dock node the views appear in.
        const bool visible = ImGui::Begin(slot.window.c_str(), &open, slot.view->window_flags() | ImGuiWindowFlags_NoFocusOnAppearing);
        if (focus && (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || --focus_->frames_left <= 0)) focus_.reset();
        if (visible) {
            slot.drawn_frame = ImGui::GetFrameCount();
            slot.view->draw(ctx_);
        }
        ImGui::End();
        if (!open) {
            slot.open = false;
            store_open_views();
        }
    }
}

void App::draw_tools() {
    notifications_.draw_history(&show_notifications_, ctx_.nav, ctx_.colors());
    if (show_shortcuts_) {
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, ImGui::GetFontSize() * 26), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Keyboard shortcuts###shortcuts", &show_shortcuts_)) {
            if (ImGui::BeginTable("##shortcuts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Action");
                ImGui::TableHeadersRow();
                auto row = [](const std::string& keys, const std::string& what) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(keys.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(what.c_str());
                };
                for (const Action& a : actions_.all())
                    if (a.shortcut) row(shortcut_label(a.shortcut), a.label);
                // Handled by the views themselves.
                row("Ctrl+Enter", "Send guidance (Agent session composer)");
                row("F7, Shift+F7", "Next, previous differing row (Diff viewer)");
                row("Ctrl+B", "Toggle raw bytes (Diff viewer)");
                row("Ctrl+S", "Verify and save (Diff viewer, manual mode)");
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }
    if (show_metrics_) ImGui::ShowMetricsWindow(&show_metrics_);
    if (show_id_stack_) ImGui::ShowIDStackToolWindow(&show_id_stack_);
    if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);
    if (show_style_editor_) {
        if (ImGui::Begin("Style editor###style_editor", &show_style_editor_)) ImGui::ShowStyleEditor();
        ImGui::End();
    }
}

void App::draw_dialogs() {
    if (open_about_) {
        ImGui::OpenPopup("About Decomp###about");
        open_about_ = false;
    }
    if (open_save_layout_) {
        ImGui::OpenPopup("Save layout###save_layout");
        layout_name_.clear();
        open_save_layout_ = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("About Decomp###about", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Decomp %s", kVersion);
        ImGui::TextDisabled("AI-assisted matching decompilation: the supervision GUI.");
        ImGui::Spacing();
        ImGui::SeparatorText("Libraries");
        ImGui::BulletText("Dear ImGui %s (docking)", IMGUI_VERSION);
        ImGui::BulletText("ImPlot %s", IMPLOT_VERSION);
        ImGui::BulletText("ImGuiColorTextEdit v1.92.9");
        ImGui::BulletText("GLFW 3.5.1");
        ImGui::BulletText("Fonts: %s, %s", std::string(ui_font_name()).c_str(), std::string(mono_font_name()).c_str());
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(ImGui::GetFontSize() * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Save layout###save_layout", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Save the current layout and open views as:");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
        const bool enter = ImGui::InputTextWithHint("##name", "Layout name", &layout_name_, ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string name(trim(layout_name_));
        if (settings_.find_layout(name)) ImGui::TextDisabled("Replaces the saved layout \"%s\".", name.c_str());
        ImGui::BeginDisabled(name.empty());
        const bool save = ImGui::Button("Save") || (enter && !name.empty());
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        if (save && !name.empty()) {
            save_layout(name);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace decomp::gui
