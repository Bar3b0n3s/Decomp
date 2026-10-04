#include "agent/loop.hpp"
#include "agent/replay_transport.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace decomp;
using namespace decomp::agent;
using namespace std::chrono_literals;

namespace {

ClientConfig test_config() {
    ClientConfig config;
    config.api_key = "sk-ant-test";
    config.base_url = "https://api.example.test";
    config.betas.clear();
    return config;
}

Json object_schema(std::string_view property, Json property_schema) {
    return Json{{"type", "object"},
                {"properties", Json{{std::string(property), std::move(property_schema)}}},
                {"required", Json::array({std::string(property)})},
                {"additionalProperties", false}};
}

// Two parallel-safe tools meet here; `overlapped` proves they ran at the same time.
struct Rendezvous {
    std::mutex mutex;
    std::condition_variable cv;
    int arrived = 0;
    bool overlapped = false;

    void arrive() {
        std::unique_lock lock(mutex);
        if (++arrived >= 2) {
            cv.notify_all();
            return;
        }
        if (cv.wait_for(lock, 10s, [&] { return arrived >= 2; })) overlapped = true;
    }
};

struct Env {
    std::shared_ptr<ReplayTransport> transport;
    Client client;
    ToolRegistry tools;
    std::atomic<int> lookups{0};
    std::atomic<int> reads{0};
    std::vector<std::string> compile_order;
    Rendezvous* rendezvous = nullptr;

    explicit Env(std::vector<Json> script, ReplayOptions options = {})
        : transport(std::make_shared<ReplayTransport>(std::move(script), options)),
          client(test_config(), transport, [](std::chrono::milliseconds) {}) {
        Tool lookup;
        lookup.name = "lookup_symbol";
        lookup.description = "Looks up a symbol.";
        lookup.input_schema = object_schema("query", {{"type", "string"}});
        lookup.handler = [this](const ToolCall& call) {
            ++lookups;
            if (rendezvous) rendezvous->arrive();
            return ToolResult::text("symbol " + call.input["query"].get<std::string>() + " at 0x00401000");
        };
        tools.add(lookup);

        Tool read;
        read.name = "read_memory";
        read.description = "Reads bytes.";
        read.input_schema = object_schema("address", {{"type", "string"}});
        read.handler = [this](const ToolCall& call) {
            ++reads;
            if (rendezvous) rendezvous->arrive();
            return ToolResult::text("bytes at " + call.input["address"].get<std::string>() + ": 55 8B EC");
        };
        tools.add(read);

        Tool compile;
        compile.name = "compile_and_diff";
        compile.description = "Compiles and diffs.";
        compile.input_schema = object_schema("source", {{"type", "string"}});
        compile.parallel_safe = false;
        compile.handler = [this](const ToolCall& call) {
            compile_order.push_back(call.id);
            return ToolResult::text("match: 87.5%");
        };
        tools.add(compile);

        Tool submit;
        submit.name = "submit_result";
        submit.description = "Finishes the session.";
        submit.input_schema = object_schema("outcome", {{"type", "string"}, {"enum", {"matched", "give_up"}}});
        submit.handler = [](const ToolCall& call) {
            ToolResult result = ToolResult::text("recorded");
            result.end_session = true;
            result.outcome = call.input;
            return result;
        };
        tools.add(submit);
    }

    Conversation conversation(ConversationSettings settings = {}) {
        Conversation c(std::move(settings), "You are a test agent.", tools.definitions());
        c.append_user_text("Match sum_array.");
        return c;
    }
};

LoopConfig finish_config() {
    LoopConfig config;
    config.finish_tool = "submit_result";
    return config;
}

Json submit_turn(std::string id = "toolu_submit") {
    return replay::message({replay::tool_use(std::move(id), "submit_result", {{"outcome", "matched"}})}, "tool_use",
                           replay::usage(100, 20));
}

struct Recorder : LoopObserver {
    std::vector<std::string> log;
    std::vector<ToolResult> results;
    std::vector<std::string> injected;
    std::optional<LoopOutcome> finished;
    std::function<void(int, const Response&)> response_hook;
    std::function<void()> text_hook;

    void on_turn_start(int turn, const Json&) override { log.push_back(std::format("turn {}", turn)); }
    void on_response(int turn, const Response& r) override {
        log.push_back(std::format("response {} {}", turn, r.stop_reason));
        if (response_hook) response_hook(turn, r);
    }
    void on_text_delta(int, std::string_view) override {
        if (text_hook) text_hook();
    }
    void on_tool_start(int, const ToolCall& call) override { log.push_back("start " + call.id); }
    void on_tool_end(int, const ToolCall& call, const ToolResult& result, std::chrono::milliseconds) override {
        log.push_back("end " + call.id);
        results.push_back(result);
    }
    void on_injected(const std::string& text) override { injected.push_back(text); }
    void on_finish(const LoopOutcome& outcome) override { finished = outcome; }
};

const Json& last_message(const RecordedRequest& request) { return request.body["messages"].back(); }

} // namespace

TEST_CASE("run_loop: parallel tool calls, one result message, nudge, then finish") {
    Env env({
        replay::message({replay::thinking("Look up both at once.", "sig_1"), replay::text("Checking."),
                         replay::tool_use("toolu_1", "lookup_symbol", {{"query", "g_counter"}}),
                         replay::tool_use("toolu_2", "read_memory", {{"address", "0x401000"}})},
                        "tool_use", replay::usage(1000, 100, 900, 0)),
        replay::message({replay::text("The function is simple; I think we're done.")}, "end_turn", replay::usage(200, 30, 0, 900)),
        submit_turn("toolu_3"),
    });
    Rendezvous rendezvous;
    env.rendezvous = &rendezvous;
    Conversation conversation = env.conversation();
    Recorder recorder;
    const LoopOutcome outcome = run_loop(env.client, conversation, env.tools, finish_config(), nullptr, &recorder);

    CHECK(outcome.status == LoopStatus::finished);
    CHECK(outcome.finish_outcome == Json{{"outcome", "matched"}});
    CHECK(outcome.turns == 3);
    CHECK(outcome.usage == Usage{1300, 150, 900, 900});
    CHECK(outcome.cost_usd > 0);
    CHECK(rendezvous.overlapped);  // the two parallel-safe calls ran concurrently
    CHECK(env.lookups == 1);
    CHECK(env.reads == 1);
    REQUIRE(recorder.finished);
    CHECK(recorder.finished->status == LoopStatus::finished);

    const auto requests = env.transport->requests();
    REQUIRE(requests.size() == 3);
    // Turn 2 starts with ONE user message holding both results, in tool_use order.
    const Json& results = last_message(requests[1]);
    CHECK(results["role"] == "user");
    REQUIRE(results["content"].size() == 2);
    CHECK(results["content"][0] == Json{{"type", "tool_result"}, {"tool_use_id", "toolu_1"}, {"content", "symbol g_counter at 0x00401000"}});
    CHECK(results["content"][1] == Json{{"type", "tool_result"}, {"tool_use_id", "toolu_2"}, {"content", "bytes at 0x401000: 55 8B EC"}});
    // Turn 3 starts with the nudge.
    const Json& nudge = last_message(requests[2]);
    CHECK(nudge["role"] == "user");
    REQUIRE(nudge["content"].size() == 1);
    CHECK(nudge["content"][0]["text"].get<std::string>().find("submit_result") != std::string::npos);

    // Transcript is complete: the submit_result call is answered.
    const Json& messages = conversation.messages();
    REQUIRE(messages.size() == 7);
    CHECK(messages[6]["content"][0]["tool_use_id"] == "toolu_3");
    CHECK(messages[5]["role"] == "assistant");
}

TEST_CASE("run_loop: refusal stops without running tools or appending content") {
    const Json details = {{"type", "refusal"}, {"category", "cyber"}, {"explanation", "This looks like malware analysis."}};
    Env env({replay::refusal(details, {replay::text("Sure, let me"), replay::tool_use("toolu_1", "lookup_symbol", {{"query", "x"}})}),
             submit_turn()});
    Conversation conversation = env.conversation();
    const LoopOutcome outcome = run_loop(env.client, conversation, env.tools, finish_config());
    CHECK(outcome.status == LoopStatus::refused);
    CHECK(outcome.stop_details == details);
    CHECK(outcome.detail.find("cyber") != std::string::npos);
    CHECK(outcome.turns == 1);
    CHECK(env.lookups == 0);
    CHECK(conversation.size() == 1);  // only the brief
    CHECK(env.transport->remaining() == 1);
}

TEST_CASE("run_loop: budgets") {
    SUBCASE("cost") {
        // 300k input tokens on claude-opus-5-5 = $1.20.
        Env env({replay::message({replay::tool_use("toolu_1", "lookup_symbol", {{"query", "a"}})}, "tool_use", replay::usage(300000, 10)),
                 submit_turn()});
        Conversation conversation = env.conversation();
        LoopConfig config = finish_config();
        config.max_cost_usd = 1.0;
        const LoopOutcome outcome = run_loop(env.client, conversation, env.tools, config);
        CHECK(outcome.status == LoopStatus::budget_exhausted);
        CHECK(outcome.detail.find("cost budget") != std::string::npos);
        CHECK(outcome.cost_usd == doctest::Approx(1.2002));
        CHECK(outcome.turns == 1);
        CHECK(env.lookups == 1);  // the turn's calls still ran and were answered
        CHECK(conversation.messages().back()["content"][0]["tool_use_id"] == "toolu_1");
    }
    SUBCASE("turns") {
        Env env({replay::message({replay::tool_use("toolu_1", "lookup_symbol", {{"query", "a"}})}, "tool_use", replay::usage(10, 10)),
                 replay::message({replay::tool_use("toolu_2", "lookup_symbol", {{"query", "b"}})}, "tool_use", replay::usage(10, 10)),
                 submit_turn()});
        Conversation conversation = env.conversation();
        LoopConfig config = finish_config();
        config.max_turns = 2;
        const LoopOutcome outcome = run_loop(env.client, conversation, env.tools, config);
        CHECK(outcome.status == LoopStatus::max_turns);
        CHECK(outcome.turns == 2);
        CHECK(env.transport->remaining() == 1);
    }
    SUBCASE("tokens") {
        Env env({replay::message({replay::tool_use("toolu_1", "lookup_symbol", {{"query", "a"}})}, "tool_use", replay::usage(400, 100, 50, 50)),
                 replay::message({replay::tool_use("toolu_2", "lookup_symbol", {{"query", "b"}})}, "tool_use", replay::usage(400, 100, 50, 50)),
                 submit_turn()});
        Conversation conversation = env.conversation();
        LoopConfig config = finish_config();
        config.max_total_tokens = 1000;
        const LoopOutcome outcome = run_loop(env.client, conversation, env.tools, config);
        CHECK(outcome.status == LoopStatus::budget_exhausted);
        CHECK(outcome.turns == 2);
        CHECK(outcome.usage.total() == 1200);
    }
    SUBCASE("finishing on the last affordable turn wins over the budget") {
        Env env({submit_turn()});
        Conversation conversation = env.conversation();
        LoopConfig config = finish_config();
        config.max_turns = 1;
        CHECK(run_loop(env.client, conversation, env.tools, config).status == LoopStatus::finished);
    }
}

TEST_CASE("run_loop: invalid tool JSON gets an INVALID_JSON error result and the loop continues") {
    const std::string broken = R"({"query": "g_count)";
    Env env({replay::message({replay::tool_use_raw("toolu_1", "lookup_symbol", broken)}, "tool_use", replay::usage(10, 10)),
             submit_turn()});
    Conversation conversation = env.conversation();
    Recorder recorder;
    const LoopOutcome outcome = run_loop(env.client, conversation, env.tools, finish_config(), nullptr, &recorder);
    CHECK(outcome.status == LoopStatus::finished);
    CHECK(env.lookups == 0);
    const auto requests = env.transport->requests();
    REQUIRE(requests.size() == 2);
    // The echoed tool_use carries {} and no internal markers.
    CHECK(requests[1].body["messages"][1]["content"][0] ==
          Json{{"type", "tool_use"}, {"id", "toolu_1"}, {"name", "lookup_symbol"}, {"input", Json::object()}});
    const Json& result = last_message(requests[1])["content"][0];
    CHECK(result["is_error"] == true);
    CHECK(result["tool_use_id"] == "toolu_1");
    CHECK(Json::parse(result["content"].get<std::string>()) == Json{{"INVALID_JSON", broken}});
    REQUIRE(recorder.results.size() == 2);
    CHECK(recorder.results[0].is_error);
}

TEST_CASE("run_loop: schema-invalid input is answered with an error, not executed") {
    Env env({replay::message({replay::tool_use("toolu_1", "lookup_symbol", {{"query", 42}})}, "tool_use", replay::usage(10, 10)),
             submit_turn()});
    Conversation conversation = env.conversation();
    CHECK(run_loop(env.client, conversation, env.tools, finish_config()).status == LoopStatus::finished);
    CHECK(env.lookups == 0);
    const Json result = last_message(env.transport->requests()[1])["content"][0];
    CHECK(result["is_error"] == true);
    CHECK(Json::parse(result["content"].get<std::string>())["error"] == "input.query: expected string, got integer");
}

TEST_CASE("run_loop: supervisor guidance and the status line follow the tool results") {
    Env env({replay::message({replay::tool_use("toolu_1", "lookup_symbol", {{"query", "a"}}),
                              replay::tool_use("toolu_2", "lookup_symbol", {{"query", "b"}})},
                             "tool_use", replay::usage(10, 10)),
             submit_turn()});
    Conversation conversation = env.conversation();
    LoopControl control;
    Recorder recorder;
    recorder.response_hook = [&](int turn, const Response&) {
        if (turn == 1) {
            control.inject("Prefer a for loop over while.");
            control.inject("Also check the calling convention.");
        }
    };
    LoopConfig config = finish_config();
    config.status_line = [] { return std::string("turns left: 38, best match: 87.5%"); };
    CHECK(run_loop(env.client, conversation, env.tools, config, &control, &recorder).status == LoopStatus::finished);

    const auto requests = env.transport->requests();
    REQUIRE(requests.size() == 2);
    const Json& message = last_message(requests[1]);
    REQUIRE(message["content"].size() == 3);
    CHECK(message["content"][0]["type"] == "tool_result");
    CHECK(message["content"][1]["type"] == "tool_result");
    CHECK(message["content"][2] == Json{{"type", "text"},
                                        {"text", "[Supervisor guidance] Prefer a for loop over while.\n\n"
                                                 "[Supervisor guidance] Also check the calling convention.\n\n"
                                                 "turns left: 38, best match: 87.5%"}});
    CHECK(recorder.injected == std::vector<std::string>{"Prefer a for loop over while.", "Also check the calling convention."});
    CHECK_FALSE(control.has_injected());
    // The final tool_result message (appended when the session ends) carries no status line.
    CHECK(conversation.messages().back()["content"].size() == 1);
}

TEST_CASE("run_loop: pause, resume and stop from another thread") {
    Env env({replay::message({replay::tool_use("toolu_1", "lookup_symbol", {{"query", "a"}})}, "tool_use", replay::usage(10, 10)),
             replay::message({replay::tool_use("toolu_2", "lookup_symbol", {{"query", "b"}})}, "tool_use", replay::usage(10, 10)),
             submit_turn()});
    Conversation conversation = env.conversation();
    LoopControl control;

    struct Sync : LoopObserver {
        std::mutex mutex;
        std::condition_variable cv;
        int responses = 0;
        int released = 0;
        bool paused = false;
        bool resumed = false;

        void on_response(int turn, const Response&) override {
            std::unique_lock lock(mutex);
            responses = turn;
            cv.notify_all();
            cv.wait_for(lock, 10s, [&] { return released >= turn; });
        }
        void on_paused() override {
            std::lock_guard lock(mutex);
            paused = true;
            cv.notify_all();
        }
        void on_resumed() override {
            std::lock_guard lock(mutex);
            resumed = true;
        }
        bool wait(const std::function<bool()>& ready) {
            std::unique_lock lock(mutex);
            return cv.wait_for(lock, 10s, ready);
        }
        void release(int turn) {
            std::lock_guard lock(mutex);
            released = turn;
            cv.notify_all();
        }
    } sync;

    std::optional<LoopOutcome> outcome;
    std::thread worker([&] { outcome = run_loop(env.client, conversation, env.tools, finish_config(), &control, &sync); });

    CHECK(sync.wait([&] { return sync.responses >= 1; }));
    control.request_pause();
    sync.release(1);
    CHECK(sync.wait([&] { return sync.paused; }));  // loop is parked before sending turn 2
    CHECK(control.is_paused());
    CHECK(env.transport->requests().size() == 1);
    control.inject("Look at the callers too.");
    control.resume();
    CHECK(sync.wait([&] { return sync.responses >= 2; }));
    control.request_stop();  // graceful: turn 2's tools still run
    sync.release(2);
    worker.join();

    REQUIRE(outcome);
    CHECK(outcome->status == LoopStatus::stopped);
    CHECK(outcome->turns == 2);
    CHECK(sync.resumed);
    CHECK(env.lookups == 2);
    const auto requests = env.transport->requests();
    REQUIRE(requests.size() == 2);
    const Json& turn2 = last_message(requests[1]);
    REQUIRE(turn2["content"].size() == 2);
    CHECK(turn2["content"][1]["text"] == "[Supervisor guidance] Look at the callers too.");
    CHECK(conversation.messages().back()["content"][0]["tool_use_id"] == "toolu_2");
    CHECK(env.transport->remaining() == 1);
}

TEST_CASE("run_loop: abort cancels the in-flight stream") {
    SUBCASE("while streaming") {
        Env env({replay::message({replay::text("A long explanation that is still streaming when the user aborts.")}, "end_turn",
                                 replay::usage(10, 10), replay::MessageOptions{.pieces = 10}),
                 submit_turn()},
                ReplayOptions{.max_chunk = 8});
        Conversation conversation = env.conversation();
        LoopControl control;
        Recorder recorder;
        recorder.text_hook = [&] { control.request_abort(); };
        const LoopOutcome outcome = run_loop(env.client, conversation, env.tools, finish_config(), &control, &recorder);
        CHECK(outcome.status == LoopStatus::aborted);
        CHECK(outcome.turns == 1);
        CHECK(conversation.size() == 1);
        CHECK(env.transport->remaining() == 1);
    }
    SUBCASE("racing the end of the stream: no tool runs") {
        Env env({replay::message({replay::tool_use("toolu_1", "lookup_symbol", {{"query", "a"}})}, "tool_use", replay::usage(10, 10)),
                 submit_turn()});
        Conversation conversation = env.conversation();
        LoopControl control;
        Recorder recorder;
        recorder.response_hook = [&](int, const Response&) { control.request_abort(); };
        const LoopOutcome outcome = run_loop(env.client, conversation, env.tools, finish_config(), &control, &recorder);
        CHECK(outcome.status == LoopStatus::aborted);
        CHECK(env.lookups == 0);
        CHECK(conversation.size() == 1);
        CHECK(outcome.usage == Usage{10, 10, 0, 0});  // the turn's tokens are still accounted
    }
    SUBCASE("while paused") {
        Env env({submit_turn()});
        Conversation conversation = env.conversation();
        LoopControl control;
        control.request_pause();
        std::optional<LoopOutcome> outcome;
        std::thread worker([&] { outcome = run_loop(env.client, conversation, env.tools, finish_config(), &control); });
        std::this_thread::sleep_for(20ms);
        control.request_abort();
        worker.join();
        REQUIRE(outcome);
        CHECK(outcome->status == LoopStatus::aborted);
        CHECK(outcome->turns == 0);
        CHECK(env.transport->requests().empty());
    }
}

TEST_CASE("run_loop: after a mid-output fallback only post-fallback tool calls run") {
    Env env({replay::message({replay::thinking("first model", "sig_a"), replay::text("Let me look this up."),
                              replay::tool_use("toolu_pre", "lookup_symbol", {{"query", "pre"}}),
                              replay::fallback("claude-opus-5-5", "claude-opus-4-8"), replay::text("Continuing."),
                              replay::tool_use("toolu_post", "read_memory", {{"address", "0x10"}})},
                             "tool_use", replay::usage(100, 50)),
             submit_turn()});
    Conversation conversation = env.conversation();
    CHECK(run_loop(env.client, conversation, env.tools, finish_config()).status == LoopStatus::finished);
    CHECK(env.lookups == 0);
    CHECK(env.reads == 1);
    const auto requests = env.transport->requests();
    REQUIRE(requests.size() == 2);
    const Json& assistant = requests[1].body["messages"][1];
    CHECK(assistant["role"] == "assistant");
    CHECK(assistant["content"] == Json::array({replay::text("Let me look this up."), replay::text("Continuing."),
                                               replay::tool_use("toolu_post", "read_memory", {{"address", "0x10"}})}));
    const Json& results = last_message(requests[1]);
    REQUIRE(results["content"].size() == 1);
    CHECK(results["content"][0]["tool_use_id"] == "toolu_post");
}

TEST_CASE("run_loop: a tool call cut off at max_tokens is answered with an error") {
    Env env({replay::message({replay::tool_use("toolu_1", "lookup_symbol", {{"query", "ok"}}),
                              replay::tool_use_raw("toolu_2", "compile_and_diff", R"({"source": "int sum_array(const int* a, int n) {)")},
                             "max_tokens", replay::usage(100, 64000)),
             submit_turn()});
    Conversation conversation = env.conversation();
    CHECK(run_loop(env.client, conversation, env.tools, finish_config()).status == LoopStatus::finished);
    CHECK(env.lookups == 1);
    CHECK(env.compile_order.empty());
    const auto requests = env.transport->requests();
    REQUIRE(requests.size() == 2);
    const Json& results = last_message(requests[1]);
    REQUIRE(results["content"].size() == 2);
    CHECK_FALSE(results["content"][0].contains("is_error"));
    CHECK(results["content"][1]["is_error"] == true);
    CHECK(results["content"][1]["content"].get<std::string>().find("max_tokens") != std::string::npos);
}

TEST_CASE("run_loop: non-parallel tools run sequentially in call order") {
    Env env({replay::message({replay::tool_use("toolu_c1", "compile_and_diff", {{"source", "a"}}),
                              replay::tool_use("toolu_l1", "lookup_symbol", {{"query", "q"}}),
                              replay::tool_use("toolu_c2", "compile_and_diff", {{"source", "b"}})},
                             "tool_use", replay::usage(10, 10)),
             submit_turn()});
    Conversation conversation = env.conversation();
    Recorder recorder;
    CHECK(run_loop(env.client, conversation, env.tools, finish_config(), nullptr, &recorder).status == LoopStatus::finished);
    CHECK(env.compile_order == std::vector<std::string>{"toolu_c1", "toolu_c2"});
    const std::vector<std::string> expected = {"turn 1",         "response 1 tool_use", "start toolu_c1", "end toolu_c1",
                                               "start toolu_l1", "end toolu_l1",        "start toolu_c2", "end toolu_c2",
                                               "turn 2",         "response 2 tool_use", "start toolu_submit", "end toolu_submit"};
    CHECK(recorder.log == expected);
}

TEST_CASE("run_loop: end_turn without the finish tool after the nudges are used up") {
    Env env({replay::message({replay::text("Done?")}, "end_turn", replay::usage(10, 10)),
             replay::message({replay::text("Done.")}, "end_turn", replay::usage(10, 10)),
             replay::message({replay::text("Really done.")}, "end_turn", replay::usage(10, 10)), submit_turn()});
    Conversation conversation = env.conversation();
    LoopConfig config = finish_config();
    config.nudge_text = "Please wrap up.";
    const LoopOutcome outcome = run_loop(env.client, conversation, env.tools, config);
    CHECK(outcome.status == LoopStatus::end_turn_without_finish);
    CHECK(outcome.turns == 3);
    const auto requests = env.transport->requests();
    REQUIRE(requests.size() == 3);
    CHECK(last_message(requests[1])["content"][0]["text"] == "Please wrap up. (Call `submit_result` when you are done.)");
    CHECK(conversation.messages().back()["role"] == "assistant");  // no dangling nudge
}

TEST_CASE("run_loop: without a finish tool, end_turn finishes") {
    Env env({replay::message({replay::text("All done.")}, "end_turn", replay::usage(10, 10))});
    Conversation conversation = env.conversation();
    const LoopOutcome outcome = run_loop(env.client, conversation, env.tools, LoopConfig{});
    CHECK(outcome.status == LoopStatus::finished);
    CHECK(outcome.finish_outcome.is_null());
}

TEST_CASE("run_loop: API errors end the loop with the error") {
    Env env({replay::http_error(400, "invalid_request_error", "tool_choice: forced tool use is not supported")});
    Conversation conversation = env.conversation();
    const LoopOutcome outcome = run_loop(env.client, conversation, env.tools, finish_config());
    CHECK(outcome.status == LoopStatus::error);
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::api);
    CHECK(outcome.detail.find("forced tool use") != std::string::npos);

    Env empty({});
    Conversation no_messages(ConversationSettings{}, "sys", empty.tools.definitions());
    const LoopOutcome nothing = run_loop(empty.client, no_messages, empty.tools, finish_config());
    CHECK(nothing.status == LoopStatus::error);
    CHECK(nothing.turns == 0);
}

TEST_CASE("LoopStatus names") {
    CHECK(to_string(LoopStatus::finished) == "finished");
    CHECK(to_string(LoopStatus::end_turn_without_finish) == "end_turn_without_finish");
    CHECK(to_string(LoopStatus::budget_exhausted) == "budget_exhausted");
}
