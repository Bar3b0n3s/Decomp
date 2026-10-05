#pragma once

#include "agent/approvals.hpp"
#include "analysis/program.hpp"
#include "core/json.hpp"
#include "events/bus.hpp"
#include "matching/match.hpp"
#include "project/project.hpp"
#include "project/units.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace decomp::agent {

// Result of one decompilation tool call, independent of the API client's types.
struct ToolOutput {
    std::string text;
    bool is_error = false;
    bool end_session = false;
    Json outcome;  // set by submit_result

    static ToolOutput ok(std::string text) {
        ToolOutput o;
        o.text = std::move(text);
        return o;
    }
    static ToolOutput error(std::string text) {
        ToolOutput o;
        o.text = std::move(text);
        o.is_error = true;
        return o;
    }
};

struct MatchAttempt {
    int number = 0;
    bool compiled = false;
    double match_percent = 0;
    bool byte_exact = false;
    std::string summary;
    std::string source;
};

// One function being matched: its tools, its brief, and what the attempts produced.
class MatchSession {
public:
    MatchSession(const Program& program, const project::Project* project, matching::MatchSetup setup, u64 va,
                 events::EventBus* bus = nullptr, std::string session_id = {}, int worker = -1);

    // Tool name -> input schema (strict-compatible: additionalProperties false, every field required).
    static Json tool_schemas();
    static std::string tool_description(std::string_view name);
    ToolOutput call(std::string_view tool, const Json& input);

    ToolOutput compile_and_diff(const Json& input);
    ToolOutput disassemble(const Json& input);
    ToolOutput read_memory(const Json& input);
    ToolOutput lookup_symbol(const Json& input);
    ToolOutput record_note(const Json& input);
    ToolOutput submit_result(const Json& input);

    // Saving a verified match asks this gate first (the setup's `cancelled` ends a wait).
    void set_approvals(std::shared_ptr<ApprovalGate> approvals) { approvals_ = std::move(approvals); }

    // First user message: target, toolchain, annotated listing, references, history.
    std::string brief() const;
    // One line appended after tool results each turn.
    std::string status_line(int turns_left) const;

    u64 va() const { return va_; }
    const Symbol& symbol() const { return symbol_; }
    double best_match() const { return best_match_; }
    const std::optional<std::string>& best_source() const { return best_source_; }
    bool matched() const { return matched_; }
    const std::string& matched_source() const { return matched_source_; }
    const std::vector<MatchAttempt>& attempts() const { return attempts_; }
    const std::string& session_id() const { return session_id_; }
    // The source of this session's latest byte-exact attempt that the supervisor has not declined, while
    // no match has been accepted (the runner saves it when the session ends without submitting it).
    std::optional<std::string> unsubmitted_exact_source() const;
    // The function's translation unit when the project has a source file for it: candidates are then
    // composed into the unit's source, compiled and verified with it, and saved there.
    const std::optional<Unit>& unit() const { return unit_; }
    // The project file the accepted match was written to (relative to the project), once one was.
    std::optional<std::string> written_path() const;

private:
    struct Evaluation {
        matching::CandidateResult result;
        std::string text;
        std::optional<project::UnitChange> unit;  // the unit source with the candidate composed in
    };
    Result<Evaluation> evaluate(const std::string& source);
    void publish(events::Payload payload) const;

    const Program& program_;
    const project::Project* project_;
    matching::MatchSetup setup_;
    u64 va_;
    Symbol symbol_;
    events::EventBus* bus_;
    std::string session_id_;
    int worker_;

    mutable std::mutex mutex_;
    std::vector<MatchAttempt> attempts_;
    double best_match_ = 0;
    std::optional<std::string> best_source_;
    bool matched_ = false;
    std::string matched_source_;
    std::vector<std::string> session_notes_;
    std::shared_ptr<ApprovalGate> approvals_;
    std::vector<std::string> declined_;  // sources the supervisor declined to save
    std::optional<Unit> unit_;
    std::optional<std::string> written_path_;
};

// The frozen system prompt (identical for every function so the prompt cache is shared).
const std::string& system_prompt();

} // namespace decomp::agent
