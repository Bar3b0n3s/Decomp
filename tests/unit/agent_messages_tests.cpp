#include "agent/messages.hpp"
#include "agent/replay_transport.hpp"
#include "agent/sse.hpp"
#include "agent/tools.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace decomp;
using namespace decomp::agent;

namespace {

struct RecordingObserver : StreamObserver {
    std::string text;
    std::string thinking;
    std::string tool_input;
    std::vector<int> started;
    std::vector<int> stopped;
    int message_starts = 0;

    void on_message_start(const Response&) override { ++message_starts; }
    void on_block_start(int index, const Json&) override { started.push_back(index); }
    void on_text_delta(int, std::string_view t) override { text += t; }
    void on_thinking_delta(int, std::string_view t) override { thinking += t; }
    void on_tool_input_delta(int, std::string_view p) override { tool_input += p; }
    void on_block_stop(int index, const Json&) override { stopped.push_back(index); }
};

struct Accumulated {
    Response response;
    std::optional<Error> error;
    std::optional<StreamError> stream_error;
    bool finished = false;
};

// Serializes scripted events to SSE and feeds them through the parser in `chunk`-byte pieces.
Accumulated accumulate(const Json& events, std::size_t chunk, StreamObserver* observer = nullptr) {
    Accumulated out;
    MessageAccumulator acc(observer);
    SseParser parser([&](std::string_view event, std::string_view data) {
        if (out.error) return;
        if (auto r = acc.apply(event, data); !r) out.error = r.error();
    });
    const std::string sse = to_sse(events);
    for (std::size_t i = 0; i < sse.size(); i += chunk) parser.feed(std::string_view(sse).substr(i, chunk));
    parser.finish();
    out.finished = acc.finished();
    out.stream_error = acc.stream_error();
    out.response = acc.take();
    return out;
}

} // namespace

TEST_CASE("MessageAccumulator assembles text, thinking with signature and tool_use split mid-token") {
    const Json input = {{"source", "int add(int a, int b) { return a + b; }"}, {"max_instructions", 64}, {"flags", {"/O2", "/Gy"}}};
    const std::vector<Json> blocks = {
        replay::thinking("Let me look at the callee first. Then compile.", "sig_EqQBCkgIARABGAIiQL"),
        replay::text("I'll check the disassembly and compile a first attempt."),
        replay::tool_use("toolu_01", "compile_and_diff", input),
    };
    replay::MessageOptions options;
    options.id = "msg_01";
    options.pieces = 4;
    const Json events = replay::events(blocks, "tool_use", replay::usage(1200, 340, 800, 4000), options);

    for (std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{64}, std::size_t{100000}}) {
        CAPTURE(chunk);
        RecordingObserver observer;
        auto acc = accumulate(events, chunk, &observer);
        REQUIRE_FALSE(acc.error);
        CHECK(acc.finished);
        const Response& r = acc.response;
        CHECK(r.id == "msg_01");
        CHECK(r.model == "claude-opus-5-5");
        CHECK(r.stop_reason == "tool_use");
        CHECK(r.stop_details.is_null());
        REQUIRE(r.content.size() == 3);
        CHECK(r.content[0] == blocks[0]);  // thinking + signature, unchanged
        CHECK(r.content[1] == blocks[1]);
        CHECK(r.content[2] == blocks[2]);
        CHECK(r.content[2]["input"] == input);
        REQUIRE(r.blocks.size() == 3);
        CHECK(r.blocks[2].input_valid);
        CHECK(r.blocks[2].complete);
        CHECK(r.blocks[2].raw_input == dump_compact(input));
        CHECK(r.usage == Usage{1200, 340, 800, 4000});
        CHECK(r.usage_raw["cache_read_input_tokens"] == 4000);
        CHECK_FALSE(r.had_fallback);
        CHECK(echo_content(r) == r.content);

        CHECK(observer.message_starts == 1);
        CHECK(observer.thinking == "Let me look at the callee first. Then compile.");
        CHECK(observer.text == "I'll check the disassembly and compile a first attempt.");
        CHECK(observer.tool_input == dump_compact(input));
        CHECK(observer.started == std::vector<int>{0, 1, 2});
        CHECK(observer.stopped == std::vector<int>{0, 1, 2});
    }

    // The input JSON really was split into several fragments, including inside tokens.
    int fragments = 0;
    for (const Json& e : events) {
        if (e["event"] == "content_block_delta" && e["data"]["delta"]["type"] == "input_json_delta") ++fragments;
    }
    CHECK(fragments >= 5);
}

TEST_CASE("MessageAccumulator: usage from message_start is overridden by cumulative message_delta counts") {
    MessageAccumulator acc;
    REQUIRE(acc.apply("message_start", R"({"type":"message_start","message":{"id":"m","model":"claude-opus-5-5","usage":{"input_tokens":10,"cache_creation_input_tokens":5,"cache_read_input_tokens":7,"output_tokens":1}}})"));
    REQUIRE(acc.apply("message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"output_tokens":20}})"));
    REQUIRE(acc.apply("message_delta", R"({"type":"message_delta","delta":{},"usage":{"output_tokens":42}})"));
    REQUIRE(acc.apply("message_stop", R"({"type":"message_stop"})"));
    const Response& r = acc.response();
    CHECK(r.usage == Usage{10, 42, 5, 7});
    CHECK(r.usage.total() == 64);
    CHECK(r.stop_reason == "end_turn");
    CHECK(acc.finished());

    // ping and unknown events are ignored; the JSON type wins over a missing SSE event name.
    MessageAccumulator other;
    CHECK(other.apply("ping", R"({"type":"ping"})"));
    CHECK(other.apply("future_event", R"({"type":"future_event","x":1})"));
    CHECK(other.apply("", R"({"type":"message_start","message":{"id":"m2"}})"));
    CHECK(other.response().id == "m2");
}

TEST_CASE("MessageAccumulator keeps redacted_thinking and unknown blocks verbatim") {
    const Json redacted = replay::redacted_thinking("EmwKAhgBEgy3va3pzix/LafPsn4aDFIT2Xlxh0L5L8rLVyIw");
    const Json unknown = {{"type", "future_block"}, {"payload", {{"x", 1}}}};
    auto acc = accumulate(replay::events({redacted, unknown, replay::text("ok")}, "end_turn", replay::usage(1, 2)), 5);
    REQUIRE_FALSE(acc.error);
    REQUIRE(acc.response.content.size() == 3);
    CHECK(acc.response.content[0] == redacted);
    CHECK(acc.response.content[1] == unknown);
    CHECK(echo_content(acc.response) == acc.response.content);
}

TEST_CASE("MessageAccumulator: invalid or empty tool input") {
    const std::string broken = R"({"source": "int f() { return \"x)";
    auto acc = accumulate(replay::events({replay::tool_use_raw("toolu_bad", "compile_and_diff", broken),
                                          replay::tool_use_raw("toolu_empty", "list_things", "")},
                                         "tool_use", replay::usage(1, 2)),
                          2);
    REQUIRE_FALSE(acc.error);
    const Response& r = acc.response;
    REQUIRE(r.content.size() == 2);
    CHECK_FALSE(r.blocks[0].input_valid);
    CHECK(r.blocks[0].raw_input == broken);
    // The echoed block is a clean tool_use: no internal markers, input {}.
    CHECK(r.content[0] == Json{{"type", "tool_use"}, {"id", "toolu_bad"}, {"name", "compile_and_diff"}, {"input", Json::object()}});
    CHECK(r.blocks[1].input_valid);
    CHECK(r.content[1]["input"] == Json::object());

    auto calls = extract_tool_calls(r);
    REQUIRE(calls.size() == 2);
    CHECK_FALSE(calls[0].input_valid);
    CHECK(calls[0].raw_input == broken);
    CHECK(calls[1].input_valid);

    // A non-object value is not a valid tool input either.
    auto array_input = accumulate(replay::events({replay::tool_use_raw("toolu_arr", "x", "[1,2]")}, "tool_use", replay::usage(1, 1)), 64);
    CHECK_FALSE(array_input.response.blocks[0].input_valid);
}

TEST_CASE("echo_content applies the mid-output fallback rules") {
    const std::vector<Json> blocks = {
        replay::thinking("first model thinking", "sig_a"),                                     // 0 dropped
        replay::text("Partial answer from the first model."),                                  // 1 kept
        replay::tool_use("toolu_pre", "lookup_symbol", {{"query", "x"}}),                      // 2 dropped
        Json{{"type", "server_tool_use"}, {"id", "srv_paired"}, {"name", "web_search"}, {"input", {{"q", "a"}}}},  // 3 kept
        Json{{"type", "web_search_tool_result"}, {"tool_use_id", "srv_paired"}, {"content", Json::array()}},     // 4 kept
        Json{{"type", "server_tool_use"}, {"id", "srv_unpaired"}, {"name", "web_search"}, {"input", {{"q", "b"}}}}, // 5 dropped
        replay::redacted_thinking("opaque"),                                                   // 6 dropped
        Json{{"type", "future_block"}, {"x", 1}},                                              // 7 dropped
        replay::fallback("claude-opus-5-5", "claude-opus-4-8"),                                // 8 dropped (marker)
        replay::thinking("second model thinking", "sig_b"),                                    // 9 kept
        replay::text("Continuing after the switch."),                                          // 10 kept
        replay::tool_use("toolu_post", "read_memory", {{"address", "0x401000"}}),               // 11 kept
        Json{{"type", "future_block"}, {"y", 2}},                                              // 12 kept (after boundary)
    };
    auto acc = accumulate(replay::events(blocks, "tool_use", replay::usage(10, 20)), 7);
    REQUIRE_FALSE(acc.error);
    const Response& r = acc.response;
    CHECK(r.had_fallback);
    CHECK(echoed_block_indices(r) == std::vector<std::size_t>{1, 3, 4, 9, 10, 11, 12});
    const Json echoed = echo_content(r);
    REQUIRE(echoed.size() == 7);
    CHECK(echoed[0] == blocks[1]);
    CHECK(echoed[3] == blocks[9]);
    for (const Json& block : echoed) CHECK(block["type"] != "fallback");

    auto calls = extract_tool_calls(r);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].id == "toolu_post");
    CHECK(calls[0].input == Json{{"address", "0x401000"}});

    // Two fallbacks: only blocks before the last one are filtered.
    const std::vector<Json> twice = {
        replay::tool_use("toolu_a", "t", Json::object()), replay::fallback("m1", "m2"),
        replay::tool_use("toolu_b", "t", Json::object()), replay::fallback("m2", "m3"),
        replay::tool_use("toolu_c", "t", Json::object()),
    };
    auto acc2 = accumulate(replay::events(twice, "tool_use", replay::usage(1, 1)), 64);
    REQUIRE(extract_tool_calls(acc2.response).size() == 1);
    CHECK(extract_tool_calls(acc2.response)[0].id == "toolu_c");
}

TEST_CASE("MessageAccumulator reports stream error events") {
    auto overloaded = accumulate(replay::stream_error("overloaded_error", "Overloaded")["events"], 3);
    REQUIRE(overloaded.error);
    CHECK(overloaded.error->code == ErrorCode::api);
    CHECK(overloaded.error->message.find("overloaded_error") != std::string::npos);
    REQUIRE(overloaded.stream_error);
    CHECK(overloaded.stream_error->type == "overloaded_error");
    CHECK(overloaded.stream_error->message == "Overloaded");
    CHECK(overloaded.stream_error->retryable());
    CHECK_FALSE(overloaded.finished);

    CHECK(StreamError{"api_error", ""}.retryable());
    CHECK_FALSE(StreamError{"invalid_request_error", ""}.retryable());

    // Malformed events are parse errors.
    MessageAccumulator acc;
    CHECK(acc.apply("message_start", "{not json").error().code == ErrorCode::parse);
    CHECK(acc.apply("content_block_delta", R"({"type":"content_block_delta","index":3,"delta":{"type":"text_delta","text":"x"}})")
              .error()
              .code == ErrorCode::parse);
    CHECK_FALSE(acc.apply("content_block_start", R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})"));
}

TEST_CASE("MessageAccumulator::from_json reads non-streaming bodies") {
    const Json body = replay::json_message(
        {replay::thinking("t", "sig"), replay::text("hello"), replay::tool_use("toolu_1", "lookup", {{"q", 1}}),
         replay::tool_use_raw("toolu_2", "lookup", "{oops")},
        "tool_use", replay::usage(5, 6, 7, 8))["body"];
    auto r = MessageAccumulator::from_json(body);
    REQUIRE(r);
    CHECK(r->id == "msg_replay");
    CHECK(r->stop_reason == "tool_use");
    CHECK(r->usage == Usage{5, 6, 7, 8});
    REQUIRE(r->content.size() == 4);
    CHECK(r->content[0]["signature"] == "sig");
    CHECK(r->blocks[2].input_valid);
    CHECK_FALSE(r->blocks[3].input_valid);
    CHECK(r->blocks[3].raw_input == "{oops");
    CHECK(r->content[3]["input"] == Json::object());

    Json refused = body;
    refused["stop_reason"] = "refusal";
    refused["stop_details"] = {{"type", "refusal"}, {"category", "cyber"}, {"explanation", "no"}};
    auto rr = MessageAccumulator::from_json(refused);
    REQUIRE(rr);
    CHECK(rr->stop_details["category"] == "cyber");

    auto error = MessageAccumulator::from_json(Json{{"type", "error"}, {"error", {{"type", "invalid_request_error"}, {"message", "bad"}}}});
    REQUIRE_FALSE(error);
    CHECK(error.error().code == ErrorCode::api);
    CHECK(error.error().message == "invalid_request_error: bad");
}

TEST_CASE("Usage arithmetic") {
    Usage a{1, 2, 3, 4};
    a.add(Usage{10, 20, 30, 40});
    CHECK(a == Usage{11, 22, 33, 44});
    CHECK(a.total() == 110);
    CHECK(Usage::from_json(a.to_json()) == a);
    Usage b{1, 1, 1, 1};
    b.merge(Json{{"output_tokens", 9}, {"unrelated", "x"}});
    CHECK(b == Usage{1, 9, 1, 1});
}
