#pragma once

// What the shell receives from outside instead of creating it, so that the platform main, the headless
// tests and the GUI integration (S6: project loading, RunController, RunStateStore) can each supply
// their own. Every member has a default under which the shell runs with no project and no run.

#include "agent/approvals.hpp"
#include "agent/loop.hpp"
#include "core/types.hpp"
#include "events/run_state.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::gui {

class Workspace;

// Commands from the GUI to the run (docs/ui.md#run-control). This base class does nothing: the GUI
// integration adapts the RunController behind it, and tests record calls. Commands are acknowledged
// through events (and so through the snapshot), never through return values, except where noted.
class RunCommands {
public:
    virtual ~RunCommands() = default;

    // Whether commands can reach a run: a project is open and no past run is shown read-only. When false
    // the shell disables every run control.
    virtual bool available() const { return false; }
    // A run is live: its controls (pause, stop, queue edits, steering, approvals) apply.
    virtual bool live() const { return false; }

    virtual void start() {}  // a run over the default selection
    virtual void start_functions(std::vector<u64> /*functions*/) {}  // a run over these, in this order
    virtual void pause() {}
    virtual void resume() {}
    virtual void stop() {}   // finish the current turns, then end the sessions
    virtual void abort() {}  // cancel in-flight requests now
    virtual void pause_worker(int /*worker*/) {}
    virtual void resume_worker(int /*worker*/) {}
    virtual void set_concurrency(int /*workers*/) {}
    virtual void set_run_budget(double /*usd*/) {}  // 0 = unlimited
    virtual void set_limits(const agent::LoopLimits& /*limits*/) {}
    // Queue edits; false (0) when the function is not in a state that allows it.
    virtual bool skip(u64 /*va*/) { return false; }
    virtual bool requeue(u64 /*va*/) { return false; }
    virtual usize enqueue(std::vector<u64> /*functions*/) { return 0; }
    virtual bool remove(u64 /*va*/) { return false; }
    virtual bool move(u64 /*va*/, usize /*index*/) { return false; }
    virtual bool pin(u64 /*va*/, bool /*pinned*/) { return false; }
    // Queues supervisor guidance for a session; returns its id, 0 when it was not queued.
    virtual u64 inject(std::string_view /*session*/, std::string_view /*text*/) { return 0; }
    // Withdraws guidance that has not been sent yet.
    virtual bool retract(std::string_view /*session*/, u64 /*id*/) { return false; }
    // Approval decisions and policies (Changes and approvals).
    virtual bool decide(u64 /*approval*/, bool /*approve*/, std::string_view /*reason*/) { return false; }
    virtual void set_policy(std::string_view /*action*/, agent::ApprovalPolicy /*policy*/) {}
    virtual std::vector<agent::PendingApproval> pending_approvals() const { return {}; }
    virtual int concurrency() const { return 0; }
    virtual double run_budget() const { return 0; }
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
    // The open project and run, for views that read or change them (null when the shell runs without
    // one, as in most headless tests).
    Workspace* workspace = nullptr;
};

} // namespace decomp::gui
