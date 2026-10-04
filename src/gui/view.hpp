#pragma once

// Views are the dockable panes of docs/ui.md#views. The App owns one instance of each (make_all_views(),
// gui/views/views.hpp), opens a window per view and calls draw() inside it with the frame's context.

#include "gui/actions.hpp"
#include "gui/fonts.hpp"
#include "gui/jobs.hpp"
#include "gui/navigation.hpp"
#include "gui/notifications.hpp"
#include "gui/services.hpp"
#include "gui/settings.hpp"
#include "gui/theme.hpp"

#include <imgui.h>

#include <memory>
#include <string>
#include <string_view>

namespace decomp::vm {
struct QueueEta;
} // namespace decomp::vm

namespace decomp::gui {

// Everything a view may use while drawing. Owned by the App and valid for its lifetime; UI thread only.
struct ViewContext {
    ViewContext(AppServices& services_, Settings& settings_, Actions& actions_, Notifications& notifications_, JobQueue& jobs_)
        : services(services_), settings(settings_), actions(actions_), notifications(notifications_), jobs(jobs_) {}
    ViewContext(const ViewContext&) = delete;
    ViewContext& operator=(const ViewContext&) = delete;

    AppServices& services;
    Settings& settings;
    Actions& actions;
    Notifications& notifications;
    JobQueue& jobs;
    Navigation nav;
    Selection selection;
    Fonts fonts;
    std::shared_ptr<const events::RunStateData> snapshot;  // loaded once per frame; null: no run
    ProjectInfo project;                                    // loaded once per frame
    // The live run's queue estimate (status bar, Run monitor), refreshed once a second; null: none.
    std::shared_ptr<const vm::QueueEta> eta;

    // Navigates (with history) to a view, e.g. ctx.open("inspector", {.va = 0x401000}).
    void open(std::string view, NavTarget target = {}) { nav.open(std::move(view), std::move(target)); }
    u64 notify(Severity severity, std::string text, std::optional<NavEntry> link = std::nullopt) {
        return notifications.notify(severity, std::move(text), std::move(link));
    }

    const ThemeColors& colors() const { return theme_colors(settings.theme); }
    DiffPalette diff_palette() const { return gui::diff_palette(settings.diff_palette, settings.theme); }

    // The current project's persisted state for a view (filters, columns, sort order): a JSON object in
    // gui.json. Call mark_settings_dirty() after changing it.
    Json& view_state(std::string_view view_id);
    void mark_settings_dirty() { settings_dirty = true; }

    // A view whose editor or custom text field has keyboard focus calls this every frame; global shortcuts
    // are suppressed while it does (ImGui text fields are detected automatically).
    void claim_keyboard() { keyboard_claimed = true; }

    bool settings_dirty = false;
    bool keyboard_claimed = false;  // read and reset by the App when it handles the shortcuts
};

class View {
public:
    virtual ~View() = default;

    // Stable identifier: ImGui window id ("Title###id"), settings keys, `decomp-gui --view <id>`.
    virtual std::string_view id() const = 0;
    // Window title and View-menu label.
    virtual std::string_view title() const = 0;
    // Draws the contents; the App has called ImGui::Begin() for the view's window (and calls End()).
    virtual void draw(ViewContext& ctx) = 0;
    // Navigation landed on this view (ctx.open(), back/forward); the selection is already updated.
    virtual void navigate(ViewContext& /*ctx*/, const NavTarget& /*target*/) {}
    // Extra flags for the view's window, e.g. ImGuiWindowFlags_MenuBar.
    virtual ImGuiWindowFlags window_flags() const { return ImGuiWindowFlags_None; }
};

} // namespace decomp::gui
