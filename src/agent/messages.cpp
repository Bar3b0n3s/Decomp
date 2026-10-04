#include "agent/messages.hpp"

#include "core/strings.hpp"

#include <set>

namespace decomp::agent {
namespace {

std::string block_type(const Json& block) { return json_string_or(block, "type", ""); }

const Json* find_object(const Json& obj, std::string_view key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    return it != obj.end() && it->is_object() ? &*it : nullptr;
}

void append_string_field(Json& block, const char* key, std::string_view text) {
    auto it = block.find(key);
    if (it == block.end() || !it->is_string()) {
        block[key] = std::string(text);
    } else {
        it->get_ref<std::string&>().append(text);
    }
}

// Server-side tool blocks (server_tool_use, mcp_tool_use, *_tool_result) as opposed to client tool_use.
bool is_server_tool_use(std::string_view type) { return type.ends_with("_tool_use"); }
bool is_server_tool_result(const Json& block, std::string_view type) {
    return type.ends_with("_tool_result") && block.contains("tool_use_id") && block["tool_use_id"].is_string();
}

// Parses streamed tool input strictly. Empty input means {}.
void finalize_tool_input(Json& block, BlockStatus& status) {
    if (trim(status.raw_input).empty()) {
        auto it = block.find("input");
        if (it == block.end() || !it->is_object()) block["input"] = Json::object();
        status.input_valid = true;
        return;
    }
    auto parsed = parse_json(status.raw_input);
    if (parsed && parsed->is_object()) {
        block["input"] = std::move(*parsed);
        status.input_valid = true;
    } else {
        block["input"] = Json::object();
        status.input_valid = false;
    }
}

} // namespace

// ---- Usage ----------------------------------------------------------------------------------------

void Usage::add(const Usage& other) {
    input_tokens += other.input_tokens;
    output_tokens += other.output_tokens;
    cache_creation_input_tokens += other.cache_creation_input_tokens;
    cache_read_input_tokens += other.cache_read_input_tokens;
}

void Usage::merge(const Json& usage) {
    if (!usage.is_object()) return;
    input_tokens = json_int_or(usage, "input_tokens", input_tokens);
    output_tokens = json_int_or(usage, "output_tokens", output_tokens);
    cache_creation_input_tokens = json_int_or(usage, "cache_creation_input_tokens", cache_creation_input_tokens);
    cache_read_input_tokens = json_int_or(usage, "cache_read_input_tokens", cache_read_input_tokens);
}

Usage Usage::from_json(const Json& usage) {
    Usage u;
    u.merge(usage);
    return u;
}

Json Usage::to_json() const {
    return Json{{"input_tokens", input_tokens},
                {"output_tokens", output_tokens},
                {"cache_creation_input_tokens", cache_creation_input_tokens},
                {"cache_read_input_tokens", cache_read_input_tokens}};
}

// ---- echo -----------------------------------------------------------------------------------------

std::vector<std::size_t> echoed_block_indices(const Response& response) {
    std::vector<std::size_t> kept;
    const Json& content = response.content;
    if (!content.is_array()) return kept;

    std::optional<std::size_t> boundary;  // index of the last fallback block
    std::set<std::string> server_use_ids;
    std::set<std::string> server_result_ids;
    for (std::size_t i = 0; i < content.size(); ++i) {
        const Json& block = content[i];
        const std::string type = block_type(block);
        if (type == "fallback") boundary = i;
        if (is_server_tool_use(type)) server_use_ids.insert(json_string_or(block, "id", ""));
        if (is_server_tool_result(block, type)) server_result_ids.insert(block["tool_use_id"].get<std::string>());
    }

    for (std::size_t i = 0; i < content.size(); ++i) {
        const Json& block = content[i];
        const std::string type = block_type(block);
        if (type == "fallback") continue;  // audit marker only
        if (boundary && i < *boundary) {
            bool keep = false;
            if (type == "text") {
                keep = true;
            } else if (is_server_tool_use(type)) {
                keep = server_result_ids.contains(json_string_or(block, "id", ""));
            } else if (is_server_tool_result(block, type)) {
                keep = server_use_ids.contains(block["tool_use_id"].get<std::string>());
            }
            // thinking, redacted_thinking, tool_use and unknown model-internal blocks are omitted.
            if (!keep) continue;
        }
        kept.push_back(i);
    }
    return kept;
}

Json echo_content(const Response& response) {
    Json out = Json::array();
    for (std::size_t i : echoed_block_indices(response)) out.push_back(response.content[i]);
    return out;
}

// ---- MessageAccumulator ----------------------------------------------------------------------------

Result<void> MessageAccumulator::apply(std::string_view event, std::string_view data) {
    auto parsed = parse_json(data);
    if (!parsed) return make_error(ErrorCode::parse, "invalid JSON in stream event '{}': {}", event, parsed.error().message);
    if (!parsed->is_object()) return make_error(ErrorCode::parse, "stream event '{}' is not a JSON object", event);
    if (!parsed->contains("type") && !event.empty()) (*parsed)["type"] = std::string(event);
    return apply(*parsed);
}

Result<void> MessageAccumulator::apply(const Json& event) {
    if (!event.is_object()) return make_error(ErrorCode::parse, "stream event is not a JSON object");
    const std::string type = json_string_or(event, "type", "");
    try {
        if (type == "message_start") return message_start(event);
        if (type == "content_block_start") return block_start(event);
        if (type == "content_block_delta") return block_delta(event);
        if (type == "content_block_stop") return block_stop(event);
        if (type == "message_delta") return message_delta(event);
        if (type == "message_stop") {
            finished_ = true;
            return {};
        }
        if (type == "error") {
            StreamError error;
            if (const Json* e = find_object(event, "error")) {
                error.type = json_string_or(*e, "type", "error");
                error.message = json_string_or(*e, "message", "");
            } else {
                error.type = "error";
            }
            error_ = error;
            return make_error(ErrorCode::api, "stream error {}: {}", error.type, error.message);
        }
        return {};  // ping and event types this client does not know
    } catch (const Json::exception& e) {
        return make_error(ErrorCode::parse, "malformed '{}' stream event: {}", type, e.what());
    }
}

Result<std::size_t> MessageAccumulator::block_index(const Json& event, std::string_view type) const {
    const long long index = json_int_or(event, "index", -1);
    if (index < 0 || static_cast<std::size_t>(index) >= response_.content.size())
        return make_error(ErrorCode::parse, "{} for unknown content block index {}", type, index);
    return static_cast<std::size_t>(index);
}

Result<void> MessageAccumulator::message_start(const Json& event) {
    const Json* message = find_object(event, "message");
    if (!message) return make_error(ErrorCode::parse, "message_start without a message object");
    response_.id = json_string_or(*message, "id", "");
    response_.model = json_string_or(*message, "model", "");
    response_.stop_reason = json_string_or(*message, "stop_reason", "");
    if (const Json* usage = find_object(*message, "usage")) {
        response_.usage.merge(*usage);
        for (const auto& [key, value] : usage->items()) response_.usage_raw[key] = value;
    }
    started_ = true;
    if (observer_) observer_->on_message_start(response_);
    return {};
}

Result<void> MessageAccumulator::block_start(const Json& event) {
    const long long index = json_int_or(event, "index", -1);
    if (index != static_cast<long long>(response_.content.size()))
        return make_error(ErrorCode::parse, "content_block_start index {} out of order (expected {})", index,
                          response_.content.size());
    const Json* block = find_object(event, "content_block");
    if (!block) return make_error(ErrorCode::parse, "content_block_start without a content_block object");
    if (block_type(*block) == "fallback") response_.had_fallback = true;
    response_.content.push_back(*block);
    response_.blocks.emplace_back();
    if (observer_) observer_->on_block_start(static_cast<int>(index), response_.content.back());
    return {};
}

Result<void> MessageAccumulator::block_delta(const Json& event) {
    TRY_ASSIGN(const std::size_t index, block_index(event, "content_block_delta"));
    const Json* delta = find_object(event, "delta");
    if (!delta) return make_error(ErrorCode::parse, "content_block_delta without a delta object");
    Json& block = response_.content[index];
    BlockStatus& status = response_.blocks[index];
    const int i = static_cast<int>(index);
    const std::string type = json_string_or(*delta, "type", "");
    if (type == "text_delta") {
        const std::string text = json_string_or(*delta, "text", "");
        append_string_field(block, "text", text);
        if (observer_) observer_->on_text_delta(i, text);
    } else if (type == "thinking_delta") {
        const std::string thinking = json_string_or(*delta, "thinking", "");
        append_string_field(block, "thinking", thinking);
        if (observer_) observer_->on_thinking_delta(i, thinking);
    } else if (type == "signature_delta") {
        append_string_field(block, "signature", json_string_or(*delta, "signature", ""));
    } else if (type == "input_json_delta") {
        const std::string partial = json_string_or(*delta, "partial_json", "");
        status.raw_input += partial;
        if (observer_) observer_->on_tool_input_delta(i, partial);
    } else if (type == "citations_delta") {
        if (auto it = delta->find("citation"); it != delta->end()) {
            if (!block.contains("citations") || !block["citations"].is_array()) block["citations"] = Json::array();
            block["citations"].push_back(*it);
        }
    }
    // Unknown delta types are ignored.
    return {};
}

Result<void> MessageAccumulator::block_stop(const Json& event) {
    TRY_ASSIGN(const std::size_t index, block_index(event, "content_block_stop"));
    Json& block = response_.content[index];
    BlockStatus& status = response_.blocks[index];
    const std::string type = block_type(block);
    if (type == "tool_use" || is_server_tool_use(type) || !status.raw_input.empty()) finalize_tool_input(block, status);
    status.complete = true;
    if (observer_) observer_->on_block_stop(static_cast<int>(index), block);
    return {};
}

Result<void> MessageAccumulator::message_delta(const Json& event) {
    if (const Json* delta = find_object(event, "delta")) {
        if (auto it = delta->find("stop_reason"); it != delta->end() && it->is_string())
            response_.stop_reason = it->get<std::string>();
        if (auto it = delta->find("stop_details"); it != delta->end() && !it->is_null()) response_.stop_details = *it;
    }
    if (const Json* usage = find_object(event, "usage")) {
        response_.usage.merge(*usage);
        for (const auto& [key, value] : usage->items()) response_.usage_raw[key] = value;
    }
    return {};
}

Result<Response> MessageAccumulator::from_json(const Json& message) {
    if (!message.is_object()) return make_error(ErrorCode::parse, "response body is not a JSON object");
    if (json_string_or(message, "type", "") == "error") {
        const Json* e = find_object(message, "error");
        return make_error(ErrorCode::api, "{}: {}", e ? json_string_or(*e, "type", "error") : "error",
                          e ? json_string_or(*e, "message", "") : "");
    }
    try {
        Response r;
        r.id = json_string_or(message, "id", "");
        r.model = json_string_or(message, "model", "");
        r.stop_reason = json_string_or(message, "stop_reason", "");
        if (auto it = message.find("stop_details"); it != message.end() && !it->is_null()) r.stop_details = *it;
        if (const Json* usage = find_object(message, "usage")) {
            r.usage = Usage::from_json(*usage);
            r.usage_raw = *usage;
        }
        if (auto it = message.find("content"); it != message.end() && it->is_array()) r.content = *it;
        for (const Json& block : r.content) {
            BlockStatus status;
            status.complete = true;
            const std::string type = block_type(block);
            if (type == "fallback") r.had_fallback = true;
            if (type == "tool_use" || is_server_tool_use(type)) {
                auto input = block.find("input");
                if (input == block.end() || !input->is_object()) {
                    status.input_valid = false;
                    status.raw_input = input == block.end() ? "" : (input->is_string() ? input->get<std::string>() : dump_compact(*input));
                }
            }
            r.blocks.push_back(std::move(status));
        }
        // Invalid tool inputs are echoed as {} (the raw text is kept in BlockStatus).
        for (std::size_t i = 0; i < r.content.size(); ++i) {
            if (!r.blocks[i].input_valid) r.content[i]["input"] = Json::object();
        }
        return r;
    } catch (const Json::exception& e) {
        return make_error(ErrorCode::parse, "malformed response body: {}", e.what());
    }
}

} // namespace decomp::agent
