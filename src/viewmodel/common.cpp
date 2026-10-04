#include "viewmodel/common.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>

namespace decomp::vm {

int size_bucket(u64 bytes) { return bytes < 2 ? 0 : static_cast<int>(std::bit_width(bytes)) - 1; }

std::string size_bucket_label(int bucket) {
    if (bucket <= 0) return "0-1 B";
    if (bucket < 10) return std::format("{}-{} B", u64{1} << bucket, (u64{1} << (bucket + 1)) - 1);
    if (bucket < 20) return std::format("{}-{} KiB", u64{1} << (bucket - 10), u64{1} << (bucket - 9));
    return std::format("{}-{} MiB", u64{1} << (bucket - 20), u64{1} << (bucket - 19));
}

double seconds_between(TimePoint from, TimePoint to) {
    return std::chrono::duration<double>(to - from).count();
}

i64 to_unix_ms(TimePoint t) { return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count(); }

TimePoint from_unix_ms(i64 ms) { return TimePoint(std::chrono::milliseconds(ms)); }

double to_unix_seconds(TimePoint t) { return std::chrono::duration<double>(t.time_since_epoch()).count(); }

namespace {

// Reads `count` decimal digits at `pos`.
std::optional<int> digits(std::string_view s, usize pos, usize count) {
    if (pos + count > s.size()) return std::nullopt;
    int v = 0;
    for (usize i = pos; i < pos + count; ++i) {
        if (s[i] < '0' || s[i] > '9') return std::nullopt;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

// "YYYY-MM-DDTHH?MM?SS" where ? is `time_sep`.
std::optional<TimePoint> parse_date_time(std::string_view s, char time_sep) {
    auto y = digits(s, 0, 4), mo = digits(s, 5, 2), d = digits(s, 8, 2), h = digits(s, 11, 2), mi = digits(s, 14, 2), sec = digits(s, 17, 2);
    if (!y || !mo || !d || !h || !mi || !sec || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ') || s[13] != time_sep ||
        s[16] != time_sep)
        return std::nullopt;
    const std::chrono::year_month_day ymd{std::chrono::year{*y}, std::chrono::month{static_cast<unsigned>(*mo)},
                                          std::chrono::day{static_cast<unsigned>(*d)}};
    if (!ymd.ok() || *h > 23 || *mi > 59 || *sec > 60) return std::nullopt;
    return TimePoint(std::chrono::sys_days(ymd)) + std::chrono::hours(*h) + std::chrono::minutes(*mi) + std::chrono::seconds(*sec);
}

} // namespace

std::optional<TimePoint> parse_iso8601(std::string_view text) {
    auto t = parse_date_time(text, ':');
    if (!t) return std::nullopt;
    usize pos = 19;
    if (pos < text.size() && text[pos] == '.') {
        usize end = pos + 1;
        double fraction = 0, scale = 0.1;
        while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
            fraction += (text[end] - '0') * scale;
            scale /= 10;
            ++end;
        }
        *t += std::chrono::milliseconds(static_cast<i64>(std::llround(fraction * 1000)));
        pos = end;
    }
    if (pos == text.size()) return t;  // no zone: taken as UTC
    if (text[pos] == 'Z' || text[pos] == 'z') return pos + 1 == text.size() ? t : std::nullopt;
    if (text[pos] != '+' && text[pos] != '-') return std::nullopt;
    auto oh = digits(text, pos + 1, 2), om = digits(text, pos + 4, 2);
    if (!oh || !om || pos + 6 != text.size() || text[pos + 3] != ':') return std::nullopt;
    const auto offset = std::chrono::hours(*oh) + std::chrono::minutes(*om);
    return text[pos] == '+' ? *t - offset : *t + offset;
}

std::optional<TimePoint> run_id_time(std::string_view run_id) { return parse_date_time(run_id, '-'); }

i64 day_number(TimePoint t, std::chrono::minutes utc_offset) {
    return std::chrono::floor<std::chrono::days>(t + utc_offset).time_since_epoch().count();
}

std::string day_label(i64 day) {
    const std::chrono::year_month_day ymd{std::chrono::sys_days(std::chrono::days(day))};
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
}

std::string format_duration(double seconds) {
    const auto s = static_cast<i64>(std::max(0.0, std::round(seconds)));
    if (s < 60) return std::format("{}s", s);
    if (s < 3600) return std::format("{}m {:02}s", s / 60, s % 60);
    if (s < 86400) return std::format("{}h {:02}m", s / 3600, s % 3600 / 60);
    return std::format("{}d {:02}h", s / 86400, s % 86400 / 3600);
}

std::string format_usd(double usd) { return std::abs(usd) < 1.0 ? std::format("${:.4f}", usd) : std::format("${:.2f}", usd); }

bool is_complete_outcome(std::string_view outcome) {
    return outcome == "matched" || outcome == "gave_up" || outcome == "refused" || outcome == "budget_exhausted" ||
           outcome == "max_turns" || outcome == "no_result";
}

namespace {

bool later(const events::SessionState& a, const events::SessionState& b) {
    return a.started != b.started ? a.started > b.started : a.id > b.id;
}

} // namespace

std::map<u64, const events::SessionState*> live_sessions(const events::RunStateData& state) {
    std::map<u64, const events::SessionState*> out;
    for (const auto& [id, s] : state.sessions) {
        if (s->finished) continue;
        auto& slot = out[s->va];
        if (!slot || later(*s, *slot)) slot = s.get();
    }
    return out;
}

std::map<u64, const events::SessionState*> latest_sessions(const events::RunStateData& state) {
    std::map<u64, const events::SessionState*> out;
    for (const auto& [id, s] : state.sessions) {
        auto& slot = out[s->va];
        if (!slot || later(*s, *slot)) slot = s.get();
    }
    return out;
}

} // namespace decomp::vm
