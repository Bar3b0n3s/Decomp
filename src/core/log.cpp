#include "core/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
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

struct SinkSlot {
    Sink fn;
    std::atomic<int> active{0};
    std::atomic<bool> removed{false};
};

struct State {
    std::mutex mutex;
    std::atomic<Level> level{Level::info};
    bool color = DECOMP_ISATTY(DECOMP_FILENO(stderr)) != 0;
    bool to_stderr = true;
    std::ofstream file;
    std::map<int, std::shared_ptr<SinkSlot>> sinks;
    int next_sink = 1;
    std::deque<Entry> recent;
    usize recent_capacity = 1000;
};

thread_local int t_worker = -1;
thread_local std::string t_session;
thread_local const SinkSlot* t_running_sink = nullptr;

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
    auto slot = std::make_shared<SinkSlot>();
    slot->fn = std::move(sink);
    state().sinks.emplace(id, std::move(slot));
    return id;
}

void remove_sink(int id) {
    std::shared_ptr<SinkSlot> slot;
    {
        std::lock_guard lock(state().mutex);
        auto it = state().sinks.find(id);
        if (it == state().sinks.end()) return;
        slot = it->second;
        state().sinks.erase(it);
    }
    slot->removed = true;
    // Wait for calls already in flight on other threads (a sink removing itself does not wait).
    while (slot->active.load() > (t_running_sink == slot.get() ? 1 : 0)) std::this_thread::yield();
}

ScopedContext::ScopedContext(int worker, std::string session) : prev_worker_(t_worker), prev_session_(std::move(t_session)) {
    t_worker = worker;
    t_session = std::move(session);
}

ScopedContext::~ScopedContext() {
    t_worker = prev_worker_;
    t_session = std::move(prev_session_);
}

std::vector<Entry> recent(usize max) {
    std::lock_guard lock(state().mutex);
    const auto& r = state().recent;
    const usize n = max == 0 ? r.size() : std::min(max, r.size());
    return std::vector<Entry>(r.end() - static_cast<std::ptrdiff_t>(n), r.end());
}

void set_recent_capacity(usize capacity) {
    std::lock_guard lock(state().mutex);
    state().recent_capacity = capacity;
    while (state().recent.size() > capacity) state().recent.pop_front();
}

void write(Level level, std::string_view module, std::string_view message) {
    auto& s = state();
    std::vector<std::shared_ptr<SinkSlot>> sinks;
    Entry entry{std::chrono::system_clock::now(), level, std::string(module), std::string(message), t_worker, t_session};
    {
        std::lock_guard lock(s.mutex);
        if (enabled(level) && s.recent_capacity > 0) {
            s.recent.push_back(entry);
            if (s.recent.size() > s.recent_capacity) s.recent.pop_front();
        }
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
            for (const auto& [id, slot] : s.sinks) {
                slot->active.fetch_add(1);
                sinks.push_back(slot);
            }
    }
    // Sinks run outside the lock, so a sink may itself log (for example through event handlers).
    for (const auto& slot : sinks) {
        if (!slot->removed && t_running_sink != slot.get()) {  // no re-entrant delivery to one sink
            const SinkSlot* outer = std::exchange(t_running_sink, slot.get());
            slot->fn(entry);
            t_running_sink = outer;
        }
        slot->active.fetch_sub(1);
    }
}

} // namespace decomp::log
