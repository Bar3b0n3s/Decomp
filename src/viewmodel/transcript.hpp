#pragma once

// Session transcripts (.decomp/runs/<run>/sessions/<file>.jsonl, written by agent::run_function) for
// the Agent session view: an incremental reader that turns the records into turns while the file
// grows, and the readable Markdown export.
//
// The file is one JSON record per line, each with "time" (ms since the epoch) and "type": session,
// request (turn 1: the whole first request), request_delta (later turns: the messages appended since
// the previous request), response, tool, retry, guidance, paused, resumed, auto_submit and outcome.

#include "core/json.hpp"
#include "core/result.hpp"
#include "events/events.hpp"
#include "viewmodel/common.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::vm {

struct TranscriptHeader {
    std::string session, function, display, model, effort;  // model and effort as configured
    u64 va = 0;
    int worker = -1;
    TimePoint time{};
};

// What a request's new user message carried, in order, plus the pause markers recorded before it.
struct UserItem {
    enum class Kind : u8 {
        tool_result,  // a tool result as the model received it (the previous turn's ToolExchange has it too)
        guidance,     // supervisor guidance, without its "[Supervisor guidance] " prefix
        status,       // the status line, "[status] turns left: ..."
        nudge,        // the loop's reminder after a turn without tool calls
        text,         // other text
        paused,       // the session was paused here
        resumed,
    };
    Kind kind = Kind::text;
    std::string text;
    std::string tool_use_id;  // tool_result
    bool is_error = false;    // tool_result
    u64 guidance_id = 0;      // guidance sent through the run controller (0: sent directly, or unknown)
    std::optional<TimePoint> time;  // guidance, paused and resumed: when it was recorded
};

struct ResponseBlock {
    enum class Kind : u8 { thinking, redacted_thinking, text, tool_use, fallback, other };
    Kind kind = Kind::other;
    std::string text;      // thinking: the summary (empty: the model gave no summary); text; other: the block type
    std::string tool_id, tool_name;  // tool_use
    Json input;            // tool_use
    std::string from_model, to_model;  // fallback: the switch point
};

struct ToolExchange {
    std::string id, name;
    Json input;          // as the tool got it (a JSON string holding the raw text when it was not valid JSON)
    std::string result;  // the result text (content blocks other than text appear as "[<type>]")
    bool is_error = false;
    long long elapsed_ms = 0;
    TimePoint time{};
};

struct RetryRecord {
    int attempt = 0;
    std::string error;
    long long delay_ms = 0;
    int status = 0;  // HTTP status, 0 for network and stream failures
    long long retry_after_ms = 0;
    TimePoint time{};
};

struct TurnRecord {
    int turn = 0;
    std::vector<UserItem> before;  // what this request added (turn 1: what followed the brief)
    TimePoint request_time{};
    bool has_request = false;
    bool has_response = false;     // false while the model is answering (or when the request failed)
    std::vector<ResponseBlock> blocks;
    std::vector<ToolExchange> tools;   // in the order they finished
    std::vector<RetryRecord> retries;  // failed attempts of this request
    std::string response_id, model, stop_reason, request_id;  // model: the one that served the turn
    Json stop_details;
    Json usage_raw;            // the API's usage object (with "iterations" after a fallback)
    events::TokenUsage usage;  // billed tokens: the sum of the iterations when there are any
    double cost_usd = 0;
    long long latency_ms = 0, ttft_ms = 0;
    bool had_fallback = false;
    TimePoint response_time{};
};

struct TranscriptOutcome {
    std::string outcome, detail;
    double best_match = 0;
    int turns = 0;
    double cost_usd = 0;
    events::TokenUsage usage;
    TimePoint time{};
};

struct AutoSubmit {
    bool accepted = false;  // the byte-exact attempt the model never submitted was verified and saved
    std::string result;
    TimePoint time{};
};

struct TranscriptDoc {
    std::optional<TranscriptHeader> header;
    std::string brief;               // the first user message's first text block
    std::vector<TurnRecord> turns;   // ascending by turn number
    // Recorded after the last request but not part of a request yet (shown at the end while live): pause
    // markers, and guidance on its way into the next request.
    std::vector<UserItem> pending;
    std::optional<AutoSubmit> auto_submit;
    std::optional<TranscriptOutcome> outcome;
    usize records = 0;    // records read
    usize unknown = 0;    // records of a type this reader does not know (skipped)
    usize malformed = 0;  // lines that are not a JSON object, or records with unusable fields (skipped)
};

// Incremental reader. Feed it the bytes appended to the file; a last line without its newline is kept
// until the rest arrives. Records are applied as they complete, so the document grows with the file.
// Not thread-safe; parse in a background job or on the UI thread (about 1.4 ms per 100 KB of
// transcript in a Release build, measured in tests/unit/viewmodel_transcript_tests.cpp).
class TranscriptReader {
public:
    void feed(std::string_view appended);
    // The file is complete: a final line without a newline is read too.
    void finish();
    // Reads what the file gained since the last call (from offset()); a file shorter than offset() was
    // rewritten and is read again from the start. Returns whether new bytes arrived.
    Result<bool> feed_file(const std::filesystem::path& path);
    void reset();

    const TranscriptDoc& doc() const { return doc_; }
    u64 offset() const { return offset_; }    // bytes of the file consumed by feed_file()
    u64 version() const { return version_; }  // increases whenever doc() changes

private:
    enum class RequestKind : u8 {
        first,    // the first request: its first user message holds the brief
        delta,    // the messages appended since the previous request
        history,  // a whole request after the first turn
    };
    void line(std::string_view text);
    void record(const Json& j);
    TurnRecord& turn(int number);
    void request(int number, const Json& messages, RequestKind kind, TimePoint time);

    TranscriptDoc doc_;
    std::string partial_;
    u64 offset_ = 0;
    u64 version_ = 0;
};

// A whole transcript at once (a finished session).
TranscriptDoc parse_transcript(std::string_view text);

// The readable export: header, brief, then each turn's user items, response (thinking summaries as
// quotes, text, tool calls with candidate sources in ```cpp fences), tool results in plain fences, and
// the turn's stop reason, usage, cost, latency and retries; then the outcome. Fences are made longer
// than any backtick run inside them, so recorded text never breaks out.
std::string to_markdown(const TranscriptDoc& doc);

} // namespace decomp::vm
