#pragma once

// Shared selection and back/forward history (docs/ui.md#navigation).

#include "core/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace decomp::gui {

// Where a link points inside a view.
struct NavTarget {
    std::optional<u64> va = {};  // a function (sets the shared selection)
    std::string session = {};    // an agent session (sets the shared selection)
    std::string anchor = {};     // a view-specific position, e.g. "turn:3", "seq:120", a symbol name

    bool operator==(const NavTarget&) const = default;
};

struct NavEntry {
    std::string view;  // view id
    NavTarget target;

    bool operator==(const NavEntry&) const = default;
};

// The selection that views follow (the Inspector shows selection.function_va, for instance).
struct Selection {
    std::optional<u64> function_va;
    std::string session;
};

// Back/forward history over every navigation. open() records an entry and requests it; back() and
// forward() move through the history and request the entry they land on. The App applies the latest
// request once per frame: it opens and focuses the view, updates the selection, and calls
// View::navigate() with the target.
class Navigation {
public:
    static constexpr usize kMaxEntries = 256;

    void open(std::string view, NavTarget target = {});
    bool back();
    bool forward();
    bool can_back() const { return index_ > 0; }
    bool can_forward() const { return index_ + 1 < entries_.size(); }
    const NavEntry* current() const { return entries_.empty() ? nullptr : &entries_[index_]; }
    const std::vector<NavEntry>& entries() const { return entries_; }

    // The entry to apply (once), if any.
    std::optional<NavEntry> take_request();

private:
    std::vector<NavEntry> entries_;
    usize index_ = 0;  // the current entry (when entries_ is not empty)
    std::optional<NavEntry> request_;
};

} // namespace decomp::gui
