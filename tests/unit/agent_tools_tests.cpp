#include "agent/tools.hpp"

#include <doctest/doctest.h>

#include <stdexcept>
#include <string>

using namespace decomp;
using namespace decomp::agent;

namespace {

Json disassemble_schema() {
    return Json::parse(R"({
        "type": "object",
        "properties": {
            "target": {"type": "string", "description": "symbol or address"},
            "max_instructions": {"type": "integer", "minimum": 1, "maximum": 4096},
            "syntax": {"type": "string", "enum": ["intel", "att"]},
            "ranges": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {"start": {"type": "integer"}, "end": {"type": ["integer", "null"]}},
                    "required": ["start", "end"],
                    "additionalProperties": false
                }
            }
        },
        "required": ["target", "max_instructions", "syntax", "ranges"],
        "additionalProperties": false
    })");
}

Tool echo_tool(std::string name) {
    Tool tool;
    tool.name = std::move(name);
    tool.description = "Echoes its input.";
    tool.input_schema = Json::parse(
        R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"],"additionalProperties":false})");
    tool.handler = [](const ToolCall& call) { return ToolResult::text(call.input["text"].get<std::string>()); };
    return tool;
}

ToolCall call(std::string name, Json input) {
    ToolCall c;
    c.id = "toolu_test";
    c.name = std::move(name);
    c.input = std::move(input);
    return c;
}

} // namespace

TEST_CASE("ToolRegistry definitions are sorted and strict") {
    ToolRegistry registry;
    registry.add(echo_tool("submit_result"));
    registry.add(echo_tool("compile_and_diff"));
    registry.add(echo_tool("lookup_symbol"));
    const Json defs = registry.definitions();
    REQUIRE(defs.size() == 3);
    CHECK(defs[0]["name"] == "compile_and_diff");
    CHECK(defs[1]["name"] == "lookup_symbol");
    CHECK(defs[2]["name"] == "submit_result");
    for (const Json& def : defs) {
        CHECK(def["strict"] == true);
        CHECK(def["eager_input_streaming"] == true);
        CHECK(def["description"] == "Echoes its input.");
        CHECK(def["input_schema"]["additionalProperties"] == false);
        CHECK(def.size() == 5);
    }
    // Stable: same bytes every time (prompt-cache prefix).
    CHECK(dump_compact(registry.definitions()) == dump_compact(defs));
    // Re-adding a name replaces the tool.
    registry.add(echo_tool("lookup_symbol"));
    CHECK(registry.size() == 3);
    CHECK(registry.names() == std::vector<std::string>{"compile_and_diff", "lookup_symbol", "submit_result"});
}

TEST_CASE("validate_json accepts valid input") {
    const Json schema = disassemble_schema();
    const Json good = Json::parse(
        R"({"target":"sum_array","max_instructions":64,"syntax":"intel","ranges":[{"start":1,"end":null},{"start":2,"end":3}]})");
    CHECK(validate_json(good, schema));
    // 64.0 is an integer value.
    Json as_float = good;
    as_float["max_instructions"] = 64.0;
    CHECK(validate_json(as_float, schema));
}

TEST_CASE("validate_json rejects invalid input with a located message") {
    const Json schema = disassemble_schema();
    const Json good = Json::parse(R"({"target":"f","max_instructions":64,"syntax":"intel","ranges":[]})");

    auto missing = good;
    missing.erase("syntax");
    auto r = validate_json(missing, schema);
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "input: missing required property 'syntax'");

    auto extra = good;
    extra["bogus"] = 1;
    r = validate_json(extra, schema);
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "input: unexpected property 'bogus'");

    auto wrong_type = good;
    wrong_type["target"] = 42;
    r = validate_json(wrong_type, schema);
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "input.target: expected string, got integer");

    auto not_integer = good;
    not_integer["max_instructions"] = 1.5;
    CHECK_FALSE(validate_json(not_integer, schema));

    auto bad_enum = good;
    bad_enum["syntax"] = "masm";
    r = validate_json(bad_enum, schema);
    REQUIRE_FALSE(r);
    CHECK(r.error().message == R"(input.syntax: "masm" is not one of ["intel","att"])");

    auto too_big = good;
    too_big["max_instructions"] = 5000;
    r = validate_json(too_big, schema);
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "input.max_instructions: 5000 is greater than the maximum 4096");

    auto nested = good;
    nested["ranges"] = Json::parse(R"([{"start":1,"end":2},{"start":"x","end":2}])");
    r = validate_json(nested, schema);
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "input.ranges[1].start: expected integer, got string");

    auto union_type = good;
    union_type["ranges"] = Json::parse(R"([{"start":1,"end":"never"}])");
    r = validate_json(union_type, schema);
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "input.ranges[0].end: expected integer or null, got string");

    CHECK_FALSE(validate_json(Json::array(), schema));
}

TEST_CASE("check_strict_schema enforces additionalProperties:false and required") {
    CHECK(check_strict_schema(disassemble_schema()));
    CHECK(check_strict_schema(Json::parse(R"({"type":"object","properties":{},"additionalProperties":false})")));

    auto r = check_strict_schema(Json::parse(R"({"type":"object","properties":{"a":{"type":"string"}},"required":["a"]})"));
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("additionalProperties") != std::string::npos);

    r = check_strict_schema(Json::parse(R"({"type":"object","properties":{"a":{"type":"string"}},"additionalProperties":false})"));
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("required") != std::string::npos);

    // Nested objects are checked too.
    auto nested = disassemble_schema();
    nested["properties"]["ranges"]["items"].erase("additionalProperties");
    r = check_strict_schema(nested);
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("input_schema.properties.ranges.items") != std::string::npos);
}

TEST_CASE("ToolRegistry::dispatch validates before running") {
    ToolRegistry registry;
    int runs = 0;
    Tool tool = echo_tool("echo");
    tool.handler = [&](const ToolCall& c) {
        ++runs;
        return ToolResult::text("echo: " + c.input["text"].get<std::string>());
    };
    registry.add(tool);
    Tool thrower = echo_tool("thrower");
    thrower.handler = [](const ToolCall&) -> ToolResult { throw std::runtime_error("disk on fire"); };
    registry.add(thrower);
    Tool object_result = echo_tool("object_result");
    object_result.handler = [](const ToolCall&) { return ToolResult{Json{{"score", 87.5}}, false, false, nullptr}; };
    registry.add(object_result);

    auto ok = registry.dispatch(call("echo", {{"text", "hi"}}));
    CHECK_FALSE(ok.is_error);
    CHECK(ok.content == "echo: hi");
    CHECK(runs == 1);

    auto unknown = registry.dispatch(call("nope", Json::object()));
    CHECK(unknown.is_error);
    CHECK(unknown.content.get<std::string>().find("Unknown tool 'nope'") != std::string::npos);
    CHECK(unknown.content.get<std::string>().find("echo, object_result, thrower") != std::string::npos);

    ToolCall broken = call("echo", Json::object());
    broken.input_valid = false;
    broken.raw_input = R"({"text": "unterminated)";
    auto invalid = registry.dispatch(broken);
    CHECK(invalid.is_error);
    CHECK(Json::parse(invalid.content.get<std::string>()) == Json{{"INVALID_JSON", R"({"text": "unterminated)"}});

    auto schema_fail = registry.dispatch(call("echo", {{"text", 5}}));
    CHECK(schema_fail.is_error);
    const Json detail = Json::parse(schema_fail.content.get<std::string>());
    CHECK(detail["INVALID_JSON"] == R"({"text":5})");
    CHECK(detail["error"] == "input.text: expected string, got integer");
    CHECK(runs == 1);  // never ran on invalid input

    auto thrown = registry.dispatch(call("thrower", {{"text", "x"}}));
    CHECK(thrown.is_error);
    CHECK(thrown.content == "Tool 'thrower' failed: disk on fire");

    auto normalized = registry.dispatch(call("object_result", {{"text", "x"}}));
    CHECK(normalized.content == R"({"score":87.5})");  // tool_result content must be a string or blocks
}

TEST_CASE("invalid_input_content escapes the raw input with the JSON library") {
    const std::string raw = "{\"a\": \"quote \\\" and newline\n";
    const Json parsed = Json::parse(invalid_input_content(raw));
    CHECK(parsed["INVALID_JSON"] == raw);
    CHECK_FALSE(parsed.contains("error"));
    CHECK(Json::parse(invalid_input_content("x", "why"))["error"] == "why");
}
