#include "agent/replay_transport.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>

namespace decomp::agent {
namespace {

// Splits `s` into up to `pieces` non-empty parts of similar size, never inside a UTF-8 sequence.
std::vector<std::string> split_pieces(std::string_view s, int pieces) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    const std::size_t count = static_cast<std::size_t>(std::max(1, pieces));
    const std::size_t step = std::max<std::size_t>(1, (s.size() + count - 1) / count);
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t end = std::min(s.size(), pos + step);
        while (end < s.size() && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) ++end;
        out.emplace_back(s.substr(pos, end - pos));
        pos = end;
    }
    return out;
}

Json event(std::string_view name, Json data) { return Json{{"event", std::string(name)}, {"data", std::move(data)}}; }

Json delta_event(std::size_t index, Json delta) {
    return event("content_block_delta", Json{{"type", "content_block_delta"}, {"index", index}, {"delta", std::move(delta)}});
}

std::string header_value(const Json& value) { return value.is_string() ? value.get<std::string>() : dump_compact(value); }

} // namespace

std::string to_sse(const Json& events, bool crlf) {
    const std::string_view eol = crlf ? "\r\n" : "\n";
    std::string out;
    if (!events.is_array()) return out;
    for (const Json& e : events) {
        const std::string name = json_string_or(e, "event", "");
        if (!name.empty()) {
            out += "event: ";
            out += name;
            out += eol;
        }
        const Json data = e.is_object() && e.contains("data") ? e["data"] : Json::object();
        const std::string text = data.is_string() ? data.get<std::string>() : dump_compact(data);
        for (const auto& line : split(text, '\n')) {
            out += "data: ";
            out += line;
            out += eol;
        }
        out += eol;
    }
    return out;
}

ReplayTransport::ReplayTransport(std::vector<Json> script, ReplayOptions options)
    : script_(std::make_move_iterator(script.begin()), std::make_move_iterator(script.end())),
      options_(options), rng_state_(options.seed) {}

Result<std::vector<Json>> ReplayTransport::parse_script(std::string_view jsonl) {
    std::vector<Json> script;
    std::size_t line_no = 0;
    for (const auto& raw_line : split(jsonl, '\n')) {
        ++line_no;
        const std::string_view line = trim(raw_line);
        if (line.empty() || line.front() == '#') continue;
        auto parsed = parse_json(line);
        if (!parsed) return make_error(ErrorCode::parse, "replay script line {}: {}", line_no, parsed.error().message);
        if (!parsed->is_object()) return make_error(ErrorCode::parse, "replay script line {}: expected a JSON object", line_no);
        script.push_back(std::move(*parsed));
    }
    return script;
}

Result<std::shared_ptr<ReplayTransport>> ReplayTransport::load(const std::filesystem::path& path, ReplayOptions options) {
    TRY_ASSIGN(const std::string text, fs::read_text(path));
    auto script = parse_script(text);
    if (!script) return std::unexpected(std::move(script.error()).with_context(fs::to_utf8(path)));
    return std::make_shared<ReplayTransport>(std::move(*script), options);
}

void ReplayTransport::push(Json response) {
    std::lock_guard lock(mutex_);
    script_.push_back(std::move(response));
}

std::vector<RecordedRequest> ReplayTransport::requests() const {
    std::lock_guard lock(mutex_);
    return requests_;
}

std::size_t ReplayTransport::remaining() const {
    std::lock_guard lock(mutex_);
    return script_.size();
}

std::size_t ReplayTransport::next_chunk_size() {
    const std::size_t max_chunk = std::max<std::size_t>(1, options_.max_chunk);
    if (options_.fixed_chunks) return max_chunk;
    rng_state_ = rng_state_ * 6364136223846793005ULL + 1442695040888963407ULL;  // 64-bit LCG
    return 1 + static_cast<std::size_t>((rng_state_ >> 33) % max_chunk);
}

Result<HttpResponse> ReplayTransport::post(const HttpRequest& request, const HttpDataCallback& on_data) {
    Json step;
    {
        std::lock_guard lock(mutex_);
        RecordedRequest record;
        record.url = request.url;
        for (const auto& [name, value] : request.headers) {
            const bool secret = iequals(name, "x-api-key") || iequals(name, "authorization");
            record.headers.emplace_back(name, secret ? "***" : value);
        }
        record.raw_body = request.body;
        auto body = parse_json(request.body);
        record.body = body ? std::move(*body) : Json(nullptr);
        requests_.push_back(std::move(record));

        if (script_.empty())
            return make_error(ErrorCode::internal, "replay script exhausted: no scripted response for request {}",
                              requests_.size());
        step = std::move(script_.front());
        script_.pop_front();
    }

    if (auto it = step.find("network_error"); it != step.end())
        return make_error(ErrorCode::network, "POST {} failed: {} (replayed)", request.url, header_value(*it));

    HttpResponse response;
    response.status = static_cast<int>(json_int_or(step, "status", 200));
    if (auto it = step.find("headers"); it != step.end() && it->is_object()) {
        for (const auto& [name, value] : it->items()) response.headers.emplace_back(to_lower(name), header_value(value));
    }

    std::string body;
    if (auto it = step.find("events"); it != step.end()) {
        body = to_sse(*it, json_bool_or(step, "crlf", false));
    } else if (auto sse = step.find("sse"); sse != step.end() && sse->is_string()) {
        body = sse->get<std::string>();
    } else if (auto b = step.find("body"); b != step.end()) {
        body = header_value(*b);
    }

    if (!response.ok()) {
        response.body_prefix = body.substr(0, std::min(body.size(), kMaxErrorBodyPrefix));
        return response;
    }

    const long long disconnect_after = json_int_or(step, "disconnect_after", -1);
    std::string_view deliver = body;
    if (disconnect_after >= 0) deliver = deliver.substr(0, std::min<std::size_t>(deliver.size(), static_cast<std::size_t>(disconnect_after)));
    std::size_t pos = 0;
    while (pos < deliver.size()) {
        std::size_t n = 0;
        {
            std::lock_guard lock(mutex_);
            n = next_chunk_size();
        }
        n = std::min(n, deliver.size() - pos);
        if (on_data && !on_data(deliver.substr(pos, n))) return make_error(ErrorCode::cancelled, "request cancelled");
        pos += n;
    }
    if (disconnect_after >= 0)
        return make_error(ErrorCode::network, "POST {} failed: connection reset after {} bytes (replayed)", request.url, pos);
    return response;
}

// ---- builders ------------------------------------------------------------------------------------

namespace replay {

Json text(std::string value) { return Json{{"type", "text"}, {"text", std::move(value)}}; }

Json thinking(std::string value, std::string signature) {
    return Json{{"type", "thinking"}, {"thinking", std::move(value)}, {"signature", std::move(signature)}};
}

Json redacted_thinking(std::string data) { return Json{{"type", "redacted_thinking"}, {"data", std::move(data)}}; }

Json tool_use(std::string id, std::string name, Json input) {
    return Json{{"type", "tool_use"}, {"id", std::move(id)}, {"name", std::move(name)}, {"input", std::move(input)}};
}

Json tool_use_raw(std::string id, std::string name, std::string raw_input) {
    Json block = tool_use(std::move(id), std::move(name), Json::object());
    block["_raw_input"] = std::move(raw_input);  // builder-only: streamed verbatim, never sent as a field
    return block;
}

Json fallback(std::string from_model, std::string to_model) {
    return Json{{"type", "fallback"}, {"from", Json{{"model", std::move(from_model)}}}, {"to", Json{{"model", std::move(to_model)}}}};
}

Json usage(long long input_tokens, long long output_tokens, long long cache_creation_input_tokens,
           long long cache_read_input_tokens) {
    return Json{{"input_tokens", input_tokens},
                {"output_tokens", output_tokens},
                {"cache_creation_input_tokens", cache_creation_input_tokens},
                {"cache_read_input_tokens", cache_read_input_tokens}};
}

Json events(const std::vector<Json>& content_blocks, std::string_view stop_reason, const Json& usage_json,
            const MessageOptions& options) {
    Json out = Json::array();

    Json start_usage = Json::object();
    for (const char* key : {"input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens"}) {
        if (usage_json.is_object() && usage_json.contains(key)) start_usage[key] = usage_json[key];
    }
    start_usage["output_tokens"] = 1;
    out.push_back(event("message_start", Json{{"type", "message_start"},
                                              {"message", Json{{"id", options.id},
                                                               {"type", "message"},
                                                               {"role", "assistant"},
                                                               {"model", options.model},
                                                               {"content", Json::array()},
                                                               {"stop_reason", nullptr},
                                                               {"stop_sequence", nullptr},
                                                               {"usage", start_usage}}}}));
    out.push_back(event("ping", Json{{"type", "ping"}}));

    for (std::size_t i = 0; i < content_blocks.size(); ++i) {
        const Json& block = content_blocks[i];
        const std::string type = json_string_or(block, "type", "");
        Json start = block;
        std::vector<Json> deltas;
        if (type == "text") {
            start["text"] = "";
            for (auto& piece : split_pieces(json_string_or(block, "text", ""), options.pieces))
                deltas.push_back(Json{{"type", "text_delta"}, {"text", piece}});
        } else if (type == "thinking") {
            start["thinking"] = "";
            start["signature"] = "";
            for (auto& piece : split_pieces(json_string_or(block, "thinking", ""), options.pieces))
                deltas.push_back(Json{{"type", "thinking_delta"}, {"thinking", piece}});
            const std::string signature = json_string_or(block, "signature", "");
            if (!signature.empty()) deltas.push_back(Json{{"type", "signature_delta"}, {"signature", signature}});
        } else if (type == "tool_use" || type == "server_tool_use") {
            start["input"] = Json::object();
            std::string raw;
            if (auto it = block.find("_raw_input"); it != block.end() && it->is_string()) {
                raw = it->get<std::string>();
                start.erase("_raw_input");
            } else if (auto input = block.find("input"); input != block.end()) {
                raw = dump_compact(*input);
            }
            // More fragments than for prose, so keys, strings and numbers are cut mid-token.
            for (auto& piece : split_pieces(raw, options.pieces * 2 + 1))
                deltas.push_back(Json{{"type", "input_json_delta"}, {"partial_json", piece}});
        }
        out.push_back(event("content_block_start", Json{{"type", "content_block_start"}, {"index", i}, {"content_block", start}}));
        for (auto& d : deltas) out.push_back(delta_event(i, std::move(d)));
        out.push_back(event("content_block_stop", Json{{"type", "content_block_stop"}, {"index", i}}));
    }

    Json delta{{"stop_reason", std::string(stop_reason)}, {"stop_sequence", nullptr}};
    if (!options.stop_details.is_null()) delta["stop_details"] = options.stop_details;
    out.push_back(event("message_delta", Json{{"type", "message_delta"}, {"delta", delta}, {"usage", usage_json}}));
    out.push_back(event("message_stop", Json{{"type", "message_stop"}}));
    return out;
}

Json message(const std::vector<Json>& content_blocks, std::string_view stop_reason, const Json& usage_json,
             const MessageOptions& options) {
    Json response{{"status", 200},
                  {"headers", Json{{"request-id", options.request_id.empty() ? "req_" + options.id : options.request_id},
                                   {"content-type", "text/event-stream"}}},
                  {"events", events(content_blocks, stop_reason, usage_json, options)}};
    if (options.crlf) response["crlf"] = true;
    return response;
}

Json refusal(const Json& stop_details, const std::vector<Json>& partial_content, const Json& usage_json,
             const MessageOptions& options) {
    MessageOptions with_details = options;
    with_details.stop_details = stop_details;
    return message(partial_content, "refusal", usage_json, with_details);
}

Json http_error(int status, std::string_view type, std::string_view message, std::optional<std::string> retry_after) {
    Json headers{{"request-id", std::format("req_error_{}", status)}, {"content-type", "application/json"}};
    if (retry_after) headers["retry-after"] = *retry_after;
    return Json{{"status", status},
                {"headers", headers},
                {"body", Json{{"type", "error"},
                              {"error", Json{{"type", std::string(type)}, {"message", std::string(message)}}},
                              {"request_id", std::format("req_error_{}", status)}}}};
}

Json stream_error(std::string_view type, std::string_view message) {
    Json evs = events({}, "end_turn", usage(0, 0));
    Json out = Json::array();
    out.push_back(evs[0]);  // message_start
    out.push_back(evs[1]);  // ping
    out.push_back(event("error", Json{{"type", "error"},
                                      {"error", Json{{"type", std::string(type)}, {"message", std::string(message)}}}}));
    return Json{{"status", 200}, {"headers", Json{{"request-id", "req_stream_error"}}}, {"events", out}};
}

Json network_error(std::string_view message) { return Json{{"network_error", std::string(message)}}; }

Json json_message(const std::vector<Json>& content_blocks, std::string_view stop_reason, const Json& usage_json,
                  const MessageOptions& options) {
    Json content = Json::array();
    for (const Json& block : content_blocks) {
        Json b = block;
        if (b.contains("_raw_input")) {
            b["input"] = b["_raw_input"];  // a non-object input marks it invalid on the client side
            b.erase("_raw_input");
        }
        content.push_back(std::move(b));
    }
    Json body{{"id", options.id},
              {"type", "message"},
              {"role", "assistant"},
              {"model", options.model},
              {"content", content},
              {"stop_reason", std::string(stop_reason)},
              {"stop_sequence", nullptr},
              {"usage", usage_json}};
    if (!options.stop_details.is_null()) body["stop_details"] = options.stop_details;
    return Json{{"status", 200},
                {"headers", Json{{"request-id", options.request_id.empty() ? "req_" + options.id : options.request_id}}},
                {"body", body}};
}

} // namespace replay

} // namespace decomp::agent
