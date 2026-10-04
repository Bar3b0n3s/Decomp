#pragma once

#include "events/bus.hpp"
#include "events/run_state.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>

namespace decomp::events {

// Live run view for the terminal: a redrawn status block on a TTY, plain activity lines otherwise.
class ProgressRenderer {
public:
    explicit ProgressRenderer(bool tty, std::FILE* out = stderr) : tty_(tty), out_(out) {}
    void attach(EventBus& bus);
    void detach(EventBus& bus);
    void finish();

    // Renders the status block for a state (also used by tests).
    static std::string render_block(const RunStateData& state, std::chrono::system_clock::time_point now, usize activity_lines = 4);

private:
    void on_event(const Event& e);
    void redraw(bool force);

    bool tty_;
    std::FILE* out_;
    std::mutex mutex_;
    RunState state_;
    u64 printed_activity_ = 0;
    int drawn_lines_ = 0;
    std::chrono::steady_clock::time_point last_draw_{};
    int subscription_ = 0;
};

} // namespace decomp::events
