#include "events/events.hpp"

#include <format>
#include <random>

namespace decomp::events {

namespace {

Json usage_json(const TokenUsage& u) {
    return {{"input", u.input}, {"output", u.output}, {"cache_write", u.cache_write}, {"cache_read", u.cache_read}};
}
TokenUsage usage_from(const Json& j) {
    return {json_int_or(j, "input", 0), json_int_or(j, "output", 0), json_int_or(j, "cache_write", 0), json_int_or(j, "cache_read", 0)};
}
std::vector<std::string> strings(const Json& j, const char* key) {
    std::vector<std::string> out;
    if (auto it = j.find(key); it != j.end() && it->is_array())
        for (const auto& v : *it) out.push_back(v.get<std::string>());
    return out;
}

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

} // namespace

std::string_view type_name(const Payload& payload) {
    return std::visit(Overloaded{
                          [](const RunStarted&) { return std::string_view("run_started"); },
                          [](const RunFinished&) { return std::string_view("run_finished"); },
                          [](const SessionStarted&) { return std::string_view("session_started"); },
                          [](const SessionFinished&) { return std::string_view("session_finished"); },
                          [](const TurnStarted&) { return std::string_view("turn_started"); },
                          [](const TurnFinished&) { return std::string_view("turn_finished"); },
                          [](const StreamDelta&) { return std::string_view("stream_delta"); },
                          [](const ToolCallStarted&) { return std::string_view("tool_call_started"); },
                          [](const ToolCallFinished&) { return std::string_view("tool_call_finished"); },
                          [](const CompileFinished&) { return std::string_view("compile_finished"); },
                          [](const DiffComputed&) { return std::string_view("diff_computed"); },
                          [](const Retry&) { return std::string_view("retry"); },
                          [](const Refusal&) { return std::string_view("refusal"); },
                          [](const Guidance&) { return std::string_view("guidance"); },
                          [](const StatusChanged&) { return std::string_view("status_changed"); },
                          [](const FileWritten&) { return std::string_view("file_written"); },
                          [](const LogLine&) { return std::string_view("log"); },
                      },
                      payload);
}

Json to_json(const Event& e) {
    Json j;
    j["seq"] = e.seq;
    j["time"] = std::chrono::duration_cast<std::chrono::milliseconds>(e.time.time_since_epoch()).count();
    j["run"] = e.run;
    if (e.worker >= 0) j["worker"] = e.worker;
    j["type"] = std::string(type_name(e.payload));
    Json d = std::visit(Overloaded{
                            [](const RunStarted& p) -> Json {
                                return {{"project", p.project}, {"model", p.model}, {"effort", p.effort}, {"workers", p.workers}, {"functions", p.functions}};
                            },
                            [](const RunFinished& p) -> Json { return {{"status", p.status}}; },
                            [](const SessionStarted& p) -> Json { return {{"session", p.session}, {"function", p.function}, {"display", p.display}, {"va", p.va}}; },
                            [](const SessionFinished& p) -> Json {
                                return {{"session", p.session}, {"outcome", p.outcome}, {"detail", p.detail}, {"best_match", p.best_match}, {"turns", p.turns}, {"cost_usd", p.cost_usd}};
                            },
                            [](const TurnStarted& p) -> Json { return {{"session", p.session}, {"turn", p.turn}}; },
                            [](const TurnFinished& p) -> Json {
                                return {{"session", p.session}, {"turn", p.turn}, {"stop_reason", p.stop_reason}, {"usage", usage_json(p.usage)},
                                        {"cost_usd", p.cost_usd}, {"latency_ms", p.latency_ms}};
                            },
                            [](const StreamDelta& p) -> Json { return {{"session", p.session}, {"kind", p.kind}, {"text", p.text}}; },
                            [](const ToolCallStarted& p) -> Json { return {{"session", p.session}, {"id", p.id}, {"tool", p.tool}, {"turn", p.turn}, {"input", p.input}}; },
                            [](const ToolCallFinished& p) -> Json {
                                return {{"session", p.session}, {"id", p.id}, {"tool", p.tool}, {"is_error", p.is_error}, {"summary", p.summary}, {"duration_ms", p.duration_ms}};
                            },
                            [](const CompileFinished& p) -> Json {
                                return {{"session", p.session}, {"ok", p.ok}, {"cached", p.cached}, {"duration_ms", p.duration_ms}, {"errors", p.errors}};
                            },
                            [](const DiffComputed& p) -> Json { return {{"session", p.session}, {"match_percent", p.match_percent}, {"byte_exact", p.byte_exact}, {"summary", p.summary}}; },
                            [](const Retry& p) -> Json { return {{"session", p.session}, {"attempt", p.attempt}, {"error", p.error}, {"delay_ms", p.delay_ms}}; },
                            [](const Refusal& p) -> Json { return {{"session", p.session}, {"category", p.category}, {"explanation", p.explanation}}; },
                            [](const Guidance& p) -> Json { return {{"session", p.session}, {"text", p.text}}; },
                            [](const StatusChanged& p) -> Json { return {{"function", p.function}, {"va", p.va}, {"status", p.status}}; },
                            [](const FileWritten& p) -> Json { return {{"path", p.path}, {"reason", p.reason}}; },
                            [](const LogLine& p) -> Json { return {{"level", p.level}, {"message", p.message}}; },
                        },
                        e.payload);
    j["data"] = std::move(d);
    return j;
}

Result<Event> event_from_json(const Json& j) {
    if (!j.is_object()) return make_error(ErrorCode::parse, "event must be an object");
    Event e;
    e.seq = static_cast<u64>(json_int_or(j, "seq", 0));
    e.time = std::chrono::system_clock::time_point(std::chrono::milliseconds(json_int_or(j, "time", 0)));
    e.run = json_string_or(j, "run", "");
    e.worker = static_cast<int>(json_int_or(j, "worker", -1));
    auto type = json_string_or(j, "type", "");
    const Json d = j.value("data", Json::object());
    auto s = [&](const char* k) { return json_string_or(d, k, ""); };
    auto i = [&](const char* k) { return json_int_or(d, k, 0); };
    auto f = [&](const char* k) { return json_number_or(d, k, 0); };
    auto b = [&](const char* k) { return json_bool_or(d, k, false); };
    if (type == "run_started") e.payload = RunStarted{s("project"), s("model"), s("effort"), static_cast<int>(i("workers")), strings(d, "functions")};
    else if (type == "run_finished") e.payload = RunFinished{s("status")};
    else if (type == "session_started") e.payload = SessionStarted{s("session"), s("function"), s("display"), static_cast<u64>(i("va"))};
    else if (type == "session_finished") e.payload = SessionFinished{s("session"), s("outcome"), s("detail"), f("best_match"), static_cast<int>(i("turns")), f("cost_usd")};
    else if (type == "turn_started") e.payload = TurnStarted{s("session"), static_cast<int>(i("turn"))};
    else if (type == "turn_finished") e.payload = TurnFinished{s("session"), static_cast<int>(i("turn")), s("stop_reason"), usage_from(d.value("usage", Json::object())), f("cost_usd"), i("latency_ms")};
    else if (type == "stream_delta") e.payload = StreamDelta{s("session"), s("kind"), s("text")};
    else if (type == "tool_call_started") e.payload = ToolCallStarted{s("session"), s("id"), s("tool"), static_cast<int>(i("turn")), d.value("input", Json())};
    else if (type == "tool_call_finished") e.payload = ToolCallFinished{s("session"), s("id"), s("tool"), b("is_error"), s("summary"), i("duration_ms")};
    else if (type == "compile_finished") e.payload = CompileFinished{s("session"), b("ok"), b("cached"), i("duration_ms"), static_cast<int>(i("errors"))};
    else if (type == "diff_computed") e.payload = DiffComputed{s("session"), f("match_percent"), b("byte_exact"), s("summary")};
    else if (type == "retry") e.payload = Retry{s("session"), static_cast<int>(i("attempt")), s("error"), i("delay_ms")};
    else if (type == "refusal") e.payload = Refusal{s("session"), s("category"), s("explanation")};
    else if (type == "guidance") e.payload = Guidance{s("session"), s("text")};
    else if (type == "status_changed") e.payload = StatusChanged{s("function"), static_cast<u64>(i("va")), s("status")};
    else if (type == "file_written") e.payload = FileWritten{s("path"), s("reason")};
    else if (type == "log") e.payload = LogLine{s("level"), s("message")};
    else return make_error(ErrorCode::parse, "unknown event type '{}'", type);
    return e;
}

std::string new_run_id() {
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::random_device rd;
    return std::format("{:%Y-%m-%dT%H-%M-%S}-{:04x}", now, rd() & 0xFFFF);
}

} // namespace decomp::events
