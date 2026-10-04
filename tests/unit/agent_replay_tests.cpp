#include "agent/loop.hpp"
#include "agent/replay_transport.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace decomp;
using namespace decomp::agent;
using std::chrono::milliseconds;

namespace {

Json strict_object(Json properties, std::vector<std::string> required) {
    return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", required}, {"additionalProperties", false}};
}

// Parallel-safe tools log from several threads at once.
struct ToolLog {
    std::mutex mutex;
    std::vector<std::string> entries;
    void push_back(std::string entry) {
        std::lock_guard lock(mutex);
        entries.push_back(std::move(entry));
    }
};

ToolRegistry decomp_tools(int& compiles, ToolLog& tool_log) {
    ToolRegistry tools;
    tools.add(Tool{"disassemble", "Annotated disassembly of a target function.",
                   strict_object({{"target", {{"type", "string"}}}, {"max_instructions", {{"type", "integer"}, {"minimum", 1}}}},
                                 {"target", "max_instructions"}),
                   true, [&tool_log](const ToolCall& call) {
                       tool_log.push_back("disassemble " + call.input["target"].get<std::string>());
                       return ToolResult::text("sum_array:\n  push esi\n  mov esi, [esp+8]\n  ...");
                   }});
    tools.add(Tool{"lookup_symbol", "Looks up a symbol by name or address.",
                   strict_object({{"query", {{"type", "string"}}}}, {"query"}), true, [&tool_log](const ToolCall& call) {
                       tool_log.push_back("lookup_symbol " + call.input["query"].get<std::string>());
                       return ToolResult::text("0x0040A1F0 data ?g_counter@@3HA (int g_counter) size=4");
                   }});
    tools.add(Tool{"compile_and_diff", "Compiles a translation unit and diffs the function against the target.",
                   strict_object({{"source", {{"type", "string"}}}}, {"source"}), false, [&](const ToolCall&) {
                       tool_log.push_back("compile_and_diff");
                       return ++compiles == 1 ? ToolResult::text("match 72.5%: 3 differing rows (loop shape)")
                                              : ToolResult::text("match 100%: exact, byte-exact");
                   }});
    tools.add(Tool{"submit_result", "Submits the final result and ends the session.",
                   strict_object({{"outcome", {{"type", "string"}, {"enum", {"matched", "give_up"}}}}, {"source", {{"type", "string"}}}},
                                 {"outcome", "source"}),
                   true, [&tool_log](const ToolCall& call) {
                       tool_log.push_back("submit_result");
                       ToolResult result = ToolResult::text("Verified and recorded.");
                       result.end_session = true;
                       result.outcome = call.input;
                       return result;
                   }});
    return tools;
}

struct SessionObserver : LoopObserver {
    std::string thinking;
    int retries = 0;
    int responses = 0;
    long long cache_reads = 0;
    void on_thinking_delta(int, std::string_view t) override { thinking += t; }
    void on_retry(int, const Error&, milliseconds) override { ++retries; }
    void on_response(int, const Response& r) override {
        ++responses;
        cache_reads += r.usage.cache_read_input_tokens;
    }
};

} // namespace

TEST_CASE("A scripted JSONL session replays end to end") {
    auto loaded = ReplayTransport::load(test::source_dir() / "tests" / "replay" / "agent_session_sum_array.jsonl");
    REQUIRE(loaded);
    std::shared_ptr<ReplayTransport> transport = *loaded;
    CHECK(transport->remaining() == 6);

    ClientConfig config;
    config.api_key = "sk-ant-replay";
    config.base_url = "https://api.anthropic.com";
    config.betas.clear();
    std::vector<milliseconds> sleeps;
    Client client(config, transport, [&](milliseconds d) { sleeps.push_back(d); });

    int compiles = 0;
    ToolLog tool_log;
    ToolRegistry tools = decomp_tools(compiles, tool_log);
    Conversation conversation(ConversationSettings{}, "You are an expert at matching decompilation of MSVC 6 binaries.",
                              tools.definitions());
    conversation.append_user_text("Function sum_array at 0x00401000 (0x2C bytes), MSVC 6 /O2. Make it match.");

    LoopConfig loop_config;
    loop_config.finish_tool = "submit_result";
    loop_config.status_line = [&] { return std::format("compiles so far: {}", compiles); };
    SessionObserver observer;
    const LoopOutcome outcome = run_loop(client, conversation, tools, loop_config, nullptr, &observer);

    CHECK(outcome.status == LoopStatus::finished);
    CHECK(outcome.turns == 5);
    CHECK(outcome.finish_outcome["outcome"] == "matched");
    CHECK(outcome.finish_outcome["source"].get<std::string>().find("while (count-- > 0)") != std::string::npos);
    // Turn 1's two read-only calls ran concurrently (either order); everything else is sequential.
    REQUIRE(tool_log.entries.size() == 5);
    std::sort(tool_log.entries.begin(), tool_log.entries.begin() + 2);
    CHECK(tool_log.entries == std::vector<std::string>{"disassemble sum_array", "lookup_symbol 0x0040A1F0",
                                                       "compile_and_diff", "compile_and_diff", "submit_result"});
    CHECK(observer.retries == 1);
    REQUIRE(sleeps.size() == 1);
    CHECK(sleeps[0] >= milliseconds(3000));
    CHECK(observer.thinking.find("pointer walk") != std::string::npos);
    CHECK(observer.cache_reads == 24780);
    CHECK(outcome.usage == Usage{35, 1610, 6970, 24780});
    CHECK(outcome.cost_usd == doctest::Approx((35 * 4.0 + 1610 * 20.0 + 6970 * 5.0 + 24780 * 0.20) / 1e6));
    CHECK(transport->remaining() == 0);

    const auto requests = transport->requests();
    REQUIRE(requests.size() == 6);
    CHECK(requests[1].raw_body == requests[2].raw_body);  // the rate-limited request was retried unchanged
    for (std::size_t i = 1; i < requests.size(); ++i) {
        CAPTURE(i);
        CHECK(is_prefix_extension(requests[i - 1].body, requests[i].body));
        CHECK(requests[i].header("x-api-key") == "***");
    }
    // Turn 5 starts with the nudge (turn 4 ended without submit_result) plus the status line.
    const Json& nudge = requests[5].body["messages"].back();
    REQUIRE(nudge["content"].size() == 2);
    CHECK(nudge["content"][0]["text"].get<std::string>().find("submit_result") != std::string::npos);
    CHECK(nudge["content"][1]["text"] == "compiles so far: 2");
}

TEST_CASE("ReplayTransport::parse_script reports bad lines") {
    auto ok = ReplayTransport::parse_script("# comment\n\n{\"network_error\":\"x\"}\r\n  {\"status\":500}  \n");
    REQUIRE(ok);
    CHECK(ok->size() == 2);

    auto bad = ReplayTransport::parse_script("{\"status\":200}\n{oops\n");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == ErrorCode::parse);
    CHECK(bad.error().message.find("line 2") != std::string::npos);

    auto not_object = ReplayTransport::parse_script("[1,2]\n");
    REQUIRE_FALSE(not_object);
    CHECK(not_object.error().message.find("line 1") != std::string::npos);

    CHECK_FALSE(ReplayTransport::load(test::source_dir() / "tests" / "replay" / "does_not_exist.jsonl"));
}

TEST_CASE("to_sse serializes events, including multi-line data") {
    const Json events = Json::array({Json{{"event", "ping"}, {"data", Json{{"type", "ping"}}}},
                                     Json{{"event", "raw"}, {"data", "line1\nline2"}}});
    CHECK(to_sse(events) == "event: ping\ndata: {\"type\":\"ping\"}\n\nevent: raw\ndata: line1\ndata: line2\n\n");
    CHECK(to_sse(events, true) == "event: ping\r\ndata: {\"type\":\"ping\"}\r\n\r\nevent: raw\r\ndata: line1\r\ndata: line2\r\n\r\n");
}
