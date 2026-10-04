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
struct RunStarted {
    std::string project, model, effort;
    int workers = 1;
    std::vector<std::string> functions;
};
struct RunFinished {
    std::string status;  // completed, stopped, aborted, error
};
struct SessionStarted {
    std::string session, function, display;
    u64 va = 0;
};
struct SessionFinished {
    std::string session;
    std::string outcome;  // matched, gave_up, budget_exhausted, refused, error, stopped, aborted
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
};
struct StreamDelta {
    std::string session;
    std::string kind;  // text, thinking
    std::string text;
};
struct ToolCallStarted {
    std::string session, id, tool;
    int turn = 0;
    Json input;
};
struct ToolCallFinished {
    std::string session, id, tool;
    bool is_error = false;
    std::string summary;
    long long duration_ms = 0;
};
struct CompileFinished {
    std::string session;
    bool ok = false;
    bool cached = false;
    long long duration_ms = 0;
    int errors = 0;
};
struct DiffComputed {
    std::string session;
    double match_percent = 0;
    bool byte_exact = false;
    std::string summary;
};
struct Retry {
    std::string session;
    int attempt = 0;
    std::string error;
    long long delay_ms = 0;
};
struct Refusal {
    std::string session, category, explanation;
};
struct Guidance {
    std::string session, text;
};
struct StatusChanged {
    std::string function;
    u64 va = 0;
    std::string status;
};
struct FileWritten {
    std::string path, reason;
};
struct LogLine {
    std::string level, message;
};

using Payload = std::variant<RunStarted, RunFinished, SessionStarted, SessionFinished, TurnStarted, TurnFinished, StreamDelta,
                             ToolCallStarted, ToolCallFinished, CompileFinished, DiffComputed, Retry, Refusal, Guidance,
                             StatusChanged, FileWritten, LogLine>;

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
