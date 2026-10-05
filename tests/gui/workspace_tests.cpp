// The workspace (project, live and past runs) and the shell around it, with fake sessions.

#include "core/fs.hpp"
#include "gui/workspace.hpp"
#include "harness.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

using namespace decomp;
using namespace decomp::gui;
using namespace decomp::gui::test;
using namespace std::chrono_literals;

namespace {

// Sessions that finish at once (every other one matches) unless held; a held session waits until it is
// released or its run is stopped.
struct FakeSessions {
    std::mutex mutex;
    std::condition_variable cv;
    bool hold = false;
    std::atomic<int> started{0};

    void release() {
        std::lock_guard lock(mutex);
        hold = false;
        cv.notify_all();
    }

    run::SessionFn fn() {
        return [this](const run::SessionRequest& r, events::EventBus& bus) {
            bus.publish(events::SessionStarted{r.session_id, std::format("f_{:x}", r.va), std::format("f_{:x}", r.va), r.va, ""}, r.worker);
            ++started;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return !hold || r.control->stop_requested() || r.control->abort_requested(); });
            }
            agent::FunctionRunResult result;
            if (r.control->abort_requested()) result.outcome = "aborted";
            else if (r.control->stop_requested()) result.outcome = "stopped";
            else result.outcome = (r.va / 0x10) % 2 ? "gave_up" : "matched";
            result.turns = 1;
            bus.publish(events::TurnFinished{r.session_id, 1, "end_turn", events::TokenUsage{100, 10, 0, 0}, 0.01, 5}, r.worker);
            bus.publish(events::SessionFinished{r.session_id, result.outcome, "", 0, 1, 0.01}, r.worker);
            return result;
        };
    }
};

struct Fixture {
    fs::TempDir dir = fs::TempDir::create("decomp-gui-workspace").value();
    std::filesystem::path root = dir.path() / "project";
    FakeSessions sessions;
    std::atomic<int> wakes{0};
    std::unique_ptr<Workspace> workspace;

    Fixture() {
        REQUIRE(project::Project::init(root, decomp::test::fixture("x86/basic.exe"), std::nullopt, "clang-cl-x86"));
        Workspace::Options options;
        options.wake = [this] { ++wakes; };
        options.stagger = 0ms;
        options.session_override = sessions.fn();
        workspace = std::make_unique<Workspace>(std::move(options));
    }

    void open() {
        REQUIRE(workspace->open_project(root));
        workspace->wait_loaded();
        REQUIRE(workspace->project_state().phase == ProjectPhase::open);
    }

    // Polls like the UI loop until `done` (or a timeout).
    bool until(const std::function<bool()>& done) {
        for (int i = 0; i < 2000; ++i) {
            workspace->poll();
            if (done()) return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }
};

} // namespace

TEST_CASE("workspace: a project loads in the background and a run over it completes") {
    Fixture fx;
    CHECK(fx.workspace->project_state().phase == ProjectPhase::none);
    CHECK_FALSE(fx.workspace->start_run({}));  // no project yet
    fx.open();
    CHECK(fx.workspace->project_state().target == "basic.exe");
    REQUIRE(fx.workspace->target_status());
    CHECK(fx.workspace->target_status()->sha1_ok);
    REQUIRE(fx.workspace->program());

    auto services = fx.workspace->services();
    CHECK(services.workspace == fx.workspace.get());
    CHECK(services.commands->available());
    CHECK_FALSE(services.commands->live());
    CHECK(services.project().open);

    auto id = fx.workspace->start_run({});
    REQUIRE(id);
    CHECK(fx.until([&] { return !fx.workspace->run_live(); }));
    const auto snap = fx.workspace->snapshot();
    REQUIRE(snap);
    CHECK(snap->status == "completed");
    CHECK(snap->finished == 13);  // every function but the import thunk
    CHECK(fx.wakes > 0);
    CHECK(fx.workspace->run_id() == *id);
    CHECK(std::filesystem::exists(fx.workspace->run_dir() / "run.json"));
    const auto& runs = fx.workspace->runs(true);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].status == "completed");
    // The finished run stays on screen until another starts or it is closed.
    fx.workspace->close_run();
    CHECK_FALSE(fx.workspace->snapshot());
}

TEST_CASE("workspace: the code analysis runs in the background and orders the runs started here") {
    Fixture fx;
    fx.open();
    CHECK_FALSE(fx.workspace->function_analysis());
    const u64 serial = fx.workspace->analysis_serial();
    fx.workspace->poll();  // starts it for the loaded program
    fx.workspace->wait_analysis();
    const auto analysis = fx.workspace->function_analysis();
    REQUIRE(analysis);
    CHECK(fx.workspace->analysis_serial() != serial);
    CHECK_FALSE(fx.workspace->analysis_progress());
    const auto program = fx.workspace->program();
    CHECK(analysis->functions.size() == program->symbols().functions().size());
    // Polling the same program again starts nothing.
    fx.workspace->poll();
    CHECK_FALSE(fx.workspace->analysis_progress());
    CHECK(fx.workspace->function_analysis() == analysis);

    // A run over every function: easy ones first, scored by the analysis.
    fx.sessions.hold = true;
    REQUIRE(fx.workspace->start_run({}));
    const auto queue = fx.workspace->controller()->queue();
    REQUIRE(queue.size() == 13);
    CHECK(std::ranges::is_sorted(queue, {}, &run::QueueItem::difficulty));
    for (const auto& item : queue) CHECK(item.difficulty == doctest::Approx(difficulty(*analysis->find(item.va))));
    fx.workspace->end_run(true);
    fx.sessions.release();
    CHECK(fx.until([&] { return !fx.workspace->run_live(); }));

    // New symbols, a new program generation: analyzed again. Closing the project drops the analysis.
    fx.workspace->reload_symbols();
    fx.workspace->poll();
    fx.workspace->wait_analysis();
    CHECK(fx.workspace->function_analysis() != analysis);
    fx.workspace->close_run();
    fx.workspace->close_project();
    CHECK_FALSE(fx.workspace->function_analysis());
}

TEST_CASE("workspace: a stopped run reopens read-only and resumes where it stopped") {
    Fixture fx;
    fx.open();
    fx.sessions.hold = true;
    RunRequest request;
    request.workers = 2;
    auto id = fx.workspace->start_run(request);
    REQUIRE(id);
    CHECK(fx.until([&] { return fx.sessions.started == 2; }));
    CHECK_FALSE(fx.workspace->open_project(fx.root));  // not while a run is live
    CHECK_FALSE(fx.workspace->start_run({}));
    auto services = fx.workspace->services();
    CHECK(services.commands->live());
    CHECK(services.commands->concurrency() == 2);
    services.commands->stop();
    fx.sessions.release();
    CHECK(fx.until([&] { return !fx.workspace->run_live(); }));
    CHECK(fx.workspace->snapshot()->status == "stopped");

    // Reopened from its log: read-only, so the run controls are off.
    fx.workspace->close_run();
    REQUIRE(fx.workspace->open_run(*id));
    fx.workspace->wait_loaded();
    CHECK(fx.workspace->run_read_only());
    CHECK_FALSE(services.commands->available());
    REQUIRE(fx.workspace->snapshot());
    CHECK(fx.workspace->snapshot()->status == "stopped");

    // Resumed: the view keeps the whole run, and the rest of the queue runs.
    REQUIRE(fx.workspace->resume_run(*id));
    CHECK(fx.until([&] { return !fx.workspace->run_live(); }));
    const auto snap = fx.workspace->snapshot();
    CHECK(snap->status == "completed");
    CHECK(fx.workspace->runs(true).front().status == "completed");
    std::set<u64> finished;
    for (const auto& [sid, s] : snap->sessions)
        if (s->finished && s->outcome != "stopped") finished.insert(s->va);
    CHECK(finished.size() == 13);
}

TEST_CASE("workspace: runs need a matching target and an API key or replay scripts") {
    Fixture fx;
    // No session override: real sessions need a key or a replay directory.
    Workspace::Options options;
    options.stagger = 0ms;
    Workspace plain(std::move(options));
    REQUIRE(plain.open_project(fx.root));
    plain.wait_loaded();
    {
        decomp::test::ScopedEnv no_key("ANTHROPIC_API_KEY", "");
        // The client reads the key when the run configuration is made.
        auto started = plain.start_run({});
        REQUIRE_FALSE(started);
        CHECK(started.error().message.find("ANTHROPIC_API_KEY") != std::string::npos);
    }
    plain.set_replay_dir(decomp::test::source_dir() / "tests" / "replay" / "run");
    CHECK(plain.replay_dir().filename() == "run");
}

TEST_CASE("the shell drives the workspace: runs window, project dialog, quitting with a live run") {
    Fixture fx;
    fx.open();
    HeadlessContext ctx;
    Settings settings;
    App app(fx.workspace->services(), settings);
    ctx.frames(3, [&] { app.frame(); });
    CHECK(app.context().project.open);

    // The palette reaches the new actions, and their windows render.
    REQUIRE(app.actions().find("runs.show"));
    REQUIRE(app.actions().find("project.open"));
    app.actions().run("runs.show");
    app.actions().run("project.open");
    ctx.frames(3, [&] { app.frame(); });

    // A live run: quitting asks first, and waits for the run to end.
    fx.sessions.hold = true;
    app.actions().run("run.start");
    CHECK(fx.until([&] { return fx.sessions.started >= 1; }));
    ctx.frames(2, [&] { app.frame(); });
    CHECK(app.run().phase == RunPhase::running);
    app.request_quit();
    ctx.frames(2, [&] { app.frame(); });
    CHECK_FALSE(app.wants_quit());
    fx.workspace->end_run(true);
    fx.sessions.release();
    CHECK(fx.until([&] { return !fx.workspace->run_live(); }));
    ctx.frames(2, [&] { app.frame(); });
    CHECK(app.run().phase == RunPhase::finished);
    app.request_quit();
    CHECK(app.wants_quit());
    CHECK(ctx.id_conflicts() == 0);
}

TEST_CASE("every view renders with a project open and a finished run, and with a past run") {
    Fixture fx;
    fx.open();
    auto id = fx.workspace->start_run({});
    REQUIRE(id);
    REQUIRE(fx.until([&] { return !fx.workspace->run_live(); }));
    HeadlessContext ctx;
    Settings settings;
    App app(fx.workspace->services(), settings);
    auto render_all = [&](const char* label) {
        CAPTURE(label);
        for (const auto& view : all_view_ids()) {
            CAPTURE(view);
            REQUIRE(app.focus_view(view));
            ctx.frames(3, [&] { app.frame(); });
            CHECK(app.view_visible(view));
        }
        CHECK(ctx.id_conflicts() == 0);
    };
    render_all("finished live run");
    fx.workspace->close_run();
    REQUIRE(fx.workspace->open_run(*id));
    fx.workspace->wait_loaded();
    render_all("past run");
}

TEST_CASE("the shell announces what a run did once, not again when it is reopened or resumed") {
    Fixture fx;
    fx.open();
    HeadlessContext ctx;
    Settings settings;
    App app(fx.workspace->services(), settings);
    ctx.frames(2, [&] { app.frame(); });
    fx.sessions.hold = true;
    RunRequest request;
    request.workers = 2;
    auto id = fx.workspace->start_run(request);
    REQUIRE(id);
    REQUIRE(fx.until([&] { return fx.sessions.started == 2; }));
    // While the run is live, the status bar has an estimate for the queue, and the Run monitor shows
    // it per function; every Run monitor tab renders the live run.
    ctx.frames(2, [&] { app.frame(); });
    REQUIRE(app.focus_view("run_monitor"));
    for (const char* tab : {"activity", "queue", "timeline", "throughput", "rate_limits"}) {
        CAPTURE(tab);
        app.context().view_state("run_monitor")["tab"] = tab;
        ctx.frames(3, [&] { app.frame(); });
        CHECK(app.view_visible("run_monitor"));
    }
    REQUIRE(app.eta());
    CHECK(app.eta()->workers == 2);
    CHECK(app.eta()->running.size() == 2);
    CHECK(app.eta()->items.size() == 11);
    CHECK(app.eta()->finish > 0);
    fx.sessions.release();
    REQUIRE(fx.until([&] { return !fx.workspace->run_live(); }));
    ctx.frames(3, [&] { app.frame(); });
    CHECK_FALSE(app.eta());  // no live run

    auto texts = [&] {
        std::vector<std::string> out;
        for (const auto& n : app.notifications().history()) out.push_back(n.text);
        return out;
    };
    const auto announced = texts();
    auto has = [&](std::string_view part) {
        return std::ranges::any_of(announced, [&](const std::string& t) { return t.find(part) != std::string::npos; });
    };
    CHECK(has("Run completed"));
    CHECK(has("matched"));
    CHECK(has("gave up"));
    // The run summary links to the dashboard or monitor; a match batch to the matched functions.
    bool linked = false;
    for (const auto& n : app.notifications().history())
        if (n.text.find("functions matched") != std::string::npos) linked = n.link && n.link->view == "function_browser";
    CHECK(linked);

    // Reopened read-only: its history is known, so nothing is announced again.
    fx.workspace->close_run();
    REQUIRE(fx.workspace->open_run(*id));
    REQUIRE(fx.until([&] { return !fx.workspace->run_loading(); }));
    ctx.frames(3, [&] { app.frame(); });
    CHECK(texts().size() == announced.size());
    CHECK(ctx.id_conflicts() == 0);
}

TEST_CASE("cost and usage: every tab renders the project's runs, live and reopened") {
    Fixture fx;
    fx.open();
    auto id = fx.workspace->start_run({});
    REQUIRE(id);
    REQUIRE(fx.until([&] { return !fx.workspace->run_live(); }));
    HeadlessContext ctx;
    Settings settings;
    App app(fx.workspace->services(), settings);
    REQUIRE(app.focus_view("cost"));
    // Let the background jobs (run summaries, the report) finish.
    auto settle = [&] {
        for (int i = 0; i < 400; ++i) {
            ctx.frames(1, [&] { app.frame(); });
            if (app.jobs().pending() == 0) break;
            std::this_thread::sleep_for(5ms);
        }
        ctx.frames(2, [&] { app.frame(); });
    };
    settle();
    for (const char* tab : {"runs", "days", "models", "functions", "tokens", "cache", "projection"}) {
        CAPTURE(tab);
        app.context().view_state("cost")["tab"] = tab;
        ctx.frames(3, [&] { app.frame(); });
        CHECK(app.view_visible("cost"));
    }
    CHECK(json_string_or(app.context().view_state("cost"), "tab", "") == "projection");
    fx.workspace->close_run();
    REQUIRE(fx.workspace->open_run(*id));
    fx.workspace->wait_loaded();
    settle();
    CHECK(ctx.id_conflicts() == 0);
}

TEST_CASE("workspace: a project named with a trailing separator or a dot opens as its directory") {
    Fixture fx;
    REQUIRE(fx.workspace->open_project(fx.root / "."));
    fx.workspace->wait_loaded();
    CHECK(fx.workspace->project_state().phase == ProjectPhase::open);
    CHECK(fx.workspace->project_state().root.filename() == fx.root.filename());
    CHECK(fx.workspace->project_state().root == std::filesystem::absolute(fx.root).lexically_normal());
}
