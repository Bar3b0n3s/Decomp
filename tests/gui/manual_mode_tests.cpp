// Manual mode end to end with the x86 fixture project and clang-cl: edit, background compiles, verify
// and save, hand back; and the Agent session and Diff viewer over a scripted run, live and past.

#include "matching/toolchain.hpp"
#include "session_views_support.hpp"
#include "viewmodel/attempts.hpp"
#include "viewmodel/common.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <mutex>

using namespace decomp;
using namespace decomp::gui;
using namespace decomp::gui::test;
using namespace std::chrono_literals;

namespace {

bool have_clang_cl() {
    if (matching::find_clang_cl()) return true;
    MESSAGE("clang-cl not found; skipping");
    return false;
}

constexpr const char* kWrong = "extern int g_counter;\n\n__declspec(noinline) int add(int a, int b) { return a - b + g_counter; }\n";
constexpr const char* kRight = "extern int g_counter;\n\n__declspec(noinline) int add(int a, int b) { return a + b + g_counter; }\n";
constexpr const char* kBroken = "extern int g_counter;\n\nint add(int a, int b) {\n    return a + nope + g_counter;\n}\n";

} // namespace

TEST_CASE("manual mode: edits recompile in the background, and verify and save writes, records and matches") {
    if (!have_clang_cl()) return;
    FixtureProject fx;
    // Hand back without a live run starts one with the edited source as guidance: record what it gets.
    std::mutex mutex;
    std::vector<std::string> handed_back;
    Workspace::Options options;
    options.stagger = 0ms;
    options.session_override = [&](const run::SessionRequest& r, events::EventBus& bus) {
        {
            std::lock_guard lock(mutex);
            handed_back = r.config.guidance;
        }
        bus.publish(events::SessionStarted{r.session_id, "?add@@YAHHH@Z", "int __cdecl add(int, int)", r.va, ""}, r.worker);
        bus.publish(events::SessionFinished{r.session_id, "gave_up", "", 0, 0, 0}, r.worker);
        agent::FunctionRunResult result;
        result.outcome = "gave_up";
        return result;
    };
    Workspace ws(std::move(options));
    REQUIRE(ws.open_project(fx.root));
    ws.wait_loaded();
    const u64 va = *ws.program()->resolve("add");
    const Symbol fn = *ws.program()->symbols().at(va);

    HeadlessContext gui;
    Settings settings;
    App app(ws.services(), settings);
    DiffViewerControl* view = diff_control(app);
    REQUIRE(view);
    // A project without a run: both views say what they need.
    for (const char* id : {"agent_session", "diff_viewer"}) {
        REQUIRE(app.focus_view(id));
        gui.frames(3, [&] { app.frame(); });
        CHECK(app.view_visible(id));
    }
    CHECK_FALSE(view->function());
    app.context().open("diff_viewer", {.va = va});
    REQUIRE(frames_until(gui, app, &ws, [&] { return view->function() == va && !view->busy(); }));
    CHECK(view->attempt_count() == 0);  // a function without attempts
    CHECK_FALSE(view->editing());

    // Editing starts from a stub; every edit marks the diff stale until its compile is shown.
    view->edit(app.context());
    CHECK(view->editing());
    view->set_text(kWrong);
    CHECK(view->stale());
    REQUIRE(frames_until(gui, app, &ws, [&] { return !view->stale() && !view->busy(); }));
    CHECK(view->shown_summary().find("not matching") != std::string::npos);

    // A compile error: no diff, diagnostics instead.
    view->set_text(kBroken);
    REQUIRE(frames_until(gui, app, &ws, [&] { return !view->stale() && !view->busy(); }));
    CHECK(view->shown_summary() == "compilation failed");
    gui.frames(3, [&] { app.frame(); });  // the diagnostics list and the editor's error markers

    // Several quick edits: only the last is compiled and shown.
    view->set_text(kWrong);
    view->set_text(kBroken);
    view->set_text(kRight);
    REQUIRE(frames_until(gui, app, &ws, [&] { return !view->stale() && !view->busy(); }));
    CHECK(view->text() == kRight);
    CHECK(view->shown_summary().find("MATCHING (byte-exact)") != std::string::npos);

    // Verify and save (Ctrl+S runs the same action).
    REQUIRE(app.actions().run("diff.verify_and_save"));
    REQUIRE(frames_until(gui, app, &ws, [&] { return !view->busy(); }));
    gui.frames(3, [&] { app.frame(); });
    const auto path = ws.project()->matched_source_path(fn);
    REQUIRE(std::filesystem::exists(path));
    CHECK(fs::read_text(path).value() == kRight);
    CHECK(ws.project()->function_info(va).status == project::FunctionStatus::matched);
    CHECK(ws.project()->function_info(va).best_match == 100.0);
    CHECK(ws.project()->function_info(va).attempts == 1);
    const auto attempts = vm::parse_attempts(ws.project()->attempts(fn));
    REQUIRE(attempts.size() == 1);
    CHECK(attempts[0].origin == "user");
    CHECK(attempts[0].byte_exact);
    CHECK(attempts[0].compiled);
    CHECK(attempts[0].attempt == 1);
    CHECK(attempts[0].session.starts_with("user-"));
    CHECK(attempts[0].source == kRight);
    CHECK(ws.project()->best_source(fn) == std::string(kRight));
    const auto changes = ws.project()->changes();
    REQUIRE_FALSE(changes.empty());
    CHECK(json_string_or(changes.back(), "source", "") == "user");
    CHECK(json_string_or(changes.back(), "reason", "") == "verified by hand");
    CHECK(frames_until(gui, app, &ws, [&] { return view->attempt_count() == 1; }));
    CHECK(app.notifications().history().size() >= 1);

    // A failing verification records the attempt but saves nothing.
    view->set_text(kWrong);
    view->verify_and_save(app.context());
    REQUIRE(frames_until(gui, app, &ws, [&] { return !view->busy(); }));
    CHECK(vm::parse_attempts(ws.project()->attempts(fn)).size() == 2);
    CHECK(fs::read_text(path).value() == kRight);
    CHECK(ws.project()->function_info(va).status == project::FunctionStatus::matched);

    // Hand back without a live run: a run on this function with the edited source as guidance.
    view->hand_back(app.context());
    CHECK_FALSE(view->editing());
    REQUIRE(frames_until(gui, app, &ws, [&] {
        std::lock_guard lock(mutex);
        return !handed_back.empty();
    }));
    {
        std::lock_guard lock(mutex);
        REQUIRE(handed_back.size() == 1);
        CHECK(handed_back[0].find("edited the source by hand") != std::string::npos);
        CHECK(handed_back[0].find("return a - b + g_counter;") != std::string::npos);
    }
    REQUIRE(frames_until(gui, app, &ws, [&] { return !ws.run_live(); }));
    gui.frames(3, [&] { app.frame(); });
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}

TEST_CASE("a scripted run in the Agent session and the Diff viewer, live and past, then taken over by hand") {
    if (!have_clang_cl()) return;
    FixtureProject fx;
    Workspace::Options options;
    options.stagger = 0ms;
    options.replay_dir = decomp::test::source_dir() / "tests" / "replay" / "run";
    Workspace ws(std::move(options));
    REQUIRE(ws.open_project(fx.root));
    ws.wait_loaded();
    const auto program = ws.program();
    std::vector<u64> functions;
    for (const char* name : {"add", "sum_array", "message", "dispatch"}) functions.push_back(*program->resolve(name));
    RunRequest request;
    request.functions = functions;
    request.workers = 2;
    const auto run_id = ws.start_run(request);
    REQUIRE(run_id);

    HeadlessContext gui;
    Settings settings;
    settings.developer.replay_dir = fs::to_utf8(decomp::test::source_dir() / "tests" / "replay" / "run");
    App app(ws.services(), settings);
    AgentSessionControl* session_view = session_control(app);
    DiffViewerControl* diff_view = diff_control(app);
    REQUIRE(session_view);
    REQUIRE(diff_view);

    // While it runs: follow the first function's session.
    app.context().open("agent_session", {.va = functions[0]});
    CHECK(frames_until(gui, app, &ws, [&] { return !session_view->session().empty(); }));
    REQUIRE(frames_until(gui, app, &ws, [&] { return !ws.run_live(); }, 6000));
    const auto snapshot = ws.snapshot();
    REQUIRE(snapshot);
    CHECK(snapshot->status == "completed");

    auto visit = [&](const char* label) {
        CAPTURE(label);
        const auto latest = vm::latest_sessions(*ws.snapshot());
        for (u64 va : functions) {
            CAPTURE(va);
            REQUIRE(latest.contains(va));
            app.context().open("agent_session", {.va = va, .session = latest.at(va)->id});
            REQUIRE(frames_until(gui, app, &ws, [&] { return session_view->transcript_loaded(); }));
            gui.frames(3, [&] { app.frame(); });
            CHECK(session_view->timeline_items() > 3);
            app.context().open("diff_viewer", {.va = va});
            REQUIRE(frames_until(gui, app, &ws, [&] { return diff_view->function() == va && !diff_view->busy(); }));
            gui.frames(3, [&] { app.frame(); });
            const std::string notifications = notification_log(app);
            CAPTURE(notifications);
            if (va == functions.back()) {
                CHECK(diff_view->attempt_count() == 0);  // dispatch's script gives up without compiling
                CHECK(diff_view->shown_summary().empty());
            } else {
                CHECK(diff_view->attempt_count() > 0);
                CHECK_FALSE(diff_view->shown_summary().empty());
            }
            app.context().open("diff_viewer", {.va = va, .anchor = "attempt:0"});
            REQUIRE(frames_until(gui, app, &ws, [&] { return !diff_view->busy(); }));
            gui.frames(2, [&] { app.frame(); });
        }
        for (const char* id : {"diff.toggle_raw", "diff.toggle_relocations", "diff.toggle_bytes", "diff.toggle_fuzzy", "diff.toggle_differing",
                               "diff.next_difference", "diff.previous_difference"})
            app.actions().run(id);
        gui.frames(3, [&] { app.frame(); });
        CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
    };
    visit("finished live run");
    ws.close_run();
    REQUIRE(ws.open_run(*run_id));
    ws.wait_loaded();
    visit("past run");

    // Take over a function without a live session: the Diff viewer edits its best attempt.
    app.context().open("agent_session", {.va = functions[0]});
    gui.frames(3, [&] { app.frame(); });
    REQUIRE(app.actions().run("session.take_over"));
    REQUIRE(frames_until(gui, app, &ws, [&] { return diff_view->editing() && !diff_view->busy() && !diff_view->stale(); }));
    CHECK(diff_view->function() == functions[0]);
    CHECK(diff_view->shown_summary().find("MATCHING (byte-exact)") != std::string::npos);  // add's best attempt matched
    gui.frames(3, [&] { app.frame(); });
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}

TEST_CASE("the Diff viewer shows a function's first attempts as a live run records them") {
    if (!have_clang_cl()) return;
    FixtureProject fx;
    Workspace::Options options;
    options.stagger = 0ms;
    options.replay_dir = decomp::test::source_dir() / "tests" / "replay" / "run";
    Workspace ws(std::move(options));
    REQUIRE(ws.open_project(fx.root));
    ws.wait_loaded();
    const u64 add = *ws.program()->resolve("add");
    const u64 sum_array = *ws.program()->resolve("sum_array");

    HeadlessContext gui;
    Settings settings;
    settings.developer.replay_dir = fs::to_utf8(decomp::test::source_dir() / "tests" / "replay" / "run");
    App app(ws.services(), settings);
    DiffViewerControl* diff_view = diff_control(app);
    REQUIRE(diff_view);
    // Shown before the run starts: no attempt yet.
    app.context().open("diff_viewer", {.va = add});
    REQUIRE(frames_until(gui, app, &ws, [&] { return diff_view->function() == add && !diff_view->busy(); }));
    CHECK(diff_view->attempt_count() == 0);
    CHECK(diff_view->shown_summary().empty());

    RunRequest request;
    request.functions = {sum_array, add};  // add's session starts once sum_array's ends
    request.workers = 1;
    const auto started = ws.start_run(request);
    REQUIRE_MESSAGE(started, (started ? std::string() : started.error().message));
    REQUIRE(frames_until(gui, app, &ws, [&] { return !ws.run_live(); }, 6000));
    REQUIRE(frames_until(gui, app, &ws, [&] { return diff_view->attempt_count() > 0 && !diff_view->busy(); }));
    gui.frames(3, [&] { app.frame(); });
    const std::string notifications = notification_log(app);
    CAPTURE(notifications);
    CHECK(diff_view->function() == add);
    CHECK(diff_view->shown_summary().find("MATCHING (byte-exact)") != std::string::npos);
}
