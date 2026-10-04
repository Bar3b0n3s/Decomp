// libcurl transport (Linux/macOS). One easy handle per request, so it is safe to use from many threads.
#ifndef _WIN32

#include "agent/http.hpp"

#include "core/process.hpp"
#include "core/strings.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <mutex>

namespace decomp::agent {
namespace {

void ensure_curl_initialized() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct EasyHandle {
    CURL* handle = curl_easy_init();
    EasyHandle() = default;
    EasyHandle(const EasyHandle&) = delete;
    EasyHandle& operator=(const EasyHandle&) = delete;
    ~EasyHandle() {
        if (handle) curl_easy_cleanup(handle);
    }
};

struct HeaderList {
    curl_slist* list = nullptr;
    HeaderList() = default;
    HeaderList(const HeaderList&) = delete;
    HeaderList& operator=(const HeaderList&) = delete;
    ~HeaderList() { curl_slist_free_all(list); }

    bool append(const std::string& line) {
        curl_slist* next = curl_slist_append(list, line.c_str());
        if (!next) return false;
        list = next;
        return true;
    }
};

struct Transfer {
    CURL* handle = nullptr;
    const HttpDataCallback* on_data = nullptr;
    HttpResponse response;
    bool status_known = false;
    bool cancelled = false;
    // Stall watchdog: libcurl's low-speed check averages over a ~5 s window, so it fires late; this
    // one fires when no byte has moved in either direction for stall_timeout.
    std::chrono::seconds stall_timeout{0};
    std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now();
    curl_off_t last_downloaded = 0;
    curl_off_t last_uploaded = 0;
    bool stalled = false;

    void touch() { last_activity = std::chrono::steady_clock::now(); }
};

void resolve_status(Transfer& t) {
    if (t.status_known) return;
    long code = 0;
    curl_easy_getinfo(t.handle, CURLINFO_RESPONSE_CODE, &code);
    t.response.status = static_cast<int>(code);
    t.status_known = true;
}

size_t on_header(char* buffer, size_t size, size_t count, void* user) {
    auto* t = static_cast<Transfer*>(user);
    const size_t n = size * count;
    t->touch();
    std::string_view line = trim(std::string_view(buffer, n));
    if (line.starts_with("HTTP/")) {
        // Start of a new response (e.g. after "100 Continue"): forget headers of the previous one.
        t->response.headers.clear();
        return n;
    }
    const auto colon = line.find(':');
    if (colon != std::string_view::npos && colon > 0) {
        t->response.headers.emplace_back(to_lower(trim(line.substr(0, colon))), std::string(trim(line.substr(colon + 1))));
    }
    return n;
}

size_t on_body(char* data, size_t size, size_t count, void* user) {
    auto* t = static_cast<Transfer*>(user);
    const size_t n = size * count;
    t->touch();
    resolve_status(*t);
    if (t->response.ok()) {
        if (!(*t->on_data)(std::string_view(data, n))) {
            t->cancelled = true;
            return 0;  // anything but n aborts the transfer
        }
    } else if (t->response.body_prefix.size() < kMaxErrorBodyPrefix) {
        const size_t take = std::min(n, kMaxErrorBodyPrefix - t->response.body_prefix.size());
        t->response.body_prefix.append(data, take);
    }
    return n;
}

// Called by libcurl frequently while data flows and about once per second when idle: detects stalls and
// lets the caller cancel a stream that is waiting for the model.
int on_progress(void* user, curl_off_t, curl_off_t downloaded, curl_off_t, curl_off_t uploaded) {
    auto* t = static_cast<Transfer*>(user);
    if (downloaded != t->last_downloaded || uploaded != t->last_uploaded) {
        t->last_downloaded = downloaded;
        t->last_uploaded = uploaded;
        t->touch();
    } else if (t->stall_timeout.count() > 0 && std::chrono::steady_clock::now() - t->last_activity >= t->stall_timeout) {
        t->stalled = true;
        return 1;
    }
    if (!(*t->on_data)(std::string_view{})) {
        t->cancelled = true;
        return 1;
    }
    return 0;
}

class CurlTransport final : public HttpTransport {
public:
    CurlTransport() { ensure_curl_initialized(); }

    Result<HttpResponse> post(const HttpRequest& request, const HttpDataCallback& on_data) override {
        static const HttpDataCallback discard = [](std::string_view) { return true; };

        EasyHandle easy;
        if (!easy.handle) return make_error(ErrorCode::internal, "curl_easy_init failed");
        CURL* h = easy.handle;

        HeaderList headers;
        for (const auto& [name, value] : request.headers) {
            if (!headers.append(std::format("{}: {}", name, value)))
                return make_error(ErrorCode::internal, "out of memory building request headers");
        }
        // libcurl would otherwise send "Expect: 100-continue" for large bodies and wait a round trip.
        if (!find_header(request.headers, "expect")) headers.append("Expect:");

        Transfer transfer;
        transfer.handle = h;
        transfer.on_data = on_data ? &on_data : &discard;
        transfer.stall_timeout = request.stall_timeout;
        char error_buffer[CURL_ERROR_SIZE] = {};

        curl_easy_setopt(h, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(h, CURLOPT_POST, 1L);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers.list);
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);  // required for multi-threaded use
        curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(h, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
        curl_easy_setopt(h, CURLOPT_ERRORBUFFER, error_buffer);
        if (request.connect_timeout.count() > 0)
            curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, static_cast<long>(request.connect_timeout.count()));
        if (request.stall_timeout.count() > 0) {
            curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
            curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, static_cast<long>(request.stall_timeout.count()));
        }
        curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(h, CURLOPT_HEADERDATA, &transfer);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &transfer);
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(h, CURLOPT_XFERINFODATA, &transfer);
        // Proxies come from the environment (libcurl default). Like the curl tool, honor CA overrides.
        auto ca_file = get_env("CURL_CA_BUNDLE");
        if (!ca_file || ca_file->empty()) ca_file = get_env("SSL_CERT_FILE");
        if (ca_file && !ca_file->empty()) curl_easy_setopt(h, CURLOPT_CAINFO, ca_file->c_str());
        if (auto ca_dir = get_env("SSL_CERT_DIR"); ca_dir && !ca_dir->empty())
            curl_easy_setopt(h, CURLOPT_CAPATH, ca_dir->c_str());

        transfer.touch();
        const CURLcode rc = curl_easy_perform(h);
        if (transfer.cancelled) return make_error(ErrorCode::cancelled, "request cancelled");
        if (transfer.stalled) {
            return make_error(ErrorCode::network, "POST {} failed: no data received for {} s (stalled)", request.url,
                              request.stall_timeout.count());
        }
        if (rc != CURLE_OK) {
            std::string detail = error_buffer[0] != '\0' ? std::string(trim(error_buffer)) : curl_easy_strerror(rc);
            return make_error(ErrorCode::network, "POST {} failed: {}", request.url, detail);
        }
        resolve_status(transfer);
        return std::move(transfer.response);
    }
};

} // namespace

std::unique_ptr<HttpTransport> make_default_transport() { return std::make_unique<CurlTransport>(); }

} // namespace decomp::agent

#endif // !_WIN32
