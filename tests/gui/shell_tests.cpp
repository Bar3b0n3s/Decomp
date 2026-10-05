// The whole shell rendered headless (docs/ui.md#testing-strategy): every view focused in turn against
// synthetic snapshots, with assertions as exceptions and every item checked for conflicting IDs.

#include "gui/layout.hpp"
#include "harness.hpp"

#include <TextEditor.h>
#include <doctest/doctest.h>
#include <imgui_internal.h>
#include <implot.h>

#include <sstream>

using namespace decomp;
using namespace decomp::gui;
using namespace decomp::gui::test;

namespace {

struct NamedSnapshot {
    const char* name;
    std::shared_ptr<const events::RunStateData> data;
};

// Renders the App for five frames with `view` focused.
void check_view(const NamedSnapshot& snapshot, const std::string& view) {
    INFO("snapshot: " << std::string(snapshot.name) << ", view: " << view);
    HeadlessContext gui;
    Settings settings;
    App app(make_services(snapshot.data), settings);
    REQUIRE(app.focus_view(view));
    CHECK_NOTHROW(gui.frames(5, [&] { app.frame(); }));
    CHECK(app.view_visible(view));
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}

void check_every_view(const NamedSnapshot& snapshot) {
    const auto ids = all_view_ids();
    REQUIRE(ids.size() == 13);
    for (const auto& id : ids) check_view(snapshot, id);
}

// What a layout consists of, from its ini text: the dock tree ("DockSpace ..." and "DockNode ..." lines)
// and the node each window is docked in (the "DockId=" lines of the "[Window][id]" sections).
std::string dock_lines(const std::string& ini) {
    std::istringstream in(ini);
    std::string line, out, window;
    while (std::getline(in, line)) {
        if (line.starts_with("[Window][")) {
            window = line;
            continue;
        }
        if (line.starts_with("[")) window.clear();
        const auto first = line.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        if (line.compare(first, 9, "DockSpace") == 0 || line.compare(first, 8, "DockNode") == 0) out += line + "\n";
        else if (!window.empty() && line.starts_with("DockId=")) out += window + " " + line + "\n";
    }
    return out;
}

} // namespace

TEST_CASE("the harness turns ImGui assertions into exceptions") {
    HeadlessContext gui;
    ImGui::GetIO().ConfigErrorRecoveryEnableDebugLog = false;  // the error is intended: keep it out of the test output
    CHECK_THROWS_AS(gui.frame([] { ImGui::End(); }), AssertionFailure);  // End() without Begin()
}

TEST_CASE("the ID conflict detector sees duplicates without hovering") {
    HeadlessContext gui;
    gui.frames(2, [] {
        ImGui::Begin("Conflicts");
        ImGui::Button("Same");
        ImGui::Button("Same");
        ImGui::End();
    });
    CHECK(gui.id_conflicts() >= 1);
    CHECK(gui.describe_conflicts().find("Same") != std::string::npos);
}

TEST_CASE("every view renders with no run") { check_every_view({"no run", nullptr}); }
TEST_CASE("every view renders an empty run state") { check_every_view({"empty", empty_snapshot()}); }
TEST_CASE("every view renders a run in progress") { check_every_view({"mid-run", mid_run_snapshot()}); }
TEST_CASE("every view renders a failed run") { check_every_view({"error", error_snapshot()}); }

TEST_CASE("all views open at once render without conflicts") {
    for (const NamedSnapshot& snapshot : {NamedSnapshot{"no run", nullptr}, NamedSnapshot{"mid-run", mid_run_snapshot()}}) {
        INFO("snapshot: " << std::string(snapshot.name));
        HeadlessContext gui;
        Settings settings;
        App app(make_services(snapshot.data), settings);
        for (const auto& id : all_view_ids()) app.set_open(id, true);
        CHECK_NOTHROW(gui.frames(5, [&] { app.frame(); }));
        CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
    }
}

TEST_CASE("the default layout opens the specified views and tabs") {
    HeadlessContext gui;
    Settings settings;
    App app(make_services(nullptr), settings);
    gui.frames(3, [&] { app.frame(); });
    for (const auto& id : layout::default_open_views()) CHECK(app.is_open(id));
    CHECK_FALSE(app.is_open("binary_explorer"));
    // One visible tab per dock node: Function browser, Dashboard, Inspector and Run monitor.
    for (const char* id : {"function_browser", "dashboard", "inspector", "run_monitor"}) CHECK(app.view_visible(id));
    for (const char* id : {"agent_session", "diff_viewer", "logs"}) CHECK_FALSE(app.view_visible(id));
    ImGuiDockNode* root = ImGui::DockBuilderGetNode(App::dockspace_id());
    REQUIRE(root != nullptr);
    CHECK(root->IsSplitNode());
}

TEST_CASE("themes, font sizes and DPI scales apply") {
    for (Theme theme : {Theme::dark, Theme::light, Theme::high_contrast}) {
        INFO("theme " << std::string(to_string(theme)));
        HeadlessContext gui;
        Settings settings;
        settings.theme = theme;
        App app(make_services(mid_run_snapshot()), settings);
        auto frame = [&] { app.frame(); };
        app.set_dpi_scale(1.5f);
        CHECK_NOTHROW(gui.frames(3, frame));
        CHECK(ImGui::GetStyle().FontScaleDpi == doctest::Approx(1.5f));
        CHECK(ImGui::GetStyle().FontSizeBase == doctest::Approx(Settings::kDefaultFontSize));

        gui.tap(ImGuiMod_Ctrl | ImGuiKey_Equal, frame);
        CHECK(settings.font_size == doctest::Approx(Settings::kDefaultFontSize + 1));
        gui.frame(frame);
        CHECK(ImGui::GetStyle().FontSizeBase == doctest::Approx(Settings::kDefaultFontSize + 1));
        gui.tap(ImGuiMod_Ctrl | ImGuiKey_Minus, frame);
        gui.tap(ImGuiMod_Ctrl | ImGuiKey_Minus, frame);
        CHECK(settings.font_size == doctest::Approx(Settings::kDefaultFontSize - 1));
        gui.tap(ImGuiMod_Ctrl | ImGuiKey_0, frame);
        CHECK(settings.font_size == doctest::Approx(Settings::kDefaultFontSize));

        const ImVec4 before = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        settings.theme = theme == Theme::light ? Theme::dark : Theme::light;
        gui.frames(2, frame);
        const ImVec4 after = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        CHECK(before.x != after.x);
        CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
    }
}

TEST_CASE("run control shortcuts reach the commands only when valid") {
    HeadlessContext gui;
    Settings settings;
    auto commands = std::make_shared<RecordingCommands>(true);
    auto snapshot = snapshot_with_status("running");
    AppServices services = make_services(nullptr, commands, ProjectInfo{"/projects/demo", "GAME.EXE", true});
    services.snapshot = [&snapshot] { return snapshot; };
    App app(std::move(services), settings);
    auto frame = [&] { app.frame(); };
    gui.frames(2, frame);

    gui.tap(ImGuiKey_F6, frame);                                    // pause
    gui.tap(ImGuiMod_Shift | ImGuiKey_F5, frame);                   // stop
    gui.tap(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F5, frame);   // abort
    gui.tap(ImGuiKey_F5, frame);                                    // running: neither start nor resume
    snapshot = snapshot_with_status("paused");
    gui.frame(frame);
    gui.tap(ImGuiKey_F6, frame);  // already paused
    gui.tap(ImGuiKey_F5, frame);  // resume
    snapshot = snapshot_with_status("completed");
    gui.frame(frame);
    gui.tap(ImGuiKey_F6, frame);  // nothing to pause
    gui.tap(ImGuiKey_F5, frame);  // start a new run
    CHECK(commands->calls == std::vector<std::string>{"pause", "stop", "abort", "resume", "start"});
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}

TEST_CASE("run controls stay disabled without a live controller or project") {
    HeadlessContext gui;
    Settings settings;
    auto offline = std::make_shared<RecordingCommands>(false);
    App app(make_services(snapshot_with_status("running"), offline, ProjectInfo{"/projects/demo", "GAME.EXE", true}), settings);
    auto frame = [&] { app.frame(); };
    gui.frames(2, frame);
    for (ImGuiKeyChord chord : {ImGuiKeyChord(ImGuiKey_F5), ImGuiKeyChord(ImGuiKey_F6), ImGuiMod_Shift | ImGuiKey_F5,
                                ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F5})
        gui.tap(chord, frame);
    CHECK(offline->calls.empty());

    // A live controller cannot start a run without a loaded project.
    HeadlessContext gui2;
    Settings settings2;
    auto live = std::make_shared<RecordingCommands>(true);
    App app2(make_services(nullptr, live, ProjectInfo{"/projects/demo", "", false}), settings2);
    auto frame2 = [&] { app2.frame(); };
    gui2.frames(2, frame2);
    gui2.tap(ImGuiKey_F5, frame2);
    CHECK(live->calls.empty());
}

TEST_CASE("global shortcuts are suppressed while a text field has focus") {
    HeadlessContext gui;
    Settings settings;
    auto commands = std::make_shared<RecordingCommands>(true);
    App app(make_services(snapshot_with_status("running"), commands, ProjectInfo{"/projects/demo", "GAME.EXE", true}), settings);
    auto frame = [&] { app.frame(); };
    gui.frames(2, frame);

    gui.tap(ImGuiMod_Ctrl | ImGuiKey_P, frame);
    gui.frame(frame);
    REQUIRE(app.palette().is_open());
    CHECK(ImGui::GetIO().WantTextInput);
    gui.tap(ImGuiKey_F6, frame);
    gui.tap(ImGuiMod_Ctrl | ImGuiKey_Equal, frame);
    CHECK(commands->calls.empty());
    CHECK(settings.font_size == doctest::Approx(Settings::kDefaultFontSize));

    gui.tap(ImGuiKey_Escape, frame);
    gui.frames(2, frame);
    CHECK_FALSE(app.palette().is_open());
    gui.tap(ImGuiKey_F6, frame);
    CHECK(commands->calls == std::vector<std::string>{"pause"});
}

TEST_CASE("a view claiming the keyboard suppresses global shortcuts") {
    HeadlessContext gui;
    Settings settings;
    auto commands = std::make_shared<RecordingCommands>(true);
    App app(make_services(snapshot_with_status("running"), commands, ProjectInfo{"/projects/demo", "GAME.EXE", true}), settings);
    auto frame = [&] {
        app.frame();
        app.context().claim_keyboard();  // as an editor with focus would, every frame
    };
    gui.frames(2, frame);
    gui.tap(ImGuiKey_F6, frame);
    CHECK(commands->calls.empty());
}

TEST_CASE("the palette runs the chosen action") {
    HeadlessContext gui;
    Settings settings;
    App app(make_services(nullptr), settings);
    auto frame = [&] { app.frame(); };
    gui.frames(2, frame);
    gui.tap(ImGuiMod_Ctrl | ImGuiKey_P, frame);
    gui.frame(frame);
    gui.type(">theme light", frame);
    CHECK(app.palette().input() == ">theme light");
    auto items = app.palette().items(app.palette().input(), app.context());
    REQUIRE_FALSE(items.empty());
    CHECK(items.front().label == "Theme: Light");
    gui.tap(ImGuiKey_Enter, frame);
    gui.frames(2, frame);
    CHECK(settings.theme == Theme::light);
    CHECK_FALSE(app.palette().is_open());

    // An address jumps to the Inspector.
    items = app.palette().items("0x401060", app.context());
    REQUIRE_FALSE(items.empty());
    CHECK(items.front().label == "Go to 0x00401060");
    items.front().run();
    gui.frames(3, frame);
    CHECK(app.context().selection.function_va == 0x401060u);
    CHECK(app.view_visible("inspector"));
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}

TEST_CASE("navigation opens views, follows the selection and goes back and forward") {
    HeadlessContext gui;
    Settings settings;
    App app(make_services(mid_run_snapshot()), settings);
    auto frame = [&] { app.frame(); };
    gui.frames(2, frame);

    app.context().open("inspector", {.va = 0x401060});
    gui.frames(3, frame);
    CHECK(app.context().selection.function_va == 0x401060u);
    CHECK(app.view_visible("inspector"));

    app.context().open("binary_explorer");  // closed in the default layout
    gui.frames(3, frame);
    CHECK(app.is_open("binary_explorer"));
    CHECK(app.view_visible("binary_explorer"));

    gui.tap(ImGuiMod_Alt | ImGuiKey_LeftArrow, frame);
    gui.frames(2, frame);
    REQUIRE(app.context().nav.current() != nullptr);
    CHECK(app.context().nav.current()->view == "inspector");
    gui.tap(ImGuiMod_Alt | ImGuiKey_RightArrow, frame);
    gui.frames(2, frame);
    CHECK(app.context().nav.current()->view == "binary_explorer");

    gui.tap(ImGuiMod_Ctrl | ImGuiKey_3, frame);  // the third view in the View menu
    gui.frames(3, frame);
    CHECK(app.context().nav.current()->view == "agent_session");
    CHECK(app.view_visible("agent_session"));
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}

TEST_CASE("named layouts restore the dock layout and the open views") {
    HeadlessContext gui;
    Settings settings;
    App app(make_services(nullptr), settings);
    auto frame = [&] { app.frame(); };
    gui.frames(3, frame);

    app.save_layout("review");
    const SavedLayout* saved = settings.find_layout("review");
    REQUIRE(saved != nullptr);
    CHECK(saved->ini.find("[Docking][Data]") != std::string::npos);
    const std::string saved_docks = dock_lines(saved->ini);
    CHECK(saved_docks.find("DockSpace") != std::string::npos);
    CHECK(saved->open_views.size() == layout::default_open_views().size());
    CHECK(app.actions().find("layout.load.review") != nullptr);

    // Change the layout: Logs moves to the left node, Settings opens, Agent session closes.
    gui.frame([&] {
        ImGuiDockNode* root = ImGui::DockBuilderGetNode(App::dockspace_id());
        REQUIRE(root != nullptr);
        REQUIRE(root->ChildNodes[0] != nullptr);
        ImGui::DockBuilderDockWindow("Logs and errors###logs", root->ChildNodes[0]->ID);
        app.frame();
    });
    app.set_open("settings", true);
    app.set_open("agent_session", false);
    gui.frames(3, frame);
    CHECK(dock_lines(layout::capture()) != saved_docks);

    CHECK(app.load_layout("review"));
    gui.frames(3, frame);
    CHECK(dock_lines(layout::capture()) == saved_docks);
    CHECK(app.is_open("agent_session"));
    CHECK_FALSE(app.is_open("settings"));
    CHECK_FALSE(app.load_layout("no such layout"));

    app.reset_layout();
    gui.frames(3, frame);
    for (const auto& id : layout::default_open_views()) CHECK(app.is_open(id));
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}

TEST_CASE("open views are remembered per project") {
    HeadlessContext gui;
    Settings settings;
    ProjectInfo project{"/projects/demo", "GAME.EXE", true};
    App app(make_services(nullptr, nullptr, project), settings);
    auto frame = [&] { app.frame(); };
    gui.frames(2, frame);
    app.set_open("cost", true);
    const auto& state = settings.project_state(project.root);
    REQUIRE(state.open_views.has_value());
    CHECK(std::ranges::find(*state.open_views, std::string("cost")) != state.open_views->end());
    CHECK_FALSE(settings.project_state({}).open_views.has_value());

    // A second App on the same settings starts with the project's views.
    HeadlessContext gui2;
    App app2(make_services(nullptr, nullptr, project), settings);
    gui2.frames(2, [&] { app2.frame(); });
    CHECK(app2.is_open("cost"));
}

TEST_CASE("toasts and the notification history render") {
    HeadlessContext gui;
    Settings settings;
    App app(make_services(nullptr), settings);
    auto frame = [&] { app.frame(); };
    app.context().notify(Severity::info, "Function matched: int __cdecl add(int, int)", NavEntry{"diff_viewer", {.va = 0x401060}});
    app.context().notify(Severity::warning, "Run budget at 80%", NavEntry{"cost", {}});
    app.context().notify(Severity::error, "Authentication error (401)", NavEntry{"settings", {}});
    app.actions().run("notifications.show");
    CHECK_NOTHROW(gui.frames(4, frame));
    CHECK(app.notifications().history().size() == 3);
    CHECK(app.notifications().unread() == 0);  // the history panel was shown
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}

TEST_CASE("the developer tools render") {
    HeadlessContext gui;
    Settings settings;
    App app(make_services(mid_run_snapshot()), settings);
    for (const char* id : {"dev.metrics", "dev.id_stack", "dev.style", "dev.demo", "help.shortcuts", "help.about"}) CHECK(app.actions().run(id));
    CHECK_NOTHROW(gui.frames(4, [&] { app.frame(); }));
}

TEST_CASE("ImPlot and the text editor work with the shared ImGui configuration") {
    HeadlessContext gui;
    TextEditor editor;
    editor.SetLanguage(TextEditor::Language::Cpp());
    const std::string source = "int add(int a, int b) {\n    return a + b;\n}";
    editor.SetText(source);
    editor.AddMarker(1, IM_COL32(220, 60, 60, 255), IM_COL32(220, 60, 60, 60), "error", "C2065: undeclared identifier");
    editor.SetChangeCallback([] {}, 300);
    const double xs[] = {1, 2, 3, 4};
    const double ys[] = {40, 62.5, 87.5, 100};
    CHECK_NOTHROW(gui.frames(5, [&] {
        ImGui::SetNextWindowSize(ImVec2(900, 700));
        ImGui::Begin("Widgets");
        if (ImPlot::BeginPlot("Score per attempt", ImVec2(-1, 250))) {
            ImPlot::SetupAxes("attempt", "match %");
            ImPlot::PlotLine("best", xs, ys, 4, {ImPlotProp_Marker, ImPlotMarker_Circle, ImPlotProp_LineWeight, 2.0f});
            ImPlot::PlotBars("score", ys, 4, 0.5, 1.0, {ImPlotProp_FillAlpha, 0.5f});
            ImPlot::EndPlot();
        }
        editor.Render("##source", ImVec2(-1, 300));
        ImGui::End();
    }));
    CHECK(editor.GetText() == source);
    CHECK(editor.GetLineCount() == 3);
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}
