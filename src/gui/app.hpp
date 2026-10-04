#pragma once

// The decomp-gui shell (docs/ui.md#layout-and-chrome): menu bar, top bar, dock space with the views,
// status bar, notifications and the command palette. The App draws one frame at a time inside an ImGui
// frame that the platform (decomp-gui's main, or the headless tests) begins and renders:
//
//   ImGui::CreateContext(); ImPlot::CreateContext();   // before the App
//   App app(services, settings);
//   per frame: <backend NewFrame>; ImGui::NewFrame(); app.frame(); ImGui::Render(); <backend render>
//
// The App configures the ImGui context it is created in (docking, keyboard navigation, fonts, style).

#include "gui/actions.hpp"
#include "gui/jobs.hpp"
#include "gui/notifications.hpp"
#include "gui/palette.hpp"
#include "gui/run_summary.hpp"
#include "gui/services.hpp"
#include "gui/settings.hpp"
#include "gui/view.hpp"

#include <imgui.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::gui {

class App {
public:
    App(AppServices services, Settings& settings);
    ~App();  // saves the settings if they changed
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Draws the whole UI; call between ImGui::NewFrame() and ImGui::Render().
    void frame();

    // The user asked to quit (File > Quit, or request_quit() for the window's close button).
    bool wants_quit() const { return quit_; }
    void request_quit();

    // Monitor content scale (glfwGetWindowContentScale): scales fonts and sizes from the next frame on.
    void set_dpi_scale(float scale);
    float dpi_scale() const { return dpi_scale_; }

    // Opens a view and gives it focus. Accepts the id, the title or either in kebab-case, ignoring case
    // ("run_monitor", "Run monitor", "run-monitor"). Returns false for an unknown name.
    bool focus_view(std::string_view name);
    // How long the platform loop may wait for input before the next frame is due (toast expiry, clocks).
    double idle_timeout();

    // Writes gui.json now if anything changed (also done periodically and on destruction).
    void save_settings();

    // Views.
    const std::vector<std::unique_ptr<View>>& views() const { return views_; }
    View* find_view(std::string_view name) const;
    bool is_open(std::string_view id) const;
    void set_open(std::string_view id, bool open);
    // Whether the view drew its contents in the latest frame() (open, and its tab is the visible one).
    bool view_visible(std::string_view id) const;
    static std::string window_name(const View& view);  // "Title###id"

    // Layouts.
    void reset_layout();                  // back to the default layout, from the next frame
    void save_layout(std::string name);   // the current layout and open views, under `name`
    bool load_layout(std::string_view name);
    static ImGuiID dockspace_id();

    ViewContext& context() { return ctx_; }
    Actions& actions() { return actions_; }
    Notifications& notifications() { return notifications_; }
    CommandPalette& palette() { return palette_; }
    JobQueue& jobs() { return jobs_; }
    const RunSummary& run() const { return run_; }
    Settings& settings() { return settings_; }

private:
    struct Slot {
        View* view = nullptr;  // owned by views_
        std::string window;    // "Title###id"
        bool open = false;
        int drawn_frame = -1;
    };

    void register_actions();
    void register_layout_actions();
    void sync_project();
    void apply_style();
    void apply_pending_layout();
    void apply_navigation();
    void store_open_views();
    usize slot_index(std::string_view id) const;  // views_.size() when unknown

    // Chrome (chrome.cpp).
    void draw_menu_bar();
    void draw_top_bar();
    void draw_status_bar();
    void draw_dockspace();
    void draw_views();
    void draw_tools();
    void draw_dialogs();

    bool can_start() const;
    void wake() const;

    AppServices services_;
    Settings& settings_;
    Actions actions_;
    Notifications notifications_;
    CommandPalette palette_;
    std::vector<std::unique_ptr<View>> views_;
    std::vector<Slot> slots_;
    JobQueue jobs_;  // after the members its jobs may reach, so that it is destroyed (joined) first
    ViewContext ctx_;
    RunSummary run_;

    std::string project_key_;
    bool project_synced_ = false;
    float dpi_scale_ = 1.0f;
    float applied_dpi_ = 0.0f;
    std::optional<Theme> applied_theme_;
    bool reset_layout_ = false;
    std::optional<std::string> pending_layout_;
    int pending_layout_wait_ = 0;  // frames to wait before applying pending_layout_
    struct FocusRequest {
        usize slot = 0;
        int frames_left = 0;
    };
    std::optional<FocusRequest> focus_;
    bool quit_ = false;
    int frame_number_ = -1;  // ImGui's frame count during the latest frame()
    bool text_input_last_frame_ = false;
    std::chrono::steady_clock::time_point last_save_{};
    std::string last_save_error_;

    // Auxiliary windows and dialogs.
    bool show_notifications_ = false;
    bool show_shortcuts_ = false;
    bool show_metrics_ = false;
    bool show_id_stack_ = false;
    bool show_style_editor_ = false;
    bool show_demo_ = false;
    bool open_about_ = false;
    bool open_save_layout_ = false;
    std::string layout_name_;
    std::vector<std::string> layout_action_ids_;
};

} // namespace decomp::gui
