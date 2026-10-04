#include "agent/rate_gate.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <thread>

namespace decomp::agent {

using namespace std::chrono_literals;
using std::chrono::milliseconds;

namespace {

std::optional<long long> parse_count(std::string_view text) {
    text = trim(text);
    long long value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size() || value < 0) return std::nullopt;
    return value;
}

} // namespace

std::optional<std::chrono::system_clock::time_point> parse_rfc3339(std::string_view t) {
    using namespace std::chrono;
    t = trim(t);
    auto digits = [&](usize pos, usize n) -> std::optional<int> {
        if (pos + n > t.size()) return std::nullopt;
        int v = 0;
        for (usize i = 0; i < n; ++i) {
            const char c = t[pos + i];
            if (c < '0' || c > '9') return std::nullopt;
            v = v * 10 + (c - '0');
        }
        return v;
    };
    if (t.size() < 20 || t[4] != '-' || t[7] != '-' || (t[10] != 'T' && t[10] != 't' && t[10] != ' ') || t[13] != ':' ||
        t[16] != ':')
        return std::nullopt;
    const auto Y = digits(0, 4), M = digits(5, 2), D = digits(8, 2), h = digits(11, 2), m = digits(14, 2), s = digits(17, 2);
    if (!Y || !M || !D || !h || !m || !s) return std::nullopt;
    const year_month_day ymd{year{*Y}, month{static_cast<unsigned>(*M)}, day{static_cast<unsigned>(*D)}};
    if (!ymd.ok() || *h > 23 || *m > 59 || *s > 60) return std::nullopt;

    usize pos = 19;
    milliseconds fraction{0};
    if (t[pos] == '.') {
        const usize start = ++pos;
        long long ms = 0, scale = 100;
        while (pos < t.size() && t[pos] >= '0' && t[pos] <= '9') {
            ms += (t[pos] - '0') * scale;
            scale /= 10;
            ++pos;
        }
        if (pos == start) return std::nullopt;
        fraction = milliseconds(ms);
    }
    minutes offset{0};
    if (pos < t.size() && (t[pos] == 'Z' || t[pos] == 'z')) {
        ++pos;
    } else if (pos < t.size() && (t[pos] == '+' || t[pos] == '-')) {
        const int sign = t[pos] == '-' ? -1 : 1;
        const auto oh = digits(pos + 1, 2), om = digits(pos + 4, 2);
        if (!oh || !om || t[pos + 3] != ':') return std::nullopt;
        offset = minutes(sign * (*oh * 60 + *om));
        pos += 6;
    } else {
        return std::nullopt;
    }
    if (pos != t.size()) return std::nullopt;
    const auto tp = sys_days{ymd} + hours{*h} + minutes{*m} + seconds{*s} + fraction - offset;
    return time_point_cast<system_clock::duration>(tp);
}

RateGate::RateGate(Options options) : options_(options) {}

std::chrono::milliseconds RateGate::wait_time_locked(Clock::time_point now) const {
    auto until = [now](const std::optional<Clock::time_point>& when) {
        return when && *when > now ? std::chrono::ceil<milliseconds>(*when - now) : 0ms;
    };
    milliseconds wait = backoff_until_ > now ? std::chrono::ceil<milliseconds>(backoff_until_ - now) : 0ms;
    if (requests_left_ == 0) wait = std::max(wait, until(requests_reset_));
    auto low = [this](long long limit, long long remaining) {
        return limit > 0 && remaining >= 0 && static_cast<double>(remaining) < options_.token_reserve * static_cast<double>(limit);
    };
    if (low(last_.input_limit, last_.input_remaining)) wait = std::max(wait, until(input_reset_));
    if (low(last_.output_limit, last_.output_remaining)) wait = std::max(wait, until(output_reset_));
    return wait;
}

RateLimitSnapshot RateGate::snapshot_locked(Clock::time_point now) const {
    RateLimitSnapshot s = last_;
    s.backoff_ms = wait_time_locked(now).count();
    return s;
}

RateLimitSnapshot RateGate::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_locked(Clock::now());
}

std::chrono::milliseconds RateGate::wait_time() const {
    std::lock_guard lock(mutex_);
    return wait_time_locked(Clock::now());
}

bool RateGate::acquire(const std::function<bool()>& cancelled, const std::function<void(milliseconds)>& on_wait) {
    bool waited = false;
    auto done = [&](bool ok) {
        if (waited && on_wait) on_wait(0ms);
        return ok;
    };
    while (true) {
        if (cancelled && cancelled()) return done(false);
        milliseconds wait{0};
        {
            std::lock_guard lock(mutex_);
            wait = wait_time_locked(Clock::now());
            if (wait.count() <= 0) {
                if (requests_left_ > 0) --requests_left_;
                break;
            }
        }
        if (!waited) {
            waited = true;
            if (on_wait) on_wait(wait);
        }
        std::this_thread::sleep_for(std::min(wait, milliseconds(100)));
    }
    return done(true);
}

void RateGate::observe(const std::vector<HttpHeader>& headers) {
    bool any = false;
    {
        std::lock_guard lock(mutex_);
        const auto now = Clock::now();
        const auto system_now = std::chrono::system_clock::now();
        std::optional<std::chrono::system_clock::time_point> earliest;
        auto read = [&](std::string_view kind, long long& limit, long long& remaining, std::optional<Clock::time_point>& reset) {
            auto value = [&](std::string_view what) { return find_header(headers, std::format("anthropic-ratelimit-{}-{}", kind, what)); };
            if (auto v = value("limit"))
                if (auto n = parse_count(*v)) {
                    limit = *n;
                    any = true;
                }
            if (auto v = value("remaining"))
                if (auto n = parse_count(*v)) {
                    remaining = *n;
                    any = true;
                }
            if (auto v = value("reset"))
                if (auto when = parse_rfc3339(*v)) {
                    any = true;
                    const auto delta = std::clamp(std::chrono::ceil<milliseconds>(*when - system_now), 0ms, options_.max_reset_wait);
                    reset = now + delta;
                    if (!earliest || *when < *earliest) {
                        earliest = when;
                        last_.reset = std::string(trim(*v));
                    }
                }
        };
        read("requests", last_.requests_limit, last_.requests_remaining, requests_reset_);
        read("input-tokens", last_.input_limit, last_.input_remaining, input_reset_);
        read("output-tokens", last_.output_limit, last_.output_remaining, output_reset_);
        if (any) requests_left_ = last_.requests_remaining;
    }
    if (any) notify();
}

void RateGate::on_throttled(int status, std::optional<milliseconds> retry_after) {
    {
        std::lock_guard lock(mutex_);
        milliseconds hold = retry_after.value_or(status == 429 ? options_.throttle_default : options_.overload_default);
        hold = std::clamp(hold, 0ms, options_.max_backoff);
        backoff_until_ = std::max(backoff_until_, Clock::now() + hold);
        last_.throttled_status = status;
    }
    notify();
}

void RateGate::set_listener(Listener listener) {
    std::lock_guard lock(mutex_);
    listener_ = listener ? std::make_shared<const Listener>(std::move(listener)) : nullptr;
}

void RateGate::notify() {
    // One notification at a time, so listeners see the updates in the order they happened.
    std::lock_guard notifying(notify_mutex_);
    std::shared_ptr<const Listener> listener;
    RateLimitSnapshot snapshot;
    {
        std::lock_guard lock(mutex_);
        listener = listener_;
        snapshot = snapshot_locked(Clock::now());
    }
    if (listener) (*listener)(snapshot);
}

} // namespace decomp::agent
