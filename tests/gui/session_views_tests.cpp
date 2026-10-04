// The Agent session and the Diff viewer in every state: synthetic snapshots, live sessions whose
// transcripts grow, past runs with missing, truncated and large transcripts, and the guidance composer.

#include "events/bus.hpp"
#include "session_views_support.hpp"
#include "viewmodel/common.hpp"

#include <doctest/doctest.h>
#include <imgui_internal.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <set>

using namespace decomp;
using namespace decomp::gui;
using namespace decomp::gui::test;
using namespace std::chrono_literals;

namespace {

// Live commands that queue guidance like a running session would.
class GuidanceCommands : public RecordingCommands {
public:
    GuidanceCommands() : RecordingCommands(true) {}
    bool live() const override { return true; }
    u64 inject(std::string_view session, std::string_view text) override {
        queued.insert(++next);
        RecordingCommands::inject(session, text);
        return next;
    }
    bool retract(std::string_view, u64 id) override { return queued.erase(id) > 0; }
    bool skip(u64 va) override {
        calls.push_back(std::format("skip {:x}", va));
        return true;
    }

    std::set<u64> queued;
    u64 next = 100;
};

struct Navigation {
    const char* view;
    NavTarget target;
};

const std::vector<Navigation>& navigations() {
    static const std::vector<Navigation> list = {
        {"agent_session", {}},
        {"agent_session", {.va = 0x401060, .session = "run-401060"}},  // live, mid-stream
        {"agent_session", {.session = "run-4010a0"}},                  // matched
        {"agent_session", {.session = "run-401200"}},                  // refused
        {"agent_session", {.va = 0x401080}},                           // the function's latest session
        {"agent_session", {.va = 0x4fffff}},                           // a function without a session
        {"agent_session", {.session = "no-such-session"}},
        {"diff_viewer", {.va = 0x401060}},
        {"diff_viewer", {.va = 0x401060, .session = "run-401060", .anchor = "manual"}},
        {"diff_viewer", {.va = 0x4010a0, .anchor = "attempt:3"}},
        {"diff_viewer", {}},
    };
    return list;
}

} // namespace

TEST_CASE("the Agent session and the Diff viewer render every synthetic state, for sessions and functions") {
    for (const auto& [name, snapshot] : {std::pair{"no run", std::shared_ptr<const events::RunStateData>()}, std::pair{"empty", empty_snapshot()},
                                         std::pair{"mid-run", mid_run_snapshot()}, std::pair{"error", error_snapshot()}}) {
        INFO("snapshot: " << name);
        HeadlessContext gui;
        Settings settings;
        App app(make_services(snapshot, std::make_shared<GuidanceCommands>(), ProjectInfo{"/projects/demo", "GAME.EXE", true}), settings);
        auto frame = [&] { app.frame(); };
        for (const auto& nav : navigations()) {
            INFO("view: " << nav.view << ", session: " << nav.target.session << ", anchor: " << nav.target.anchor);
            app.context().open(nav.view, nav.target);
            CHECK_NOTHROW(gui.frames(4, frame));
            CHECK(app.view_visible(nav.view));
        }
        // Both views open side by side, with every toggle of the Diff viewer.
        app.set_open("agent_session", true);
        app.set_open("diff_viewer", true);
        for (const char* id : {"diff.toggle_raw", "diff.toggle_relocations", "diff.toggle_bytes", "diff.toggle_fuzzy", "diff.toggle_differing"})
            app.actions().run(id);
        CHECK_NOTHROW(gui.frames(3, frame));
        CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
    }
}

TEST_CASE("the guidance composer queues guidance, shows it pending and retracts it") {
    HeadlessContext gui;
    Settings settings;
    auto commands = std::make_shared<GuidanceCommands>();
    App app(make_services(mid_run_snapshot(), commands, ProjectInfo{"/projects/demo", "GAME.EXE", true}), settings);
    auto frame = [&] { app.frame(); };
    app.context().open("agent_session", {.va = 0x401060, .session = "run-401060"});
    gui.frames(3, frame);
    AgentSessionControl* view = session_control(app);
    REQUIRE(view);
    CHECK(view->session() == "run-401060");
    CHECK_FALSE(view->transcript_loaded());  // no workspace: the snapshot's summary and live buffer only
    CHECK(view->timeline_items() == 1);       // the turn being streamed

    const u64 first = view->send_guidance(app.context(), "Try swapping the operands of the add.");
    const u64 second = view->send_guidance(app.context(), "Declare the loop counter first.");
    CHECK(first != 0);
    CHECK(view->pending_guidance() == std::vector<u64>{first, second});
    CHECK(commands->calls.size() == 2);
    CHECK(commands->calls[0] == "inject run-401060: Try swapping the operands of the add.");
    gui.frames(3, frame);  // the pending list and its Retract buttons
    CHECK(view->retract_guidance(app.context(), second));
    CHECK(view->pending_guidance() == std::vector<u64>{first});
    commands->queued.clear();  // sent meanwhile: too late to retract
    CHECK_FALSE(view->retract_guidance(app.context(), first));
    CHECK(view->pending_guidance() == std::vector<u64>{first});
    CHECK(view->send_guidance(app.context(), "   ") == 0);  // nothing to send

    // Ending the session and taking over go through the commands.
    app.actions().run("session.end");
    CHECK(commands->calls.back() == "skip 401060");
    app.actions().run("session.take_over");  // no project: unavailable
    gui.frames(3, frame);
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());

    // Every action the views registered has a unique id and shortcut (F7, Shift+F7, Ctrl+B, Ctrl+S).
    app.context().open("diff_viewer", {.va = 0x401060});
    gui.frames(3, frame);
    std::map<ImGuiKeyChord, std::vector<std::string>> chords;
    for (const Action& a : app.actions().all())
        if (a.shortcut) chords[a.shortcut].push_back(a.id);
    for (const auto& [chord, owners] : chords) {
        if (owners == std::vector<std::string>{"run.start", "run.resume"}) continue;
        CHECK_MESSAGE(owners.size() == 1, shortcut_label(chord) << " is bound to " << owners.size() << " actions");
    }
    for (const char* id : {"diff.next_difference", "diff.previous_difference", "diff.toggle_bytes", "diff.verify_and_save", "session.export_markdown"})
        CHECK_MESSAGE(app.actions().find(id) != nullptr, id);
    CHECK(app.actions().find("diff.next_difference")->shortcut == ImGuiKey_F7);
    CHECK(app.actions().find("diff.previous_difference")->shortcut == (ImGuiMod_Shift | ImGuiKey_F7));
    CHECK(app.actions().find("diff.toggle_bytes")->shortcut == (ImGuiMod_Ctrl | ImGuiKey_B));
    CHECK(app.actions().find("diff.verify_and_save")->shortcut == (ImGuiMod_Ctrl | ImGuiKey_S));
}

namespace {

// A session that writes its transcript the way the runner does and holds in its first turn until
// released; queued guidance is taken (and recorded) only when the test allows it.
struct HeldSession {
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    bool take_guidance = false;
    std::atomic<int> started{0};

    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
    void allow_guidance() {
        std::lock_guard lock(mutex);
        take_guidance = true;
        cv.notify_all();
    }

    run::SessionFn fn() {
        return [this](const run::SessionRequest& r, events::EventBus& bus) {
            // Runs on a worker thread: no test assertions here, the test checks what the view reads.
            auto append = [&](const std::string& text) { (void)fs::append_text(r.transcript, text); };
            TranscriptLines t;
            t.header(r.session_id, "?add@@YAHHH@Z", "int __cdecl add(int, int)", r.va);
            t.request(1, "# Target function\nfunction: int __cdecl add(int, int)");
            (void)fs::create_directories(r.transcript.parent_path());
            append(t.text);
            const std::string relative = "sessions/" + fs::to_utf8(r.transcript.filename());
            bus.publish(events::SessionStarted{r.session_id, "?add@@YAHHH@Z", "int __cdecl add(int, int)", r.va, relative}, r.worker);
            bus.publish(events::TurnStarted{r.session_id, 1}, r.worker);
            bus.publish(events::StreamDelta{r.session_id, "thinking", "The listing adds both arguments and g_counter."}, r.worker);
            bus.publish(events::StreamDelta{r.session_id, "text", "Writing a first candidate."}, r.worker);
            ++started;
            std::unique_lock lock(mutex);
            while (!released && !r.control->abort_requested() && !r.control->stop_requested()) {
                if (take_guidance && r.control->has_injected()) {
                    TranscriptLines g;
                    for (const auto& injected : r.control->take_injected()) {
                        g.guidance(1, injected.id, injected.text);
                        bus.publish(events::Guidance{r.session_id, injected.text, injected.id}, r.worker);
                    }
                    append(g.text);
                }
                cv.wait_for(lock, 10ms);
            }
            lock.unlock();
            const std::string source = "extern int g_counter;\nint add(int a, int b) { return a + b + g_counter; }\n";
            TranscriptLines end;
            end.response(1, "", "Done.", "toolu_1", source);
            end.tool(1, "toolu_1", source, "compile: ok\nmatch 100.0% (4/4 equal) - MATCHING (byte-exact)\n\nattempt 1: best so far 100.0%");
            end.outcome("matched", 100.0, 1);
            append(end.text);
            bus.publish(events::TurnFinished{r.session_id, 1, "tool_use", events::TokenUsage{1200, 350, 800, 5200}, 0.03, 2300}, r.worker);
            bus.publish(events::DiffComputed{r.session_id, 100.0, true, "byte-exact", 1}, r.worker);
            bus.publish(events::SessionFinished{r.session_id, "matched", "", 100.0, 1, 0.03}, r.worker);
            agent::FunctionRunResult result;
            result.outcome = "matched";
            result.matched = true;
            result.turns = 1;
            return result;
        };
    }
};

} // namespace

TEST_CASE("a live session's transcript is read as it grows; guidance stays pending until it is sent") {
    FixtureProject fx;
    HeldSession held;
    Workspace::Options options;
    options.stagger = 0ms;
    options.session_override = held.fn();
    Workspace ws(std::move(options));
    REQUIRE(ws.open_project(fx.root));
    ws.wait_loaded();
    const u64 va = *ws.program()->resolve("add");
    RunRequest request;
    request.functions = {va};
    request.workers = 1;
    REQUIRE(ws.start_run(request));

    HeadlessContext gui;
    Settings settings;
    App app(ws.services(), settings);
    REQUIRE(frames_until(gui, app, &ws, [&] { return held.started == 1; }));
    const auto snapshot = ws.snapshot();
    REQUIRE(snapshot);
    const std::string session = vm::live_sessions(*snapshot).at(va)->id;

    app.context().open("agent_session", {.va = va, .session = session});
    AgentSessionControl* view = session_control(app);
    REQUIRE(view);
    REQUIRE(frames_until(gui, app, &ws, [&] { return view->transcript_loaded(); }));
    gui.frames(3, [&] { app.frame(); });
    CHECK(view->session() == session);
    CHECK(view->timeline_items() >= 3);  // brief, turn 1 (waiting for its response) and the turn being streamed

    // Guidance through the run controller: pending until the transcript records it.
    const u64 kept = view->send_guidance(app.context(), "Keep the operand order of the listing.");
    const u64 dropped = view->send_guidance(app.context(), "Never mind.");
    REQUIRE(kept != 0);
    REQUIRE(dropped != 0);
    CHECK(view->retract_guidance(app.context(), dropped));
    CHECK(view->pending_guidance() == std::vector<u64>{kept});
    held.allow_guidance();
    CHECK(frames_until(gui, app, &ws, [&] { return view->pending_guidance().empty(); }));

    // Take over: the session's worker is paused and the Diff viewer edits the function; handing back
    // queues the edited source as guidance for the same session.
    REQUIRE(app.actions().run("session.take_over"));
    DiffViewerControl* diff = diff_control(app);
    REQUIRE(frames_until(gui, app, &ws, [&] { return diff->editing() && diff->function() == va; }));
    auto has_control = [&](const std::string& command, const std::string& target_prefix) {
        const auto snap = ws.snapshot();
        return std::ranges::any_of(snap->controls, [&](const events::ControlRecord& c) {
            return c.control.command == command && c.control.target.starts_with(target_prefix);
        });
    };
    CHECK(frames_until(gui, app, &ws, [&] { return has_control("pause", "worker"); }));
    diff->set_text("extern int g_counter;\nint add(int a, int b) { return b + a + g_counter; }\n");
    diff->hand_back(app.context());
    CHECK_FALSE(diff->editing());
    CHECK(frames_until(gui, app, &ws, [&] { return has_control("inject", session); }));
    const auto injected = ws.snapshot();
    CHECK(std::ranges::any_of(injected->controls, [](const events::ControlRecord& c) {
        return c.control.command == "inject" && c.control.detail.find("return b + a + g_counter;") != std::string::npos;
    }));

    // The session ends: the response and the outcome arrive through the transcript.
    const usize before = view->timeline_items();
    held.release();
    CHECK(frames_until(gui, app, &ws, [&] { return !ws.run_live() && view->timeline_items() > before; }));
    gui.frames(5, [&] { app.frame(); });

    // The Diff viewer follows the session's function (no attempts recorded by the fake session).
    app.context().open("diff_viewer", {.va = va, .session = session});
    CHECK(frames_until(gui, app, &ws, [&] { return diff_control(app)->function() == va; }));
    gui.frames(3, [&] { app.frame(); });
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}

TEST_CASE("past runs: a missing transcript, a truncated one, and a large one that stays smooth") {
    FixtureProject fx;
    const auto program = fx.project.open_program().value();
    const u64 add = *program.resolve("add");
    const u64 sum = *program.resolve("sum_array");
    const u64 mix = *program.resolve("mix");
    PastRun past(fx.project.runs_dir());
    const std::string big = past.id + "-401060", truncated = past.id + "-401080", missing = past.id + "-4011c0";
    // The large session: hundreds of turns, megabytes of transcript.
    const std::string text = big_transcript(big, add, 400, 40);
    REQUIRE(fs::write_text(past.dir / "sessions" / "add_401060.jsonl", text));
    // Cut off in the middle of a record, with a line that is not JSON before it.
    std::string cut = big_transcript(truncated, sum, 3, 10);
    cut = cut.substr(0, cut.size() * 2 / 3) + "\nnot json\n" + cut.substr(cut.size() * 2 / 3, 40);
    REQUIRE(fs::write_text(past.dir / "sessions" / "sum_array.jsonl", cut));
    events::RunStarted started;
    started.project = "project";
    started.model = "claude-opus-5-5";
    started.effort = "high";
    started.workers = 2;
    past.publish(started, -1);
    past.publish(events::SessionStarted{big, "?add@@YAHHH@Z", "int __cdecl add(int, int)", add, "sessions/add_401060.jsonl"}, 0);
    for (int i = 1; i <= 400; ++i) past.publish(events::DiffComputed{big, 50.0 + (i % 50), false, "not matching", i}, 0);
    past.publish(events::SessionFinished{big, "gave_up", "cannot match", 99.0, 400, 12.5}, 0);
    past.publish(events::SessionStarted{truncated, "?sum_array@@YAHPBHH@Z", "int __cdecl sum_array(int const *, int)", sum, "sessions/sum_array.jsonl"}, 1);
    past.publish(events::SessionFinished{truncated, "error", "the process died", 0.0, 3, 0.1}, 1);
    past.publish(events::SessionStarted{missing, "?mix@@YANNN@Z", "double __cdecl mix(double, double)", mix, "sessions/mix.jsonl"}, 1);
    past.publish(events::SessionFinished{missing, "gave_up", "", 0.0, 1, 0.1}, 1);
    past.finish();
    std::printf("large transcript: %zu bytes, 400 turns\n", text.size());
    REQUIRE(text.size() > 2'000'000);

    Workspace::Options options;
    Workspace ws(std::move(options));
    REQUIRE(ws.open_project(fx.root));
    ws.wait_loaded();
    REQUIRE(ws.open_run(past.id));
    ws.wait_loaded();
    REQUIRE(ws.run_read_only());

    HeadlessContext gui;
    Settings settings;
    App app(ws.services(), settings);
    AgentSessionControl* view = session_control(app);
    REQUIRE(view);

    // Missing: the view says so and still renders.
    app.context().open("agent_session", {.va = mix, .session = missing});
    gui.frames(20, [&] { app.frame(); });
    CHECK(view->session() == missing);
    CHECK_FALSE(view->transcript_loaded());

    // Truncated: what was read shows, the rest is skipped.
    app.context().open("agent_session", {.va = sum, .session = truncated});
    REQUIRE(frames_until(gui, app, &ws, [&] { return view->transcript_loaded(); }));
    gui.frames(5, [&] { app.frame(); });
    CHECK(view->timeline_items() > 3);

    // Large: loaded off the UI thread, then drawn while scrolling through it.
    app.context().open("agent_session", {.va = add, .session = big});
    REQUIRE(frames_until(gui, app, &ws, [&] { return view->transcript_loaded(); }));
    gui.frames(3, [&] { app.frame(); });
    CHECK(view->timeline_items() > 400 * 5);
    CHECK(app.view_visible("agent_session"));
    // The timeline's scrolling child, to put the mouse over it and to see how far it scrolled.
    auto timeline = [] {
        for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
            if (std::string_view(w->Name).find("##timeline") != std::string_view::npos && w->WasActive) return w;
        return static_cast<ImGuiWindow*>(nullptr);
    };
    ImGuiWindow* child = timeline();
    REQUIRE(child);
    const ImVec2 over(child->Pos.x + child->Size.x * 0.5f, child->Pos.y + child->Size.y * 0.5f);
    using Clock = std::chrono::steady_clock;
    double total_ms = 0, worst_ms = 0, deepest = 0;
    const int frames = 120;
    for (int i = 0; i < frames; ++i) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(over.x, over.y);
        io.AddMouseWheelEvent(0.0f, i < 100 ? -40.0f : 150.0f);  // down through the session, then back up
        const auto t0 = Clock::now();
        gui.frame([&] { app.frame(); });
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        total_ms += ms;
        worst_ms = std::max(worst_ms, ms);
        if (ImGuiWindow* w = timeline()) deepest = std::max<double>(deepest, w->Scroll.y);
    }
    const double mean_ms = total_ms / frames;
    std::printf("Agent session with a %zu-byte transcript: %.2f ms per frame on average, %.2f ms at worst (scrolled %.0f px)\n", text.size(),
                mean_ms, worst_ms, deepest);
    CHECK(deepest > 10'000);  // the frames really scrolled through the session
#ifdef NDEBUG
    CHECK_MESSAGE(mean_ms < 5 * 8.0, "frame budget exceeded: " << mean_ms << " ms");
#endif
    // The export of the large transcript renders it whole.
    REQUIRE(app.actions().find("session.export_markdown"));
    CHECK(app.actions().run("session.export_markdown"));
    std::error_code ec;
    usize exports = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fx.root / ".decomp" / "exports", ec)) exports += entry.path().extension() == ".md";
    CHECK(exports == 1);
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}
