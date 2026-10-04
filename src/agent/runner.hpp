#pragma once

// Runs the built-in agent on one function: a MatchSession's tools wrapped into a ToolRegistry, the
// conversation, the tool-use loop, events for the supervision views, a JSONL transcript, and the
// project's status/history updates.

#include "agent/approvals.hpp"
#include "agent/client.hpp"
#include "agent/conversation.hpp"
#include "agent/loop.hpp"
#include "analysis/program.hpp"
#include "events/bus.hpp"
#include "matching/match.hpp"
#include "project/project.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace decomp::agent {

struct AgentRunConfig {
    ConversationSettings conversation;
    LoopConfig loop;  // finish_tool and status_line are set by run_function
    ClientConfig client;
    std::shared_ptr<HttpTransport> transport;  // the default HTTPS transport when null
    // Supervisor guidance sent with the brief in the first message (later guidance goes through
    // LoopControl::inject and joins the next turn's tool results).
    std::vector<std::string> guidance;
    // Session id for events and the transcript; default "<run>-<va hex>".
    std::string session_id;
    // Asked before a verified match is saved; null: saved without asking.
    std::shared_ptr<ApprovalGate> approvals;
    // Called once, when the API starts answering the session's first request (the run controller
    // staggers worker start-up on it). Runs on the session's thread.
    std::function<void()> on_first_message;
};

// Model, effort, fallbacks and budgets from a project's agent settings.
AgentRunConfig run_config_from(const project::AgentSettings& settings);

struct FunctionRunResult {
    // matched, gave_up, refused, budget_exhausted, run_budget_exhausted, max_turns, no_result (stopped
    // talking without submitting), stopped, skipped, aborted, error
    std::string outcome;
    std::string detail;
    bool matched = false;
    double best_match = 0;
    int turns = 0;
    double cost_usd = 0;
    Usage usage;
    std::optional<std::string> best_source;
    std::filesystem::path matched_source;  // written into the project when matched
    StopReason stop_reason = StopReason::user;  // when stopped or aborted
    bool auto_submitted = false;  // matched by saving a byte-exact attempt the model did not submit
};

// Runs one session to completion. `transcript_path` (may be empty) receives one JSON record per line:
// a session header, the first request in full, then only the messages each later request appends,
// every response, tool call, retry and supervisor guidance, and the outcome. The API key is never
// recorded. An abort through `control` also cancels a compile in progress and an approval wait. When
// the session ends without submitting a byte-exact attempt (and was not aborted), that attempt is
// verified and saved as if it had been submitted.
FunctionRunResult run_function(const Program& program, project::Project* project, const matching::MatchSetup& setup, u64 va,
                               const AgentRunConfig& config, events::EventBus& bus, const std::filesystem::path& transcript_path,
                               LoopControl* control = nullptr, int worker = 0);

} // namespace decomp::agent
