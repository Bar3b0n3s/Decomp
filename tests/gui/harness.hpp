#pragma once

// Headless ImGui for the GUI tests (docs/ui.md#testing-strategy): a fresh ImGui + ImPlot context per
// test with Dear ImGui's null backend (no window, no GPU), assertions turned into exceptions, and every
// item checked for conflicting IDs.

#include "events/run_state.hpp"
#include "gui/app.hpp"
#include "gui/assert.hpp"
#include "gui/debug.hpp"
#include "gui/services.hpp"

#include <imgui.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct ImPlotContext;

namespace decomp::gui::test {

// What the tests' assertion handler throws.
struct AssertionFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class HeadlessContext {
public:
    static constexpr float kWidth = 1600.0f;
    static constexpr float kHeight = 1000.0f;

    HeadlessContext();
    ~HeadlessContext();
    HeadlessContext(const HeadlessContext&) = delete;
    HeadlessContext& operator=(const HeadlessContext&) = delete;

    // One frame around `body`: null backend NewFrame, ImGui::NewFrame(), body, ImGui::Render(), and the
    // null renderer (which answers ImGui 1.92's texture requests).
    void frame(const std::function<void()>& body);
    void frames(int count, const std::function<void()>& body);

    // Presses `chord` (modifiers and key) during one frame and releases it in the next.
    void tap(ImGuiKeyChord chord, const std::function<void()>& body);
    // Types text into the focused text field during one frame.
    void type(const char* text, const std::function<void()>& body);

    // Conflicting IDs among all items so far, plus ImGui's own (hover-based) counter.
    int id_conflicts() const;
    std::string describe_conflicts() const;

private:
    void key(ImGuiKeyChord chord, bool down);

    ImGuiContext* imgui_ = nullptr;
    ImPlotContext* implot_ = nullptr;
    std::unique_ptr<debug::IdConflictDetector> detector_;
    AssertHandler previous_handler_ = nullptr;
};

// Synthetic snapshots, folded from events through events::RunState like a live run.
std::shared_ptr<const events::RunStateData> empty_snapshot();
std::shared_ptr<const events::RunStateData> mid_run_snapshot();
std::shared_ptr<const events::RunStateData> error_snapshot();
// A running (or other status) run with workers busy.
std::shared_ptr<const events::RunStateData> snapshot_with_status(const std::string& status);

// Records every command; available() as configured.
class RecordingCommands : public RunCommands {
public:
    explicit RecordingCommands(bool live = true) : live_(live) {}
    bool available() const override { return live_; }
    void start() override { calls.push_back("start"); }
    void pause() override { calls.push_back("pause"); }
    void resume() override { calls.push_back("resume"); }
    void stop() override { calls.push_back("stop"); }
    void abort() override { calls.push_back("abort"); }
    void pause_worker(int worker) override { calls.push_back("pause_worker " + std::to_string(worker)); }
    void resume_worker(int worker) override { calls.push_back("resume_worker " + std::to_string(worker)); }
    void set_concurrency(int workers) override { calls.push_back("set_concurrency " + std::to_string(workers)); }
    u64 inject(std::string_view session, std::string_view text) override {
        calls.push_back("inject " + std::string(session) + ": " + std::string(text));
        return calls.size();
    }

    std::vector<std::string> calls;

private:
    bool live_;
};

// Services that serve a fixed snapshot and project.
AppServices make_services(std::shared_ptr<const events::RunStateData> snapshot, std::shared_ptr<RunCommands> commands = nullptr,
                          ProjectInfo project = {});

// The ids of every view, in View-menu order.
std::vector<std::string> all_view_ids();

} // namespace decomp::gui::test
