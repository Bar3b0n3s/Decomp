#pragma once

#include "core/types.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::log {

enum class Level { trace, debug, info, warn, error, off };

std::string_view to_string(Level level);

void set_level(Level level);
Level level();
bool enabled(Level level);

// Colored output on stderr (auto-detected for TTYs by default).
void set_color(bool enabled);
// Mirrors every message (regardless of level) into a file. Empty path disables.
void set_file(const std::filesystem::path& path);
// When false, nothing is printed to stderr (sinks and the log file still receive messages).
void set_stderr(bool enabled);

// One logged message. `worker` and `session` come from the writing thread's ScopedContext.
struct Entry {
    std::chrono::system_clock::time_point time;
    Level level = Level::info;
    std::string module;
    std::string message;
    int worker = -1;
    std::string session;
};

// Extra receivers (e.g. the event bus). Called for messages at or above the current level, outside
// the logger's lock. remove_sink() waits until no call of that sink is in flight, so a sink may
// capture objects that are destroyed right after it is removed.
using Sink = std::function<void(const Entry&)>;
int add_sink(Sink sink);
void remove_sink(int id);

// Attributes messages written by this thread to a worker and session while the object lives.
class ScopedContext {
public:
    ScopedContext(int worker, std::string session);
    ~ScopedContext();
    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

private:
    int prev_worker_;
    std::string prev_session_;
};

// The most recent messages (oldest first), kept in memory for views such as Logs & errors.
std::vector<Entry> recent(usize max = 0);
void set_recent_capacity(usize capacity);  // default 1000; 0 disables

void write(Level level, std::string_view module, std::string_view message);

template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::trace)) write(Level::trace, {}, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::debug)) write(Level::debug, {}, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::info)) write(Level::info, {}, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::warn)) write(Level::warn, {}, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::error)) write(Level::error, {}, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace decomp::log
