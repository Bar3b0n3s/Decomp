// WinHTTP transport (Windows). Synchronous WinHTTP on the calling thread; one shared session handle
// (WinHTTP session handles are thread-safe), one connection + request handle per call.
//
// Cancellation: on_data is consulted whenever data arrives (the API streams `ping` events while the
// model works). A synchronous WinHTTP request must not be closed from another thread, so an idle
// stream is only cancelled once its next chunk arrives.
#ifdef _WIN32

#include "agent/http.hpp"

#include "core/strings.hpp"
#include "core/version.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <format>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace decomp::agent {
namespace {

class Handle {
public:
    Handle() = default;
    explicit Handle(HINTERNET h) : h_(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h_) WinHttpCloseHandle(h_);
    }
    HINTERNET get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HINTERNET h_ = nullptr;
};

std::string describe_error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                        FORMAT_MESSAGE_FROM_HMODULE;
    const DWORD n = FormatMessageW(flags, GetModuleHandleW(L"winhttp.dll"), code, 0,
                                   reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text;
    if (n > 0 && buffer) text = std::string(trim(wide_to_utf8(std::wstring_view(buffer, n))));
    if (buffer) LocalFree(buffer);
    if (text.empty()) return std::format("WinHTTP error {}", code);
    return std::format("{} (error {})", text, code);
}

Error last_error(std::string_view what, const HttpRequest& request) {
    const DWORD code = GetLastError();
    return Error{ErrorCode::network, std::format("{} {} failed: {}: {}", request.method, request.url, what, describe_error(code))};
}

DWORD to_timeout_ms(std::chrono::seconds s) {
    if (s.count() <= 0) return 0;  // 0 = infinite for WinHttpSetTimeouts
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(s).count();
    return static_cast<DWORD>(std::min<long long>(ms, std::numeric_limits<int>::max()));
}

std::vector<HttpHeader> parse_raw_headers(std::wstring_view raw) {
    std::vector<HttpHeader> headers;
    const std::string text = wide_to_utf8(raw);
    bool first = true;
    for (const auto& line : split_lines(text)) {
        if (first) {  // status line
            first = false;
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        std::string_view view(line);
        headers.emplace_back(to_lower(trim(view.substr(0, colon))), std::string(trim(view.substr(colon + 1))));
    }
    return headers;
}

class WinHttpTransport final : public HttpTransport {
public:
    WinHttpTransport() {
        const std::wstring agent = utf8_to_wide(std::format("decomp/{}", kVersion));
        HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
        // Automatic proxy discovery needs Windows 8.1+; fall back to the static WinHTTP proxy setting.
        if (!session)
            session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            init_error_ = describe_error(GetLastError());
            return;
        }
        // Older Windows versions do not enable TLS 1.2 for WinHTTP by default.
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
        session_ = session;
    }

    WinHttpTransport(const WinHttpTransport&) = delete;
    WinHttpTransport& operator=(const WinHttpTransport&) = delete;

    ~WinHttpTransport() override {
        if (session_) WinHttpCloseHandle(session_);
    }

    Result<HttpResponse> send(const HttpRequest& request, const HttpDataCallback& on_data) override {
        if (!session_) return make_error(ErrorCode::network, "WinHttpOpen failed: {}", init_error_);
        if (request.method != "GET" && request.method != "POST")
            return make_error(ErrorCode::invalid_argument, "unsupported HTTP method '{}'", request.method);

        // Crack the URL into scheme, host, port and path.
        const std::wstring wide_url = utf8_to_wide(request.url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = static_cast<DWORD>(-1);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &parts))
            return make_error(ErrorCode::invalid_argument, "invalid URL '{}': {}", request.url, describe_error(GetLastError()));
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty()) path = L"/";
        const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

        Handle connection(WinHttpConnect(session_, host.c_str(), parts.nPort, 0));
        if (!connection) return std::unexpected(last_error("WinHttpConnect", request));

        const std::wstring verb = utf8_to_wide(request.method);
        Handle req(WinHttpOpenRequest(connection.get(), verb.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
        if (!req) return std::unexpected(last_error("WinHttpOpenRequest", request));

        const DWORD connect_ms = to_timeout_ms(request.connect_timeout);
        const DWORD stall_ms = to_timeout_ms(request.stall_timeout);
        if (!WinHttpSetTimeouts(req.get(), static_cast<int>(connect_ms), static_cast<int>(connect_ms),
                                static_cast<int>(stall_ms), static_cast<int>(stall_ms)))
            return std::unexpected(last_error("WinHttpSetTimeouts", request));

        DWORD disable = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(req.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));

        // One header per call: REPLACE semantics are only well-defined for a single header line.
        for (const auto& [name, value] : request.headers) {
            const std::wstring line = utf8_to_wide(std::format("{}: {}", name, value));
            if (!WinHttpAddRequestHeaders(req.get(), line.c_str(), static_cast<DWORD>(line.size()),
                                          WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
                return make_error(ErrorCode::network, "{} {} failed: cannot add header '{}': {}", request.method, request.url, name,
                                  describe_error(GetLastError()));
        }

        if (request.body.size() > std::numeric_limits<DWORD>::max())
            return make_error(ErrorCode::invalid_argument, "request body too large ({} bytes)", request.body.size());
        const DWORD body_size = request.method == "GET" ? 0 : static_cast<DWORD>(request.body.size());
        void* body = body_size ? const_cast<char*>(request.body.data()) : WINHTTP_NO_REQUEST_DATA;
        if (!WinHttpSendRequest(req.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, body, body_size, body_size, 0))
            return std::unexpected(last_error("WinHttpSendRequest", request));
        if (!WinHttpReceiveResponse(req.get(), nullptr))
            return std::unexpected(last_error("WinHttpReceiveResponse", request));

        HttpResponse response;
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (!WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
            return std::unexpected(last_error("WinHttpQueryHeaders(status)", request));
        response.status = static_cast<int>(status);

        DWORD raw_size = 0;
        WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                            WINHTTP_NO_OUTPUT_BUFFER, &raw_size, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && raw_size > 0) {
            std::wstring raw(raw_size / sizeof(wchar_t) + 1, L'\0');
            DWORD size = static_cast<DWORD>(raw.size() * sizeof(wchar_t));
            if (WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                    &size, WINHTTP_NO_HEADER_INDEX)) {
                raw.resize(size / sizeof(wchar_t));
                response.headers = parse_raw_headers(raw);
            }
        }

        std::vector<char> buffer;
        while (true) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(req.get(), &available))
                return std::unexpected(last_error("reading response", request));
            if (available == 0) break;  // end of body
            buffer.resize(std::max<std::size_t>(available, buffer.size()));
            DWORD read = 0;
            if (!WinHttpReadData(req.get(), buffer.data(), available, &read))
                return std::unexpected(last_error("reading response", request));
            if (read == 0) break;
            const std::string_view chunk(buffer.data(), read);
            if (response.ok()) {
                if (on_data && !on_data(chunk)) return make_error(ErrorCode::cancelled, "request cancelled");
            } else if (response.body_prefix.size() < kMaxErrorBodyPrefix) {
                response.body_prefix.append(chunk.substr(0, kMaxErrorBodyPrefix - response.body_prefix.size()));
            }
        }
        return response;
    }

private:
    HINTERNET session_ = nullptr;
    std::string init_error_;
};

} // namespace

std::unique_ptr<HttpTransport> make_default_transport() { return std::make_unique<WinHttpTransport>(); }

} // namespace decomp::agent

#endif // _WIN32
