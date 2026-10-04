#pragma once

#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <string_view>

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

// Extra receivers (e.g. the event bus). Called for messages at or above the current level.
using Sink = std::function<void(Level, std::string_view module, std::string_view message)>;
int add_sink(Sink sink);
void remove_sink(int id);

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
