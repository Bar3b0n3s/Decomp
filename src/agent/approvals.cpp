#include "agent/approvals.hpp"

#include <algorithm>

namespace decomp::agent {

using namespace std::chrono_literals;

std::string_view to_string(ApprovalPolicy policy) {
    switch (policy) {
    case ApprovalPolicy::automatic: return "auto";
    case ApprovalPolicy::ask: return "ask";
    case ApprovalPolicy::deny: return "deny";
    }
    return "auto";
}

std::optional<ApprovalPolicy> parse_approval_policy(std::string_view text) {
    if (text == "auto") return ApprovalPolicy::automatic;
    if (text == "ask") return ApprovalPolicy::ask;
    if (text == "deny") return ApprovalPolicy::deny;
    return std::nullopt;
}

Result<std::map<std::string, ApprovalPolicy, std::less<>>> resolve_approval_policies(const std::map<std::string, std::string>& configured,
                                                                                     const std::vector<std::string>& overrides,
                                                                                     bool allow_ask) {
    std::map<std::string, ApprovalPolicy, std::less<>> out;
    auto add = [&](const std::string& action, const std::string& text, std::string_view origin) -> Result<void> {
        auto policy = parse_approval_policy(text);
        if (!policy) return make_error(ErrorCode::invalid_argument, "{}: approval policy '{}' for '{}' must be auto, ask or deny", origin, text, action);
        if (*policy == ApprovalPolicy::ask && !allow_ask)
            return make_error(ErrorCode::invalid_argument,
                              "{}: '{}' asks for approval, which needs someone to answer (decomp-gui); use auto or deny here", origin,
                              action);
        out[action] = *policy;
        return {};
    };
    for (const auto& [action, text] : configured) {
        // An "ask" in decomp.json is for the GUI; an override can replace it for a command-line run.
        const bool overridden = std::ranges::any_of(overrides, [&](const std::string& o) { return o.starts_with(action + "="); });
        if (!overridden) TRY(add(action, text, "decomp.json agent.approvals"));
    }
    for (const auto& o : overrides) {
        const auto eq = o.find('=');
        if (eq == std::string::npos || eq == 0) return make_error(ErrorCode::invalid_argument, "--policy '{}' must look like write_source=deny", o);
        const std::string action = o.substr(0, eq);
        if (action != kWriteSourceAction) return make_error(ErrorCode::invalid_argument, "--policy: unknown action '{}' (known: write_source)", action);
        TRY(add(action, o.substr(eq + 1), "--policy"));
    }
    return out;
}

void ApprovalGate::set_policy(std::string action, ApprovalPolicy policy) {
    std::lock_guard lock(mutex_);
    policies_[std::move(action)] = policy;
}

ApprovalPolicy ApprovalGate::policy(std::string_view action) const {
    std::lock_guard lock(mutex_);
    auto it = policies_.find(action);
    return it == policies_.end() ? ApprovalPolicy::automatic : it->second;
}

std::map<std::string, ApprovalPolicy, std::less<>> ApprovalGate::policies() const {
    std::lock_guard lock(mutex_);
    return policies_;
}

void ApprovalGate::publish(events::Payload payload, int worker) const {
    if (bus_) bus_->publish(std::move(payload), worker);
}

ApprovalDecision ApprovalGate::request(ApprovalRequest request, const std::function<bool()>& cancelled, int worker) {
    u64 id = 0;
    ApprovalPolicy policy = ApprovalPolicy::automatic;
    std::shared_ptr<Waiting> waiting;
    {
        std::lock_guard lock(mutex_);
        id = next_id_++;
        if (auto it = policies_.find(request.action); it != policies_.end()) policy = it->second;
        if (policy == ApprovalPolicy::ask) {
            waiting = std::make_shared<Waiting>();
            waiting->item = PendingApproval{id, request, std::chrono::system_clock::now()};
            waiting_[id] = waiting;
        }
    }
    if (policy != ApprovalPolicy::ask) {
        ApprovalDecision d = policy == ApprovalPolicy::automatic
                                 ? ApprovalDecision{"approved", "policy", ""}
                                 : ApprovalDecision{"denied", "policy", "the approval policy denies this action"};
        publish(events::ApprovalDecided{id, d.verdict, d.by, d.reason}, worker);
        return d;
    }

    // Published outside the lock: a decision arriving meanwhile is kept until the wait below sees it.
    publish(events::ApprovalRequested{id, request.action, request.session, request.function, request.va, request.path, request.summary},
            worker);
    publish(events::WorkerPhaseChanged{"waiting for approval", request.session, request.function}, worker);
    ApprovalDecision decision;
    {
        std::unique_lock lock(mutex_);
        while (!waiting->decision) {
            if (cancelled && cancelled()) {
                waiting->decision = ApprovalDecision{"cancelled", "abort", "the session was aborted"};
                break;
            }
            cv_.wait_for(lock, 100ms);
        }
        decision = *waiting->decision;
        waiting_.erase(id);
    }
    publish(events::ApprovalDecided{id, decision.verdict, decision.by, decision.reason}, worker);
    return decision;
}

bool ApprovalGate::decide(u64 id, bool approve, std::string reason, std::string by) {
    {
        std::lock_guard lock(mutex_);
        auto it = waiting_.find(id);
        if (it == waiting_.end() || it->second->decision) return false;
        it->second->decision = ApprovalDecision{approve ? "approved" : "denied", std::move(by), std::move(reason)};
    }
    cv_.notify_all();
    return true;
}

void ApprovalGate::cancel_all(std::string reason) {
    {
        std::lock_guard lock(mutex_);
        for (auto& [id, w] : waiting_)
            if (!w->decision) w->decision = ApprovalDecision{"cancelled", "shutdown", reason};
    }
    cv_.notify_all();
}

std::vector<PendingApproval> ApprovalGate::pending() const {
    std::lock_guard lock(mutex_);
    std::vector<PendingApproval> out;
    for (const auto& [id, w] : waiting_)
        if (!w->decision) out.push_back(w->item);
    return out;
}

} // namespace decomp::agent
