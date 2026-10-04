#pragma once

// Scripted HttpTransport for tests and the CLI `--replay` flag, plus builders for scripted responses.

#include "agent/http.hpp"
#include "analysis/symbols.hpp"
#include "core/json.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::agent {

struct RecordedRequest {
    std::string method;
    std::string url;
    std::vector<HttpHeader> headers;  // x-api-key and authorization values are masked as "***"
    std::string raw_body;
    Json body;  // parsed raw_body (null when it was not JSON)

    std::optional<std::string> header(std::string_view name) const { return find_header(headers, name); }
};

struct ReplayOptions {
    // 2xx bodies are delivered in pseudo-random chunks of 1..max_chunk bytes (deterministic per seed) to
    // exercise stream parsing across arbitrary boundaries.
    std::size_t max_chunk = 61;
    std::uint64_t seed = 0x5EED;
    bool fixed_chunks = false;  // exactly max_chunk bytes per chunk instead
};

// Each post() consumes the next scripted response (one JSON object per JSONL line):
//   {"status":200,"headers":{"request-id":"req_1"},"events":[{"event":"message_start","data":{...}},...]}
//       -> the events serialized as SSE ("crlf": true for \r\n line endings)
//   {"status":200,"sse":"event: ping\ndata: {...}\n\n"}  -> raw SSE text
//   {"status":429,"headers":{"retry-after":"0"},"body":{...}}  -> a plain body (JSON value or string)
//   {"network_error":"connection reset"}  -> fails with ErrorCode::network
// "disconnect_after": N delivers only the first N body bytes and then fails with a network error.
// Every request is recorded. When the script is exhausted post() fails with ErrorCode::internal.
class ReplayTransport : public HttpTransport {
public:
    explicit ReplayTransport(std::vector<Json> script = {}, ReplayOptions options = {});

    // Loads a JSONL script; blank lines and lines starting with '#' are skipped.
    static Result<std::shared_ptr<ReplayTransport>> load(const std::filesystem::path& path, ReplayOptions options = {});
    static Result<std::vector<Json>> parse_script(std::string_view jsonl);

    void push(Json response);
    Result<HttpResponse> send(const HttpRequest& request, const HttpDataCallback& on_data) override;

    std::vector<RecordedRequest> requests() const;
    std::size_t remaining() const;

private:
    std::size_t next_chunk_size();

    mutable std::mutex mutex_;
    std::deque<Json> script_;
    std::vector<RecordedRequest> requests_;
    ReplayOptions options_;
    std::uint64_t rng_state_;
};

// Serializes [{"event": name, "data": json}, ...] as SSE text.
std::string to_sse(const Json& events, bool crlf = false);

// The script for one function in a replay directory (a scripted multi-function run): the first that
// exists of "<safe name>.jsonl" (e.g. "Player__Hit_401000.jsonl"), "<safe name without the address>.jsonl"
// ("Player__Hit.jsonl") and "default.jsonl".
std::optional<std::filesystem::path> find_replay_script(const std::filesystem::path& dir, const Symbol& fn);

// Builders for scripted responses.
namespace replay {

Json text(std::string value);
Json thinking(std::string value, std::string signature);
Json redacted_thinking(std::string data);
Json tool_use(std::string id, std::string name, Json input);
// A tool_use whose streamed input is exactly `raw_input` (e.g. deliberately invalid JSON).
Json tool_use_raw(std::string id, std::string name, std::string raw_input);
Json fallback(std::string from_model, std::string to_model);
Json usage(long long input_tokens, long long output_tokens, long long cache_creation_input_tokens = 0,
           long long cache_read_input_tokens = 0);

struct MessageOptions {
    std::string id = "msg_replay";
    std::string model = "claude-opus-5-5";
    std::string request_id = {};  // response header; empty: "req_" + id
    Json stop_details = nullptr;  // included in message_delta when not null
    int pieces = 3;          // deltas per text, thinking or tool input
    bool crlf = false;
};

// The complete stream event sequence for a message: message_start, ping, then for every block
// content_block_start, its deltas (text and thinking split into several deltas, a signature_delta,
// tool input JSON split mid-token into input_json_delta fragments) and content_block_stop, then
// message_delta (stop_reason, stop_details, usage) and message_stop.
Json events(const std::vector<Json>& content_blocks, std::string_view stop_reason, const Json& usage_json,
            const MessageOptions& options = {});
// A scripted 200 streaming response carrying events(...).
Json message(const std::vector<Json>& content_blocks, std::string_view stop_reason, const Json& usage_json,
             const MessageOptions& options = {});
// A refusal (stop_reason "refusal" with stop_details) after optional partial content.
Json refusal(const Json& stop_details, const std::vector<Json>& partial_content = {}, const Json& usage_json = usage(100, 5),
             const MessageOptions& options = {});
// A non-2xx response with the API's error body.
Json http_error(int status, std::string_view type, std::string_view message,
                std::optional<std::string> retry_after = std::nullopt);
// A 200 stream that fails with an `error` event after message_start.
Json stream_error(std::string_view type, std::string_view message);
Json network_error(std::string_view message);
// A non-streaming 200 response body for the given message.
Json json_message(const std::vector<Json>& content_blocks, std::string_view stop_reason, const Json& usage_json,
                  const MessageOptions& options = {});

} // namespace replay

} // namespace decomp::agent
