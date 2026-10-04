#pragma once

// Client-side tools: definitions sent to the API, dispatch of tool calls to handlers, and a JSON-schema
// validator for tool inputs (with eager_input_streaming the API does not validate them).

#include "agent/messages.hpp"
#include "core/json.hpp"
#include "core/result.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace decomp::agent {

struct ToolResult {
    Json content = "";         // a string or an array of content blocks
    bool is_error = false;
    bool end_session = false;  // the loop finishes after this turn (e.g. submit_result)
    Json outcome;              // reported as LoopOutcome::finish_outcome when end_session is set

    static ToolResult text(std::string text) { return ToolResult{std::move(text), false, false, nullptr}; }
    static ToolResult error(std::string text) { return ToolResult{std::move(text), true, false, nullptr}; }
};

struct ToolCall {
    std::string id;
    std::string name;
    Json input = Json::object();
    bool input_valid = true;  // false when the streamed input was not a JSON object
    std::string raw_input;    // the input text as streamed
};

struct Tool {
    std::string name;
    std::string description;
    Json input_schema;          // must set additionalProperties:false and list `required` (strict mode)
    bool parallel_safe = true;  // may run concurrently with other parallel-safe calls of the same turn
    std::function<ToolResult(const ToolCall&)> handler;
};

class ToolRegistry {
public:
    // Adds a tool; a tool with the same name is replaced.
    void add(Tool tool);

    // Tool definitions sorted by name, each {name, description, input_schema, strict: true,
    // eager_input_streaming: true}. Stable across calls so the prompt-cache prefix never changes.
    Json definitions() const;

    // Validates and runs a call. Unknown tools, unparseable input ({"INVALID_JSON": "<raw>"}), schema
    // violations and handler exceptions come back as is_error results; nothing throws.
    ToolResult dispatch(const ToolCall& call) const;

    const Tool* find(std::string_view name) const;
    std::vector<std::string> names() const;
    bool empty() const { return tools_.empty(); }
    std::size_t size() const { return tools_.size(); }

private:
    std::map<std::string, Tool, std::less<>> tools_;
};

// Validates `value` against the schema subset used by tool definitions: type (incl. arrays of types),
// properties, required, additionalProperties (false or a schema), enum, const, items, anyOf,
// minimum/maximum/exclusiveMinimum/exclusiveMaximum, minLength/maxLength, minItems/maxItems.
// The error message names the offending location, e.g. "input.items[2].name: expected string".
Result<void> validate_json(const Json& value, const Json& schema);

// Checks the strict-tool-use requirements: every object schema sets additionalProperties:false and
// lists `required` (names that exist in `properties`).
Result<void> check_strict_schema(const Json& schema);

// Error content for a call whose input could not be used: {"INVALID_JSON": "<raw>"} (plus "error"
// when a reason is given), serialized with the JSON library.
std::string invalid_input_content(std::string_view raw_input, std::string_view reason = {});

// The client tool calls of a response: tool_use blocks that remain in echo_content(), in order.
std::vector<ToolCall> extract_tool_calls(const Response& response);

} // namespace decomp::agent
