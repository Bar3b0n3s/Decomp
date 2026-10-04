#pragma once

// The notification center (docs/ui.md#notification-center): toasts in a corner and a history panel.
// Info and warning toasts dismiss themselves; errors stay until dismissed. Every notification can link to
// its source.

#include "core/types.hpp"
#include "gui/navigation.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::gui {

struct ThemeColors;

enum class Severity { info, warning, error };
std::string_view to_string(Severity severity);  // "Info", "Warning", "Error"

struct Notification {
    u64 id = 0;
    Severity severity = Severity::info;
    std::string text;
    std::optional<NavEntry> link;  // what "Open" navigates to
    std::chrono::system_clock::time_point time{};
    bool toast = true;       // false: history only
    bool dismissed = false;  // the toast was closed or has expired
    double shown_at = -1;    // ImGui time when the toast first showed (expiry counts from there)
};

class Notifications {
public:
    static constexpr double kInfoSeconds = 5.0;
    static constexpr double kWarningSeconds = 8.0;
    static constexpr usize kHistoryLimit = 500;
    static constexpr usize kMaxToasts = 5;

    // Seconds a toast of this severity stays up; nullopt: until dismissed (errors).
    static std::optional<double> lifetime(Severity severity);

    // Called after notify() from any thread (the App wakes the UI with it).
    void set_wake(std::function<void()> wake) { wake_ = std::move(wake); }

    // Thread-safe. Returns the notification's id.
    u64 notify(Severity severity, std::string text, std::optional<NavEntry> link = std::nullopt, bool toast = true);

    // The rest is for the UI thread.
    void dismiss(u64 id);
    void dismiss_all();
    void clear();
    const std::deque<Notification>& history();  // oldest first
    usize unread();
    void mark_read();
    // Toasts to show at ImGui time `now`, oldest first (at most kMaxToasts); marks expired ones dismissed.
    std::vector<const Notification*> visible_toasts(double now);
    // Seconds until the next visible toast expires; nullopt when none will.
    std::optional<double> next_expiry(double now);

    // Bottom-right corner of the main viewport's work area, above the status bar.
    void draw_toasts(Navigation& nav, const ThemeColors& colors);
    void draw_history(bool* open, Navigation& nav, const ThemeColors& colors);

private:
    void drain();  // moves posted notifications into the history

    std::mutex mutex_;
    std::vector<Notification> inbox_;
    u64 next_id_ = 1;
    std::function<void()> wake_;

    std::deque<Notification> history_;
    usize unread_ = 0;
    bool show_info_ = true, show_warnings_ = true, show_errors_ = true;
    std::string filter_;
};

} // namespace decomp::gui
