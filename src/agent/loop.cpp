#include "agent/loop.hpp"

#include "agent/cost.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"

#include <format>
#include <future>
#include <set>
#include <system_error>
#include <utility>

namespace decomp::agent {

std::string_view to_string(LoopStatus status) {
    switch (status) {
    case LoopStatus::finished: return "finished";
    case LoopStatus::end_turn_without_finish: return "end_turn_without_finish";
    case LoopStatus::refused: return "refused";
    case LoopStatus::budget_exhausted: return "budget_exhausted";
    case LoopStatus::max_turns: return "max_turns";
    case LoopStatus::aborted: return "aborted";
    case LoopStatus::stopped: return "stopped";
    case LoopStatus::error: return "error";
    }
    return "unknown";
}

// ---- LoopControl -----------------------------------------------------------------------------------

void LoopControl::request_pause() {
    std::lock_guard lock(mutex_);
    paused_ = true;
}

void LoopControl::resume() {
    {
        std::lock_guard lock(mutex_);
        paused_ = false;
    }
    cv_.notify_all();
}

void LoopControl::request_stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
}

void LoopControl::request_abort() {
    {
        std::lock_guard lock(mutex_);
        abort_ = true;
    }
    cv_.notify_all();
}

void LoopControl::inject(std::string text) {
    std::lock_guard lock(mutex_);
    injected_.push_back(std::move(text));
}

bool LoopControl::is_paused() const {
    std::lock_guard lock(mutex_);
    return paused_;
}

bool LoopControl::has_injected() const {
    std::lock_guard lock(mutex_);
    return !injected_.empty();
}

bool LoopControl::wait_while_paused() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return !paused_ || stop_.load() || abort_.load(); });
    return !stop_.load() && !abort_.load();
}

std::vector<std::string> LoopControl::take_injected() {
    std::lock_guard lock(mutex_);
    return std::exchange(injected_, {});
}

// ---- run_loop --------------------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

struct PreparedCall {
    ToolCall call;
    bool cut_off = false;  // max_tokens hit while this call was being written
};

struct ToolRun {
    ToolResult result;
    std::chrono::milliseconds elapsed{0};
};

ToolRun run_tool(const ToolRegistry& tools, const ToolCall& call) {
    const auto start = Clock::now();
    ToolRun run{tools.dispatch(call), {}};
    run.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    return run;
}

Json tool_result_block(const ToolCall& call, const ToolResult& result) {
    Json block{{"type", "tool_result"}, {"tool_use_id", call.id}, {"content", result.content}};
    if (result.is_error) block["is_error"] = true;
    return block;
}

Json text_block(std::string text) { return Json{{"type", "text"}, {"text", std::move(text)}}; }

class LoopRun {
public:
    LoopRun(Client& client, Conversation& conversation, const ToolRegistry& tools, const LoopConfig& config,
            LoopControl* control, LoopObserver* observer)
        : client_(client), conversation_(conversation), tools_(tools), config_(config), control_(control),
          observer_(observer) {}

    LoopOutcome run();

private:
    std::optional<std::pair<LoopStatus, std::string>> check_control();
    std::optional<std::pair<LoopStatus, std::string>> check_budgets() const;
    bool wall_exceeded() const {
        return config_.max_wall.count() > 0 && Clock::now() - start_ >= config_.max_wall;
    }
    std::vector<PreparedCall> prepare_calls(const Response& response) const;
    std::vector<ToolRun> execute(const std::vector<PreparedCall>& calls, int turn);
    void flush_pending(bool with_extras);
    LoopOutcome finish(LoopStatus status, std::string detail);
    std::string nudge_text(const Response& response) const;

    Client& client_;
    Conversation& conversation_;
    const ToolRegistry& tools_;
    const LoopConfig& config_;
    LoopControl* control_;
    LoopObserver* observer_;

    Clock::time_point start_ = Clock::now();
    LoopOutcome outcome_;
    // The next user message, appended right before the next request so that guidance injected in the
    // meantime (e.g. while paused) still joins it.
    Json pending_ = Json::array();
    bool pending_has_results_ = false;
    int nudges_used_ = 0;
};

LoopOutcome LoopRun::run() {
    start_ = Clock::now();
    while (true) {
        if (auto stop = check_control()) return finish(stop->first, std::move(stop->second));
        if (outcome_.turns >= config_.max_turns)
            return finish(LoopStatus::max_turns, std::format("reached the turn limit ({})", config_.max_turns));
        if (wall_exceeded())
            return finish(LoopStatus::budget_exhausted,
                          std::format("wall-clock budget of {} s exhausted", config_.max_wall.count()));

        flush_pending(true);
        if (conversation_.messages().empty()) {
            outcome_.error = Error{ErrorCode::invalid_argument, "the conversation has no messages to send"};
            return finish(LoopStatus::error, outcome_.error->describe());
        }
        const int turn = ++outcome_.turns;
        const Json request = conversation_.build_request();
        if (observer_) observer_->on_turn_start(turn, request);

        RequestOptions options;
        options.betas = conversation_.betas();
        if (control_) options.cancelled = [control = control_] { return control->abort_requested(); };
        auto response = client_.create_message(request, observer_, options);
        if (!response) {
            if (response.error().code == ErrorCode::cancelled && control_ && control_->abort_requested())
                return finish(LoopStatus::aborted, std::format("aborted during turn {}", turn));
            outcome_.error = response.error();
            return finish(LoopStatus::error, response.error().describe());
        }

        const ResponseCost cost = response_cost(*response, conversation_.settings().model);
        outcome_.usage.add(cost.usage);
        outcome_.cost_usd += cost.usd;
        if (observer_) observer_->on_response(turn, *response);
        // An abort that raced with the end of the stream still stops before any tool runs.
        if (control_ && control_->abort_requested())
            return finish(LoopStatus::aborted, std::format("aborted during turn {}", turn));

        // The stop reason decides what may be used from this turn.
        if (response->stop_reason == "refusal") {
            outcome_.stop_details = response->stop_details;
            std::string detail = "the model declined the request";
            if (response->stop_details.is_object()) {
                const std::string category = json_string_or(response->stop_details, "category", "");
                const std::string explanation = json_string_or(response->stop_details, "explanation", "");
                if (!category.empty()) detail += std::format(" (category: {})", category);
                if (!explanation.empty()) detail += ": " + explanation;
            }
            return finish(LoopStatus::refused, std::move(detail));
        }

        std::vector<PreparedCall> calls = prepare_calls(*response);
        if (Json echoed = echo_content(*response); !echoed.empty()) conversation_.append_assistant(std::move(echoed));

        if (!calls.empty()) {
            std::vector<ToolRun> runs = execute(calls, turn);
            std::optional<std::size_t> ended;
            for (std::size_t i = 0; i < calls.size(); ++i) {
                pending_.push_back(tool_result_block(calls[i].call, runs[i].result));
                if (runs[i].result.end_session && !ended) ended = i;
            }
            pending_has_results_ = true;
            if (ended) {
                outcome_.finish_outcome = runs[*ended].result.outcome;
                return finish(LoopStatus::finished, std::format("'{}' ended the session", calls[*ended].call.name));
            }
        } else if (config_.finish_tool.empty()) {
            return finish(LoopStatus::finished, std::format("the model ended its turn ({})", response->stop_reason));
        } else if (nudges_used_ >= config_.max_nudges) {
            return finish(LoopStatus::end_turn_without_finish,
                          std::format("the model stopped ({}) without calling '{}'", response->stop_reason, config_.finish_tool));
        }

        if (auto exhausted = check_budgets()) return finish(exhausted->first, std::move(exhausted->second));

        if (calls.empty()) {
            ++nudges_used_;
            pending_.push_back(text_block(nudge_text(*response)));
        }
    }
}

std::optional<std::pair<LoopStatus, std::string>> LoopRun::check_control() {
    if (!control_) return std::nullopt;
    auto requested = [this]() -> std::optional<std::pair<LoopStatus, std::string>> {
        if (control_->abort_requested()) return std::pair{LoopStatus::aborted, std::string("aborted by the supervisor")};
        if (control_->stop_requested()) return std::pair{LoopStatus::stopped, std::string("stopped by the supervisor")};
        return std::nullopt;
    };
    if (auto r = requested()) return r;
    if (control_->is_paused()) {
        if (observer_) observer_->on_paused();
        const bool resumed = control_->wait_while_paused();
        if (!resumed) return requested();
        if (observer_) observer_->on_resumed();
    }
    return std::nullopt;
}

std::optional<std::pair<LoopStatus, std::string>> LoopRun::check_budgets() const {
    if (config_.max_total_tokens > 0 && outcome_.usage.total() >= config_.max_total_tokens)
        return std::pair{LoopStatus::budget_exhausted, std::format("token budget exhausted ({} of {} tokens)",
                                                                   outcome_.usage.total(), config_.max_total_tokens)};
    if (config_.max_cost_usd > 0 && outcome_.cost_usd >= config_.max_cost_usd)
        return std::pair{LoopStatus::budget_exhausted,
                         std::format("cost budget exhausted (${:.4f} of ${:.4f})", outcome_.cost_usd, config_.max_cost_usd)};
    if (wall_exceeded())
        return std::pair{LoopStatus::budget_exhausted,
                         std::format("wall-clock budget of {} s exhausted", config_.max_wall.count())};
    if (outcome_.turns >= config_.max_turns)
        return std::pair{LoopStatus::max_turns, std::format("reached the turn limit ({})", config_.max_turns)};
    return std::nullopt;
}

std::vector<PreparedCall> LoopRun::prepare_calls(const Response& response) const {
    std::vector<PreparedCall> calls;
    for (ToolCall& call : extract_tool_calls(response)) calls.push_back(PreparedCall{std::move(call), false});
    if (calls.empty()) return calls;

    // A tool call that was still being written when max_tokens hit is incomplete even if what arrived
    // happens to parse: answer it with an error instead of running it.
    std::set<std::string> cut_off;
    for (std::size_t i = 0; i < response.content.size() && i < response.blocks.size(); ++i) {
        if (json_string_or(response.content[i], "type", "") == "tool_use" && !response.blocks[i].complete)
            cut_off.insert(json_string_or(response.content[i], "id", ""));
    }
    if (response.stop_reason == "max_tokens") {
        const Json& last_block = response.content.back();
        PreparedCall& last_call = calls.back();
        if ((json_string_or(last_block, "type", "") == "tool_use" && json_string_or(last_block, "id", "") == last_call.call.id) ||
            !last_call.call.input_valid)
            cut_off.insert(last_call.call.id);
    }
    for (PreparedCall& call : calls) call.cut_off = cut_off.contains(call.call.id);
    return calls;
}

std::vector<ToolRun> LoopRun::execute(const std::vector<PreparedCall>& calls, int turn) {
    std::vector<ToolRun> runs(calls.size());
    auto executable = [&](std::size_t i) { return !calls[i].cut_off && calls[i].call.input_valid; };
    auto parallel = [&](std::size_t i) {
        const Tool* tool = tools_.find(calls[i].call.name);
        return tool && tool->parallel_safe;
    };
    auto start = [&](std::size_t i) {
        if (observer_) observer_->on_tool_start(turn, calls[i].call);
    };
    auto end = [&](std::size_t i) {
        if (observer_) observer_->on_tool_end(turn, calls[i].call, runs[i].result, runs[i].elapsed);
    };

    std::size_t i = 0;
    while (i < calls.size()) {
        if (!executable(i)) {
            start(i);
            runs[i].result = calls[i].cut_off
                                 ? ToolResult::error("Your output was cut off at max_tokens while you were writing this "
                                                     "tool call, so it was not executed. Send it again as a smaller "
                                                     "call (for example, split the work across several calls).")
                                 : ToolResult::error(invalid_input_content(calls[i].call.raw_input));
            end(i);
            ++i;
            continue;
        }
        std::size_t batch_end = i + 1;
        if (parallel(i)) {
            while (batch_end < calls.size() && executable(batch_end) && parallel(batch_end)) ++batch_end;
        }
        if (batch_end - i == 1) {
            start(i);
            runs[i] = run_tool(tools_, calls[i].call);
            end(i);
            ++i;
            continue;
        }
        // Consecutive parallel-safe calls run concurrently; results are still reported in call order.
        std::vector<std::future<ToolRun>> futures;
        futures.reserve(batch_end - i);
        for (std::size_t k = i; k < batch_end; ++k) {
            start(k);
            auto task = [this, &call = calls[k].call] { return run_tool(tools_, call); };
            try {
                futures.push_back(std::async(std::launch::async, task));
            } catch (const std::system_error&) {
                futures.push_back(std::async(std::launch::deferred, task));  // no thread available
            }
        }
        for (std::size_t k = i; k < batch_end; ++k) {
            runs[k] = futures[k - i].get();
            end(k);
        }
        i = batch_end;
    }
    return runs;
}

void LoopRun::flush_pending(bool with_extras) {
    std::string extra;
    if (with_extras) {
        if (control_) {
            for (std::string& text : control_->take_injected()) {
                if (trim(text).empty()) continue;
                if (observer_) observer_->on_injected(text);
                if (!extra.empty()) extra += "\n\n";
                extra += "[Supervisor guidance] " + text;
            }
        }
        // The status line rides along with tool results and nudges; it never forms a message alone.
        if (config_.status_line && !pending_.empty()) {
            const std::string line = config_.status_line();
            if (!trim(line).empty()) {
                if (!extra.empty()) extra += "\n\n";
                extra += line;
            }
        }
    }
    if (pending_.empty() && extra.empty()) return;
    Json blocks = std::exchange(pending_, Json::array());
    if (!extra.empty()) blocks.push_back(text_block(std::move(extra)));
    conversation_.append_user_blocks(std::move(blocks));
    pending_has_results_ = false;
}

LoopOutcome LoopRun::finish(LoopStatus status, std::string detail) {
    // Keep the transcript complete: every tool_use of the last assistant message gets its tool_result.
    if (pending_has_results_) flush_pending(false);
    outcome_.status = status;
    outcome_.detail = std::move(detail);
    log::debug("agent loop finished: {} ({}) after {} turns, ${:.4f}", to_string(status), outcome_.detail,
               outcome_.turns, outcome_.cost_usd);
    if (observer_) observer_->on_finish(outcome_);
    return outcome_;
}

std::string LoopRun::nudge_text(const Response& response) const {
    if (response.stop_reason == "max_tokens") {
        return std::format("Your previous response was cut off at max_tokens. Continue with shorter steps, and call "
                           "`{}` when you are done.",
                           config_.finish_tool);
    }
    if (config_.nudge_text.empty()) {
        return std::format("You ended your turn without calling `{0}`. Continue working on the task, and call `{0}` "
                           "when you are done.",
                           config_.finish_tool);
    }
    if (config_.nudge_text.find(config_.finish_tool) != std::string::npos) return config_.nudge_text;
    return std::format("{} (Call `{}` when you are done.)", config_.nudge_text, config_.finish_tool);
}

} // namespace

LoopOutcome run_loop(Client& client, Conversation& conversation, const ToolRegistry& tools, const LoopConfig& config,
                     LoopControl* control, LoopObserver* observer) {
    return LoopRun(client, conversation, tools, config, control, observer).run();
}

} // namespace decomp::agent
