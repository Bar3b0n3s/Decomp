// The workspace (project, live and past runs) and the shell around it, with fake sessions.

#include "core/fs.hpp"
#include "gui/workspace.hpp"
#include "harness.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

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
