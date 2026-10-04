#include "agent/runner.hpp"

#include "agent/cost.hpp"
#include "agent/match_session.hpp"
#include "agent/tools.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"

#include <chrono>
#include <format>
#include <fstream>
#include <mutex>

namespace decomp::agent {

AgentRunConfig run_config_from(const project::AgentSettings& s) {
    AgentRunConfig c;
    c.conversation.model = s.model;
    c.conversation.effort = s.effort;
    c.conversation.fallbacks = s.fallbacks;
    c.loop.max_turns = s.max_turns;
    c.loop.max_cost_usd = s.max_usd_per_function;
    c.loop.max_total_tokens = s.max_tokens_per_function;
    c.loop.max_wall = std::chrono::minutes(s.max_minutes_per_function);
    return c;
}

namespace {

// One JSON record per line. Requests carry no credentials (the key only travels in headers).
class Transcript {
public:
    explicit Transcript(const std::filesystem::path& path) {
        if (path.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        out_.open(path, std::ios::app | std::ios::binary);
        if (!out_) log::warn("cannot write the transcript {}", path.string());
    }
    void write(const Json& record) {
        std::lock_guard lock(mutex_);
        if (!out_.is_open()) return;
        out_ << dump_compact(record) << '\n';
        out_.flush();
    }

private:
    std::ofstream out_;
    std::mutex mutex_;
};

events::TokenUsage to_event_usage(const Usage& u) {
    return {u.input_tokens, u.output_tokens, u.cache_creation_input_tokens, u.cache_read_input_tokens};
}

std::string first_line(const Json& content, usize max = 200) {
    std::string text = content.is_string() ? content.get<std::string>() : dump_compact(content);
    if (auto nl = text.find('\n'); nl != std::string::npos) text.resize(nl);
    return truncate_utf8(text, max);
}

// Loop hooks -> events (for the progress view / GUI) + transcript records.
class Bridge : public LoopObserver {
public:
    Bridge(events::EventBus& bus, Transcript& transcript, std::string session, int worker, std::string model)
        : bus_(bus), transcript_(transcript), session_(std::move(session)), worker_(worker), model_(std::move(model)) {}

    int current_turn() const { return turn_; }

    void on_turn_start(int turn, const Json& request) override {
        turn_ = turn;
        turn_started_ = std::chrono::steady_clock::now();
        bus_.publish(events::TurnStarted{session_, turn}, worker_);
        const Json& messages = request.contains("messages") ? request["messages"] : Json::array();
        if (turn == 1 || messages.size() < logged_messages_) {
            transcript_.write({{"type", "request"}, {"turn", turn}, {"body", request}});
        } else {
            // History is append-only, so the new messages fully describe this request.
            Json added = Json::array();
            for (usize i = logged_messages_; i < messages.size(); ++i) added.push_back(messages[i]);
            transcript_.write({{"type", "request_delta"}, {"turn", turn}, {"messages", std::move(added)}});
        }
        logged_messages_ = messages.size();
    }
    void on_text_delta(int, std::string_view text) override { bus_.publish(events::StreamDelta{session_, "text", std::string(text)}, worker_); }
    void on_thinking_delta(int, std::string_view text) override {
        bus_.publish(events::StreamDelta{session_, "thinking", std::string(text)}, worker_);
    }
    void on_retry(int attempt, const Error& error, std::chrono::milliseconds delay) override {
        bus_.publish(events::Retry{session_, attempt, error.message, delay.count()}, worker_);
        transcript_.write({{"type", "retry"}, {"turn", turn_}, {"attempt", attempt}, {"error", error.describe()}, {"delay_ms", delay.count()}});
    }
    void on_response(int turn, const Response& response) override {
        const ResponseCost cost = response_cost(response, model_);
        if (cost.unknown_model && !warned_price_) {
            warned_price_ = true;
            log::warn("no price known for model '{}': spend and the USD budget are estimated with {} prices", response.model,
                      price_for(model_).model);
        }
        const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - turn_started_);
        bus_.publish(events::TurnFinished{session_, turn, response.stop_reason, to_event_usage(cost.usage), cost.usd, latency.count()}, worker_);
        transcript_.write({{"type", "response"},
                           {"turn", turn},
                           {"id", response.id},
                           {"model", response.model},
                           {"stop_reason", response.stop_reason},
                           {"stop_details", response.stop_details},
                           {"content", response.content},
                           {"usage", response.usage_raw},
                           {"had_fallback", response.had_fallback},
                           {"cost_usd", cost.usd},
                           {"latency_ms", latency.count()},
                           {"request_id", response.header("request-id").value_or("")}});
    }
    void on_tool_start(int turn, const ToolCall& call) override {
        bus_.publish(events::ToolCallStarted{session_, call.id, call.name, turn, call.input}, worker_);
    }
    void on_tool_end(int turn, const ToolCall& call, const ToolResult& result, std::chrono::milliseconds elapsed) override {
        bus_.publish(events::ToolCallFinished{session_, call.id, call.name, result.is_error, first_line(result.content), elapsed.count()}, worker_);
        transcript_.write({{"type", "tool"},
                           {"turn", turn},
                           {"id", call.id},
                           {"name", call.name},
                           {"input", call.input_valid ? call.input : Json(call.raw_input)},
                           {"is_error", result.is_error},
                           {"result", result.content},
                           {"elapsed_ms", elapsed.count()}});
    }
    void on_injected(const std::string& text) override {
        bus_.publish(events::Guidance{session_, text}, worker_);
        transcript_.write({{"type", "guidance"}, {"turn", turn_}, {"text", text}});
    }
    void on_paused() override { transcript_.write({{"type", "paused"}, {"turn", turn_}}); }
    void on_resumed() override { transcript_.write({{"type", "resumed"}, {"turn", turn_}}); }

private:
    events::EventBus& bus_;
    Transcript& transcript_;
    std::string session_;
    int worker_;
    std::string model_;
    int turn_ = 0;
    usize logged_messages_ = 0;
    bool warned_price_ = false;
    std::chrono::steady_clock::time_point turn_started_{};
};

std::string outcome_name(const LoopOutcome& o) {
    switch (o.status) {
    case LoopStatus::finished:
        return o.finish_outcome.is_object() ? json_string_or(o.finish_outcome, "outcome", "no_result") : "no_result";
    case LoopStatus::end_turn_without_finish: return "no_result";
    case LoopStatus::refused: return "refused";
    case LoopStatus::budget_exhausted: return "budget_exhausted";
    case LoopStatus::max_turns: return "max_turns";
    case LoopStatus::aborted: return "aborted";
    case LoopStatus::stopped: return "stopped";
    case LoopStatus::error: return "error";
    }
    return "error";
}

ToolRegistry make_tools(MatchSession& session) {
    ToolRegistry tools;
    const Json schemas = MatchSession::tool_schemas();
    for (auto it = schemas.begin(); it != schemas.end(); ++it) {
        Tool t;
        t.name = it.key();
        t.description = MatchSession::tool_description(t.name);
        t.input_schema = *it;
        // Lookups may run side by side; compiles, notes and the final submission run one at a time.
        t.parallel_safe = t.name == "disassemble" || t.name == "read_memory" || t.name == "lookup_symbol";
        t.handler = [&session, name = t.name](const ToolCall& call) {
            ToolOutput out = session.call(name, call.input);
            ToolResult r = out.is_error ? ToolResult::error(std::move(out.text)) : ToolResult::text(std::move(out.text));
            r.end_session = out.end_session;
            r.outcome = std::move(out.outcome);
            return r;
        };
        tools.add(std::move(t));
    }
    return tools;
}

project::FunctionStatus final_status(const FunctionRunResult& r, const project::FunctionInfo& before) {
    using project::FunctionStatus;
    if (r.matched || before.status == FunctionStatus::matched) return FunctionStatus::matched;
    if (r.outcome == "refused") return FunctionStatus::refused;
    if (r.outcome == "gave_up") return FunctionStatus::gave_up;
    if (r.best_match > 0 || before.best_match > 0) return FunctionStatus::nonmatching;
    return before.status == FunctionStatus::in_progress ? FunctionStatus::unstarted : before.status;
}

} // namespace

FunctionRunResult run_function(const Program& program, project::Project* project, const matching::MatchSetup& setup, u64 va,
                               const AgentRunConfig& config, events::EventBus& bus, const std::filesystem::path& transcript_path,
                               LoopControl* control, int worker) {
    const std::string session_id = bus.run_id().empty() ? std::format("{:x}", va) : std::format("{}-{:x}", bus.run_id(), va);
    MatchSession session(program, project, setup, va, &bus, session_id, worker);
    const Symbol& sym = session.symbol();
    const std::string display = sym.display.empty() ? sym.name : sym.display;
    bus.publish(events::SessionStarted{session_id, sym.name, display, va}, worker);

    // "in_progress" is only announced, not saved: a crash must not leave it behind in symbols.txt.
    const project::FunctionInfo before = project ? project->function_info(va) : project::FunctionInfo{};
    if (before.status != project::FunctionStatus::matched) bus.publish(events::StatusChanged{display, va, "in_progress"}, worker);

    ToolRegistry tools = make_tools(session);
    Conversation conversation(config.conversation, system_prompt(), tools.definitions());
    Transcript transcript(transcript_path);
    Json first = Json::array({Json{{"type", "text"}, {"text", session.brief()}}});
    for (const auto& text : config.guidance) {
        if (trim(text).empty()) continue;
        first.push_back(Json{{"type", "text"}, {"text", "[Supervisor guidance] " + text}});
        bus.publish(events::Guidance{session_id, text}, worker);
        transcript.write({{"type", "guidance"}, {"turn", 0}, {"text", text}});
    }
    conversation.append_user_blocks(std::move(first));

    Bridge bridge(bus, transcript, session_id, worker, config.conversation.model);
    LoopConfig loop = config.loop;
    loop.finish_tool = "submit_result";
    loop.status_line = [&session, &bridge, max = loop.max_turns] { return session.status_line(std::max(0, max - bridge.current_turn())); };

    std::shared_ptr<HttpTransport> transport = config.transport;
    if (!transport) transport = make_default_transport();
    Client client(config.client, transport);
    const LoopOutcome outcome = run_loop(client, conversation, tools, loop, control, &bridge);

    FunctionRunResult result;
    result.outcome = outcome_name(outcome);
    result.detail = outcome.error ? outcome.error->describe() : outcome.detail;
    result.turns = outcome.turns;
    result.cost_usd = outcome.cost_usd;
    result.usage = outcome.usage;
    result.matched = session.matched();
    if (result.matched) result.outcome = "matched";
    if (result.outcome == "gave_up" && outcome.finish_outcome.is_object())
        result.detail = json_string_or(outcome.finish_outcome, "reason", result.detail);
    result.best_match = session.best_match();
    result.best_source = session.best_source();
    if (result.matched && project) result.matched_source = project->matched_source_path(sym);

    if (outcome.status == LoopStatus::refused) {
        const Json& sd = outcome.stop_details;
        bus.publish(events::Refusal{session_id, sd.is_object() ? json_string_or(sd, "category", "") : "",
                                    sd.is_object() ? json_string_or(sd, "explanation", "") : ""},
                    worker);
    }
    transcript.write({{"type", "outcome"},
                      {"outcome", result.outcome},
                      {"detail", result.detail},
                      {"best_match", result.best_match},
                      {"turns", result.turns},
                      {"cost_usd", result.cost_usd},
                      {"usage", result.usage.to_json()}});

    if (project) {
        project::FunctionInfo info = project->function_info(va);
        info.attempts = before.attempts + static_cast<int>(session.attempts().size());
        info.cost_usd = before.cost_usd + result.cost_usd;
        info.best_match = std::max(before.best_match, result.best_match);
        info.status = final_status(result, before);
        if (auto r = project->update_function(va, info); !r) log::warn("cannot update symbols.txt: {}", r.error().message);
        else bus.publish(events::StatusChanged{display, va, std::string(project::to_string(info.status))}, worker);
    }
    bus.publish(events::SessionFinished{session_id, result.outcome, result.detail, result.best_match, result.turns, result.cost_usd}, worker);
    return result;
}

} // namespace decomp::agent
