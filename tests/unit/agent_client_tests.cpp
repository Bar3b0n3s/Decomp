#include "agent/client.hpp"
#include "agent/replay_transport.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <vector>

using namespace decomp;
using namespace decomp::agent;
using std::chrono::milliseconds;

namespace {

constexpr const char* kTestKey = "sk-ant-test-0123456789";

ClientConfig test_config() {
    ClientConfig config;
    config.api_key = kTestKey;
    config.base_url = "https://api.example.test/";
    config.betas.clear();
    return config;
}

struct Harness {
    std::shared_ptr<ReplayTransport> transport;
    std::vector<milliseconds> sleeps;
    Client client;

    explicit Harness(std::vector<Json> script, ClientConfig config = test_config(), ReplayOptions options = {})
        : transport(std::make_shared<ReplayTransport>(std::move(script), options)),
          client(std::move(config), transport, [this](milliseconds d) { sleeps.push_back(d); }) {}
};

struct RetryObserver : StreamObserver {
    struct Retry {
        int attempt;
        Error error;
        milliseconds delay;
    };
    std::vector<Retry> retries;
    std::string text;
    void on_retry(int attempt, const Error& error, milliseconds delay) override { retries.push_back({attempt, error, delay}); }
    void on_text_delta(int, std::string_view t) override { text += t; }
};

Json streaming_request(std::string text = "hi") {
    return Json{{"model", "claude-opus-5-5"},
                {"max_tokens", 1024},
                {"stream", true},
                {"messages", Json::array({{{"role", "user"}, {"content", std::move(text)}}})}};
}

Json hello_response() { return replay::message({replay::text("Hello there.")}, "end_turn", replay::usage(12, 4)); }

// Captures the headers actually handed to the transport (ReplayTransport masks the key in its records).
struct CapturingTransport : HttpTransport {
    HttpRequest last;
    ReplayTransport inner{std::vector<Json>{hello_response()}};
    Result<HttpResponse> post(const HttpRequest& request, const HttpDataCallback& on_data) override {
        last = request;
        return inner.post(request, on_data);
    }
};

} // namespace

TEST_CASE("Client streams a message and captures response headers") {
    Harness h({replay::message({replay::text("Hello there.")}, "end_turn", replay::usage(12, 4),
                               replay::MessageOptions{.id = "msg_7", .request_id = "req_007"})},
              test_config(), ReplayOptions{.max_chunk = 1});
    RetryObserver observer;
    auto r = h.client.create_message(streaming_request(), &observer);
    REQUIRE(r);
    CHECK(r->id == "msg_7");
    CHECK(r->content == Json::array({replay::text("Hello there.")}));
    CHECK(observer.text == "Hello there.");
    CHECK(r->header("request-id") == "req_007");
    CHECK_FALSE(r->header("content-type"));  // only request-id / rate-limit / retry-after are kept
    CHECK(observer.retries.empty());
    CHECK(h.sleeps.empty());
}

TEST_CASE("Client sends the API headers and a deterministic body") {
    auto capture = std::make_shared<CapturingTransport>();
    ClientConfig config = test_config();
    config.betas = {"beta-a", "beta-b"};
    Client client(config, capture, [](milliseconds) {});
    RequestOptions options;
    options.betas = {"beta-b", "server-side-fallback-2026-07-01"};
    const Json request = streaming_request("deterministic?");
    REQUIRE(client.create_message(request, nullptr, options));

    const HttpRequest& sent = capture->last;
    CHECK(sent.url == "https://api.example.test/v1/messages");
    CHECK(find_header(sent.headers, "x-api-key") == kTestKey);
    CHECK(find_header(sent.headers, "anthropic-version") == "2023-06-01");
    CHECK(find_header(sent.headers, "content-type") == "application/json");
    CHECK(find_header(sent.headers, "anthropic-beta") == "beta-a,beta-b,server-side-fallback-2026-07-01");
    CHECK(sent.body == dump_compact(request));

    // The replay recorder never keeps the key.
    auto recorded = capture->inner.requests();
    REQUIRE(recorded.size() == 1);
    CHECK(recorded[0].header("x-api-key") == "***");
    CHECK(recorded[0].raw_body.find(kTestKey) == std::string::npos);
    CHECK(recorded[0].body == request);

    // Same request -> byte-identical body; no beta header when there are no betas.
    Harness h({hello_response(), hello_response()});
    REQUIRE(h.client.create_message(request));
    REQUIRE(h.client.create_message(request));
    auto requests = h.transport->requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].raw_body == requests[1].raw_body);
    CHECK_FALSE(requests[0].header("anthropic-beta"));
    CHECK(requests[0].header("x-api-key") == "***");
}

TEST_CASE("Client retries 429 honoring retry-after") {
    Harness h({replay::http_error(429, "rate_limit_error", "Number of request tokens has exceeded your rate limit", "7"),
               hello_response()});
    RetryObserver observer;
    auto r = h.client.create_message(streaming_request(), &observer);
    REQUIRE(r);
    CHECK(observer.text == "Hello there.");
    REQUIRE(observer.retries.size() == 1);
    CHECK(observer.retries[0].attempt == 1);
    CHECK(observer.retries[0].error.code == ErrorCode::api);
    CHECK(observer.retries[0].error.message.find("rate_limit_error") != std::string::npos);
    CHECK(observer.retries[0].delay >= milliseconds(7000));
    REQUIRE(h.sleeps.size() == 1);
    CHECK(h.sleeps[0] >= milliseconds(7000));
    CHECK(h.transport->requests().size() == 2);
}

TEST_CASE("Client parses retry-after variants") {
    ClientConfig config = test_config();
    config.backoff_base = milliseconds(1);
    config.backoff_cap = milliseconds(1);
    Json fractional = replay::http_error(429, "rate_limit_error", "a", "1.5");
    Json in_ms = replay::http_error(529, "overloaded_error", "b");
    in_ms["headers"]["retry-after-ms"] = "250";
    Json garbage = replay::http_error(429, "rate_limit_error", "c", "Wed, 21 Oct 2026 07:28:00 GMT");
    Harness h({fractional, in_ms, garbage, hello_response()}, config);
    REQUIRE(h.client.create_message(streaming_request()));
    REQUIRE(h.sleeps.size() == 3);
    CHECK(h.sleeps[0] == milliseconds(1500));
    CHECK(h.sleeps[1] == milliseconds(250));
    CHECK(h.sleeps[2] <= milliseconds(1));  // HTTP-date not supported: plain backoff
}

TEST_CASE("Client retries 529 overloaded responses and overloaded stream errors") {
    Harness h({replay::http_error(529, "overloaded_error", "Overloaded"), replay::stream_error("overloaded_error", "Overloaded"),
               replay::stream_error("api_error", "Internal server error"), hello_response()});
    RetryObserver observer;
    auto r = h.client.create_message(streaming_request(), &observer);
    REQUIRE(r);
    REQUIRE(observer.retries.size() == 3);
    CHECK(observer.retries[0].error.message.find("HTTP 529 overloaded_error") != std::string::npos);
    CHECK(observer.retries[1].error.message.find("stream error overloaded_error") != std::string::npos);
    CHECK(observer.retries[2].error.message.find("api_error") != std::string::npos);
    CHECK(observer.retries[2].attempt == 3);
    CHECK(h.sleeps.size() == 3);
    // Exponential backoff with equal jitter: attempt n waits within [base*2^n/2, base*2^n].
    CHECK(h.sleeps[0] >= milliseconds(500));
    CHECK(h.sleeps[0] <= milliseconds(1000));
    CHECK(h.sleeps[1] >= milliseconds(1000));
    CHECK(h.sleeps[1] <= milliseconds(2000));
    CHECK(h.sleeps[2] >= milliseconds(2000));
    CHECK(h.sleeps[2] <= milliseconds(4000));
}

TEST_CASE("Client does not retry client errors") {
    for (int status : {400, 401, 402, 403, 404, 413}) {
        CAPTURE(status);
        Harness h({replay::http_error(status, "invalid_request_error", "messages.1.content: unexpected tool_use_id"),
                   hello_response()});
        RetryObserver observer;
        auto r = h.client.create_message(streaming_request(), &observer);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::api);
        CHECK(r.error().message.find(std::format("HTTP {} invalid_request_error", status)) != std::string::npos);
        CHECK(r.error().message.find("unexpected tool_use_id") != std::string::npos);
        CHECK(r.error().message.find("request-id") != std::string::npos);
        CHECK(observer.retries.empty());
        CHECK(h.sleeps.empty());
        CHECK(h.transport->remaining() == 1);
    }
    // A stream error that is not overloaded/api_error is not retried either.
    Harness h({replay::stream_error("invalid_request_error", "bad"), hello_response()});
    auto r = h.client.create_message(streaming_request());
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "stream error invalid_request_error: bad");
}

TEST_CASE("Client retries network errors and truncated streams") {
    Json truncated = hello_response();
    truncated["disconnect_after"] = 150;  // mid-stream reset
    Json no_stop = hello_response();
    no_stop["events"].erase(no_stop["events"].size() - 1);  // drop message_stop: the stream just ends
    Harness h({replay::network_error("Connection reset by peer"), truncated, no_stop, hello_response()});
    RetryObserver observer;
    auto r = h.client.create_message(streaming_request(), &observer);
    REQUIRE(r);
    REQUIRE(observer.retries.size() == 3);
    CHECK(observer.retries[0].error.code == ErrorCode::network);
    CHECK(observer.retries[1].error.code == ErrorCode::network);
    CHECK(observer.retries[2].error.message.find("before message_stop") != std::string::npos);
    CHECK(r->content == Json::array({replay::text("Hello there.")}));
}

TEST_CASE("Client keeps a complete message when the connection drops after message_stop") {
    Json complete_then_reset = hello_response();
    complete_then_reset["disconnect_after"] = 1 << 20;  // every byte arrives, then the connection resets
    Harness h({complete_then_reset, hello_response()});
    RetryObserver observer;
    auto r = h.client.create_message(streaming_request(), &observer);
    REQUIRE(r);
    CHECK(r->content == Json::array({replay::text("Hello there.")}));
    CHECK(observer.retries.empty());
    CHECK(h.transport->remaining() == 1);
}

TEST_CASE("Client gives up after max_retries") {
    ClientConfig config = test_config();
    config.max_retries = 2;
    config.backoff_base = milliseconds(100);
    config.backoff_cap = milliseconds(150);
    Harness h({replay::http_error(500, "api_error", "boom"), replay::http_error(500, "api_error", "boom"),
               replay::http_error(500, "api_error", "boom"), hello_response()},
              config);
    auto r = h.client.create_message(streaming_request());
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::api);
    CHECK(r.error().message.find("gave up after 2 retries") != std::string::npos);
    REQUIRE(h.sleeps.size() == 2);
    CHECK(h.sleeps[1] <= milliseconds(150));  // capped
    CHECK(h.transport->remaining() == 1);
}

TEST_CASE("Client honors x-should-retry and 408/409") {
    Json no_retry = replay::http_error(500, "api_error", "do not retry");
    no_retry["headers"]["x-should-retry"] = "false";
    Harness h({no_retry, hello_response()});
    CHECK_FALSE(h.client.create_message(streaming_request()));

    Harness h2({replay::http_error(408, "timeout_error", "t"), replay::http_error(409, "conflict", "c"), hello_response()});
    CHECK(h2.client.create_message(streaming_request()));
    CHECK(h2.sleeps.size() == 2);
}

TEST_CASE("Client reports a missing API key without sending anything") {
    ClientConfig config = test_config();
    config.api_key.clear();
    Harness h({hello_response()}, config);
    auto r = h.client.create_message(streaming_request());
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::invalid_argument);
    CHECK(r.error().message == "ANTHROPIC_API_KEY is not set");
    CHECK(h.transport->requests().empty());
}

TEST_CASE("Client handles non-streaming responses") {
    Harness h({replay::json_message({replay::text("plain")}, "end_turn", replay::usage(3, 4))});
    Json request = streaming_request();
    request["stream"] = false;
    auto r = h.client.create_message(request);
    REQUIRE(r);
    CHECK(r->content == Json::array({replay::text("plain")}));
    CHECK(r->usage == Usage{3, 4, 0, 0});
    CHECK(h.transport->requests()[0].header("accept") == "application/json");
}

TEST_CASE("Client cancellation stops the stream and is not retried") {
    Harness h({replay::message({replay::text("a long answer that keeps streaming for a while")}, "end_turn", replay::usage(1, 1),
                               replay::MessageOptions{.pieces = 8}),
               hello_response()},
              test_config(), ReplayOptions{.max_chunk = 4});
    struct CancellingObserver : StreamObserver {
        bool cancel = false;
        void on_text_delta(int, std::string_view) override { cancel = true; }
    } observer;
    RequestOptions options;
    options.cancelled = [&] { return observer.cancel; };
    auto r = h.client.create_message(streaming_request(), &observer, options);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::cancelled);
    CHECK(h.transport->remaining() == 1);
    CHECK(h.sleeps.empty());
}

TEST_CASE("ReplayTransport errors when the script runs out") {
    Harness h({});
    auto r = h.client.create_message(streaming_request());
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::internal);
    CHECK(r.error().message.find("replay script exhausted") != std::string::npos);
    CHECK(h.transport->requests().size() == 1);
}
