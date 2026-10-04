#pragma once

#include "core/result.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace decomp::agent {

using HttpHeader = std::pair<std::string, std::string>;

// Case-insensitive header lookup (first match).
std::optional<std::string> find_header(const std::vector<HttpHeader>& headers, std::string_view name);

struct HttpRequest {
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body;
    std::chrono::seconds connect_timeout{30};
    // The transfer fails with ErrorCode::network when no byte arrives for this long.
    std::chrono::seconds stall_timeout{120};
};

// Non-2xx bodies are captured into HttpResponse::body_prefix up to this many bytes.
inline constexpr std::size_t kMaxErrorBodyPrefix = 64 * 1024;

struct HttpResponse {
    int status = 0;
    std::vector<HttpHeader> headers;  // names lower-cased, in arrival order
    std::string body_prefix;          // first <= 64 KiB of a non-2xx body (2xx bodies go to on_data)

    std::optional<std::string> header(std::string_view name) const { return find_header(headers, name); }
    bool ok() const { return status >= 200 && status < 300; }
};

// Receives the body of a 2xx response as it arrives. Returning false aborts the transfer, and post()
// then fails with ErrorCode::cancelled. Transports may also call it with an empty chunk while the
// transfer is idle (libcurl: about once per second) so callers can cancel a silent stream; it is never
// invoked concurrently.
using HttpDataCallback = std::function<bool(std::string_view chunk)>;

class HttpTransport {
public:
    virtual ~HttpTransport() = default;

    // POSTs the request. Network failures (DNS, connect, TLS, reset, stall) -> ErrorCode::network;
    // aborted by on_data -> ErrorCode::cancelled. HTTP error statuses are not errors at this level.
    virtual Result<HttpResponse> post(const HttpRequest& request, const HttpDataCallback& on_data) = 0;
};

// libcurl on Linux/macOS, WinHTTP on Windows. Safe to share between threads.
std::unique_ptr<HttpTransport> make_default_transport();

} // namespace decomp::agent
