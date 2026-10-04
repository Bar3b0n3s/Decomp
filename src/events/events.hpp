#pragma once

#include "core/json.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <chrono>
#include <string>
#include <variant>
#include <vector>

namespace decomp::events {

struct TokenUsage {
    long long input = 0, output = 0, cache_write = 0, cache_read = 0;
    long long total() const { return input + output + cache_write + cache_read; }
    TokenUsage& operator+=(const TokenUsage& o) {
        input += o.input;
        output += o.output;
        cache_write += o.cache_write;
        cache_read += o.cache_read;
        return *this;
    }
};

// --- payloads -----------------------------------------------------------------------------------
// Fields added after the first slice have defaults, so older logs and positional initializers work.
struct RunStarted {
    std::string project, model, effort;
    int workers = 1;
    std::vector<std::string> functions;  // readable names of the selection
    std::vector<u64> vas = {};           // the selection's addresses
    Json config = {};                    // budgets, policies and other settings of the run
};
struct RunFinished {
    std::string status;  // completed, stopped, aborted, error
};
struct RunResumed {
    std::vector<u64> interrupted;  // functions whose sessions were cut off and are restarted
};
struct SessionStarted {
    std::string session, function, display;
    u64 va = 0;
    std::string transcript = {};  // relative to the run directory
};
struct SessionFinished {
    std::string session;
    std::string outcome;  // matched, gave_up, budget_exhausted, refused, error, stopped, aborted, ...
    std::string detail;
    double best_match = 0;
    int turns = 0;
    double cost_usd = 0;
};
struct TurnStarted {
    std::string session;
    int turn = 0;
};
struct TurnFinished {
    std::string session;
    int turn = 0;
    std::string stop_reason;
    TokenUsage usage;
    double cost_usd = 0;
    long long latency_ms = 0;
    std::string model = {};      // the model that served the turn
    long long ttft_ms = 0;       // time to the first streamed event
    bool had_fallback = false;   // a fallback model served (part of) the turn
};
struct StreamDelta {
    std::string session;
    std::string kind;  // text, thinking
    std::string text;
};
struct ToolCallStarted {
    std::string session, id, tool;
    int turn = 0;
    Json input;  // a preview: long strings are shortened (the transcript has the full input)
};
struct ToolCallFinished {
    std::string session, id, tool;
    bool is_error = false;
    std::string summary;
    long long duration_ms = 0;
};
struct CompileStarted {
    std::string session, toolchain, command;
};
struct CompileFinished {
    std::string session;
    bool ok = false;
    bool cached = false;
    long long duration_ms = 0;
    int errors = 0;
    int exit_code = -1;
    std::string command = {};
    std::string output = {};  // at most kMaxCompileOutput bytes
    std::string toolchain = {};
};
struct DiffComputed {
    std::string session;
    double match_percent = 0;
    bool byte_exact = false;
    std::string summary;
    int attempt = 0;
    int equal = 0, encoding = 0, operand = 0, opcode = 0, inserted = 0, deleted = 0;
};
struct Retry {
    std::string session;
    int attempt = 0;
    std::string error;
    long long delay_ms = 0;
    int status = 0;  // HTTP status, when the failure had one
    long long retry_after_ms = 0;
};
struct Refusal {
    std::string session, category, explanation;
};
struct Guidance {
    std::string session, text;
    u64 id = 0;  // guidance id from the controller (0 when sent directly)
};
struct StatusChanged {
    std::string function;
    u64 va = 0;
    std::string status;
    std::string old_status = {};
    double best = 0;
};
struct FileWritten {
    std::string path, reason;
    u64 size = 0;
    std::string sha1 = {};
    std::string session = {};
    std::string approval = {};  // how the write was allowed: policy, approved
};
struct LogLine {
    std::string level, message;
    std::string session = {};
};
// Phases the reducer cannot derive from other events (idle, waiting for a slot, waiting for
// approval, retiring); the reducer derives the rest.
struct WorkerPhaseChanged {
    std::string phase, session, function;
};
struct RateLimitUpdated {
    long long requests_limit = -1, requests_remaining = -1;
    long long input_tokens_limit = -1, input_tokens_remaining = -1;
    long long output_tokens_limit = -1, output_tokens_remaining = -1;
    std::string reset;        // earliest reset time reported by the API
    long long backoff_ms = 0;  // how long new requests are held back
};
struct BudgetChanged {
    std::string scope;  // run or function
    double usd = 0;     // 0 = unlimited
    long long tokens = 0;
    int turns = 0;
    int minutes = 0;
};
struct SymbolChanged {
    u64 va = 0;
    std::string old_name, new_name, kind;
    u32 size = 0;
    std::string source, session;
};
struct ApprovalRequested {
    u64 id = 0;
    std::string action, session, function;
    u64 va = 0;
    std::string path, summary;
};
struct ApprovalDecided {
    u64 id = 0;
    std::string verdict;  // approved, denied, cancelled
    std::string by;       // policy or user
    std::string reason;
};
struct QueueEntry {
    u64 va = 0;
    std::string function;
    bool pinned = false;
    double difficulty = 0;
};
struct QueueUpdated {
    std::vector<QueueEntry> items;
};
// Acknowledges a supervisor command once it has been applied.
struct Control {
    std::string command;  // pause, resume, stop, abort, skip, requeue, move, pin, concurrency, ...
    std::string target;   // worker, session or function it applied to (empty: the run)
    std::string detail;
};

inline constexpr usize kMaxCompileOutput = 4096;

using Payload = std::variant<RunStarted, RunFinished, SessionStarted, SessionFinished, TurnStarted, TurnFinished, StreamDelta,
                             ToolCallStarted, ToolCallFinished, CompileFinished, DiffComputed, Retry, Refusal, Guidance,
                             StatusChanged, FileWritten, LogLine, RunResumed, CompileStarted, WorkerPhaseChanged,
                             RateLimitUpdated, BudgetChanged, SymbolChanged, ApprovalRequested, ApprovalDecided, QueueUpdated,
                             Control>;

std::string_view type_name(const Payload& payload);

struct Event {
    u64 seq = 0;
    std::chrono::system_clock::time_point time{};
    std::string run;
    int worker = -1;
    Payload payload;
};

Json to_json(const Event& e);
Result<Event> event_from_json(const Json& j);

// "2026-10-04T01-23-45-1a2b": sortable, filesystem-safe.
std::string new_run_id();

} // namespace decomp::events
