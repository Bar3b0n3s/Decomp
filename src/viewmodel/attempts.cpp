#include "viewmodel/attempts.hpp"

#include <format>

namespace decomp::vm {

std::vector<AttemptRecord> parse_attempts(std::span<const Json> lines) {
    std::vector<AttemptRecord> out;
    out.reserve(lines.size());
    for (usize i = 0; i < lines.size(); ++i) {
        const Json& j = lines[i];
        if (!j.is_object()) continue;
        AttemptRecord r;
        r.index = i;
        r.attempt = static_cast<int>(json_int_or(j, "attempt", 0));
        r.session = json_string_or(j, "session", "");
        r.origin = json_string_or(j, "origin", "agent");
        r.time = json_string_or(j, "time", "");
        r.compiled = json_bool_or(j, "compiled", false);
        r.byte_exact = json_bool_or(j, "byte_exact", false);
        r.match_percent = json_number_or(j, "match_percent", 0);
        r.summary = json_string_or(j, "summary", "");
        r.source = json_string_or(j, "source", "");
        out.push_back(std::move(r));
    }
    return out;
}

std::optional<usize> best_attempt(std::span<const AttemptRecord> attempts, const std::optional<std::string>& best_source) {
    if (!best_source) return std::nullopt;
    for (usize i = attempts.size(); i-- > 0;)
        if (attempts[i].source == *best_source) return i;
    return std::nullopt;
}

std::vector<usize> session_attempts(std::span<const AttemptRecord> attempts, std::string_view session) {
    std::vector<usize> out;
    for (usize i = 0; i < attempts.size(); ++i)
        if (attempts[i].session == session) out.push_back(i);
    return out;
}

std::string manual_session_id(TimePoint time) {
    return std::format("user-{:%Y-%m-%dT%H-%M-%S}", std::chrono::floor<std::chrono::seconds>(time));
}

int next_attempt_number(std::span<const AttemptRecord> attempts, std::string_view session) {
    int last = 0;
    for (const auto& a : attempts)
        if (a.session == session) last = std::max(last, a.attempt);
    return last + 1;
}

Json user_attempt_json(std::string_view session, int attempt, TimePoint time, bool compiled, double match_percent, bool byte_exact,
                       std::string_view summary, std::string_view source) {
    return Json{{"session", std::string(session)},
                {"attempt", attempt},
                {"origin", "user"},
                {"time", std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::milliseconds>(time))},
                {"compiled", compiled},
                {"match_percent", match_percent},
                {"byte_exact", byte_exact},
                {"summary", std::string(summary)},
                {"source", std::string(source)}};
}

} // namespace decomp::vm
