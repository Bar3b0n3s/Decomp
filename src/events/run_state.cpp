#include "events/run_state.hpp"

#include "events/bus.hpp"

#include <algorithm>
#include <format>

namespace decomp::events {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <class T>
void push_capped(std::deque<T>& d, T value, usize cap) {
    d.push_back(std::move(value));
    while (d.size() > cap) d.pop_front();
}

void append_capped(std::string& s, std::string_view text, usize cap) {
    s += text;
    if (s.size() > cap) s.erase(0, s.size() - cap);
}

} // namespace

SessionState& RunState::session(const std::string& id) {
    auto& ptr = data_.sessions[id];
    if (!ptr) {
        ptr = std::make_shared<SessionState>();
        ptr->id = id;
        ptr->gen = frozen_gen_;
    } else if (ptr->gen < frozen_gen_) {
        // Shared with a snapshot: change a copy.
        ptr = std::make_shared<SessionState>(*ptr);
        ptr->gen = frozen_gen_;
    }
    return *ptr;
}

void RunState::activity(const Event& e, std::string line) {
    auto t = std::chrono::floor<std::chrono::seconds>(e.time);
    push_capped(data_.activity, std::format("[{:%H:%M:%S}] {}", t, line), kActivityLines);
    ++data_.activity_total;
}

void RunState::error_line(std::string line) { push_capped(data_.errors, std::move(line), kErrors); }

void RunState::set_worker_phase(const Event& e, const std::string& session_id, const std::string& phase) {
    if (e.worker < 0) return;
    auto& w = data_.workers[e.worker];
    w.id = e.worker;
    const bool changed = w.phase != phase || w.session != session_id;
    w.session = session_id;
    if (!session_id.empty())
        if (const auto* s = data_.session(session_id)) w.function = s->display.empty() ? s->function : s->display;
    if (session_id.empty()) w.function.clear();
    if (!changed) return;
    if (!w.spans.empty() && w.spans.back().end == TimePoint{}) w.spans.back().end = e.time;
    w.phase = phase;
    w.phase_since = e.time;
    push_capped(w.spans, PhaseSpan{phase, w.function, e.time, {}}, kSpans);
}

MinuteStats& RunState::minute(const Event& e) {
    const i64 key = std::chrono::duration_cast<std::chrono::minutes>(e.time.time_since_epoch()).count();
    auto& m = data_.minutes[key];
    while (data_.minutes.size() > kMinutes && data_.minutes.begin()->first != key) data_.minutes.erase(data_.minutes.begin());
    return m;
}

void RunState::apply(const Event& e) {
    data_.last_seq = std::max(data_.last_seq, e.seq);
    if (data_.run_id.empty()) data_.run_id = e.run;
    auto name_of = [&](const std::string& session_id) {
        const auto* s = data_.session(session_id);
        return s ? (s->display.empty() ? s->function : s->display) : session_id;
    };

    std::visit(Overloaded{
                   [&](const RunStarted& p) {
                       data_.project = p.project;
                       data_.model = p.model;
                       data_.effort = p.effort;
                       data_.worker_count = p.workers;
                       data_.planned = p.functions;
                       data_.planned_vas = p.vas;
                       data_.config = p.config;
                       data_.started = e.time;
                       data_.status = "running";
                       activity(e, std::format("run started: {} function(s), {} worker(s), model {} ({})", p.functions.size(), p.workers,
                                               p.model, p.effort));
                   },
                   [&](const RunFinished& p) {
                       data_.status = p.status;
                       data_.ended = e.time;
                       for (auto& [id, w] : data_.workers) {
                           Event idle = e;
                           idle.worker = id;
                           set_worker_phase(idle, {}, "idle");
                       }
                       activity(e, std::format("run {}: {} matched of {} finished, ${:.2f}", p.status, data_.matched, data_.finished, data_.cost_usd));
                   },
                   [&](const RunResumed& p) {
                       data_.interrupted = p.interrupted;
                       data_.status = "running";
                       // Sessions that were cut off by the interruption are over.
                       for (auto& [id, ptr] : data_.sessions)
                           if (!ptr->finished) {
                               auto& s = session(id);
                               s.finished = true;
                               s.outcome = "interrupted";
                               s.phase = "done";
                               s.ended = e.time;
                           }
                       activity(e, std::format("run resumed: {} interrupted session(s) restart", p.interrupted.size()));
                   },
                   [&](const SessionStarted& p) {
                       // The dispatched function leaves the queue of pending work.
                       if (std::ranges::any_of(*data_.queue, [&](const QueueEntry& q) { return q.va == p.va; })) {
                           auto rest = std::make_shared<std::vector<QueueEntry>>();
                           for (const auto& q : *data_.queue)
                               if (q.va != p.va) rest->push_back(q);
                           data_.queue = std::move(rest);
                       }
                       auto& s = session(p.session);
                       s.function = p.function;
                       s.display = p.display;
                       s.va = p.va;
                       s.worker = e.worker;
                       s.transcript = p.transcript;
                       s.started = e.time;
                       s.phase = "starting";
                       set_worker_phase(e, p.session, "starting");
                       activity(e, std::format("{}: session started", p.display.empty() ? p.function : p.display));
                   },
                   [&](const SessionFinished& p) {
                       auto& s = session(p.session);
                       const bool counted = s.finished;
                       s.finished = true;
                       s.outcome = p.outcome;
                       s.detail = p.detail;
                       s.best_match = std::max(s.best_match, p.best_match);
                       s.turn = std::max(s.turn, p.turns);
                       s.phase = "done";
                       s.ended = e.time;
                       s.matched = p.outcome == "matched";
                       s.live_text.clear();
                       s.live_thinking.clear();
                       if (!counted) {
                           ++data_.finished;
                           if (s.matched) ++data_.matched;
                       }
                       set_worker_phase(e, {}, "idle");
                       activity(e, std::format("{}: {} (best {:.1f}%, {} turns, ${:.2f}){}", name_of(p.session), p.outcome, s.best_match,
                                               p.turns, s.cost_usd, p.detail.empty() ? "" : " - " + p.detail));
                   },
                   [&](const TurnStarted& p) {
                       auto& s = session(p.session);
                       s.turn = p.turn;
                       s.phase = "waiting for model";
                       s.stream_tail.clear();
                       s.live_text.clear();
                       s.live_thinking.clear();
                       set_worker_phase(e, p.session, s.phase);
                   },
                   [&](const TurnFinished& p) {
                       auto& s = session(p.session);
                       s.usage += p.usage;
                       s.cost_usd += p.cost_usd;
                       if (!p.model.empty()) s.model = p.model;
                       s.last_ttft_ms = p.ttft_ms;
                       if (p.had_fallback) {
                           ++s.fallback_turns;
                           ++data_.fallback_turns;
                       }
                       data_.usage += p.usage;
                       data_.cost_usd += p.cost_usd;
                       auto& m = minute(e);
                       ++m.turns;
                       m.output_tokens += p.usage.output;
                       m.cost_usd += p.cost_usd;
                       if (p.ttft_ms > 0) {
                           m.ttft_sum_ms += p.ttft_ms;
                           ++m.ttft_count;
                       }
                       s.phase = p.stop_reason == "tool_use" ? "running tools" : "turn done";
                       set_worker_phase(e, p.session, s.phase);
                   },
                   [&](const StreamDelta& p) {
                       // Deltas are not in the event log, so they only feed the live buffers; phase changes arrive as
                       // worker_phase_changed events and replay into the same timeline.
                       auto& s = session(p.session);
                       append_capped(p.kind == "thinking" ? s.live_thinking : s.live_text, p.text, kLiveText);
                       if (p.kind != "thinking") append_capped(s.stream_tail, p.text, kStreamTail);
                   },
                   [&](const ToolCallStarted& p) {
                       auto& s = session(p.session);
                       ++s.tool_calls;
                       ++data_.tool_calls;
                       s.last_tool = p.tool;
                       s.phase = p.tool == "compile_and_diff" || p.tool == "submit_result" ? "compiling" : "running " + p.tool;
                       set_worker_phase(e, p.session, s.phase);
                   },
                   [&](const ToolCallFinished& p) {
                       auto& s = session(p.session);
                       s.last_tool_summary = p.summary;
                       if (p.is_error) error_line(std::format("{}: {} failed: {}", name_of(p.session), p.tool, p.summary));
                       activity(e, std::format("{}: turn {} {} -> {}", name_of(p.session), s.turn, p.tool, p.summary));
                   },
                   [&](const CompileStarted& p) {
                       if (!p.session.empty()) {
                           auto& s = session(p.session);
                           s.phase = "compiling";
                           set_worker_phase(e, p.session, s.phase);
                       }
                   },
                   [&](const CompileFinished& p) {
                       if (!p.session.empty()) {
                           auto& s = session(p.session);
                           ++s.compiles;
                           if (!p.ok) ++s.compile_errors;
                       }
                       ++data_.compiles;
                       ++minute(e).compiles;
                       auto rec = std::make_shared<CompileRecord>();
                       rec->time = e.time;
                       rec->session = p.session;
                       rec->toolchain = p.toolchain;
                       rec->command = p.command;
                       rec->output = p.output;
                       rec->ok = p.ok;
                       rec->cached = p.cached;
                       rec->duration_ms = p.duration_ms;
                       rec->exit_code = p.exit_code;
                       rec->errors = p.errors;
                       push_capped(data_.recent_compiles, std::shared_ptr<const CompileRecord>(std::move(rec)), kCompiles);
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
                       ++minute(e).retries;
                       s.phase = "backoff";
                       set_worker_phase(e, p.session, s.phase);
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
                       push_capped(data_.files_written, FileRecord{e.time, p}, kFiles);
                       activity(e, std::format("wrote {} ({})", p.path, p.reason));
                   },
                   [&](const LogLine& p) {
                       auto rec = std::make_shared<LogRecord>();
                       rec->time = e.time;
                       rec->level = p.level;
                       rec->message = p.message;
                       rec->session = p.session;
                       rec->worker = e.worker;
                       push_capped(data_.log_tail, std::shared_ptr<const LogRecord>(std::move(rec)), kLogTail);
                       if (p.level == "error" || p.level == "warn") error_line(p.message);
                   },
                   [&](const WorkerPhaseChanged& p) {
                       if (!p.session.empty() && data_.sessions.contains(p.session)) session(p.session).phase = p.phase;
                       if (e.worker < 0) return;
                       set_worker_phase(e, p.session, p.phase);
                       if (!p.function.empty()) data_.workers[e.worker].function = p.function;
                   },
                   [&](const RateLimitUpdated& p) {
                       data_.rate_limit.last = p;
                       data_.rate_limit.time = e.time;
                       data_.rate_limit.known = true;
                   },
                   [&](const BudgetChanged& p) {
                       (p.scope == "function" ? data_.budget.function : data_.budget.run) = p;
                       activity(e, std::format("{} budget set: ${:.2f}{}", p.scope, p.usd, p.usd == 0 ? " (unlimited)" : ""));
                   },
                   [&](const SymbolChanged& p) {
                       push_capped(data_.symbol_changes, SymbolChangeRecord{e.time, p}, kSymbolChanges);
                       activity(e, std::format("symbol {:#x}: {} -> {} ({})", p.va, p.old_name.empty() ? "(new)" : p.old_name,
                                               p.new_name.empty() ? "(removed)" : p.new_name, p.source));
                   },
                   [&](const ApprovalRequested& p) {
                       auto& a = data_.approvals[p.id];
                       const bool was_pending = a.verdict == "pending" && a.request.id == p.id;
                       a.request = p;
                       a.verdict = "pending";
                       a.requested = e.time;
                       if (!was_pending) ++data_.approvals_pending;
                       activity(e, std::format("approval requested: {} for {}", p.action, p.function));
                   },
                   [&](const ApprovalDecided& p) {
                       auto& a = data_.approvals[p.id];
                       if (a.verdict == "pending" && a.request.id == p.id && data_.approvals_pending > 0) --data_.approvals_pending;
                       a.request.id = p.id;
                       a.verdict = p.verdict;
                       a.by = p.by;
                       a.reason = p.reason;
                       a.decided = e.time;
                       activity(e, std::format("approval {} {} by {}{}", p.id, p.verdict, p.by, p.reason.empty() ? "" : ": " + p.reason));
                   },
                   [&](const QueueUpdated& p) { data_.queue = std::make_shared<const std::vector<QueueEntry>>(p.items); },
                   [&](const Control& p) {
                       push_capped(data_.controls, ControlRecord{e.time, p}, kControls);
                       // Run-wide commands change what the run is doing (per-worker ones name a target).
                       if (p.target.empty() && (data_.status == "running" || data_.status == "paused" || data_.status == "stopping")) {
                           if (p.command == "pause") data_.status = "paused";
                           else if (p.command == "resume") data_.status = "running";
                           else if (p.command == "stop") data_.status = "stopping";
                           else if (p.command == "abort") data_.status = "aborting";
                       }
                       activity(e, std::format("{}{}{}", p.command, p.target.empty() ? "" : " " + p.target, p.detail.empty() ? "" : ": " + p.detail));
                   },
               },
               e.payload);
}

RunState RunState::replay(const std::vector<Event>& events) {
    RunState s;
    for (const auto& e : events) s.apply(e);
    return s;
}

RunStateStore::~RunStateStore() { detach(); }

void RunStateStore::attach(EventBus& bus) {
    detach();
    bus_ = &bus;
    subscription_ = bus.subscribe([this](const Event& e) { apply(e); });
}

void RunStateStore::detach() {
    if (bus_) {
        bus_->unsubscribe(subscription_);
        bus_ = nullptr;
    }
}

void RunStateStore::apply(const Event& e) {
    std::lock_guard lock(mutex_);
    state_.apply(e);
    ++version_;
}

std::shared_ptr<const RunStateData> RunStateStore::snapshot() {
    std::lock_guard lock(mutex_);
    if (snapshot_ && snapshot_version_ == version_) return snapshot_;
    state_.freeze();
    snapshot_ = std::make_shared<const RunStateData>(state_.data());
    snapshot_version_ = version_;
    return snapshot_;
}

u64 RunStateStore::version() const {
    std::lock_guard lock(mutex_);
    return version_;
}

} // namespace decomp::events
