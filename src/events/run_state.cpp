#include "events/run_state.hpp"

#include <format>

namespace decomp::events {

namespace {
template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
} // namespace

SessionState& RunState::session(const std::string& id) {
    auto& s = data_.sessions[id];
    if (s.id.empty()) s.id = id;
    return s;
}

void RunState::activity(const Event& e, std::string line) {
    auto t = std::chrono::floor<std::chrono::seconds>(e.time);
    data_.activity.push_back(std::format("[{:%H:%M:%S}] {}", t, line));
    ++data_.activity_total;
    while (data_.activity.size() > kActivityLines) data_.activity.pop_front();
}

void RunState::apply(const Event& e) {
    data_.last_seq = std::max(data_.last_seq, e.seq);
    if (data_.run_id.empty()) data_.run_id = e.run;
    auto set_worker = [&](const std::string& session_id, const std::string& phase) {
        if (e.worker < 0) return;
        auto& w = data_.workers[e.worker];
        w.id = e.worker;
        w.session = session_id;
        w.phase = phase;
    };
    auto name_of = [&](const std::string& session_id) {
        auto it = data_.sessions.find(session_id);
        return it == data_.sessions.end() ? session_id : (it->second.display.empty() ? it->second.function : it->second.display);
    };

    std::visit(Overloaded{
                   [&](const RunStarted& p) {
                       data_.project = p.project;
                       data_.model = p.model;
                       data_.effort = p.effort;
                       data_.worker_count = p.workers;
                       data_.planned = p.functions;
                       data_.started = e.time;
                       data_.status = "running";
                       activity(e, std::format("run started: {} function(s), model {} ({})", p.functions.size(), p.model, p.effort));
                   },
                   [&](const RunFinished& p) {
                       data_.status = p.status;
                       data_.ended = e.time;
                       for (auto& [id, w] : data_.workers) {
                           w.session.clear();
                           w.phase = "idle";
                       }
                       activity(e, std::format("run {}: {} matched of {} finished, ${:.2f}", p.status, data_.matched, data_.finished, data_.cost_usd));
                   },
                   [&](const SessionStarted& p) {
                       auto& s = session(p.session);
                       s.function = p.function;
                       s.display = p.display;
                       s.va = p.va;
                       s.worker = e.worker;
                       s.started = e.time;
                       s.phase = "starting";
                       set_worker(p.session, "starting");
                       activity(e, std::format("{}: session started", p.display.empty() ? p.function : p.display));
                   },
                   [&](const SessionFinished& p) {
                       auto& s = session(p.session);
                       s.finished = true;
                       s.outcome = p.outcome;
                       s.detail = p.detail;
                       s.best_match = std::max(s.best_match, p.best_match);
                       s.turn = std::max(s.turn, p.turns);
                       s.phase = "done";
                       s.ended = e.time;
                       s.matched = p.outcome == "matched";
                       ++data_.finished;
                       if (s.matched) ++data_.matched;
                       set_worker({}, "idle");
                       activity(e, std::format("{}: {} (best {:.1f}%, {} turns, ${:.2f}){}", name_of(p.session), p.outcome, s.best_match,
                                               p.turns, s.cost_usd, p.detail.empty() ? "" : " - " + p.detail));
                   },
                   [&](const TurnStarted& p) {
                       auto& s = session(p.session);
                       s.turn = p.turn;
                       s.phase = "waiting for model";
                       s.stream_tail.clear();
                       set_worker(p.session, s.phase);
                   },
                   [&](const TurnFinished& p) {
                       auto& s = session(p.session);
                       s.usage += p.usage;
                       s.cost_usd += p.cost_usd;
                       data_.usage += p.usage;
                       data_.cost_usd += p.cost_usd;
                       s.phase = p.stop_reason == "tool_use" ? "running tools" : "turn done";
                       set_worker(p.session, s.phase);
                   },
                   [&](const StreamDelta& p) {
                       auto& s = session(p.session);
                       s.phase = p.kind == "thinking" ? "thinking" : "writing";
                       s.stream_tail += p.text;
                       if (s.stream_tail.size() > kStreamTail) s.stream_tail.erase(0, s.stream_tail.size() - kStreamTail);
                       set_worker(p.session, s.phase);
                   },
                   [&](const ToolCallStarted& p) {
                       auto& s = session(p.session);
                       ++s.tool_calls;
                       ++data_.tool_calls;
                       s.last_tool = p.tool;
                       s.phase = p.tool == "compile_and_diff" || p.tool == "submit_result" ? "compiling" : "running " + p.tool;
                       set_worker(p.session, s.phase);
                   },
                   [&](const ToolCallFinished& p) {
                       auto& s = session(p.session);
                       s.last_tool_summary = p.summary;
                       if (p.is_error) {
                           data_.errors.push_back(std::format("{}: {} failed: {}", name_of(p.session), p.tool, p.summary));
                           while (data_.errors.size() > 50) data_.errors.pop_front();
                       }
                       activity(e, std::format("{}: turn {} {} -> {}", name_of(p.session), s.turn, p.tool, p.summary));
                   },
                   [&](const CompileFinished& p) {
                       auto& s = session(p.session);
                       ++s.compiles;
                       ++data_.compiles;
                       if (!p.ok) ++s.compile_errors;
                   },
                   [&](const DiffComputed& p) {
                       auto& s = session(p.session);
                       s.last_match = p.match_percent;
                       s.best_match = std::max(s.best_match, p.match_percent);
                       s.scores.push_back(p.match_percent);
                       if (p.byte_exact) s.matched = true;
                   },
                   [&](const Retry& p) {
                       auto& s = session(p.session);
                       ++s.retries;
                       ++data_.retries;
                       s.phase = "backoff";
                       set_worker(p.session, s.phase);
                       activity(e, std::format("{}: retry {} in {} ms ({})", name_of(p.session), p.attempt, p.delay_ms, p.error));
                   },
                   [&](const Refusal& p) {
                       auto& s = session(p.session);
                       s.refusal_category = p.category;
                       ++data_.refusals;
                       activity(e, std::format("{}: request declined (category: {})", name_of(p.session), p.category.empty() ? "unknown" : p.category));
                   },
                   [&](const Guidance& p) { activity(e, std::format("{}: guidance sent: {}", name_of(p.session), p.text)); },
                   [&](const StatusChanged& p) { activity(e, std::format("{} -> {}", p.function, p.status)); },
                   [&](const FileWritten& p) {
                       data_.files_written.push_back(p.path);
                       activity(e, std::format("wrote {} ({})", p.path, p.reason));
                   },
                   [&](const LogLine& p) {
                       if (p.level == "error" || p.level == "warn") {
                           data_.errors.push_back(p.message);
                           while (data_.errors.size() > 50) data_.errors.pop_front();
                       }
                   },
               },
               e.payload);
}

RunState RunState::replay(const std::vector<Event>& events) {
    RunState s;
    for (const auto& e : events) s.apply(e);
    return s;
}

} // namespace decomp::events
