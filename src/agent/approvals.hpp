#pragma once

// Supervisor approval for actions the agent takes on the project (today: saving a verified match).
// Each action has a policy: auto (approved at once), ask (the session waits for a decision from the
// supervisor, e.g. the GUI's approval queue) or deny.

#include "core/result.hpp"
#include "core/types.hpp"
#include "events/bus.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::agent {

enum class ApprovalPolicy { automatic, ask, deny };

std::string_view to_string(ApprovalPolicy policy);  // auto, ask, deny
std::optional<ApprovalPolicy> parse_approval_policy(std::string_view text);

// Saving a byte-exact source into the project's src/ tree.
inline constexpr std::string_view kWriteSourceAction = "write_source";

// The policies of decomp.json's agent.approvals with `overrides` ("write_source=deny", e.g. from the
// command line) applied on top. Without `allow_ask` (command-line runs, where nobody can answer) an
// "ask" policy is an error.
Result<std::map<std::string, ApprovalPolicy, std::less<>>> resolve_approval_policies(const std::map<std::string, std::string>& configured,
                                                                                     const std::vector<std::string>& overrides,
                                                                                     bool allow_ask);

struct ApprovalRequest {
    std::string action;  // kWriteSourceAction
    std::string session;
    std::string function;  // display name
    u64 va = 0;
    std::string path;      // project-relative path of the file to write
    std::string summary;   // one line for lists
    std::string content;   // the new content
    std::string previous;  // what it replaces (empty for a new file)
};

struct ApprovalDecision {
    std::string verdict;  // approved, denied, cancelled
    std::string by;       // user, policy, abort, shutdown
    std::string reason;

    bool approved() const { return verdict == "approved"; }
};

struct PendingApproval {
    u64 id = 0;
    ApprovalRequest request;
    std::chrono::system_clock::time_point requested{};
};

class ApprovalGate {
public:
    explicit ApprovalGate(events::EventBus* bus = nullptr) : bus_(bus) {}

    void set_policy(std::string action, ApprovalPolicy policy);
    ApprovalPolicy policy(std::string_view action) const;  // actions without a policy are automatic
    std::map<std::string, ApprovalPolicy, std::less<>> policies() const;

    // auto and deny answer at once (approval_decided by "policy"). ask publishes approval_requested and
    // blocks until decide() or cancel_all(); `cancelled` is polled every 100 ms (an abort ends the wait
    // with verdict "cancelled"). A graceful stop does not end the wait.
    ApprovalDecision request(ApprovalRequest request, const std::function<bool()>& cancelled = {}, int worker = -1);

    // Decides a waiting request; false when the id is unknown or already decided.
    bool decide(u64 id, bool approve, std::string reason = {}, std::string by = "user");
    // Cancels every waiting request (shutdown).
    void cancel_all(std::string reason);
    std::vector<PendingApproval> pending() const;

private:
    struct Waiting {
        PendingApproval item;
        std::optional<ApprovalDecision> decision;
    };

    void publish(events::Payload payload, int worker) const;

    events::EventBus* bus_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, ApprovalPolicy, std::less<>> policies_;
    std::map<u64, std::shared_ptr<Waiting>> waiting_;
    u64 next_id_ = 1;
};

} // namespace decomp::agent
