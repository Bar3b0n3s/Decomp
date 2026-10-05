#pragma once

// Helpers shared by the Dashboard, Function browser, Inspector, Binary explorer and Symbols views:
// status colors (always shown with their label), links to functions and addresses with "open in"
// context menus, access to the open project, and background jobs keyed by their inputs.

#include "analysis/program.hpp"
#include "core/log.hpp"
#include "gui/jobs.hpp"
#include "gui/view.hpp"
#include "project/project.hpp"

#include <imgui.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace decomp::gui {

class Workspace;

// ---- statuses and colors --------------------------------------------------------------------------

// "matched", "in progress", "non-matching", "gave up", ... (readable; project::to_string() is the file form).
std::string_view status_text(project::FunctionStatus status);
// A color per status for the current theme. Statuses also differ in lightness, and every use carries the
// status's name (legend, label or tooltip), so color is never the only cue.
ImVec4 status_color(const ViewContext& ctx, project::FunctionStatus status);
// A sequential, colorblind-safe ramp (viridis) for a best-match percentage 0..100.
ImVec4 best_match_color(double percent);
// status_dot() and the status's name.
void status_cell(const ViewContext& ctx, project::FunctionStatus status);

// ---- formatting -----------------------------------------------------------------------------------

std::string bytes_text(u64 bytes);  // "512 B", "1.5 KiB", "3.2 MiB"
std::string local_date_time(std::chrono::system_clock::time_point time);  // "2026-10-04 13:22"
// local_utc_offset() (days in charts start at local midnight) is in gui/widgets.hpp.

// ---- links ----------------------------------------------------------------------------------------

// The context menu items that open a function in the views that can show it: Inspector, Function
// browser, Diff viewer, Agent session, Binary explorer, Symbols. `session` selects the agent session.
void function_open_items(ViewContext& ctx, u64 va, std::string_view session = {});
// A function name as a link: a click opens the Inspector; right-click offers function_open_items().
// `label` must be unique in the current ID scope (or push an ID around it).
bool function_link(ViewContext& ctx, std::string_view label, u64 va, std::string_view session = {});
// An address as a link to the Binary explorer's hex view (the Inspector for a function's start).
bool address_link(ViewContext& ctx, std::string_view label, u64 va, bool is_function = false);

// ---- the open project -------------------------------------------------------------------------------

struct ProjectAccess {
    Workspace* workspace = nullptr;
    project::Project* project = nullptr;
    std::shared_ptr<const Program> program;

    bool ready() const { return project && program; }
};
ProjectAccess project_access(ViewContext& ctx);
// When the project is not ready, draws why (no project, loading, failed) and what the view will show.
bool require_project(ViewContext& ctx, const ProjectAccess& access, std::string_view what);
// The run the live overlays use: the snapshot while a live controller runs it (a past run is history).
const events::RunStateData* live_run(ViewContext& ctx);

// ---- background jobs ------------------------------------------------------------------------------

// A value computed in the background from inputs summarized by `Key`: update() resubmits the job when the
// key changes (latest wins), at most once per `min_interval` while it keeps changing; the previous value
// stays shown until the new one arrives.
template <class Key, class T>
class KeyedJob {
public:
    // `make` returns the job: a callable taking `const CancelToken&` (or nothing) and returning T.
    template <class F>
    void update(JobQueue& jobs, const Key& key, F&& make, std::chrono::milliseconds min_interval = std::chrono::milliseconds(0)) {
        if (requested_ && *requested_ == key) return;
        const auto now = std::chrono::steady_clock::now();
        if (requested_ && now - submitted_ < min_interval) return;
        job_.submit(jobs, make());
        requested_ = key;
        submitted_ = now;
    }
    // Takes a finished result; true when a new value arrived.
    bool poll() {
        std::optional<T> result;
        try {
            result = job_.poll();
        } catch (const std::exception& e) {
            log::warn("background job failed: {}", e.what());
            job_.cancel();
            return false;
        }
        if (!result) return false;
        value_ = std::move(*result);
        value_key_ = requested_;
        return true;
    }
    // Forgets the inputs (the next update() submits again); the value stays until replaced.
    void invalidate() { requested_.reset(); }
    void reset() {
        job_.cancel();
        requested_.reset();
        value_.reset();
        value_key_.reset();
    }
    bool busy() const { return job_.busy(); }
    // The latest job was submitted for this key (running or done).
    bool requested(const Key& key) const { return requested_ && *requested_ == key; }
    const std::optional<T>& value() const { return value_; }
    std::optional<T>& value() { return value_; }
    // The key the shown value was computed for.
    const std::optional<Key>& value_key() const { return value_key_; }
    bool current(const Key& key) const { return value_key_ && *value_key_ == key; }

private:
    LatestWins<T> job_;
    std::optional<Key> requested_, value_key_;
    std::optional<T> value_;
    std::chrono::steady_clock::time_point submitted_{};
};

// Inputs most project-derived values depend on: which project, its symbols and function states
// (Project::version()) and the program generation (Workspace::program()).
struct ProjectInputs {
    std::string root;
    u64 version = 0;
    // Compared by owner, which stays unique while this key exists (no address reuse after a free).
    std::weak_ptr<const Program> program;

    bool operator==(const ProjectInputs& o) const {
        return root == o.root && version == o.version && !program.owner_before(o.program) && !o.program.owner_before(program);
    }
};
ProjectInputs project_inputs(const ProjectAccess& access);

// A small spinner-free busy marker: "(updating)" in muted text after the previous item.
void busy_marker(const ViewContext& ctx, bool busy, std::string_view text = "updating...");

// Toolbars that wrap in narrow docks: call before every item but the first of a row with the item's
// width; the item stays on the line when it fits.
void same_line_or_wrap(float item_width);
float button_width(std::string_view label);    // a Button() with this label ("###id" suffixes ignored)
float checkbox_width(std::string_view label);  // a Checkbox() with this label

// ImGui::InputTextMultiline() for a std::string. Its frame is a child window registered under the
// field's own ID, so the field's two items are marked as an intended duplicate.
bool multiline_text(const char* id, std::string* text, ImVec2 size);

// How long a derived value may lag behind its inputs while a live run keeps changing them; without a
// live run every change is applied at once.
std::chrono::milliseconds refresh_interval(ViewContext& ctx, std::chrono::milliseconds live);

} // namespace decomp::gui
