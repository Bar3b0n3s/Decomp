#pragma once

// docs/ui.md "Notifications" as code: which run snapshots produce which notifications, each exactly
// once. The GUI calls NotificationRules::update() with every new snapshot (or every frame; an unchanged
// snapshot costs a few comparisons) and posts what it returns to its notification center, mapping
// NotificationLink onto its own NavEntry (view id, function address, session, anchor).
//
// Notifications are identified by keys built from what the snapshot recorded (session ids, approval
// ids, event times), so a repeated snapshot, a snapshot rebuilt by replaying the event log, or a run
// that is resumed (same run id, new sessions) never repeats one. Opening a past run should call prime()
// first, so its history does not come back as toasts.
//
// What the snapshot does not record cannot be detected: health checks run outside a run (use
// health_check_notification()). A compiler that cannot be started records no compile; it is detected
// from the tool error the session reports (RunStateData::errors), timed with the caller's `now`.

#include "events/run_state.hpp"
#include "matching/health.hpp"
#include "viewmodel/common.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace decomp::vm {

enum class Severity : u8 { info, warning, error };
std::string_view to_string(Severity severity);  // "info", "warning", "error"

// Where "Open" leads: a view id (gui/views/views.cpp: "dashboard", "agent_session", "diff_viewer",
// "function_browser", "cost", "settings", "run_monitor", "toolchains", "changes", "logs"), plus an
// optional function, session and view-specific anchor ("turn:3", "status:matched", "approval:7").
struct NotificationLink {
    std::string view;
    std::optional<u64> va;
    std::string session;
    std::string anchor;
};

struct Notification {
    std::string key;   // identity (stable across snapshots of the same run)
    std::string kind;  // matched, gave_up, refused, budget, auth, rate_limit, toolchain, run_finished, approval, fallback
    Severity severity = Severity::info;
    bool toast = true;    // false: history only
    bool sticky = false;  // the toast stays until dismissed (errors)
    std::string text;
    NotificationLink link;
    TimePoint time{};  // when it happened (event time; the newest one for a batch)
};

struct NotificationOptions {
    // Per-function notifications (matched, gave up, refused, function budget) of one kind that first
    // appear within this time of each other form a batch; a batch is posted when this long has passed
    // since its first item (or at once when the run ends). Zero: batches are what one update() finds.
    std::chrono::milliseconds batch_window{1500};
    // A batch of at least this many becomes one summary notification (fewer are posted one by one).
    usize batch_summary = 3;
    // Rate-limit storm: at least this many retries after a 429 in two consecutive minutes (the
    // snapshot counts retries per minute). A storm is notified once; another needs a minute without
    // 429s first. Storms that started more than 10 minutes before the newest activity are history.
    int storm_retries = 5;
    double budget_warning = 0.8;  // share of a budget that warns (100% is an error)
};

class NotificationRules {
public:
    explicit NotificationRules(NotificationOptions options = {});

    // Everything the snapshot shows counts as notified (opening a past run or attaching to one).
    void prime(const events::RunStateData& state);
    // The notifications due now. `now` is the caller's clock (system_clock::now()), used only to post
    // batches; detection uses the times recorded in the snapshot. O(sessions + recorded compiles, logs,
    // errors and minutes) when the snapshot changed (same run id and last_seq: nothing to scan).
    std::vector<Notification> update(const events::RunStateData& state, TimePoint now);
    // Whether a batch is waiting for its window to pass (call update() again by then).
    bool waiting() const;
    // Forgets everything (another project).
    void reset();

private:
    struct Batch {
        struct Item {
            Notification notification;
            std::string subject;  // the function, for a summary
        };
        TimePoint first_seen{};
        std::vector<Item> items;
    };
    void scan(const events::RunStateData& state, TimePoint now, bool emit, std::vector<Notification>& out);
    void offer(Notification n, bool emit, std::vector<Notification>& out);
    void offer_batched(Notification n, std::string subject, bool emit, TimePoint now);
    void flush(TimePoint now, bool all, std::vector<Notification>& out);

    NotificationOptions options_;
    std::unordered_set<std::string> seen_;
    std::map<std::string, Batch> batches_;  // by kind (+ severity class)
    std::string last_run_;
    u64 last_seq_ = 0;
    bool scanned_ = false;
};

// A failed toolchain health check (Toolchains view): sticky error opening the Toolchains view; nullopt
// when the check passed.
std::optional<Notification> health_check_notification(const std::string& toolchain, const matching::HealthReport& report, TimePoint now);

} // namespace decomp::vm
