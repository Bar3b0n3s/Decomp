#pragma once

#include "events/events.hpp"

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace decomp::events {

class EventBus;

using TimePoint = std::chrono::system_clock::time_point;

struct SessionState {
    std::string id, function, display;
    u64 va = 0;
    int worker = -1;
    bool finished = false;
    std::string outcome, detail;
    std::string phase;  // waiting for model, thinking, writing, running <tool>, compiling, backoff, done
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
    std::string stream_tail;  // last characters of streamed text (compact live views)
    std::string refusal_category;
    std::string transcript;  // relative to the run directory
    std::string model;       // model that served the last turn
    long long last_ttft_ms = 0;
    int fallback_turns = 0;
    std::string live_text, live_thinking;  // the current turn's streamed text (capped; cleared each turn)
    TimePoint started{}, ended{};
    u64 gen = 0;  // copy-on-write generation (see RunStateStore)
};

struct PhaseSpan {
    std::string phase, function;
    TimePoint start{}, end{};  // end is unset while the span is open
};

struct WorkerState {
    int id = -1;
    std::string session;  // empty when idle
    std::string phase;
    std::string function;
    TimePoint phase_since{};
    std::deque<PhaseSpan> spans;  // newest last, capped (worker timeline)
};

struct CompileRecord {
    TimePoint time{};
    std::string session, toolchain, command, output;
    bool ok = false, cached = false;
    long long duration_ms = 0;
    int exit_code = -1, errors = 0;
};

struct LogRecord {
    TimePoint time{};
    std::string level, message, session;
    int worker = -1;
};

struct ApprovalState {
    ApprovalRequested request;
    std::string verdict = "pending";  // pending, approved, denied, cancelled
    std::string by, reason;
    TimePoint requested{}, decided{};
};

struct RateLimitState {
    RateLimitUpdated last;
    TimePoint time{};
    bool known = false;
};

struct RateLimitRecord {
    TimePoint time{};
    RateLimitUpdated snapshot;
};

// One problem, grouped by kind for Logs and errors.
struct ErrorRecord {
    TimePoint time{};
    std::string kind;  // api (a retried request), tool, session, compiler, log
    std::string session;
    int worker = -1;
    std::string message;
    int status = 0;          // api: HTTP status (0: network or stream failure)
    long long delay_ms = 0;  // api: the retry's delay
    u64 seq = 0;
};

struct BudgetState {
    BudgetChanged run, function;
};

// Activity in one minute (throughput charts).
struct MinuteStats {
    int turns = 0, compiles = 0, retries = 0;
    long long output_tokens = 0;
    long long ttft_sum_ms = 0;
    int ttft_count = 0;
    double cost_usd = 0;
};

struct ControlRecord {
    TimePoint time{};
    Control control;
};

struct SymbolChangeRecord {
    TimePoint time{};
    SymbolChanged change;
};

struct FileRecord {
    TimePoint time{};
    FileWritten file;
};

struct RunStateData {
    std::string run_id, project, model, effort, status;
    int worker_count = 0;
    std::vector<std::string> planned;
    std::vector<u64> planned_vas;
    Json config;
    std::vector<u64> interrupted;  // sessions cut off before a resume
    TimePoint started{}, ended{};
    std::map<std::string, std::shared_ptr<SessionState>> sessions;
    std::map<int, WorkerState> workers;
    std::shared_ptr<const std::vector<QueueEntry>> queue = std::make_shared<const std::vector<QueueEntry>>();
    std::map<u64, ApprovalState> approvals;
    int approvals_pending = 0;
    RateLimitState rate_limit;
    BudgetState budget;
    TokenUsage usage;
    double cost_usd = 0;
    int matched = 0, finished = 0, retries = 0, refusals = 0, tool_calls = 0, compiles = 0, fallback_turns = 0;
    std::deque<std::string> activity;  // recent human-readable lines, newest last
    u64 activity_total = 0;            // lines ever added (activity keeps only the newest)
    std::deque<std::string> errors;
    std::deque<ErrorRecord> error_log;          // structured, newest last
    std::deque<RateLimitRecord> rate_history;  // rate-limit snapshots, newest last
    std::deque<FileRecord> files_written;
    std::deque<std::shared_ptr<const CompileRecord>> recent_compiles;
    std::deque<std::shared_ptr<const LogRecord>> log_tail;
    std::deque<ControlRecord> controls;
    std::deque<SymbolChangeRecord> symbol_changes;
    std::map<i64, MinuteStats> minutes;  // minutes since the epoch -> stats
    u64 last_seq = 0;

    double cache_hit_rate() const {
        long long input_total = usage.input + usage.cache_read + usage.cache_write;
        return input_total ? static_cast<double>(usage.cache_read) / static_cast<double>(input_total) : 0.0;
    }
    const SessionState* session(const std::string& id) const {
        auto it = sessions.find(id);
        return it == sessions.end() ? nullptr : it->second.get();
    }
};

// Folds events into the state every view renders. Pure: replaying a log yields the live state.
class RunState {
public:
    void apply(const Event& e);
    const RunStateData& data() const { return data_; }
    static RunState replay(const std::vector<Event>& events);
    // Marks every current session as shared with a snapshot: the next change to one clones it first.
    void freeze() { ++frozen_gen_; }

    static constexpr usize kActivityLines = 200;
    static constexpr usize kStreamTail = 600;
    static constexpr usize kLiveText = 16 * 1024;
    static constexpr usize kErrors = 50;
    static constexpr usize kLogTail = 500;
    static constexpr usize kCompiles = 200;
    static constexpr usize kSpans = 1000;
    static constexpr usize kMinutes = 1440;
    static constexpr usize kFiles = 500;
    static constexpr usize kControls = 200;
    static constexpr usize kSymbolChanges = 500;
    static constexpr usize kErrorLog = 300;
    static constexpr usize kRateHistory = 240;

private:
    void activity(const Event& e, std::string line);
    void error_line(std::string line);
    void error_record(const Event& e, std::string kind, std::string session, std::string message, int status = 0, long long delay_ms = 0);
    SessionState& session(const std::string& id);
    void set_worker_phase(const Event& e, const std::string& session_id, const std::string& phase);
    MinuteStats& minute(const Event& e);
    RunStateData data_;
    u64 frozen_gen_ = 0;
};

// The reducer behind a lock, with immutable snapshots for readers on other threads (the GUI takes one
// per frame). Snapshots share unchanged sessions with the live state (copy-on-write).
class RunStateStore {
public:
    RunStateStore() = default;
    ~RunStateStore();
    RunStateStore(const RunStateStore&) = delete;
    RunStateStore& operator=(const RunStateStore&) = delete;

    void attach(EventBus& bus);
    void detach();
    void apply(const Event& e);
    std::shared_ptr<const RunStateData> snapshot();
    u64 version() const;

private:
    mutable std::mutex mutex_;
    RunState state_;
    u64 version_ = 0, snapshot_version_ = ~u64{0};
    std::shared_ptr<const RunStateData> snapshot_;
    EventBus* bus_ = nullptr;
    int subscription_ = 0;
};

} // namespace decomp::events
