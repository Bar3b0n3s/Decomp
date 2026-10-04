#include "gui/app.hpp"

#include "core/fs.hpp"
#include "core/log.hpp"
#include "gui/fonts.hpp"
#include "gui/layout.hpp"
#include "gui/theme.hpp"
#include "gui/views/views.hpp"
#include "gui/workspace.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <format>

namespace decomp::gui {

namespace {

// "Run monitor", "run-monitor" and "RUN_MONITOR" all name the view "run_monitor".
std::string normalize_view_name(std::string_view name) {
    std::string out;
    for (char c : name) {
        if (c == '-' || c == ' ') out += '_';
        else out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

constexpr auto kSaveInterval = std::chrono::seconds(2);

} // namespace

App::App(AppServices services, Settings& settings)
    : services_(std::move(services)),
      settings_(settings),
      views_(make_all_views()),
      jobs_(0, [this] { wake(); }),
      ctx_(services_, settings_, actions_, notifications_, jobs_) {
    if (!services_.snapshot) services_.snapshot = [] { return std::shared_ptr<const events::RunStateData>(); };
    if (!services_.commands) services_.commands = std::make_shared<RunCommands>();
    if (!services_.project) services_.project = [] { return ProjectInfo{}; };
    if (!services_.post_empty_event) services_.post_empty_event = [] {};
    notifications_.set_wake([this] { wake(); });

    for (const auto& view : views_) slots_.push_back({view.get(), window_name(*view), false, -1});

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigInputTextCursorBlink = false;  // no animations (docs/ui.md#accessibility)
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ctx_.fonts = load_fonts(*io.Fonts);
    settings_.font_size = std::clamp(settings_.font_size, Settings::kMinFontSize, Settings::kMaxFontSize);
    ImGui::GetStyle().FontSizeBase = settings_.font_size;  // before the first frame: no _NextFrame hack needed
    apply_style();
    register_actions();
}

App::~App() { save_settings(); }

void App::wake() const {
    if (services_.post_empty_event) services_.post_empty_event();
}

std::string App::window_name(const View& view) { return std::format("{}###{}", view.title(), view.id()); }

ImGuiID App::dockspace_id() { return ImHashStr("decomp.dockspace"); }

usize App::slot_index(std::string_view id) const {
    for (usize i = 0; i < slots_.size(); ++i)
        if (slots_[i].view->id() == id) return i;
    return slots_.size();
}

View* App::find_view(std::string_view name) const {
    const std::string wanted = normalize_view_name(name);
    for (const auto& slot : slots_)
        if (normalize_view_name(slot.view->id()) == wanted || normalize_view_name(slot.view->title()) == wanted) return slot.view;
    return nullptr;
}

bool App::is_open(std::string_view id) const {
    const usize i = slot_index(id);
    return i < slots_.size() && slots_[i].open;
}

void App::set_open(std::string_view id, bool open) {
    const usize i = slot_index(id);
    if (i == slots_.size() || slots_[i].open == open) return;
    slots_[i].open = open;
    store_open_views();
}

bool App::view_visible(std::string_view id) const {
    const usize i = slot_index(id);
    return i < slots_.size() && slots_[i].open && frame_number_ >= 0 && slots_[i].drawn_frame == frame_number_;
}

bool App::focus_view(std::string_view name) {
    View* view = find_view(name);
    if (!view) return false;
    ctx_.open(std::string(view->id()));
    return true;
}

void App::request_quit() {
    // A live run is ended first, the way the user chooses (draw_quit_dialogs).
    if (services_.workspace && services_.workspace->run_live()) {
        if (!quit_after_run_) open_quit_dialog_ = true;
        return;
    }
    quit_ = true;
}

void App::open_project(const std::filesystem::path& root) {
    Workspace* ws = services_.workspace;
    if (!ws) return;
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(root, ec).lexically_normal();
    if (auto r = ws->open_project(absolute); !r) {
        notifications_.notify(Severity::error, std::format("Cannot open {}: {}", fs::to_utf8(absolute), r.error().message));
        return;
    }
    reported_project_error_.clear();
    settings_.add_recent_project(absolute);
    ctx_.mark_settings_dirty();
}

void App::poll_workspace() {
    Workspace* ws = services_.workspace;
    if (!ws) return;
    // The developer setting may change in Settings; new runs use the current value.
    if (fs::to_utf8(ws->replay_dir()) != settings_.developer.replay_dir) ws->set_replay_dir(fs::from_utf8(settings_.developer.replay_dir));
    ws->poll();
    if (auto error = ws->take_error(); !error.empty()) notifications_.notify(Severity::error, error);
    const ProjectState& project = ws->project_state();
    if (project.phase == ProjectPhase::failed && project.error != reported_project_error_) {
        reported_project_error_ = project.error;
        notifications_.notify(Severity::error, std::format("Cannot open the project {}: {}", fs::to_utf8(project.root), project.error));
    }
    if (quit_after_run_ && !ws->run_live()) quit_ = true;

    // Project-wide progress (status bar), recomputed only when symbols or function states change.
    if (project.phase == ProjectPhase::open) {
        const auto program = ws->program();
        const u64 version = ws->project()->version();
        if (!progress_.progress || progress_.version != version || progress_.program != program.get()) {
            progress_.progress = project::compute_progress(program->symbols(), *ws->project());
            progress_.version = version;
            progress_.program = program.get();
        }
    } else {
        progress_ = {};
    }
}

void App::set_dpi_scale(float scale) {
    if (scale > 0.25f && scale < 8.0f) dpi_scale_ = scale;
}

double App::idle_timeout() {
    double timeout = 1.0;  // the chrome's clocks refresh at least once a second
    if (auto expiry = notifications_.next_expiry(ImGui::GetTime())) timeout = std::min(timeout, std::max(*expiry, 0.05));
    if (run_.phase == RunPhase::running || run_.phase == RunPhase::stopping) timeout = std::min(timeout, 0.5);
    return timeout;
}

void App::save_settings() {
    if (!ctx_.settings_dirty) return;
    ctx_.settings_dirty = false;
    last_save_ = std::chrono::steady_clock::now();
    if (auto r = gui::save_settings(settings_); !r) {
        const std::string message = std::format("Could not save the GUI settings: {}", r.error().message);
        log::warn("{}", message);
        if (message != last_save_error_) notifications_.notify(Severity::error, message, NavEntry{"settings", {}});
        last_save_error_ = message;
    } else {
        last_save_error_.clear();
    }
}

void App::store_open_views() {
    std::vector<std::string> open;
    for (const auto& slot : slots_)
        if (slot.open) open.emplace_back(slot.view->id());
    settings_.project_state(ctx_.project.root).open_views = std::move(open);
    ctx_.mark_settings_dirty();
}

void App::sync_project() {
    const std::string key = project_key(ctx_.project.root);
    if (project_synced_ && key == project_key_) return;
    project_key_ = key;
    project_synced_ = true;
    const ProjectViewState& state = settings_.project_state(ctx_.project.root);
    const std::vector<std::string>& open = state.open_views ? *state.open_views : layout::default_open_views();
    for (auto& slot : slots_) slot.open = std::ranges::find(open, slot.view->id()) != open.end();
}

void App::apply_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    if (!applied_theme_ || *applied_theme_ != settings_.theme || applied_dpi_ != dpi_scale_) {
        build_style(settings_.theme, dpi_scale_, style);
        apply_plot_theme(settings_.theme);
        applied_theme_ = settings_.theme;
        applied_dpi_ = dpi_scale_;
    }
    settings_.font_size = std::clamp(settings_.font_size, Settings::kMinFontSize, Settings::kMaxFontSize);
    if (style.FontSizeBase != settings_.font_size) {
        // Inside a frame ImGui keeps resetting FontSizeBase to the frame's font size; this is how its own
        // style editor changes the size (applied by the next NewFrame()).
        if (ImGui::GetCurrentContext()->WithinFrameScope) style._NextFrameFontSizeBase = settings_.font_size;
        else style.FontSizeBase = settings_.font_size;
    }
}

void App::apply_pending_layout() {
    if (!pending_layout_) return;
    // ImGui re-docks only the windows that were active in the previous frame (sorted by their saved tab
    // order); the layout's views are opened one frame earlier so that they are among them.
    if (pending_layout_wait_ > 0) {
        --pending_layout_wait_;
        return;
    }
    layout::apply(*pending_layout_);
    pending_layout_.reset();
}

void App::apply_navigation() {
    auto request = ctx_.nav.take_request();
    if (!request) return;
    const usize i = slot_index(request->view);
    if (i == slots_.size()) {
        log::debug("navigation to unknown view '{}'", request->view);
        return;
    }
    if (request->target.va) ctx_.selection.function_va = request->target.va;
    if (!request->target.session.empty()) ctx_.selection.session = request->target.session;
    if (!slots_[i].open) {
        slots_[i].open = true;
        store_open_views();
    }
    focus_ = FocusRequest{i, 3};  // a docked window may need a couple of frames before it can take focus
    slots_[i].view->navigate(ctx_, request->target);
}

void App::reset_layout() {
    reset_layout_ = true;
    for (auto& slot : slots_) {
        const auto& open = layout::default_open_views();
        slot.open = std::ranges::find(open, slot.view->id()) != open.end();
    }
    store_open_views();
}

void App::save_layout(std::string name) {
    SavedLayout saved{std::move(name), layout::capture(), {}};
    for (const auto& slot : slots_)
        if (slot.open) saved.open_views.emplace_back(slot.view->id());
    notifications_.notify(Severity::info, std::format("Saved the layout \"{}\".", saved.name));
    settings_.put_layout(std::move(saved));
    ctx_.mark_settings_dirty();
    register_layout_actions();
}

bool App::load_layout(std::string_view name) {
    const SavedLayout* saved = settings_.find_layout(name);
    if (!saved) return false;
    pending_layout_ = saved->ini;
    pending_layout_wait_ = 1;
    for (auto& slot : slots_) slot.open = std::ranges::find(saved->open_views, slot.view->id()) != saved->open_views.end();
    store_open_views();
    return true;
}

bool App::can_start() const {
    return services_.commands->available() && ctx_.project.open && (run_.phase == RunPhase::none || run_.phase == RunPhase::finished);
}

void App::frame() {
    frame_number_ = ImGui::GetFrameCount();
    poll_workspace();
    ctx_.snapshot = services_.snapshot();
    ctx_.project = services_.project();
    run_ = summarize(ctx_.snapshot.get());
    sync_project();
    apply_style();
    apply_pending_layout();

    // Shortcuts are handled before the views draw, so a claim made while drawing the previous frame (or
    // since) counts; the text editor sets io.WantTextInput mid-frame, hence the value kept from then.
    const ImGuiIO& io = ImGui::GetIO();
    actions_.handle_shortcuts(io.WantTextInput || text_input_last_frame_ || ctx_.keyboard_claimed);
    ctx_.keyboard_claimed = false;
    apply_navigation();

    draw_menu_bar();
    draw_top_bar();
    draw_status_bar();
    draw_dockspace();
    draw_views();
    draw_tools();
    palette_.draw(ctx_);
    notifications_.draw_toasts(ctx_.nav, ctx_.colors());
    draw_dialogs();

    text_input_last_frame_ = ImGui::GetIO().WantTextInput;
    if (ctx_.settings_dirty && std::chrono::steady_clock::now() - last_save_ >= kSaveInterval) save_settings();
}

} // namespace decomp::gui
