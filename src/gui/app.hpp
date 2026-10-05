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
#include "project/progress.hpp"
#include "gui/notifications.hpp"
#include "gui/palette.hpp"
#include "gui/run_summary.hpp"
#include "gui/services.hpp"
#include "gui/settings.hpp"
#include "gui/view.hpp"
#include "viewmodel/eta.hpp"
#include "viewmodel/notification_rules.hpp"

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

    // The user asked to quit (File > Quit, or request_quit() for the window's close button). With a live
    // run, request_quit() first asks whether to stop or abort it, and quitting waits for the workers.
    bool wants_quit() const { return quit_; }
    void request_quit();

    // Opens a project in the workspace (in the background) and records it in the recent projects.
    void open_project(const std::filesystem::path& root);

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
    // The live run's estimated time to finish its queue (status bar); nullopt without a live run or
    // before the first estimate.
    const vm::QueueEta* eta() const { return eta_.eta.get(); }

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
    void draw_runs_window();
    void draw_project_dialog();
    void draw_quit_dialogs();
    void poll_workspace();
    void update_run_notifications();
    void update_eta();

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
    bool show_runs_ = false;
    bool open_project_dialog_ = false;
    std::string project_path_;
    bool open_quit_dialog_ = false;
    bool quit_after_run_ = false;  // quit once the live run has ended
    std::string reported_project_error_;
    // Project-wide progress for the status bar, recomputed in the background when the project or program
    // changes.
    struct ProgressCache {
        u64 version = ~u64{0};
        const void* program = nullptr;
        JobHandle<project::Progress> job;
        std::optional<project::Progress> progress;
    } progress_;
    // Run notifications (docs/ui.md#notifications): what the rules find in each new snapshot goes to the
    // notification center. A resumed or reopened run's history is primed first, so it is not news.
    vm::NotificationRules notification_rules_;
    u64 notified_serial_ = ~u64{0};
    std::string notified_run_;
    bool notifications_primed_ = false;
    // The queue's ETA: session durations from the project's recent runs (loaded in the background) plus
    // the live run's own, re-estimated once a second.
    struct EtaState {
        std::filesystem::path project;
        u64 serial = ~u64{0};
        JobHandle<vm::DurationModel> loading;
        std::optional<vm::DurationModel> base;
        std::chrono::steady_clock::time_point computed{};
        std::shared_ptr<const vm::QueueEta> eta;  // also ViewContext::eta
    } eta_;
    std::string layout_name_;
    std::vector<std::string> layout_action_ids_;
};

} // namespace decomp::gui
