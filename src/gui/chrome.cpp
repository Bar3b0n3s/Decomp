// The shell's chrome (docs/ui.md#layout-and-chrome): menu bar, top bar, status bar, dock space, the view
// windows, tool windows and dialogs.

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "core/version.hpp"
#include "gui/app.hpp"
#include "gui/fonts.hpp"
#include "gui/layout.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"
#include "viewmodel/common.hpp"

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
    Workspace* ws = services_.workspace;
    const bool live = ws && ws->run_live();
    if (ImGui::BeginMenu("File")) {
        actions_.menu_item("project.open");
        if (ImGui::BeginMenu("Recent projects", ws && !live && !settings_.recent_projects.empty())) {
            std::optional<std::string> chosen;
            for (const auto& path : settings_.recent_projects)
                if (ImGui::MenuItem(path.c_str())) chosen = path;
            if (chosen) open_project(fs::from_utf8(*chosen));
            ImGui::EndMenu();
        }
        actions_.menu_item("project.close");
        ImGui::Separator();
        actions_.menu_item("runs.show", "Runs...", show_runs_);
        actions_.menu_item("run.close");
        ImGui::Separator();
        actions_.menu_item("app.quit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Run")) {
        for (const char* id : {"run.start", "run.resume", "run.pause", "run.stop", "run.abort"}) actions_.menu_item(id);
        ImGui::Separator();
        actions_.menu_item("runs.show", "Runs...", show_runs_);
        if (!services_.commands->available()) {
            ImGui::Separator();
            ImGui::TextDisabled(ws && ws->run_read_only() ? "A past run is shown (read-only)." : "Open a project to start a run.");
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

        // Spend against the run budget: amber from 80%, red at 100%.
        if (run_.budget_usd > 0) {
            const double used = run_.cost_usd / run_.budget_usd;
            const ImVec4& color = used >= 1.0 ? c.error : used >= 0.8 ? c.warn : c.ok;
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
            ImGui::ProgressBar(static_cast<float>(std::min(used, 1.0)), ImVec2(ImGui::GetFontSize() * 9, 0),
                               std::format("${:.2f} / ${:.2f}", run_.cost_usd, run_.budget_usd).c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Run spend against the run budget (%.0f%%). Open Cost and usage.", used * 100.0);
            if (ImGui::IsItemClicked()) ctx_.open("cost");
        } else {
            ImGui::Text("Spend $%.2f", run_.cost_usd);
        }
        vertical_separator();

        // API health, always with text.
        switch (run_.api) {
        case ApiHealth::unknown: status_label("API: no requests yet", c.muted); break;
        case ApiHealth::ok: status_label("API: ok", c.ok); break;
        case ApiHealth::degraded: status_label(run_.backoff_left_ms > 0 ? "API: rate limited" : "API: retrying", c.warn); break;
        case ApiHealth::failing: status_label("API: key rejected", c.error); break;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            std::string tip;
            if (run_.last_ttft_ms > 0) tip += std::format("Time to first token: {} ms\n", run_.last_ttft_ms);
            tip += std::format("Retries in the last five minutes: {}", run_.recent_retries);
            if (!run_.last_error.empty()) tip += "\nLast error: " + run_.last_error;
            ImGui::SetTooltip("%s", tip.c_str());
        }
        // Rate limits from the latest response headers, and the backoff countdown.
        if (run_.rate_known && run_.requests_limit > 0) {
            vertical_separator();
            ImGui::Text("Requests %lld/%lld", run_.requests_remaining, run_.requests_limit);
            if (run_.backoff_left_ms > 0) {
                ImGui::SameLine();
                ImGui::TextColored(c.warn, "backoff %.1f s", static_cast<double>(run_.backoff_left_ms) / 1000.0);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                std::string tip = std::format("Requests remaining: {} of {}", run_.requests_remaining, run_.requests_limit);
                if (run_.input_limit > 0) tip += std::format("\nInput tokens remaining: {} of {}", run_.input_remaining, run_.input_limit);
                if (run_.output_limit > 0)
                    tip += std::format("\nOutput tokens remaining: {} of {}", run_.output_remaining, run_.output_limit);
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }

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
        // Project-wide progress (what `decomp status` prints); the run's own figures otherwise.
        if (progress_.progress) {
            const project::Progress& p = *progress_.progress;
            ImGui::Text("Bytes matched %.1f%%", p.percent_bytes());
            vertical_separator();
            ImGui::Text("Functions %zu/%zu matched", p.matched_functions, p.functions);
        } else {
            ImGui::TextUnformatted("Bytes matched");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", dash);
            vertical_separator();
            if (run_.phase == RunPhase::none && run_.planned == 0) ImGui::Text("Functions %s", dash);
            else ImGui::Text("Functions %d/%d matched", run_.matched, run_.planned);
        }
        vertical_separator();
        if (run_.phase == RunPhase::none) ImGui::Text("Queue %s", dash);
        else ImGui::Text("Queue %zu", run_.queued);
        vertical_separator();
        if (const auto& eta = eta_.eta; eta && run_.phase != RunPhase::none) {
            const bool done = eta->items.empty() && eta->running.empty() && eta->beyond_head == 0;
            ImGui::Text("ETA %s", done ? dash : vm::format_duration(eta->finish).c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                usize samples = 0;
                for (const auto& r : eta->running) samples = std::max(samples, r.estimate.samples);
                for (const auto& i : eta->items) samples = std::max(samples, i.estimate.samples);
                ImGui::SetTooltip("Time until the queue is done on %d worker(s): %zu running, %zu queued.\n%s", eta->workers,
                                  eta->running.size(), eta->items.size() + eta->beyond_head,
                                  samples ? "Estimated from how long functions of similar size took in this project's runs."
                                          : "No finished sessions yet: a rough default by function size.");
            }
        } else {
            ImGui::Text("ETA %s", dash);
        }
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
    draw_runs_window();
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
    draw_project_dialog();
    draw_quit_dialogs();
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

void App::draw_project_dialog() {
    if (open_project_dialog_) {
        ImGui::OpenPopup("Open project###open_project");
        if (project_path_.empty() && !settings_.recent_projects.empty()) project_path_ = settings_.recent_projects.front();
        open_project_dialog_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Open project###open_project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Project directory (the one with decomp.json):");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 32);
    const bool enter = ImGui::InputTextWithHint("##path", "/path/to/project", &project_path_, ImGuiInputTextFlags_EnterReturnsTrue);
    std::error_code ec;
    const std::string path(trim(project_path_));
    const bool exists = !path.empty() && std::filesystem::exists(fs::from_utf8(path) / project::Project::kConfigFile, ec);
    if (!path.empty() && !exists) ImGui::TextColored(ctx_.colors().warn, "No decomp.json there (decomp init creates a project).");
    if (!settings_.recent_projects.empty()) {
        ImGui::SeparatorText("Recent");
        for (const auto& recent : settings_.recent_projects)
            if (ImGui::Selectable(recent.c_str(), recent == path)) project_path_ = recent;
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(!exists);
    const bool open = ImGui::Button("Open") || (enter && exists);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    if (open && exists) {
        open_project(fs::from_utf8(path));
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::draw_quit_dialogs() {
    Workspace* ws = services_.workspace;
    if (open_quit_dialog_) {
        ImGui::OpenPopup("Quit###quit_run");
        open_quit_dialog_ = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quit###quit_run", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("A run is in progress. Quitting ends it; it can be resumed later (Runs...).");
        ImGui::Spacing();
        if (ImGui::Button("Stop and quit")) {
            if (ws) ws->end_run(false);
            quit_after_run_ = true;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Sessions finish their current turn first.");
        ImGui::SameLine();
        if (ImGui::Button("Abort and quit")) {
            if (ws) ws->end_run(true);
            quit_after_run_ = true;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Requests and compiles in flight are cancelled.");
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // While the run winds down before quitting.
    if (quit_after_run_ && ws && ws->run_live()) {
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("Quitting###quitting", nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::Text("Waiting for %d session(s) to end...", run_.workers_active);
            if (ImGui::Button("Abort now")) ws->end_run(true);
            ImGui::SameLine();
            if (ImGui::Button("Keep running")) quit_after_run_ = false;
        }
        ImGui::End();
    }
}

void App::draw_runs_window() {
    if (!show_runs_) return;
    Workspace* ws = services_.workspace;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 52, ImGui::GetFontSize() * 22), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Runs###runs", &show_runs_)) {
        ImGui::End();
        return;
    }
    if (!ws || ws->project_state().phase != ProjectPhase::open) {
        ImGui::TextDisabled("Open a project to see its runs.");
        ImGui::End();
        return;
    }
    const bool live = ws->run_live();
    bool refresh = ImGui::Button("Refresh");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", fs::to_utf8(ws->project()->runs_dir()).c_str());
    if (ws->run_loading()) {
        ImGui::SameLine();
        ImGui::TextDisabled("loading a run...");
    }
    const auto& runs = ws->runs(refresh);
    if (runs.empty()) ImGui::TextDisabled("No runs yet. Start one with Run > Start run (F5).");
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    std::optional<std::string> open, resume;
    std::optional<std::vector<u64>> again;
    if (!runs.empty() && ImGui::BeginTable("##runs", 7, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Run");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Done", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Matched", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Spent", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Model");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const auto& r : runs) {
            ImGui::PushID(r.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool shown = r.id == ws->run_id();
            if (shown) ImGui::TextColored(ctx_.colors().accent, "%s (shown)", r.id.c_str());
            else ImGui::TextUnformatted(r.id.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.status.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%zu/%zu", r.done, r.functions);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", r.matched);
            ImGui::TableNextColumn();
            ImGui::Text("$%.2f", r.spent_usd);
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", r.model.c_str(), r.replay ? " (replay)" : "");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(live || r.live);
            if (ImGui::SmallButton("Open")) open = r.id;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Show this run (read-only), replayed from its event log.");
            ImGui::SameLine();
            const bool resumable = r.status == "stopped" || r.status == "budget_exhausted" || r.status == "interrupted" || r.status == "aborted";
            ImGui::BeginDisabled(!resumable);
            if (ImGui::SmallButton("Resume")) resume = r.id;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Continue the run: finished functions stay finished; the others start over.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Run again")) {
                std::vector<u64> vas;
                if (auto q = r.run.find("queue"); q != r.run.end() && q->is_array())
                    for (const auto& item : *q)
                        if (item.contains("va") && item["va"].is_number_unsigned()) vas.push_back(item["va"].get<u64>());
                again = std::move(vas);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("A new run over the same functions.");
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (open) {
        if (auto r = ws->open_run(*open); !r) notifications_.notify(Severity::error, std::format("Cannot open run {}: {}", *open, r.error().message));
    }
    if (resume) {
        if (auto r = ws->resume_run(*resume); !r)
            notifications_.notify(Severity::error, std::format("Cannot resume run {}: {}", *resume, r.error().message));
    }
    if (again && !again->empty()) services_.commands->start_functions(std::move(*again));
    ImGui::End();
}

} // namespace decomp::gui
