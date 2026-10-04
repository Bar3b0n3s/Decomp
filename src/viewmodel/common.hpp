#pragma once

// Small helpers shared by the view models (namespace decomp::vm): size buckets, time arithmetic,
// plot series and display formatting. Everything here is pure and cheap.

#include "core/types.hpp"
#include "events/run_state.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::vm {

using TimePoint = events::TimePoint;

// A plot series: parallel x and y values (ImPlot takes the two arrays as they are).
struct Series {
    std::vector<double> x, y;

    usize size() const { return x.size(); }
    bool empty() const { return x.empty(); }
    void push(double px, double py) {
        x.push_back(px);
        y.push_back(py);
    }
};

// Function sizes in powers of two: bucket b holds sizes in [2^b, 2^(b+1)) bytes; sizes 0 and 1 are
// bucket 0. A 15-byte function is in bucket 3 (8-15 bytes), a 1000-byte one in bucket 9 (512-1023).
int size_bucket(u64 bytes);
// "0-1 B", "8-15 B", "512-1023 B", "1-2 KiB", "64-128 KiB".
std::string size_bucket_label(int bucket);

// to - from, in seconds (negative when `to` is earlier).
double seconds_between(TimePoint from, TimePoint to);
i64 to_unix_ms(TimePoint t);
TimePoint from_unix_ms(i64 ms);
double to_unix_seconds(TimePoint t);
// "2026-10-04T13:00:00Z", with optional fractional seconds and a "+02:00"-style offset instead of Z.
std::optional<TimePoint> parse_iso8601(std::string_view text);
// A run id ("2026-10-04T13-00-00-1a2b") -> the UTC time it was created.
std::optional<TimePoint> run_id_time(std::string_view run_id);
// Days since the Unix epoch of `t` shifted by `utc_offset` (the caller's time zone; 0 = UTC days).
i64 day_number(TimePoint t, std::chrono::minutes utc_offset = {});
// "2026-10-04" for a day number.
std::string day_label(i64 day);

// "0s", "45s", "3m 05s", "1h 02m", "2d 03h".
std::string format_duration(double seconds);
// "$0.0042" below a dollar, "$12.34" above.
std::string format_usd(double usd);

// Session outcomes that end the work on the session's own terms, so its duration and cost show what
// the function took: matched, gave_up, refused, budget_exhausted, max_turns and no_result. Stopped,
// aborted, skipped, interrupted, error and run_budget_exhausted sessions were cut short.
bool is_complete_outcome(std::string_view outcome);

// The unfinished session working on each function, by address (the most recently started one when a
// function has several). The pointers point into `state` and live as long as it does.
std::map<u64, const events::SessionState*> live_sessions(const events::RunStateData& state);

// The latest session of each function (finished or not), by address, as run summaries count them:
// the most recently started one, ties broken by session id.
std::map<u64, const events::SessionState*> latest_sessions(const events::RunStateData& state);

} // namespace decomp::vm
