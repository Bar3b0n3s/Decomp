#include "agent/conversation.hpp"
#include "agent/loop.hpp"
#include "agent/replay_transport.hpp"
#include "agent/tools.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <vector>

using namespace decomp;
using namespace decomp::agent;

namespace {

Tool simple_tool(std::string name, bool end_session = false) {
    Tool tool;
    tool.name = name;
    tool.description = "Test tool " + name + ".";
    tool.input_schema = Json::parse(
        R"({"type":"object","properties":{"value":{"type":"string"}},"required":["value"],"additionalProperties":false})");
    tool.handler = [name, end_session](const ToolCall& call) {
        ToolResult result = ToolResult::text(name + " -> " + call.input["value"].get<std::string>());
        if (end_session) {
            result.end_session = true;
            result.outcome = call.input;
        }
        return result;
    };
    return tool;
}

ClientConfig test_config() {
    ClientConfig config;
    config.api_key = "sk-ant-test";
    config.base_url = "https://api.example.test";
    config.betas.clear();
    return config;
}

const char* kSystem = "You are a matching decompiler. Use the tools; call submit_result when done.";

} // namespace

TEST_CASE("Conversation request shape") {
    ToolRegistry tools;
    tools.add(simple_tool("lookup_symbol"));
    tools.add(simple_tool("compile_and_diff"));
    Conversation conversation(ConversationSettings{}, kSystem, tools.definitions());
    conversation.append_user_text("Match sum_array at 0x401000.");
    const Json request = conversation.build_request();

    CHECK(request["model"] == "claude-opus-5-5");
    CHECK(request["max_tokens"] == 64000);
    CHECK(request["stream"] == true);
    CHECK(request["thinking"] == Json{{"type", "adaptive"}, {"display", "summarized"}});
    CHECK(request["output_config"] == Json{{"effort", "high"}});
    CHECK(request["cache_control"] == Json{{"type", "ephemeral"}});
    const Json system_block{{"type", "text"}, {"text", kSystem}, {"cache_control", Json{{"type", "ephemeral"}}}};
    CHECK(request["system"] == Json::array({system_block}));
    CHECK(request["tools"] == tools.definitions());
    CHECK(request["tools"][0]["name"] == "compile_and_diff");  // sorted
    CHECK(request["tool_choice"] == Json{{"type", "auto"}});
    CHECK(request["fallbacks"] == "default");
    CHECK(conversation.betas() == std::vector<std::string>{"server-side-fallback-2026-07-01"});
    const Json user_message{{"role", "user"},
                            {"content", Json::array({Json{{"type", "text"}, {"text", "Match sum_array at 0x401000."}}})}};
    CHECK(request["messages"] == Json::array({user_message}));
    CHECK(request.size() == 11);

    // Fields that are 400s on claude-opus-5-5 are never sent.
    for (const char* forbidden : {"temperature", "top_p", "top_k", "budget_tokens", "stop_sequences"})
        CHECK_FALSE(request.contains(forbidden));
    CHECK_FALSE(request["thinking"].contains("budget_tokens"));
    CHECK(request["thinking"]["type"] == "adaptive");
}

TEST_CASE("Conversation settings: fallbacks off, no tools, no system prompt") {
    ConversationSettings settings;
    settings.model = "claude-sonnet-5-5";
    settings.fallbacks = false;
    settings.effort = "max";
    settings.thinking_display = "";
    settings.stream = false;
    settings.max_tokens = 1000;
    Conversation conversation(settings, "", Json::array());
    conversation.append_user_text("hi");
    const Json request = conversation.build_request();
    CHECK_FALSE(request.contains("fallbacks"));
    CHECK(conversation.betas().empty());
    CHECK_FALSE(request.contains("tools"));
    CHECK_FALSE(request.contains("tool_choice"));  // tool_choice without tools is invalid
    CHECK_FALSE(request.contains("system"));
    CHECK(request["thinking"] == Json{{"type", "adaptive"}});
    CHECK(request["output_config"]["effort"] == "max");
    CHECK(request["model"] == "claude-sonnet-5-5");
    CHECK(request["stream"] == false);
}

TEST_CASE("Conversation appends verbatim and is_prefix_extension detects edits") {
    Conversation conversation(ConversationSettings{}, kSystem, Json::array());
    conversation.append_user_text("brief");
    const Json first = conversation.build_request();
    const Json assistant = Json::array({replay::thinking("", "sig_empty_thinking"), replay::text("Working on it."),
                                        replay::tool_use("toolu_1", "lookup_symbol", {{"value", "x"}})});
    conversation.append_assistant(assistant);
    conversation.append_user_blocks(Json{{"type", "tool_result"}, {"tool_use_id", "toolu_1"}, {"content", "ok"}});
    const Json second = conversation.build_request();

    CHECK(second["messages"][1]["content"] == assistant);  // thinking with empty text passed back unchanged
    CHECK(second["messages"][2]["content"].is_array());     // single block wrapped
    CHECK(conversation.size() == 3);
    CHECK(is_prefix_extension(first, second));
    CHECK(is_prefix_extension(second, second));
    CHECK_FALSE(is_prefix_extension(second, first));

    Json edited = second;
    edited["messages"][0]["content"][0]["text"] = "brief (edited)";
    CHECK_FALSE(is_prefix_extension(first, edited));

    Json other_system = second;
    other_system["system"][0]["text"] = "changed";
    CHECK_FALSE(is_prefix_extension(first, other_system));

    Json other_effort = second;
    other_effort["output_config"]["effort"] = "low";
    CHECK_FALSE(is_prefix_extension(first, other_effort));
}

TEST_CASE("Every request of a multi-turn loop extends the previous one byte-for-byte") {
    const std::vector<Json> script = {
        replay::message({replay::thinking("Need the symbol first.", "sig_t1"),
                         replay::tool_use("toolu_1", "lookup_symbol", {{"value", "g_counter"}})},
                        "tool_use", replay::usage(2500, 120, 2400, 0)),
        replay::message({replay::thinking("Two things at once.", "sig_t2"), replay::text("Checking both."),
                         replay::tool_use("toolu_2", "lookup_symbol", {{"value", "sum_array"}}),
                         replay::tool_use("toolu_3", "compile_and_diff", {{"value", "int sum_array();"}})},
                        "tool_use", replay::usage(300, 400, 260, 2400)),
        replay::message({replay::thinking("", "sig_t3"), replay::text("I believe this matches now.")}, "end_turn",
                        replay::usage(500, 60, 480, 2660)),
        replay::message({replay::tool_use("toolu_4", "submit_result", {{"value", "matched"}})}, "tool_use",
                        replay::usage(120, 40, 100, 3140)),
    };
    auto transport = std::make_shared<ReplayTransport>(script);
    Client client(test_config(), transport, [](std::chrono::milliseconds) {});
    ToolRegistry tools;
    tools.add(simple_tool("lookup_symbol"));
    tools.add(simple_tool("compile_and_diff"));
    tools.add(simple_tool("submit_result", true));
    Conversation conversation(ConversationSettings{}, kSystem, tools.definitions());
    conversation.append_user_text("Match sum_array.");

    LoopControl control;
    struct Injector : LoopObserver {
        LoopControl* control = nullptr;
        void on_response(int turn, const Response&) override {
            if (turn == 2) control->inject("Keep the loop counter in a register.");
        }
    } injector;
    injector.control = &control;
    LoopConfig config;
    config.finish_tool = "submit_result";
    int status_calls = 0;
    config.status_line = [&](const LoopProgress& p) {
        ++status_calls;
        return std::format("turns left: {}", p.turns_left());
    };

    const LoopOutcome outcome = run_loop(client, conversation, tools, config, &control, &injector);
    REQUIRE(outcome.status == LoopStatus::finished);
    CHECK(outcome.turns == 4);

    const auto requests = transport->requests();
    REQUIRE(requests.size() == 4);
    for (std::size_t i = 1; i < requests.size(); ++i) {
        CAPTURE(i);
        CHECK(is_prefix_extension(requests[i - 1].body, requests[i].body));
        CHECK(requests[i].body["messages"].size() == requests[i - 1].body["messages"].size() + 2);
        CHECK(dump_compact(requests[i].body["system"]) == dump_compact(requests[0].body["system"]));
        CHECK(dump_compact(requests[i].body["tools"]) == dump_compact(requests[0].body["tools"]));
        CHECK(requests[i].header("anthropic-beta") == "server-side-fallback-2026-07-01");
    }
    // The final conversation (with the submit_result tool_result) still extends the last request.
    CHECK(is_prefix_extension(requests.back().body, conversation.build_request()));
    // Assistant turns are echoed verbatim, thinking signatures included.
    CHECK(requests[1].body["messages"][1]["content"][0] == replay::thinking("Need the symbol first.", "sig_t1"));
    CHECK(requests[3].body["messages"][5]["content"][0] == replay::thinking("", "sig_t3"));
}
