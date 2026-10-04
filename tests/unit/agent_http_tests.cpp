// Exercises the libcurl transport against a local one-shot HTTP server (no network access needed).
#ifndef _WIN32

#include "agent/http.hpp"
#include "core/strings.hpp"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <format>
#include <functional>
#include <string>
#include <thread>

using namespace decomp;
using namespace decomp::agent;
using namespace std::chrono_literals;

namespace {

struct ReceivedRequest {
    std::string head;
    std::string body;
};

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // a client that hung up must not kill the test with SIGPIPE
#else
constexpr int kSendFlags = 0;  // macOS: SO_NOSIGPIPE is set on the socket instead
#endif

void send_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::send(fd, data.data(), data.size(), kSendFlags);
        if (n <= 0) return;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
}

// Waits until the client closes the connection (or the timeout passes).
void wait_for_close(int fd, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buffer[256];
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 50) > 0) {
            if (::recv(fd, buffer, sizeof(buffer), 0) <= 0) return;
        }
    }
}

ReceivedRequest read_request(int fd) {
    ReceivedRequest request;
    std::string data;
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) return request;
        data.append(buffer, static_cast<std::size_t>(n));
    }
    request.head = data.substr(0, header_end);
    request.body = data.substr(header_end + 4);
    std::size_t length = 0;
    for (const auto& line : split_lines(request.head)) {
        const auto colon = line.find(':');
        if (colon != std::string::npos && iequals(trim(std::string_view(line).substr(0, colon)), "content-length"))
            length = static_cast<std::size_t>(std::strtoull(line.c_str() + colon + 1, nullptr, 10));
    }
    while (request.body.size() < length) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        request.body.append(buffer, static_cast<std::size_t>(n));
    }
    return request;
}

// Accepts one connection on 127.0.0.1, reads the request and hands the socket to `handler`.
class OneShotServer {
public:
    using Handler = std::function<void(int fd)>;

    explicit OneShotServer(Handler handler) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t length = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &length);
        port_ = ntohs(addr.sin_port);
        ::listen(listen_fd_, 1);
        thread_ = std::thread([this, handler = std::move(handler)] {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) return;
#ifdef SO_NOSIGPIPE
            int no_sigpipe = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
            received_ = read_request(fd);
            handler(fd);
            ::close(fd);
        });
    }

    OneShotServer(const OneShotServer&) = delete;
    OneShotServer& operator=(const OneShotServer&) = delete;

    ~OneShotServer() {
        ::shutdown(listen_fd_, SHUT_RDWR);  // unblocks accept() when no client came
        join();
        ::close(listen_fd_);
    }

    std::string url() const { return std::format("http://127.0.0.1:{}/v1/messages", port_); }

    // Valid after the exchange; joins the server thread first.
    const ReceivedRequest& received() {
        join();
        return received_;
    }

private:
    void join() {
        if (thread_.joinable()) thread_.join();
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::thread thread_;
    ReceivedRequest received_;
};

HttpRequest make_request(const std::string& url) {
    HttpRequest request;
    request.url = url;
    request.headers = {{"x-api-key", "local-secret"}, {"content-type", "application/json"}, {"anthropic-version", "2023-06-01"}};
    request.body = R"({"hello":1})";
    request.connect_timeout = std::chrono::seconds(5);
    request.stall_timeout = std::chrono::seconds(10);
    return request;
}

} // namespace

TEST_CASE("curl transport streams a 2xx body and captures headers") {
    OneShotServer server([](int fd) {
        send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nRequest-Id: req_local\r\n"
                     "Anthropic-RateLimit-Requests-Remaining: 49\r\nConnection: close\r\n\r\n");
        for (const char* piece : {"event: ping\n", "data: {\"type\":", "\"ping\"}\n\n"}) {
            send_all(fd, piece);
            std::this_thread::sleep_for(20ms);
        }
    });
    auto transport = make_default_transport();
    std::string body;
    int chunks = 0;
    auto response = transport->post(make_request(server.url()), [&](std::string_view chunk) {
        if (!chunk.empty()) ++chunks;
        body += chunk;
        return true;
    });
    REQUIRE(response);
    CHECK(response->status == 200);
    CHECK(response->ok());
    CHECK(response->header("request-id") == "req_local");
    CHECK(response->header("Anthropic-RateLimit-Requests-Remaining") == "49");
    bool lower_case = true;
    for (const auto& [name, value] : response->headers) lower_case = lower_case && name == to_lower(name);
    CHECK(lower_case);
    CHECK(body == "event: ping\ndata: {\"type\":\"ping\"}\n\n");
    CHECK(chunks >= 2);
    CHECK(response->body_prefix.empty());

    const ReceivedRequest& received = server.received();
    CHECK(received.head.starts_with("POST /v1/messages HTTP/1.1\r\n"));
    CHECK(received.head.find("x-api-key: local-secret") != std::string::npos);
    CHECK(received.head.find("anthropic-version: 2023-06-01") != std::string::npos);
    CHECK(received.head.find("Expect:") == std::string::npos);
    CHECK(received.body == R"({"hello":1})");
}

TEST_CASE("curl transport returns non-2xx bodies in body_prefix") {
    const std::string error_body =
        R"({"type":"error","error":{"type":"rate_limit_error","message":"slow down"},"request_id":"req_x"})";
    OneShotServer server([&](int fd) {
        send_all(fd, std::format("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 3\r\nContent-Type: application/json\r\n"
                                 "Content-Length: {}\r\n\r\n{}",
                                 error_body.size(), error_body));
    });
    auto transport = make_default_transport();
    std::string body;
    auto response = transport->post(make_request(server.url()), [&](std::string_view chunk) {
        body += chunk;
        return true;
    });
    REQUIRE(response);
    CHECK(response->status == 429);
    CHECK(response->header("retry-after") == "3");
    CHECK(response->body_prefix == error_body);
    CHECK(body.empty());
}

TEST_CASE("curl transport: on_data returning false cancels the transfer") {
    OneShotServer server([](int fd) {
        send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\nevent: ping\n");
        std::this_thread::sleep_for(50ms);
        send_all(fd, "data: {}\n\n");
        wait_for_close(fd, 5000);
    });
    auto transport = make_default_transport();
    auto response = transport->post(make_request(server.url()), [](std::string_view chunk) { return chunk.empty(); });
    REQUIRE_FALSE(response);
    CHECK(response.error().code == ErrorCode::cancelled);
}

TEST_CASE("curl transport: idle polls let the caller cancel a silent stream") {
    OneShotServer server([](int fd) {
        send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
        wait_for_close(fd, 10000);  // never sends a byte of body
    });
    auto transport = make_default_transport();
    int polls = 0;
    std::size_t bytes = 0;
    const auto start = std::chrono::steady_clock::now();
    // Cancel once the stream has been silent for a while: only an idle poll can deliver that decision.
    auto response = transport->post(make_request(server.url()), [&](std::string_view chunk) {
        bytes += chunk.size();
        if (chunk.empty()) ++polls;
        return std::chrono::steady_clock::now() - start < 400ms;
    });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(response);
    CHECK(response.error().code == ErrorCode::cancelled);
    CHECK(bytes == 0);
    CHECK(polls > 0);
    CHECK(elapsed >= 400ms);
    CHECK(elapsed < 3s);
}

TEST_CASE("curl transport: a stalled stream fails with a network error") {
    OneShotServer server([](int fd) {
        send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\nevent: ping\n");
        wait_for_close(fd, 10000);  // then silence
    });
    auto transport = make_default_transport();
    HttpRequest request = make_request(server.url());
    request.stall_timeout = std::chrono::seconds(1);
    std::string body;
    const auto start = std::chrono::steady_clock::now();
    auto response = transport->post(request, [&](std::string_view chunk) {
        body += chunk;
        return true;
    });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(response);
    CHECK(response.error().code == ErrorCode::network);
    CHECK(response.error().message.find("stalled") != std::string::npos);
    CHECK(body == "event: ping\n");
    CHECK(elapsed >= 1s);
    CHECK(elapsed < 4s);
}

TEST_CASE("curl transport: connection refused is a network error") {
    // Reserve a port, then close it so nothing listens there.
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t length = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length);
    const int port = ntohs(addr.sin_port);
    ::close(fd);

    auto transport = make_default_transport();
    auto response = transport->post(make_request(std::format("http://127.0.0.1:{}/v1/messages", port)),
                                    [](std::string_view) { return true; });
    REQUIRE_FALSE(response);
    CHECK(response.error().code == ErrorCode::network);
    CHECK(response.error().message.find("127.0.0.1") != std::string::npos);
}

#endif // !_WIN32
