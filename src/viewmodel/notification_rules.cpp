#include "viewmodel/notification_rules.hpp"

#include "core/strings.hpp"
#include "run/store.hpp"

#include <algorithm>
#include <format>
#include <limits>

namespace decomp::vm {

std::string_view to_string(Severity severity) {
    switch (severity) {
    case Severity::info: return "info";
    case Severity::warning: return "warning";
    case Severity::error: return "error";
    }
    return "info";
}

namespace {

// 401/403 answers (Client::describe_http_error: "HTTP 401 authentication_error: ..."), and a missing key.
bool is_auth_error(std::string_view text) {
    return text.find("HTTP 401") != std::string_view::npos || text.find("HTTP 403") != std::string_view::npos ||
           text.find("authentication_error") != std::string_view::npos || text.find("permission_error") != std::string_view::npos ||
           text.find("ANTHROPIC_API_KEY is not set") != std::string_view::npos;
}

// The same failure seen by several sessions differs only in its request id.
std::string without_request_id(std::string_view text) {
    std::string out(text);
    for (usize at = out.find(" (request-id "); at != std::string::npos; at = out.find(" (request-id ", at)) {
        const usize close = out.find(')', at);
        if (close == std::string::npos) break;
        out.erase(at, close + 1 - at);
    }
    return out;
}

std::string clip(std::string_view text, usize max = 160) {
    std::string out(trim(text));
    if (auto nl = out.find('\n'); nl != std::string::npos) out.resize(nl);
    if (out.size() > max) {
        usize cut = max;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out = out.substr(0, cut) + "...";
    }
    return out;
}

std::string name_of(const events::SessionState& s) { return s.display.empty() ? s.function : s.display; }

double budget_usd(const events::BudgetChanged& set, std::string_view scope, const Json& config, std::initializer_list<const char*> path) {
    if (set.scope == scope) return set.usd;
    const Json* j = &config;
    for (const char* key : path) {
        if (!j->is_object()) return 0;
        auto it = j->find(key);
        if (it == j->end()) return 0;
        j = &*it;
    }
    return j->is_number() ? j->get<double>() : 0;
}

// Why a compile record points at the toolchain rather than at the candidate source; nullopt for an
// ordinary result (success, compile errors, a cancelled compile, a cached failure).
std::optional<std::string> toolchain_failure(const events::CompileRecord& c) {
    if (c.ok || c.cached || c.output.find("[compile cancelled]") != std::string::npos) return std::nullopt;
    if (c.output.find("[compiler timed out after") != std::string::npos) return "timeout";
    if (c.exit_code < -1) return "crash";                         // a Windows exception code (NTSTATUS)
    if (c.exit_code > 128 && c.exit_code <= 192) return "crash";  // killed by a signal (POSIX: 128 + signal)
    if (c.exit_code == 0) return "no_object";
    if (c.exit_code > 0 && c.errors == 0) return "no_diagnostics";
    return std::nullopt;
}

std::string toolchain_text(const std::string& kind, const events::CompileRecord& c) {
    const std::string tc = c.toolchain.empty() ? std::string("the compiler") : "toolchain " + c.toolchain;
    if (kind == "timeout") return std::format("Compile timed out ({}, after {:.0f} s)", tc, static_cast<double>(c.duration_ms) / 1000.0);
    if (kind == "crash") return std::format("The compiler crashed ({}, exit code {:#x})", tc, static_cast<u32>(c.exit_code));
    if (kind == "no_object") return std::format("The compiler reported success but wrote no object ({})", tc);
    return std::format("The compiler failed without diagnostics ({}, exit code {}): {}", tc, c.exit_code, clip(c.output));
}

constexpr std::string_view kStartFailure = " failed: internal error: ";

} // namespace

NotificationRules::NotificationRules(NotificationOptions options) : options_(options) {}

void NotificationRules::reset() {
    seen_.clear();
    batches_.clear();
    last_run_.clear();
    last_seq_ = 0;
    scanned_ = false;
}

bool NotificationRules::waiting() const {
    return std::ranges::any_of(batches_, [](const auto& b) { return !b.second.items.empty(); });
}

void NotificationRules::offer(Notification n, bool emit, std::vector<Notification>& out) {
    if (!seen_.insert(n.key).second || !emit) return;
    out.push_back(std::move(n));
}

void NotificationRules::offer_batched(Notification n, std::string subject, bool emit, TimePoint now) {
    if (!seen_.insert(n.key).second || !emit) return;
    Batch& b = batches_[std::format("{}/{}", n.kind, to_string(n.severity))];
    if (b.items.empty()) b.first_seen = now;
    b.items.push_back(Batch::Item{std::move(n), std::move(subject)});
}

void NotificationRules::flush(TimePoint now, bool all, std::vector<Notification>& out) {
    for (auto& [key, batch] : batches_) {
        if (batch.items.empty()) continue;
        if (!all && now - batch.first_seen < options_.batch_window) continue;
        std::vector<Batch::Item> items = std::exchange(batch.items, {});
        if (items.size() < std::max<usize>(options_.batch_summary, 2)) {
            for (auto& item : items) out.push_back(std::move(item.notification));
            continue;
        }
        Notification summary;
        const Notification& first = items.front().notification;
        summary.kind = first.kind;
        summary.severity = first.severity;
        summary.sticky = first.sticky;
        summary.toast = first.toast;
        summary.key = std::format("batch:{}:{}:{}", first.kind, first.key, items.size());
        std::vector<std::string> names;
        for (const auto& item : items) {
            summary.time = std::max(summary.time, item.notification.time);
            if (names.size() < 3) names.push_back(item.subject);
        }
        const std::string more = items.size() > names.size() ? std::format(" and {} more", items.size() - names.size()) : std::string();
        if (first.kind == "matched") {
            summary.text = std::format("{} functions matched", items.size());
            summary.link = {"function_browser", std::nullopt, "", "status:matched"};
        } else if (first.kind == "gave_up") {
            summary.text = std::format("{} functions gave up", items.size());
            summary.link = {"function_browser", std::nullopt, "", "status:gave_up"};
        } else if (first.kind == "refused") {
            summary.text = std::format("{} functions were refused", items.size());
            summary.link = {"function_browser", std::nullopt, "", "status:refused"};
        } else {
            summary.text = first.severity == Severity::error ? std::format("{} sessions reached their function budget", items.size())
                                                             : std::format("{} sessions used {:.0f}% of their function budget", items.size(),
                                                                           options_.budget_warning * 100);
            summary.link = {"cost", std::nullopt, "", ""};
        }
        summary.text += ": " + join(names, ", ") + more;
        out.push_back(std::move(summary));
    }
}

void NotificationRules::scan(const events::RunStateData& state, TimePoint now, bool emit, std::vector<Notification>& out) {
    const std::string& run = state.run_id;
    const double fn_budget = budget_usd(state.budget.function, "function", state.config, {"limits", "max_usd"});
    const double run_budget = budget_usd(state.budget.run, "run", state.config, {"run_budget_usd"});
    const double warn = options_.budget_warning;

    for (const auto& [id, ptr] : state.sessions) {
        const events::SessionState& s = *ptr;
        const std::string name = name_of(s);
        const TimePoint when = s.ended != TimePoint{} ? s.ended : s.started;
        if (s.finished) {
            Notification n;
            n.time = when;
            n.link.va = s.va;
            n.link.session = s.id;
            if (s.outcome == "matched") {
                n.kind = "matched";
                n.key = std::format("matched:{}:{}", run, s.id);
                n.text = std::format("Matched {} ({} turns, {})", name, s.turn, format_usd(s.cost_usd));
                n.link.view = "diff_viewer";
                offer_batched(std::move(n), name, emit, now);
            } else if (s.outcome == "gave_up") {
                n.kind = "gave_up";
                n.key = std::format("gave_up:{}:{}", run, s.id);
                n.text = s.detail.empty() ? std::format("Gave up on {}", name) : std::format("Gave up on {}: {}", name, clip(s.detail, 100));
                n.link.view = "agent_session";
                offer_batched(std::move(n), name, emit, now);
            } else if (s.outcome == "refused") {
                n.kind = "refused";
                n.key = std::format("refused:{}:{}", run, s.id);
                n.severity = Severity::warning;
                n.text = std::format("{} was refused{}", name, s.refusal_category.empty() ? "" : " (category: " + s.refusal_category + ")");
                n.link.view = "agent_session";
                n.link.anchor = std::format("turn:{}", s.turn);
                offer_batched(std::move(n), name, emit, now);
            } else if (s.outcome == "error" && is_auth_error(s.detail)) {
                n.kind = "auth";
                n.key = std::format("auth:{}:{}", run, without_request_id(s.detail));
                n.severity = Severity::error;
                n.sticky = true;
                n.text = "Authentication failed: " + clip(without_request_id(s.detail));
                n.link = {"settings", std::nullopt, "", ""};
                offer(std::move(n), emit, out);
            }
        }
        // Function budget: the session's spend against the per-function limit; an exhausted budget of
        // any kind (tokens, time) ends the session as budget_exhausted.
        const bool exhausted = (s.finished && s.outcome == "budget_exhausted") || (fn_budget > 0 && s.cost_usd >= fn_budget);
        if (exhausted) {
            Notification n;
            n.kind = "budget";
            n.key = std::format("budget100:function:{}:{}", run, s.id);
            n.severity = Severity::error;
            n.sticky = true;
            n.time = when;
            n.text = s.detail.empty() || s.outcome != "budget_exhausted" ? std::format("{} reached its budget ({})", name, format_usd(s.cost_usd))
                                                                          : std::format("{} reached its budget: {}", name, clip(s.detail, 100));
            n.link = {"cost", s.va, s.id, ""};
            seen_.insert(std::format("budget80:function:{}:{}", run, s.id));
            offer_batched(std::move(n), name, emit, now);
        } else if (fn_budget > 0 && s.cost_usd >= warn * fn_budget) {
            Notification n;
            n.kind = "budget";
            n.key = std::format("budget80:function:{}:{}", run, s.id);
            n.severity = Severity::warning;
            n.time = when;
            n.text = std::format("{} has used {:.0f}% of its budget ({} of {})", name, 100 * s.cost_usd / fn_budget, format_usd(s.cost_usd),
                                 format_usd(fn_budget));
            n.link = {"cost", s.va, s.id, ""};
            offer_batched(std::move(n), name, emit, now);
        }
        if (s.fallback_turns > 0) {
            Notification n;
            n.kind = "fallback";
            n.key = std::format("fallback:{}:{}:{}", run, s.id, s.fallback_turns);
            n.toast = false;
            n.time = when;
            n.text = std::format("A fallback model served turn {} of {}{}", s.turn, name, s.model.empty() ? "" : " (served by " + s.model + ")");
            n.link = {"agent_session", s.va, s.id, std::format("turn:{}", s.turn)};
            offer(std::move(n), emit, out);
        }
    }

    // Run budget (raising it re-arms both thresholds: the keys carry the limit).
    if (run_budget > 0 || state.status == "budget_exhausted") {
        const std::string limit = std::format("{:.4f}", run_budget);
        Notification n;
        n.kind = "budget";
        n.time = state.ended != TimePoint{} ? state.ended : now;
        n.link = {"cost", std::nullopt, "", ""};
        if (state.status == "budget_exhausted" || (run_budget > 0 && state.cost_usd >= run_budget)) {
            seen_.insert(std::format("budget80:run:{}:{}", run, limit));
            n.key = std::format("budget100:run:{}:{}", run, limit);
            n.severity = Severity::error;
            n.sticky = true;
            n.text = run_budget > 0 ? std::format("The run budget is spent ({} of {})", format_usd(state.cost_usd), format_usd(run_budget))
                                    : std::string("The run budget is spent");
            offer(std::move(n), emit, out);
        } else if (state.cost_usd >= warn * run_budget) {
            n.key = std::format("budget80:run:{}:{}", run, limit);
            n.severity = Severity::warning;
            n.text = std::format("The run has used {:.0f}% of its budget ({} of {})", 100 * state.cost_usd / run_budget, format_usd(state.cost_usd),
                                 format_usd(run_budget));
            offer(std::move(n), emit, out);
        }
    }

    // Authentication errors logged during the run.
    for (const auto& line : state.log_tail) {
        if ((line->level != "error" && line->level != "warn") || !is_auth_error(line->message)) continue;
        Notification n;
        n.kind = "auth";
        n.key = std::format("auth:{}:{}", run, without_request_id(line->message));
        n.severity = Severity::error;
        n.sticky = true;
        n.time = line->time;
        n.text = "Authentication failed: " + clip(without_request_id(line->message));
        n.link = {"settings", std::nullopt, "", ""};
        offer(std::move(n), emit, out);
    }

    // Rate-limit storms, from the 429 retries counted per minute.
    if (!state.minutes.empty()) {
        const i64 newest = state.minutes.rbegin()->first;
        bool storm = false;
        i64 previous_minute = std::numeric_limits<i64>::min();
        int previous_count = 0;
        for (const auto& [minute, stats] : state.minutes) {
            if (minute != previous_minute + 1) {
                storm = false;  // a gap is a quiet minute
                previous_count = 0;
            }
            const int count = stats.rate_limited;
            if (storm) {
                if (count == 0) storm = false;
            } else if (count > 0 && count + previous_count >= options_.storm_retries) {
                storm = true;
                if (minute >= newest - 10) {
                    Notification n;
                    n.kind = "rate_limit";
                    n.key = std::format("rate_limit:{}:{}", run, minute);
                    n.severity = Severity::warning;
                    n.time = TimePoint(std::chrono::minutes(minute));
                    n.text = std::format("Rate-limit storm: {} requests were answered 429 within two minutes", count + previous_count);
                    n.link = {"run_monitor", std::nullopt, "", std::format("minute:{}", minute)};
                    offer(std::move(n), emit, out);
                }
            }
            previous_minute = minute;
            previous_count = count;
        }
    }

    // Toolchain failures: compiles that failed for reasons other than the source, one notification per
    // toolchain and kind of failure in a run.
    for (const auto& c : state.recent_compiles) {
        const auto kind = toolchain_failure(*c);
        if (!kind) continue;
        Notification n;
        n.kind = "toolchain";
        n.key = std::format("toolchain:{}:{}:{}", run, c->toolchain, *kind);
        n.severity = Severity::error;
        n.sticky = true;
        n.time = c->time;
        n.text = toolchain_text(*kind, *c);
        n.link = {"toolchains", std::nullopt, c->session, std::format("compile:{}", to_unix_ms(c->time))};
        offer(std::move(n), emit, out);
    }
    // A compiler that cannot be started never reports a compile: the tool call fails with an internal error.
    for (const auto& line : state.errors) {
        const usize at = line.find(kStartFailure);
        if (at == std::string::npos) continue;
        const std::string_view head = std::string_view(line).substr(0, at);
        if (!head.ends_with("compile_and_diff") && !head.ends_with("submit_result")) continue;
        const std::string message = line.substr(at + kStartFailure.size());
        Notification n;
        n.kind = "toolchain";
        n.key = std::format("toolchain:{}:start:{}", run, message);
        n.severity = Severity::error;
        n.sticky = true;
        n.time = now;
        n.text = "The compiler could not run: " + clip(message);
        n.link = {"toolchains", std::nullopt, "", ""};
        offer(std::move(n), emit, out);
    }

    // Approvals waiting for the supervisor.
    for (const auto& [approval_id, a] : state.approvals) {
        if (a.verdict != "pending") continue;
        Notification n;
        n.kind = "approval";
        n.key = std::format("approval:{}:{}", run, approval_id);
        n.severity = Severity::warning;
        n.time = a.requested;
        n.text = std::format("Approval requested: {} for {}", a.request.action, a.request.function.empty() ? a.request.path : a.request.function);
        n.link = {"changes", a.request.va ? std::optional<u64>(a.request.va) : std::nullopt, a.request.session, std::format("approval:{}", approval_id)};
        offer(std::move(n), emit, out);
    }

    // The run ended (each end of a resumed run is its own).
    if (!run::is_live_status(state.status) && !state.status.empty() && state.ended != TimePoint{}) {
        usize matched = 0, gave_up = 0, refused = 0;
        for (const auto& [va, s] : latest_sessions(state)) {
            matched += s->outcome == "matched";
            gave_up += s->outcome == "gave_up";
            refused += s->outcome == "refused";
        }
        Notification n;
        n.kind = "run_finished";
        n.key = std::format("run_finished:{}:{}", run, to_unix_ms(state.ended));
        n.time = state.ended;
        n.text = std::format("Run {}: {} matched, {} gave up, {} refused, {}, {}", state.status, matched, gave_up, refused, format_usd(state.cost_usd),
                             format_duration(seconds_between(state.started, state.ended)));
        n.link = {"dashboard", std::nullopt, "", ""};
        offer(std::move(n), emit, out);
    }
}

std::vector<Notification> NotificationRules::update(const events::RunStateData& state, TimePoint now) {
    std::vector<Notification> out;
    if (scanned_ && state.run_id != last_run_) flush(now, true, out);  // another run: post what waits
    if (!scanned_ || state.run_id != last_run_ || state.last_seq != last_seq_) {
        scan(state, now, true, out);
        last_run_ = state.run_id;
        last_seq_ = state.last_seq;
        scanned_ = true;
    }
    flush(now, !run::is_live_status(state.status), out);
    // The run's summary comes after the batches it concludes.
    std::ranges::stable_partition(out, [](const Notification& n) { return n.kind != "run_finished"; });
    return out;
}

void NotificationRules::prime(const events::RunStateData& state) {
    std::vector<Notification> ignored;
    scan(state, TimePoint{}, false, ignored);
    last_run_ = state.run_id;
    last_seq_ = state.last_seq;
    scanned_ = true;
}

std::optional<Notification> health_check_notification(const std::string& toolchain, const matching::HealthReport& report, TimePoint now) {
    if (report.ok) return std::nullopt;
    Notification n;
    n.kind = "toolchain";
    n.key = std::format("health:{}:{}", toolchain, to_unix_ms(now));
    n.severity = Severity::error;
    n.sticky = true;
    n.time = now;
    const std::string why = !trim(report.output).empty() ? clip(report.output) : report.object;
    n.text = std::format("Toolchain {} failed its health check{}", toolchain, why.empty() ? "" : ": " + why);
    n.link = {"toolchains", std::nullopt, "", "toolchain:" + toolchain};
    return n;
}

} // namespace decomp::vm
