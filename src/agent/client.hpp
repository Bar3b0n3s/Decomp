#pragma once

// Claude Messages API client over an HttpTransport: request headers, SSE streaming into a
// MessageAccumulator, and retries with exponential backoff + jitter that honor `retry-after`.

#include "agent/http.hpp"
#include "agent/messages.hpp"
#include "core/json.hpp"
#include "core/result.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::agent {

inline constexpr std::string_view kDefaultBaseUrl = "https://api.anthropic.com";

std::string default_api_key();   // $ANTHROPIC_API_KEY, or empty
std::string default_base_url();  // $ANTHROPIC_BASE_URL, or kDefaultBaseUrl

struct ClientConfig {
    std::string api_key = default_api_key();  // never logged or recorded
    std::string base_url = default_base_url();
    std::string anthropic_version = "2023-06-01";
    std::vector<std::string> betas;  // sent on every request (anthropic-beta, comma-joined)
    int max_retries = 4;
    std::chrono::milliseconds backoff_base{1000};
    std::chrono::milliseconds backoff_cap{60000};
    std::chrono::seconds connect_timeout{30};
    // A stream sends pings while the model works; a non-streaming request sends nothing until the whole
    // response is ready, so raise this for long non-streaming requests (or stream them).
    std::chrono::seconds stall_timeout{120};
};

struct RequestOptions {
    std::vector<std::string> betas;  // added to ClientConfig::betas for this request (duplicates removed)
    // Polled while the request is in flight and between retries; returning true aborts the request
    // with ErrorCode::cancelled.
    std::function<bool()> cancelled;
};

// Whether a failed request with this HTTP status is retried (408, 409, 429, 5xx incl. 529).
bool is_retryable_status(int status);

class Client {
public:
    using SleepFn = std::function<void(std::chrono::milliseconds)>;

    // `sleep` waits out retry backoff. The default sleeps for real and wakes early when the request is
    // cancelled; tests inject a recorder.
    Client(ClientConfig config, std::shared_ptr<HttpTransport> transport, SleepFn sleep = {});

    // POSTs `request_body` (serialized deterministically) to {base_url}/v1/messages. When the body has
    // "stream": true the SSE stream is parsed and `observer` sees the deltas. Retries 408/409/429/5xx,
    // network failures and overloaded_error/api_error stream events; other HTTP errors fail with
    // ErrorCode::api and the API's error type and message.
    Result<Response> create_message(const Json& request_body, StreamObserver* observer = nullptr,
                                    const RequestOptions& options = {});

    const ClientConfig& config() const { return config_; }

private:
    struct AttemptError {
        Error error;
        bool retryable = false;
        std::optional<std::chrono::milliseconds> retry_after;
    };

    std::expected<Response, AttemptError> attempt(const HttpRequest& request, bool stream, StreamObserver* observer,
                                                  const RequestOptions& options);
    std::chrono::milliseconds backoff_delay(int attempt, std::optional<std::chrono::milliseconds> retry_after);
    void wait(std::chrono::milliseconds delay, const RequestOptions& options);

    ClientConfig config_;
    std::shared_ptr<HttpTransport> transport_;
    SleepFn sleep_;
    std::mutex rng_mutex_;
    std::mt19937_64 rng_;
};

} // namespace decomp::agent
