#pragma once

#include "gui/view.hpp"

#include <memory>
#include <optional>
#include <string>

namespace decomp::gui {

std::unique_ptr<View> make_diff_viewer_view();

// What the Diff viewer's manual mode offers besides its window (tests drive it through this; the view
// returned by make_diff_viewer_view() implements it).
class DiffViewerControl {
public:
    virtual ~DiffViewerControl() = default;

    virtual std::optional<u64> function() const = 0;
    virtual usize attempt_count() const = 0;  // attempts.jsonl of the function, once loaded
    // The one-line summary of the diff shown (empty while none is).
    virtual std::string shown_summary() const = 0;

    // Manual mode: editing starts from the shown attempt (or the best one).
    virtual bool editing() const = 0;
    virtual void edit(ViewContext& ctx) = 0;
    virtual void set_text(std::string text) = 0;  // replaces the editor's text, like typing it
    virtual std::string text() const = 0;
    // A compile of the text is due, running, or the diff shown is older than the text.
    virtual bool stale() const = 0;
    // A compile, a verification or the loading of the attempt history is running.
    virtual bool busy() const = 0;
    virtual void verify_and_save(ViewContext& ctx) = 0;
    virtual void hand_back(ViewContext& ctx) = 0;
};

} // namespace decomp::gui
