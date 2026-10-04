#pragma once

// What the shell receives from outside instead of creating it, so that the platform main, the headless
// tests and the GUI integration (S6: project loading, RunController, RunStateStore) can each supply
// their own. Every member has a default under which the shell runs with no project and no run.

#include "core/types.hpp"
#include "events/run_state.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace decomp::gui {

// Commands from the GUI to the run (docs/ui.md#run-control). This base class does nothing: the GUI
// integration adapts the RunController behind it, and tests record calls. Commands are acknowledged
// through events (and so through the snapshot), never through return values, except where noted.
class RunCommands {
public:
    virtual ~RunCommands() = default;

    // Whether commands reach a live run. When false (no run, or a past run opened read-only) the shell
    // disables every run control.
    virtual bool available() const { return false; }

    virtual void start() {}
    virtual void pause() {}
    virtual void resume() {}
    virtual void stop() {}   // finish the current turns, then end the sessions
    virtual void abort() {}  // cancel in-flight requests now
    virtual void pause_worker(int /*worker*/) {}
    virtual void resume_worker(int /*worker*/) {}
    virtual void set_concurrency(int /*workers*/) {}
    // Queues supervisor guidance for a session; returns its id, 0 when it was not queued.
    virtual u64 inject(std::string_view /*session*/, std::string_view /*text*/) { return 0; }
};

struct ProjectInfo {
    std::filesystem::path root;  // empty: no project
    std::string target;          // the target binary's file name, once loaded
    bool open = false;           // loaded; a root alone names a project that is not loaded (yet)
};

struct AppServices {
    // The run state to render; nullptr when there is no run. Called once per frame, on the UI thread.
    std::function<std::shared_ptr<const events::RunStateData>()> snapshot;
    // Never null once the App is constructed (defaults to the no-op RunCommands).
    std::shared_ptr<RunCommands> commands;
    // Called once per frame.
    std::function<ProjectInfo()> project;
    // Wakes the UI loop; callable from any thread (glfwPostEmptyEvent in decomp-gui).
    std::function<void()> post_empty_event;
};

} // namespace decomp::gui
