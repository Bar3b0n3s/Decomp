#pragma once

// Append-only conversation state. Model, settings, system prompt and tools are frozen at construction
// and serialized identically into every request, so the prompt-cache prefix stays byte-identical and
// thinking blocks stay valid; messages can only be appended and assistant content is stored verbatim.

#include "core/json.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace decomp::agent {

// Beta header that enables `fallbacks: "default"` (server-side refusal fallbacks).
inline constexpr const char* kServerSideFallbackBeta = "server-side-fallback-2026-07-01";

struct ConversationSettings {
    std::string model = "claude-opus-5-5";
    int max_tokens = 64000;
    std::string effort = "high";                // output_config.effort: low|medium|high|xhigh|max
    std::string thinking_display = "summarized";  // thinking.display; thinking itself is always adaptive
    bool fallbacks = true;                      // fallbacks: "default" + the server-side-fallback beta
    bool stream = true;
};

class Conversation {
public:
    // `tools` is the array of tool definitions (e.g. ToolRegistry::definitions()), already sorted.
    Conversation(ConversationSettings settings, std::string system_prompt, Json tools);

    // The full Messages API request body: the frozen prefix plus the messages so far.
    Json build_request() const;
    // Beta headers the request needs.
    std::vector<std::string> betas() const;

    void append_user_text(std::string text);
    // `blocks`: an array of content blocks (a single block object is wrapped).
    void append_user_blocks(Json blocks);
    // Appends assistant content exactly as given (see echo_content()).
    void append_assistant(Json content);

    const Json& messages() const { return messages_; }
    std::size_t size() const { return messages_.size(); }
    const ConversationSettings& settings() const { return settings_; }
    const std::string& system_prompt() const { return system_prompt_; }
    const Json& tools() const { return tools_; }

private:
    void append(const char* role, Json content);

    ConversationSettings settings_;
    std::string system_prompt_;
    Json tools_;
    Json prefix_;  // every request field except `messages`
    Json messages_ = Json::array();
};

// True when everything except `messages` serializes identically in both requests and the messages of
// `prev_request` are a prefix of those in `next_request` (the append-only invariant).
bool is_prefix_extension(const Json& prev_request, const Json& next_request);

} // namespace decomp::agent
