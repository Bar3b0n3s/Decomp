// The shell's actions: what the menus, the command palette and the keyboard shortcuts run
// (docs/ui.md#keyboard-shortcuts).

#include "gui/app.hpp"

#include <format>

namespace decomp::gui {

void App::register_actions() {
    const auto commands = [this] { return services_.commands.get(); };
    const auto live = [this] { return services_.commands->available(); };

    actions_.add({.id = "palette.open",
                  .label = "Search and command palette",
                  .category = "General",
                  .shortcut = ImGuiMod_Ctrl | ImGuiKey_P,
                  .run = [this] {
                      if (palette_.is_open()) palette_.close();
                      else palette_.open();
                  },
                  .in_text_input = true,
                  .in_palette = false});

    // Run control. Start and Resume share F5; at most one of them is enabled at a time.
    actions_.add({.id = "run.start",
                  .label = "Start run",
                  .category = "Run",
                  .shortcut = ImGuiKey_F5,
                  .enabled = [this] { return can_start(); },
                  .run = [commands] { commands()->start(); }});
    actions_.add({.id = "run.resume",
                  .label = "Resume run",
                  .category = "Run",
                  .shortcut = ImGuiKey_F5,
                  .enabled = [this, live] { return live() && run_.phase == RunPhase::paused; },
                  .run = [commands] { commands()->resume(); }});
    actions_.add({.id = "run.pause",
                  .label = "Pause run",
                  .category = "Run",
                  .shortcut = ImGuiKey_F6,
                  .enabled = [this, live] { return live() && run_.phase == RunPhase::running; },
                  .run = [commands] { commands()->pause(); }});
    actions_.add({.id = "run.stop",
                  .label = "Stop run (finish the current turns)",
                  .category = "Run",
                  .shortcut = ImGuiMod_Shift | ImGuiKey_F5,
                  .enabled = [this, live] { return live() && (run_.phase == RunPhase::running || run_.phase == RunPhase::paused); },
                  .run = [commands] { commands()->stop(); }});
    actions_.add({.id = "run.abort",
                  .label = "Abort run (cancel requests now)",
                  .category = "Run",
                  .shortcut = ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F5,
                  .enabled = [this, live] {
                      return live() && (run_.phase == RunPhase::running || run_.phase == RunPhase::paused || run_.phase == RunPhase::stopping);
                  },
                  .run = [commands] { commands()->abort(); }});

    // Navigation.
    actions_.add({.id = "nav.back",
                  .label = "Back",
                  .category = "Navigate",
                  .shortcut = ImGuiMod_Alt | ImGuiKey_LeftArrow,
                  .enabled = [this] { return ctx_.nav.can_back(); },
                  .run = [this] { ctx_.nav.back(); }});
    actions_.add({.id = "nav.forward",
                  .label = "Forward",
                  .category = "Navigate",
                  .shortcut = ImGuiMod_Alt | ImGuiKey_RightArrow,
                  .enabled = [this] { return ctx_.nav.can_forward(); },
                  .run = [this] { ctx_.nav.forward(); }});
    for (usize i = 0; i < slots_.size(); ++i) {
        const std::string id(slots_[i].view->id());
        actions_.add({.id = "view." + id,
                      .label = std::format("Show {}", slots_[i].view->title()),
                      .category = "View",
                      .shortcut = i < 9 ? ImGuiMod_Ctrl | static_cast<ImGuiKey>(ImGuiKey_1 + static_cast<int>(i)) : 0,
                      .run = [this, id] { ctx_.open(id); }});
    }

    // Appearance.
    auto change_font = [this](float size) {
        settings_.font_size = std::clamp(size, Settings::kMinFontSize, Settings::kMaxFontSize);
        ctx_.mark_settings_dirty();
    };
    actions_.add({.id = "font.larger",
                  .label = "Larger font",
                  .category = "View",
                  .shortcut = ImGuiMod_Ctrl | ImGuiKey_Equal,
                  .alternates = {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Equal, ImGuiMod_Ctrl | ImGuiKey_KeypadAdd},
                  .run = [this, change_font] { change_font(settings_.font_size + 1); }});
    actions_.add({.id = "font.smaller",
                  .label = "Smaller font",
                  .category = "View",
                  .shortcut = ImGuiMod_Ctrl | ImGuiKey_Minus,
                  .alternates = {ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract},
                  .run = [this, change_font] { change_font(settings_.font_size - 1); }});
    actions_.add({.id = "font.reset",
                  .label = "Reset font size",
                  .category = "View",
                  .shortcut = ImGuiMod_Ctrl | ImGuiKey_0,
                  .alternates = {ImGuiMod_Ctrl | ImGuiKey_Keypad0},
                  .run = [change_font] { change_font(Settings::kDefaultFontSize); }});
    for (Theme theme : {Theme::dark, Theme::light, Theme::high_contrast}) {
        actions_.add({.id = std::format("theme.{}", to_string(theme)),
                      .label = std::format("Theme: {}", theme_label(theme)),
                      .category = "View",
                      .run = [this, theme] {
                          settings_.theme = theme;
                          ctx_.mark_settings_dirty();
                      }});
    }
    for (DiffPaletteKind kind : {DiffPaletteKind::standard, DiffPaletteKind::okabe_ito}) {
        actions_.add({.id = std::format("diff_palette.{}", to_string(kind)),
                      .label = std::format("Diff palette: {}", diff_palette_label(kind)),
                      .category = "View",
                      .run = [this, kind] {
                          settings_.diff_palette = kind;
                          ctx_.mark_settings_dirty();
                      }});
    }

    // Layouts and windows.
    actions_.add({.id = "layout.reset", .label = "Reset layout", .category = "Layout", .run = [this] { reset_layout(); }});
    actions_.add({.id = "layout.save", .label = "Save layout as...", .category = "Layout", .run = [this] { open_save_layout_ = true; }});
    actions_.add({.id = "notifications.show",
                  .label = "Show notifications",
                  .category = "View",
                  .run = [this] { show_notifications_ = true; }});
    actions_.add({.id = "notifications.dismiss",
                  .label = "Dismiss all toasts",
                  .category = "View",
                  .run = [this] { notifications_.dismiss_all(); }});
    actions_.add({.id = "help.shortcuts", .label = "Keyboard shortcuts", .category = "Help", .run = [this] { show_shortcuts_ = true; }});
    actions_.add({.id = "help.about", .label = "About Decomp", .category = "Help", .run = [this] { open_about_ = true; }});
    actions_.add({.id = "dev.metrics",
                  .label = "Dear ImGui metrics and debugger",
                  .category = "Developer",
                  .run = [this] { show_metrics_ = true; }});
    actions_.add({.id = "dev.id_stack", .label = "ID stack tool", .category = "Developer", .run = [this] { show_id_stack_ = true; }});
    actions_.add({.id = "dev.style", .label = "Style editor", .category = "Developer", .run = [this] { show_style_editor_ = true; }});
    actions_.add({.id = "dev.demo", .label = "Dear ImGui demo", .category = "Developer", .run = [this] { show_demo_ = true; }});
    actions_.add({.id = "app.quit", .label = "Quit", .category = "General", .run = [this] { request_quit(); }});

    register_layout_actions();
}

void App::register_layout_actions() {
    for (const auto& id : layout_action_ids_) actions_.remove(id);
    layout_action_ids_.clear();
    for (const auto& saved : settings_.layouts) {
        std::string id = "layout.load." + saved.name;
        const std::string name = saved.name;
        actions_.add({.id = id,
                      .label = std::format("Load layout: {}", saved.name),
                      .category = "Layout",
                      .run = [this, name] { load_layout(name); }});
        layout_action_ids_.push_back(std::move(id));
    }
}

} // namespace decomp::gui
