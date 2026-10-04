#pragma once

// When the Diff viewer's manual mode recompiles an edited source (docs/ui.md "Manual mode"): edits are
// debounced (a compile starts once the text has been quiet for a while), the latest edit wins (a result
// for an older edit never replaces a newer one), and the diff shown is stale while the text is newer
// than it. Each edit is a generation; the view runs the compiles (as cancellable jobs) and reports them
// here. Pure: times come from the caller.

#include "viewmodel/common.hpp"

#include <chrono>
#include <optional>

namespace decomp::vm {

class RecompileSchedule {
public:
    explicit RecompileSchedule(std::chrono::milliseconds quiet = std::chrono::milliseconds(500)) : quiet_(quiet) {}

    // The text changed at `now`.
    void edited(TimePoint now);
    // A compile should start: the latest edit is quiet long enough and neither shown nor compiling.
    bool due(TimePoint now) const;
    // When the latest edit becomes due (nullopt: nothing waits), so the UI can wake up for it.
    std::optional<TimePoint> due_at() const;
    // A compile of the latest edit starts; returns its generation. A compile still running for an older
    // generation is superseded (the caller cancels it).
    u64 start();
    // A compile ended. True when its result should be shown: it is the newest compile started and newer
    // than what is shown. False for a superseded (or cancelled) one.
    bool finished(u64 generation);
    // Starts over with text whose diff is already shown (a loaded attempt): nothing is due or stale.
    void reset();

    bool compiling() const { return compiling_ != 0; }
    // The diff shown is older than the text (an edit waits or compiles).
    bool stale() const { return shown_ != latest_; }
    u64 latest() const { return latest_; }  // the generation of the text
    u64 shown() const { return shown_; }    // the generation whose result is shown (0: none since reset)
    std::chrono::milliseconds quiet() const { return quiet_; }

private:
    std::chrono::milliseconds quiet_;
    u64 latest_ = 0;     // generation of the latest edit
    u64 shown_ = 0;      // generation whose result is shown
    u64 started_ = 0;    // newest generation a compile was started for
    u64 compiling_ = 0;  // generation compiling now (0: none)
    TimePoint last_edit_{};
};

} // namespace decomp::vm
