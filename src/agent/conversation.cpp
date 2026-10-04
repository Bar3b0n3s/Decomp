#include "agent/conversation.hpp"

#include <utility>

namespace decomp::agent {

Conversation::Conversation(ConversationSettings settings, std::string system_prompt, Json tools)
    : settings_(std::move(settings)), system_prompt_(std::move(system_prompt)), tools_(std::move(tools)) {
    if (!tools_.is_array()) tools_ = Json::array();

    // Thinking is always adaptive on the default model: never send {"type":"disabled"}, budget_tokens,
    // temperature, or a forced tool_choice (all 400s there).
    Json thinking{{"type", "adaptive"}};
    if (!settings_.thinking_display.empty()) thinking["display"] = settings_.thinking_display;

    prefix_ = Json::object();
    prefix_["model"] = settings_.model;
    prefix_["max_tokens"] = settings_.max_tokens;
    prefix_["stream"] = settings_.stream;
    prefix_["thinking"] = std::move(thinking);
    if (!settings_.effort.empty()) prefix_["output_config"] = Json{{"effort", settings_.effort}};
    // Automatic caching of the whole prefix plus an explicit breakpoint on the system prompt.
    prefix_["cache_control"] = Json{{"type", "ephemeral"}};
    if (!system_prompt_.empty()) {
        prefix_["system"] = Json::array({Json{{"type", "text"},
                                              {"text", system_prompt_},
                                              {"cache_control", Json{{"type", "ephemeral"}}}}});
    }
    if (!tools_.empty()) {
        prefix_["tools"] = tools_;
        prefix_["tool_choice"] = Json{{"type", "auto"}};
    }
    if (settings_.fallbacks) prefix_["fallbacks"] = "default";
}

Json Conversation::build_request() const {
    Json request = prefix_;
    request["messages"] = messages_;
    return request;
}

std::vector<std::string> Conversation::betas() const {
    std::vector<std::string> out;
    if (settings_.fallbacks) out.emplace_back(kServerSideFallbackBeta);
    return out;
}

void Conversation::append_user_text(std::string text) {
    append("user", Json::array({Json{{"type", "text"}, {"text", std::move(text)}}}));
}

void Conversation::append_user_blocks(Json blocks) {
    if (blocks.is_object()) blocks = Json::array({std::move(blocks)});
    append("user", std::move(blocks));
}

void Conversation::append_assistant(Json content) { append("assistant", std::move(content)); }

void Conversation::append(const char* role, Json content) {
    messages_.push_back(Json{{"role", role}, {"content", std::move(content)}});
}

bool is_prefix_extension(const Json& prev_request, const Json& next_request) {
    if (!prev_request.is_object() || !next_request.is_object()) return false;
    Json prev_rest = prev_request;
    Json next_rest = next_request;
    Json prev_messages = prev_rest.contains("messages") ? prev_rest["messages"] : Json::array();
    Json next_messages = next_rest.contains("messages") ? next_rest["messages"] : Json::array();
    prev_rest.erase("messages");
    next_rest.erase("messages");
    if (dump_compact(prev_rest) != dump_compact(next_rest)) return false;
    if (!prev_messages.is_array() || !next_messages.is_array()) return false;
    if (prev_messages.size() > next_messages.size()) return false;
    for (std::size_t i = 0; i < prev_messages.size(); ++i) {
        if (dump_compact(prev_messages[i]) != dump_compact(next_messages[i])) return false;
    }
    return true;
}

} // namespace decomp::agent
