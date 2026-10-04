#pragma once

// Generic tool-use loop: send the conversation, check the stop reason, run the requested tools, return
// all results in one user message, repeat until the finish tool ends the session or a limit is hit.

#include "agent/client.hpp"
#include "agent/conversation.hpp"
#include "agent/cost.hpp"
#include "agent/messages.hpp"
#include "agent/tools.hpp"
#include "core/json.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::agent {

// Limits of one session. LoopControl::set_limits() replaces them while the loop runs; they are checked
// before every request and after every turn.
struct LoopLimits {
    int max_turns = 40;
    long long max_total_tokens = 0;    // 0 = unlimited; counts input + output + cache tokens of all turns
    double max_cost_usd = 0;           // 0 = unlimited
    std::chrono::seconds max_wall{0};  // 0 = unlimited

    bool operator==(const LoopLimits&) const = default;
};

// Where a running loop stands (for the status line).
struct LoopProgress {
    int turns = 0;      // requests sent so far
    LoopLimits limits;  // in effect now
    double cost_usd = 0;
    long long tokens = 0;
    std::chrono::seconds elapsed{0};

    int turns_left() const { return std::max(0, limits.max_turns - turns); }
};

struct LoopConfig {
    LoopLimits limits;
    int max_nudges = 2;              // end_turn without the finish tool -> remind this many times
    std::string finish_tool;         // e.g. "submit_result"; empty: end_turn finishes the loop
    std::string nudge_text;          // empty: a default reminder naming the finish tool
    // Appended (after the tool results) to every user message the loop sends, e.g.
    // "turns left: 12, best match: 87.5%". Called on the loop thread.
    std::function<std::string(const LoopProgress&)> status_line;
    // Spend shared by the sessions of a run: every turn's cost is added, and once the ledger's limit is
    // reached the loop ends before its next request (LoopStatus::run_budget_exhausted).
    std::shared_ptr<SpendLedger> ledger;
};

enum class LoopStatus {
    finished,                 // a tool result ended the session (or end_turn without a finish tool)
    end_turn_without_finish,  // the model stopped without calling the finish tool, nudges exhausted
    refused,                  // stop_reason "refusal"
    budget_exhausted,         // the session's tokens, cost or wall clock
    run_budget_exhausted,     // the run's shared spend ledger reached its limit
    max_turns,
    aborted,                  // LoopControl::request_abort()
    stopped,                  // LoopControl::request_stop()
    error,                    // API/transport error after retries
};

std::string_view to_string(LoopStatus status);

// Why a supervisor stopped or aborted a loop.
enum class StopReason {
    user,        // the supervisor's Stop or Abort
    skip,        // the function was skipped
    run_budget,  // the run budget ran out
    shutdown,    // the run is shutting down
};

std::string_view to_string(StopReason reason);

struct LoopOutcome {
    LoopStatus status = LoopStatus::error;
    StopReason stop_reason = StopReason::user;  // for stopped and aborted
    std::string detail;
    Json finish_outcome;  // ToolResult::outcome of the tool that ended the session
    int turns = 0;        // requests sent
    Usage usage;          // tokens billed over all turns (all fallback attempts included)
    double cost_usd = 0;
    std::optional<Error> error;
    Json stop_details;    // refusal details
};

// Supervisor guidance waiting for the next user message.
struct Injected {
    u64 id = 0;  // unique in the process
    std::string text;
};

// Thread-safe commands for a running loop. Pause, stop, limits and injected guidance take effect between
// turns; abort also cancels the request in flight.
class LoopControl {
public:
    void request_pause();
    void resume();
    void request_stop(StopReason reason = StopReason::user);   // graceful: finish the current turn, then stop
    void request_abort(StopReason reason = StopReason::user);  // cancel the in-flight request and stop
    // Supervisor guidance, delivered as "[Supervisor guidance] <text>" in the next user message. Returns
    // its id (for retract() and the guidance event).
    u64 inject(std::string text);
    // Withdraws guidance that has not been sent yet; false when it is already on its way (or unknown).
    bool retract(u64 id);
    // Replaces the limits the loop started with.
    void set_limits(const LoopLimits& limits);

    bool is_paused() const;
    bool stop_requested() const { return stop_.load(); }
    bool abort_requested() const { return abort_.load(); }
    StopReason stop_reason() const;
    std::optional<LoopLimits> limits() const;
    bool has_injected() const;
    std::vector<Injected> pending_injected() const;

    // Loop side: blocks while paused; returns false when stop or abort was requested.
    bool wait_while_paused();
    std::vector<Injected> take_injected();

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool paused_ = false;
    std::atomic<bool> stop_{false};
    std::atomic<bool> abort_{false};
    StopReason reason_ = StopReason::user;
    std::optional<LoopLimits> limits_;
    std::vector<Injected> injected_;
};

// Hooks into the loop; every hook runs on the thread that called run_loop().
class LoopObserver : public StreamObserver {
public:
    virtual void on_turn_start(int /*turn*/, const Json& /*request*/) {}
    virtual void on_response(int /*turn*/, const Response& /*response*/) {}
    virtual void on_tool_start(int /*turn*/, const ToolCall& /*call*/) {}
    virtual void on_tool_end(int /*turn*/, const ToolCall& /*call*/, const ToolResult& /*result*/,
                             std::chrono::milliseconds /*elapsed*/) {}
    virtual void on_injected(const Injected& /*guidance*/) {}
    virtual void on_paused() {}
    virtual void on_resumed() {}
    virtual void on_finish(const LoopOutcome& /*outcome*/) {}
};

LoopOutcome run_loop(Client& client, Conversation& conversation, const ToolRegistry& tools, const LoopConfig& config,
                     LoopControl* control = nullptr, LoopObserver* observer = nullptr);

} // namespace decomp::agent
