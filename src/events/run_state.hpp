#pragma once

#include "events/events.hpp"

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace decomp::events {

struct SessionState {
    std::string id, function, display;
    u64 va = 0;
    int worker = -1;
    bool finished = false;
    std::string outcome, detail;
    std::string phase;  // waiting for model, streaming, running <tool>, compiling, backoff, done
    int turn = 0;
    int tool_calls = 0;
    int compiles = 0, compile_errors = 0;
    double best_match = 0, last_match = 0;
    bool matched = false;
    std::vector<double> scores;  // one per diff
    TokenUsage usage;
    double cost_usd = 0;
    int retries = 0;
    std::string last_tool, last_tool_summary;
    std::string stream_tail;  // last characters of streamed text (for live views)
    std::string refusal_category;
    std::chrono::system_clock::time_point started{}, ended{};
};

struct WorkerState {
    int id = -1;
    std::string session;  // empty when idle
    std::string phase;
};

struct RunStateData {
    std::string run_id, project, model, effort, status;
    int worker_count = 0;
    std::vector<std::string> planned;
    std::chrono::system_clock::time_point started{}, ended{};
    std::map<std::string, SessionState> sessions;
    std::map<int, WorkerState> workers;
    TokenUsage usage;
    double cost_usd = 0;
    int matched = 0, finished = 0, retries = 0, refusals = 0, tool_calls = 0, compiles = 0;
    std::deque<std::string> activity;  // recent human-readable lines, newest last
    u64 activity_total = 0;            // lines ever added (activity keeps only the newest)
    std::deque<std::string> errors;
    std::vector<std::string> files_written;
    u64 last_seq = 0;

    double cache_hit_rate() const {
        long long input_total = usage.input + usage.cache_read + usage.cache_write;
        return input_total ? static_cast<double>(usage.cache_read) / static_cast<double>(input_total) : 0.0;
    }
};

// Folds events into the state every view renders. Pure: replaying a log yields the live state.
class RunState {
public:
    void apply(const Event& e);
    const RunStateData& data() const { return data_; }
    static RunState replay(const std::vector<Event>& events);

    static constexpr usize kActivityLines = 200;
    static constexpr usize kStreamTail = 600;

private:
    void activity(const Event& e, std::string line);
    SessionState& session(const std::string& id);
    RunStateData data_;
};

} // namespace decomp::events
