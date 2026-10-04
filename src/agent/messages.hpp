#pragma once

// Messages API response model: token usage, the assembled response, the streaming accumulator that
// builds it from SSE events, and the rules for echoing assistant content back into the conversation.

#include "agent/http.hpp"
#include "core/json.hpp"
#include "core/result.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace decomp::agent {

struct Usage {
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_creation_input_tokens = 0;
    long long cache_read_input_tokens = 0;

    // input + output + cache writes + cache reads.
    long long total() const {
        return input_tokens + output_tokens + cache_creation_input_tokens + cache_read_input_tokens;
    }
    // Sums `other` into this usage (accumulating over turns).
    void add(const Usage& other);
    // Overrides the fields present as integers in an API `usage` object. The streamed counts are
    // cumulative, so a later event replaces (not adds to) an earlier value.
    void merge(const Json& usage);
    static Usage from_json(const Json& usage);
    Json to_json() const;

    bool operator==(const Usage&) const = default;
};

// Bookkeeping for one content block. Kept out of the block JSON, which is echoed back verbatim.
struct BlockStatus {
    bool complete = false;    // content_block_stop received (always true for non-streaming bodies)
    bool input_valid = true;  // tool_use/server_tool_use: the input parsed strictly as a JSON object
    std::string raw_input;    // tool_use/server_tool_use: the input JSON text as received
};

struct Response {
    std::string id;
    std::string model;
    std::string stop_reason;
    Json stop_details;               // null unless the API sent one (refusals)
    Json content = Json::array();    // blocks exactly as received, `fallback` blocks included
    std::vector<BlockStatus> blocks; // parallel to `content`
    Usage usage;                     // top-level usage: the attempt that produced the message
    Json usage_raw = Json::object(); // the API's usage object merged over the stream (incl. `iterations`)
    std::vector<HttpHeader> headers; // request-id, anthropic-ratelimit-*, retry-after
    bool had_fallback = false;       // a server-side fallback switched models mid-response

    std::optional<std::string> header(std::string_view name) const { return find_header(headers, name); }
};

// The content to append as the assistant message. Blocks are passed back unchanged (thinking blocks
// keep their signatures). After a mid-output fallback, the thinking, redacted_thinking, tool_use,
// unpaired server-tool and unknown blocks that precede the last `fallback` block are omitted (text and
// paired server-tool blocks stay); `fallback` blocks themselves are dropped.
Json echo_content(const Response& response);
// Indices into response.content of the blocks echo_content() keeps, in order.
std::vector<std::size_t> echoed_block_indices(const Response& response);

// An `error` event received inside a 200 stream.
struct StreamError {
    std::string type;  // e.g. "overloaded_error"
    std::string message;

    bool retryable() const { return type == "overloaded_error" || type == "api_error"; }
};

// Live view of a streamed response. All hooks run on the thread that called Client::create_message.
class StreamObserver {
public:
    virtual ~StreamObserver() = default;

    virtual void on_message_start(const Response& /*partial*/) {}
    virtual void on_block_start(int /*index*/, const Json& /*block*/) {}
    virtual void on_text_delta(int /*index*/, std::string_view /*text*/) {}
    virtual void on_thinking_delta(int /*index*/, std::string_view /*thinking*/) {}
    virtual void on_tool_input_delta(int /*index*/, std::string_view /*partial_json*/) {}
    virtual void on_block_stop(int /*index*/, const Json& /*block*/) {}
    // A failed attempt is retried after `delay`; `attempt` counts retries from 1. Partial output already
    // reported for the failed attempt is superseded by the next one.
    virtual void on_retry(int /*attempt*/, const Error& /*error*/, std::chrono::milliseconds /*delay*/) {}
};

// Assembles a Response from Messages API stream events.
class MessageAccumulator {
public:
    explicit MessageAccumulator(StreamObserver* observer = nullptr) : observer_(observer) {}

    // Applies one SSE event. The JSON `type` field selects the handler (the SSE event name is used when
    // it is missing). Fails with ErrorCode::parse on malformed events and with ErrorCode::api on a stream
    // `error` event (details in stream_error()). Unknown event types are ignored.
    Result<void> apply(std::string_view event, std::string_view data);
    Result<void> apply(const Json& event);

    bool started() const { return started_; }    // message_start seen
    bool finished() const { return finished_; }  // message_stop seen
    const std::optional<StreamError>& stream_error() const { return error_; }
    const Response& response() const { return response_; }
    Response take() { return std::move(response_); }

    // Builds a Response from a non-streaming Messages API body.
    static Result<Response> from_json(const Json& message);

private:
    Result<void> message_start(const Json& event);
    Result<void> block_start(const Json& event);
    Result<void> block_delta(const Json& event);
    Result<void> block_stop(const Json& event);
    Result<void> message_delta(const Json& event);
    Result<std::size_t> block_index(const Json& event, std::string_view type) const;

    StreamObserver* observer_ = nullptr;
    Response response_;
    bool started_ = false;
    bool finished_ = false;
    std::optional<StreamError> error_;
};

} // namespace decomp::agent
