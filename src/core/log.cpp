#include "core/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define DECOMP_ISATTY _isatty
#define DECOMP_FILENO _fileno
#else
#include <unistd.h>
#define DECOMP_ISATTY isatty
#define DECOMP_FILENO fileno
#endif

namespace decomp::log {
namespace {

struct State {
    std::mutex mutex;
    std::atomic<Level> level{Level::info};
    bool color = DECOMP_ISATTY(DECOMP_FILENO(stderr)) != 0;
    bool to_stderr = true;
    std::ofstream file;
    std::map<int, Sink> sinks;
    int next_sink = 1;
};

State& state() {
    static State s;
    return s;
}

std::string_view color_for(Level level) {
    switch (level) {
    case Level::trace: return "\x1b[90m";
    case Level::debug: return "\x1b[36m";
    case Level::info: return "";
    case Level::warn: return "\x1b[33m";
    case Level::error: return "\x1b[31m";
    case Level::off: return "";
    }
    return "";
}

} // namespace

std::string_view to_string(Level level) {
    switch (level) {
    case Level::trace: return "trace";
    case Level::debug: return "debug";
    case Level::info: return "info";
    case Level::warn: return "warn";
    case Level::error: return "error";
    case Level::off: return "off";
    }
    return "?";
}

void set_level(Level level) { state().level = level; }
Level level() { return state().level; }
bool enabled(Level l) { return l >= state().level.load() && l != Level::off; }

void set_color(bool enabled) {
    std::lock_guard lock(state().mutex);
    state().color = enabled;
}

void set_stderr(bool enabled) {
    std::lock_guard lock(state().mutex);
    state().to_stderr = enabled;
}

void set_file(const std::filesystem::path& path) {
    std::lock_guard lock(state().mutex);
    auto& s = state();
    if (s.file.is_open()) s.file.close();
    if (!path.empty()) s.file.open(path, std::ios::app);
}

int add_sink(Sink sink) {
    std::lock_guard lock(state().mutex);
    int id = state().next_sink++;
    state().sinks.emplace(id, std::move(sink));
    return id;
}

void remove_sink(int id) {
    std::lock_guard lock(state().mutex);
    state().sinks.erase(id);
}

void write(Level level, std::string_view module, std::string_view message) {
    auto& s = state();
    std::vector<Sink> sinks;
    {
        std::lock_guard lock(s.mutex);
        if (s.to_stderr && enabled(level)) {
            std::string line;
            if (s.color) line += color_for(level);
            if (level != Level::info) {
                line += to_string(level);
                line += ": ";
            }
            if (!module.empty()) {
                line += "[";
                line += module;
                line += "] ";
            }
            line += message;
            if (s.color && !color_for(level).empty()) line += "\x1b[0m";
            line += '\n';
            std::fwrite(line.data(), 1, line.size(), stderr);
        }
        if (s.file.is_open()) {
            auto now = std::chrono::system_clock::now();
            s.file << std::format("{:%Y-%m-%dT%H:%M:%S} {} {}{}\n", std::chrono::floor<std::chrono::seconds>(now),
                                  to_string(level), module.empty() ? "" : std::string(module) + ": ", message);
            s.file.flush();
        }
        if (enabled(level))
            for (const auto& [id, sink] : s.sinks) sinks.push_back(sink);
    }
    // Sinks run outside the lock, so a sink may itself log (for example through event handlers).
    for (const auto& sink : sinks) sink(level, module, message);
}

} // namespace decomp::log
