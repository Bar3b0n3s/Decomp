#pragma once

// A function's attempt history (.decomp/functions/<fn>/attempts.jsonl, docs/project-format.md) for the
// Agent session's source history and the Diff viewer's attempt slider, and the record of an attempt
// made by hand (manual mode), with the same fields as the agent's.

#include "core/json.hpp"
#include "viewmodel/common.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::vm {

struct AttemptRecord {
    usize index = 0;  // position in attempts.jsonl (0-based)
    int attempt = 0;  // number within its session
    std::string session;
    std::string origin = "agent";  // agent, or user for attempts made by hand
    std::string time;              // as recorded (UTC, ISO 8601)
    bool compiled = false, byte_exact = false;
    double match_percent = 0;
    std::string summary, source;
};

// Records that are not JSON objects are skipped; missing fields keep their defaults.
std::vector<AttemptRecord> parse_attempts(std::span<const Json> lines);

// The attempt best.cpp holds: the latest attempt whose source is `best_source`. nullopt when there is no
// best source or no attempt has it.
std::optional<usize> best_attempt(std::span<const AttemptRecord> attempts, const std::optional<std::string>& best_source);

// The attempts of one session, in order (indices into `attempts`).
std::vector<usize> session_attempts(std::span<const AttemptRecord> attempts, std::string_view session);

// A session id for edits made by hand: "user-<UTC time>" ("user-2026-10-04T12-00-00").
std::string manual_session_id(TimePoint time);
// The next attempt number in a session (1 for a new one).
int next_attempt_number(std::span<const AttemptRecord> attempts, std::string_view session);

// The attempts.jsonl record of an attempt made by hand: attempt, session, origin "user", time,
// compiled, match_percent, byte_exact, summary and source, as MatchSession records the agent's.
Json user_attempt_json(std::string_view session, int attempt, TimePoint time, bool compiled, double match_percent, bool byte_exact,
                       std::string_view summary, std::string_view source);

} // namespace decomp::vm
