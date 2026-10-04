#pragma once

// Shared pacing for the Messages API across the sessions of a run. Every response (errors included)
// reports the account's rate limits in anthropic-ratelimit-* headers; the gate keeps the latest values,
// holds every session back after a 429 or 529, and waits for the reset when a limit is used up, so
// parallel workers do not keep hitting the limit one after another.

#include "agent/http.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::agent {

struct RateLimitSnapshot {
    long long requests_limit = -1, requests_remaining = -1;  // -1: not reported (yet)
    long long input_limit = -1, input_remaining = -1;        // input tokens per minute
    long long output_limit = -1, output_remaining = -1;      // output tokens per minute
    std::string reset;         // earliest reset time reported (RFC 3339), informational
    long long backoff_ms = 0;  // how long requests are held back from now (0: open)
    int throttled_status = 0;  // the 429 or 529 behind the last backoff
};

class RateGate {
public:
    using Clock = std::chrono::steady_clock;
    using Listener = std::function<void(const RateLimitSnapshot&)>;

    struct Options {
        // Wait for the reset when less than this share of the input or output tokens remains.
        double token_reserve = 0.02;
        // Longest wait derived from a reported reset time (guards against clock skew).
        std::chrono::milliseconds max_reset_wait{60'000};
        // Hold-back after a 429 or 529 that carries no retry-after, and the longest hold-back at all.
        std::chrono::milliseconds throttle_default{10'000};
        std::chrono::milliseconds overload_default{2'000};
        std::chrono::milliseconds max_backoff{5 * 60'000};
    };

    RateGate() : RateGate(Options{}) {}
    explicit RateGate(Options options);

    // Blocks until a request may go out. `on_wait` hears the expected wait before blocking and 0 when the
    // wait is over. Returns false when `cancelled` returned true first.
    bool acquire(const std::function<bool()>& cancelled = {},
                 const std::function<void(std::chrono::milliseconds)>& on_wait = {});
    // Reads the anthropic-ratelimit-* headers of a response (any status).
    void observe(const std::vector<HttpHeader>& headers);
    // A 429 (rate limited) or 529 (overloaded) answer: hold every request back for a while.
    void on_throttled(int status, std::optional<std::chrono::milliseconds> retry_after);

    // Called after every change, outside the gate's lock (e.g. to publish rate_limit_updated).
    void set_listener(Listener listener);
    RateLimitSnapshot snapshot() const;
    std::chrono::milliseconds wait_time() const;  // how long acquire() would wait now

private:
    std::chrono::milliseconds wait_time_locked(Clock::time_point now) const;
    RateLimitSnapshot snapshot_locked(Clock::time_point now) const;
    void notify();

    Options options_;
    std::mutex notify_mutex_;  // serializes listener calls
    mutable std::mutex mutex_;
    RateLimitSnapshot last_;
    Clock::time_point backoff_until_{};
    std::optional<Clock::time_point> requests_reset_, input_reset_, output_reset_;
    long long requests_left_ = -1;  // decremented per request between header updates
    std::shared_ptr<const Listener> listener_;
};

// "2026-10-04T12:00:00Z", "2026-10-04T12:00:00.25+02:00", ... -> a system clock time.
std::optional<std::chrono::system_clock::time_point> parse_rfc3339(std::string_view text);

} // namespace decomp::agent
