#include "agent/client.hpp"

#include "agent/sse.hpp"
#include "core/log.hpp"
#include "core/process.hpp"
#include "core/strings.hpp"
#include "core/version.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <thread>

namespace decomp::agent {
namespace {

// Non-streaming bodies larger than this are rejected (a Messages response is far smaller).
constexpr std::size_t kMaxResponseBody = 64u * 1024 * 1024;
// Upper bound for a server-provided retry-after.
constexpr std::chrono::milliseconds kMaxRetryAfter{15 * 60 * 1000};

bool is_cancelled(const RequestOptions& options) { return options.cancelled && options.cancelled(); }

std::string_view default_error_type(int status) {
    switch (status) {
    case 400: return "invalid_request_error";
    case 401: return "authentication_error";
    case 402: return "billing_error";
    case 403: return "permission_error";
    case 404: return "not_found_error";
    case 413: return "request_too_large";
    case 429: return "rate_limit_error";
    case 500: return "api_error";
    case 529: return "overloaded_error";
    default: return "http_error";
    }
}

std::string describe_http_error(const HttpResponse& response) {
    std::string type;
    std::string message;
    if (auto body = parse_json(response.body_prefix); body && body->is_object()) {
        if (auto it = body->find("error"); it != body->end() && it->is_object()) {
            type = json_string_or(*it, "type", "");
            message = json_string_or(*it, "message", "");
        }
    }
    if (type.empty()) type = default_error_type(response.status);
    if (message.empty()) {
        const auto body = trim(response.body_prefix);
        message = body.empty() ? "(no response body)" : truncate_utf8(body, 500);
    }
    std::string text = std::format("HTTP {} {}: {}", response.status, type, message);
    if (auto request_id = response.header("request-id")) text += std::format(" (request-id {})", *request_id);
    return text;
}

// Non-negative decimal such as "3" or "1.5" (hand-rolled: floating-point from_chars is not everywhere).
std::optional<double> parse_number(std::string_view text) {
    text = trim(text);
    if (text.empty() || text.size() > 32) return std::nullopt;
    double value = 0;
    double scale = 0;  // 0 while in the integer part
    bool digits = false;
    for (char c : text) {
        if (c == '.' && scale == 0) {
            scale = 1;
        } else if (c >= '0' && c <= '9') {
            digits = true;
            if (scale == 0) {
                value = value * 10 + (c - '0');
            } else {
                scale /= 10;
                value += (c - '0') * scale;
            }
        } else {
            return std::nullopt;
        }
    }
    if (!digits) return std::nullopt;
    return value;
}

std::optional<std::chrono::milliseconds> parse_retry_after(const HttpResponse& response) {
    std::optional<double> ms;
    if (auto header = response.header("retry-after-ms")) ms = parse_number(*header);
    if (!ms) {
        if (auto header = response.header("retry-after")) {
            if (auto seconds = parse_number(*header)) ms = *seconds * 1000.0;  // HTTP-dates are not supported
        }
    }
    if (!ms) return std::nullopt;
    const double clamped = std::min(*ms, static_cast<double>(kMaxRetryAfter.count()));
    return std::chrono::milliseconds(static_cast<long long>(std::ceil(clamped)));
}

std::vector<HttpHeader> capture_headers(const std::vector<HttpHeader>& headers) {
    std::vector<HttpHeader> kept;
    for (const auto& [name, value] : headers) {
        const std::string lower = to_lower(name);
        if (lower == "request-id" || lower == "retry-after" || lower.starts_with("anthropic-ratelimit-"))
            kept.emplace_back(lower, value);
    }
    return kept;
}

std::string api_url(std::string_view base_url, std::string_view path) {
    std::string_view base = trim(base_url);
    if (base.empty()) base = kDefaultBaseUrl;
    while (base.ends_with('/')) base.remove_suffix(1);
    return std::string(base) + std::string(path);
}

// Rate limited or overloaded: the shared gate holds every session back.
bool is_throttle_status(int status) { return status == 429 || status == 529; }

} // namespace

std::string default_api_key() { return get_env("ANTHROPIC_API_KEY").value_or(""); }

std::string default_base_url() {
    auto url = get_env("ANTHROPIC_BASE_URL");
    return url && !trim(*url).empty() ? std::string(trim(*url)) : std::string(kDefaultBaseUrl);
}

bool is_retryable_status(int status) {
    return status == 408 || status == 409 || status == 429 || (status >= 500 && status <= 599);
}

Client::Client(ClientConfig config, std::shared_ptr<HttpTransport> transport, SleepFn sleep)
    : config_(std::move(config)), transport_(std::move(transport)), sleep_(std::move(sleep)), rng_(std::random_device{}()) {}

Result<Response> Client::create_message(const Json& request_body, StreamObserver* observer, const RequestOptions& options) {
    if (trim(config_.api_key).empty()) return make_error(ErrorCode::invalid_argument, "ANTHROPIC_API_KEY is not set");
    if (!transport_) return make_error(ErrorCode::invalid_argument, "no HTTP transport configured");
    if (!request_body.is_object()) return make_error(ErrorCode::invalid_argument, "request body must be a JSON object");

    const bool stream = json_bool_or(request_body, "stream", false);

    HttpRequest request;
    request.url = api_url(config_.base_url, "/v1/messages");
    request.connect_timeout = config_.connect_timeout;
    request.stall_timeout = config_.stall_timeout;
    request.headers = base_headers(stream);
    request.headers.emplace_back("content-type", "application/json");
    std::vector<std::string> betas;
    auto add_betas = [&betas](const std::vector<std::string>& list) {
        for (const auto& beta : list) {
            if (!beta.empty() && std::ranges::find(betas, beta) == betas.end()) betas.push_back(beta);
        }
    };
    add_betas(config_.betas);
    add_betas(options.betas);
    if (!betas.empty()) request.headers.emplace_back("anthropic-beta", join(betas, ","));
    request.body = dump_compact(request_body);

    for (int attempt_no = 0;; ++attempt_no) {
        if (is_cancelled(options)) return make_error(ErrorCode::cancelled, "request cancelled");
        if (config_.gate) {
            auto on_wait = [observer](std::chrono::milliseconds expected) {
                if (observer) observer->on_rate_wait(expected);
            };
            if (!config_.gate->acquire(options.cancelled, on_wait)) return make_error(ErrorCode::cancelled, "request cancelled");
        }
        auto result = attempt(request, stream, observer, options);
        if (result) return std::move(*result);

        AttemptError& failure = result.error();
        if (config_.gate && is_throttle_status(failure.status)) config_.gate->on_throttled(failure.status, failure.retry_after);
        if (!failure.retryable || attempt_no >= config_.max_retries) {
            if (failure.retryable && attempt_no > 0)
                failure.error.message += std::format(" (gave up after {} retries)", attempt_no);
            return std::unexpected(std::move(failure.error));
        }
        const auto delay = backoff_delay(attempt_no, failure.retry_after);
        log::debug("Messages API request failed ({}); retry {} of {} in {} ms", failure.error.message, attempt_no + 1,
                   config_.max_retries, delay.count());
        if (observer) observer->on_retry(RetryInfo{attempt_no + 1, failure.error, delay, failure.status, failure.retry_after});
        wait(delay, options);
    }
}

std::vector<HttpHeader> Client::base_headers(bool stream) const {
    return {
        {"x-api-key", config_.api_key},
        {"anthropic-version", config_.anthropic_version},
        {"accept", stream ? "text/event-stream" : "application/json"},
        {"user-agent", std::format("decomp/{}", kVersion)},
    };
}

Result<std::vector<ModelInfo>> Client::list_models(const RequestOptions& options) {
    if (trim(config_.api_key).empty()) return make_error(ErrorCode::invalid_argument, "ANTHROPIC_API_KEY is not set");
    if (!transport_) return make_error(ErrorCode::invalid_argument, "no HTTP transport configured");
    constexpr std::size_t kMaxBody = 16u * 1024 * 1024;
    constexpr int kMaxPages = 20;

    std::vector<ModelInfo> models;
    std::string after;
    for (int page = 0; page < kMaxPages; ++page) {
        HttpRequest request;
        request.method = "GET";
        request.url = api_url(config_.base_url, "/v1/models?limit=1000" + (after.empty() ? std::string() : "&after_id=" + after));
        request.connect_timeout = config_.connect_timeout;
        request.stall_timeout = config_.stall_timeout;
        request.headers = base_headers(false);

        Json body_json;
        for (int attempt_no = 0;; ++attempt_no) {
            if (is_cancelled(options)) return make_error(ErrorCode::cancelled, "request cancelled");
            std::string body;
            bool too_large = false;
            auto sent = transport_->send(request, [&](std::string_view chunk) {
                if (is_cancelled(options)) return false;
                if (body.size() + chunk.size() > kMaxBody) {
                    too_large = true;
                    return false;
                }
                body.append(chunk);
                return true;
            });
            if (is_cancelled(options)) return make_error(ErrorCode::cancelled, "request cancelled");
            if (too_large) return make_error(ErrorCode::api, "the model list exceeds 16 MiB");
            bool retryable = false;
            std::optional<std::chrono::milliseconds> retry_after;
            Error failure;
            if (!sent) {
                retryable = sent.error().code == ErrorCode::network;
                failure = std::move(sent.error());
            } else if (!sent->ok()) {
                retryable = is_retryable_status(sent->status);
                retry_after = parse_retry_after(*sent);
                failure = Error{ErrorCode::api, describe_http_error(*sent)};
            } else {
                auto parsed = parse_json(body);
                if (!parsed || !parsed->is_object())
                    return make_error(ErrorCode::parse, "invalid JSON in the model list: {}", parsed ? "not an object" : parsed.error().message);
                body_json = std::move(*parsed);
                break;
            }
            if (!retryable || attempt_no >= config_.max_retries) return std::unexpected(std::move(failure));
            wait(backoff_delay(attempt_no, retry_after), options);
        }

        const auto data = body_json.find("data");
        if (data == body_json.end() || !data->is_array()) return make_error(ErrorCode::parse, "the model list has no `data` array");
        for (const Json& m : *data) {
            if (!m.is_object()) continue;
            ModelInfo info{json_string_or(m, "id", ""), json_string_or(m, "display_name", ""), json_string_or(m, "created_at", "")};
            if (!info.id.empty()) models.push_back(std::move(info));
        }
        after = json_string_or(body_json, "last_id", "");
        if (!json_bool_or(body_json, "has_more", false) || after.empty()) break;
    }
    return models;
}

std::expected<Response, Client::AttemptError> Client::attempt(const HttpRequest& request, bool stream,
                                                              StreamObserver* observer, const RequestOptions& options) {
    MessageAccumulator accumulator(observer);
    std::optional<Error> stream_failure;
    SseParser parser([&](std::string_view event, std::string_view data) {
        if (stream_failure) return;
        if (auto applied = accumulator.apply(event, data); !applied) stream_failure = std::move(applied.error());
    });
    std::string body;
    bool body_too_large = false;

    auto on_data = [&](std::string_view chunk) -> bool {
        if (is_cancelled(options)) return false;
        if (chunk.empty()) return true;  // idle poll
        if (stream) {
            parser.feed(chunk);
            return !stream_failure;
        }
        if (body.size() + chunk.size() > kMaxResponseBody) {
            body_too_large = true;
            return false;
        }
        body.append(chunk);
        return true;
    };

    auto stream_error = [&]() -> AttemptError {
        if (const auto& error = accumulator.stream_error()) {
            const int status = error->type == "overloaded_error" ? 529 : error->type == "rate_limit_error" ? 429 : 0;
            return AttemptError{Error{ErrorCode::api, std::format("stream error {}: {}", error->type, error->message)},
                                error->retryable(), std::nullopt, status};
        }
        return AttemptError{*stream_failure, false, std::nullopt};
    };

    auto posted = transport_->send(request, on_data);
    // Every answer reports the rate limits, error answers included.
    if (posted && config_.gate) config_.gate->observe(posted->headers);
    if (is_cancelled(options)) return std::unexpected(AttemptError{Error{ErrorCode::cancelled, "request cancelled"}, false, {}});
    if (stream_failure) return std::unexpected(stream_error());
    if (body_too_large)
        return std::unexpected(AttemptError{Error{ErrorCode::api, "response body exceeds 64 MiB"}, false, {}});
    if (!posted) {
        // A connection that drops after message_stop still delivered a complete message.
        if (stream && posted.error().code == ErrorCode::network && accumulator.finished()) return accumulator.take();
        const bool retryable = posted.error().code == ErrorCode::network;
        return std::unexpected(AttemptError{std::move(posted.error()), retryable, {}});
    }

    HttpResponse& http = *posted;
    if (!http.ok()) {
        AttemptError failure{Error{ErrorCode::api, describe_http_error(http)}, is_retryable_status(http.status),
                             parse_retry_after(http), http.status};
        // The API can override the status-based decision.
        if (auto should_retry = http.header("x-should-retry")) {
            if (*should_retry == "true") failure.retryable = true;
            if (*should_retry == "false") failure.retryable = false;
        }
        return std::unexpected(std::move(failure));
    }

    Response response;
    if (stream) {
        parser.finish();
        if (stream_failure) return std::unexpected(stream_error());
        if (!accumulator.finished()) {
            return std::unexpected(AttemptError{
                Error{ErrorCode::network, std::format("stream ended before message_stop ({} events received)",
                                                      parser.events_dispatched())},
                true, {}});
        }
        response = accumulator.take();
    } else {
        auto json = parse_json(body);
        if (!json) return std::unexpected(AttemptError{Error{ErrorCode::parse, "invalid JSON response body: " + json.error().message}, false, {}});
        auto parsed = MessageAccumulator::from_json(*json);
        if (!parsed) return std::unexpected(AttemptError{std::move(parsed.error()), false, {}});
        response = std::move(*parsed);
    }
    response.headers = capture_headers(http.headers);
    return response;
}

std::chrono::milliseconds Client::backoff_delay(int attempt_no, std::optional<std::chrono::milliseconds> retry_after) {
    const double base = static_cast<double>(std::max<long long>(0, config_.backoff_base.count()));
    const double cap = static_cast<double>(std::max<long long>(0, config_.backoff_cap.count()));
    const double exponential = std::min(cap, base * std::pow(2.0, std::min(attempt_no, 30)));
    double factor = 1.0;
    {
        std::lock_guard lock(rng_mutex_);
        factor = std::uniform_real_distribution<double>(0.5, 1.0)(rng_);  // "equal jitter"
    }
    auto delay = std::chrono::milliseconds(static_cast<long long>(exponential * factor));
    if (retry_after) delay = std::max(delay, *retry_after);
    return delay;
}

void Client::wait(std::chrono::milliseconds delay, const RequestOptions& options) {
    if (sleep_) {
        sleep_(delay);
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + delay;
    while (!is_cancelled(options)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return;
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(deadline - now, std::chrono::milliseconds(100)));
    }
}

} // namespace decomp::agent
