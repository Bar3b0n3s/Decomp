// Transcripts written by the real runner (agent::run_function over scripted API responses), read back
// whole and incrementally.

#include "agent/replay_transport.hpp"
#include "agent/runner.hpp"
#include "events/bus.hpp"
#include "viewmodel/transcript.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace decomp;
using namespace decomp::vm;
using namespace decomp::agent;
using namespace std::chrono_literals;

namespace {

Json tool_turn(const std::string& id, const std::string& tool, Json input, Json usage = replay::usage(1200, 300, 0, 4000)) {
    return replay::message({replay::thinking("Working on it.", "sig_" + id), replay::tool_use(id, tool, std::move(input))}, "tool_use", usage,
                           {.id = "msg_" + id});
}

Json give_up(const std::string& id) {
    return replay::message({replay::text("Giving up."),
                            replay::tool_use(id, "submit_result", {{"outcome", "give_up"}, {"source", ""}, {"reason", "cannot match"}})},
                           "tool_use", replay::usage(900, 200, 0, 5000), {.id = "msg_" + id});
}

// Calls `on_request(n)` before forwarding the n-th request (1-based) to the replay script.
class HookTransport : public HttpTransport {
public:
    HookTransport(std::shared_ptr<ReplayTransport> inner, std::function<void(int)> on_request)
        : inner_(std::move(inner)), on_request_(std::move(on_request)) {}
    Result<HttpResponse> send(const HttpRequest& request, const HttpDataCallback& on_data) override {
        on_request_(++count_);
        return inner_->send(request, on_data);
    }

private:
    std::shared_ptr<ReplayTransport> inner_;
    std::function<void(int)> on_request_;
    int count_ = 0;
};

struct Session {
    test::FixtureProject fx;
    std::filesystem::path transcript = fx.dir.path() / "run" / "sessions" / "add.jsonl";

    FunctionRunResult run(std::vector<Json> script, std::function<void(AgentRunConfig&)> configure = {}, LoopControl* control = nullptr,
                          std::function<void(int)> hook = {}) {
        auto replay = std::make_shared<ReplayTransport>(std::move(script));
        AgentRunConfig config = run_config_from(project::AgentSettings{});
        config.transport = hook ? std::shared_ptr<HttpTransport>(std::make_shared<HookTransport>(replay, hook)) : replay;
        config.client.api_key = "sk-test";
        config.client.backoff_base = 1ms;
        config.client.backoff_cap = 1ms;
        if (configure) configure(config);
        matching::MatchSetup setup;
        setup.toolchain.name = "missing";
        setup.toolchain.compiler = "decomp-test-no-such-compiler";
        setup.work_dir = fx.dir.path() / "work";
        events::EventBus bus("run-t");
        return run_function(fx.program, &fx.project, setup, fx.va("add"), config, bus, transcript, control, 0);
    }
    std::string text() const { return fs::read_text(transcript).value(); }
};

usize count(const std::string& haystack, std::string_view needle) {
    usize n = 0;
    for (usize at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1)) ++n;
    return n;
}

} // namespace

TEST_CASE("transcript: guidance, tool calls, a failed compile and giving up") {
    Session s;
    LoopControl control;
    const auto r = s.run(
        {
            tool_turn("toolu_1", "lookup_symbol", {{"query", "g_counter"}}),
            tool_turn("toolu_2", "compile_and_diff", {{"source", "int add(int a, int b) { return a + b; }\n"}}),
            give_up("toolu_3"),
        },
        [](AgentRunConfig& c) { c.guidance = {"Start with the listing."}; }, &control,
        [&](int n) {
            if (n == 1) control.inject("Keep the order of the operands.");
        });
    REQUIRE(r.outcome == "gave_up");
    const TranscriptDoc doc = parse_transcript(s.text());
    CHECK(doc.malformed == 0);
    CHECK(doc.unknown == 0);
    REQUIRE(doc.header);
    CHECK(doc.header->session == std::format("run-t-{:x}", s.fx.va("add")));
    CHECK(doc.header->function == "?add@@YAHHH@Z");
    CHECK(doc.header->display == "int __cdecl add(int, int)");
    CHECK(doc.header->va == s.fx.va("add"));
    CHECK(doc.header->model == project::AgentSettings{}.model);
    CHECK(doc.header->worker == 0);
    CHECK(doc.brief.starts_with("# Target function"));
    CHECK(doc.pending.empty());
    REQUIRE(doc.turns.size() == 3);

    const TurnRecord& t1 = doc.turns[0];
    CHECK(t1.turn == 1);
    CHECK(t1.has_request);
    REQUIRE(t1.before.size() == 1);  // the brief went to doc.brief
    CHECK(t1.before[0].kind == UserItem::Kind::guidance);
    CHECK(t1.before[0].text == "Start with the listing.");
    CHECK(t1.before[0].time);
    REQUIRE(t1.has_response);
    REQUIRE(t1.blocks.size() == 2);
    CHECK(t1.blocks[0].kind == ResponseBlock::Kind::thinking);
    CHECK(t1.blocks[0].text == "Working on it.");
    CHECK(t1.blocks[1].kind == ResponseBlock::Kind::tool_use);
    CHECK(t1.blocks[1].tool_name == "lookup_symbol");
    CHECK(t1.blocks[1].input == Json{{"query", "g_counter"}});
    CHECK(t1.stop_reason == "tool_use");
    CHECK(t1.model == replay::MessageOptions{}.model);
    CHECK(t1.request_id == "req_msg_toolu_1");
    CHECK(t1.usage.input == 1200);
    CHECK(t1.usage.cache_read == 4000);
    CHECK(t1.cost_usd > 0);
    CHECK_FALSE(t1.had_fallback);
    REQUIRE(t1.tools.size() == 1);
    CHECK(t1.tools[0].id == "toolu_1");
    CHECK(t1.tools[0].result.find("g_counter") != std::string::npos);
    CHECK_FALSE(t1.tools[0].is_error);

    const TurnRecord& t2 = doc.turns[1];
    REQUIRE(t2.before.size() == 3);
    CHECK(t2.before[0].kind == UserItem::Kind::tool_result);
    CHECK(t2.before[0].tool_use_id == "toolu_1");
    CHECK(t2.before[0].text == t1.tools[0].result);
    CHECK(t2.before[1].kind == UserItem::Kind::guidance);
    CHECK(t2.before[1].text == "Keep the order of the operands.");
    CHECK(t2.before[1].guidance_id != 0);
    CHECK(t2.before[1].time);
    CHECK(t2.before[2].kind == UserItem::Kind::status);
    CHECK(t2.before[2].text.starts_with("[status] turns left: 39"));
    REQUIRE(t2.tools.size() == 1);
    CHECK(t2.tools[0].name == "compile_and_diff");
    CHECK(t2.tools[0].is_error);  // no compiler
    CHECK(t2.tools[0].input["source"] == "int add(int a, int b) { return a + b; }\n");

    const TurnRecord& t3 = doc.turns[2];
    REQUIRE(t3.before.size() == 2);
    CHECK(t3.before[0].is_error);
    CHECK(t3.before[1].kind == UserItem::Kind::status);
    REQUIRE(t3.blocks.size() == 2);
    CHECK(t3.blocks[0].text == "Giving up.");
    REQUIRE(doc.outcome);
    CHECK(doc.outcome->outcome == "gave_up");
    CHECK(doc.outcome->turns == 3);
    CHECK(doc.outcome->cost_usd == doctest::Approx(t1.cost_usd + t2.cost_usd + t3.cost_usd));

    const std::string md = to_markdown(doc);
    CHECK(md.starts_with("# Agent session: int __cdecl add(int, int)\n"));
    CHECK(md.find("```cpp\nint add(int a, int b) { return a + b; }\n```") != std::string::npos);
    CHECK(md.find("**Supervisor guidance**") != std::string::npos);
    CHECK(md.find("> Keep the order of the operands.") != std::string::npos);
    CHECK(md.find("*Thinking (summary):*\n\n> Working on it.") != std::string::npos);
    CHECK(md.find("## Turn 3") != std::string::npos);
    CHECK(md.find("- Outcome: **gave_up**") != std::string::npos);
    CHECK(md.find("````text\n# Target function") != std::string::npos);  // the brief has its own fences
    CHECK(count(md, "**Tool result**") == 0);  // results are shown once, with their calls
    CHECK(count(md, "**Result of `") == 3);
}

TEST_CASE("transcript: a retry, a nudge, a fallback and summaries without text") {
    Session s;
    Json usage = replay::usage(10, 20, 0, 0);
    usage["iterations"] = Json::array({Json{{"type", "message"}, {"model", "model-a"}, {"input_tokens", 100}, {"output_tokens", 5}},
                                       Json{{"type", "fallback_message"}, {"model", "model-b"}, {"input_tokens", 10}, {"output_tokens", 20}}});
    const auto r = s.run({
        replay::http_error(429, "rate_limit_error", "slow down", "0"),
        replay::message({replay::text("Let me think about it first.")}, "end_turn", replay::usage(500, 40), {.id = "msg_a"}),
        replay::message({replay::thinking("", "sig"), replay::text("partial"), replay::fallback("model-a", "model-b"),
                         replay::text("continued"),
                         replay::tool_use("toolu_9", "submit_result", {{"outcome", "give_up"}, {"source", ""}, {"reason", "x"}})},
                        "tool_use", usage, {.id = "msg_b", .model = "model-b"}),
    });
    REQUIRE(r.outcome == "gave_up");
    const TranscriptDoc doc = parse_transcript(s.text());
    REQUIRE(doc.turns.size() == 2);
    const TurnRecord& t1 = doc.turns[0];
    REQUIRE(t1.retries.size() == 1);
    CHECK(t1.retries[0].status == 429);
    CHECK(t1.retries[0].attempt == 1);
    CHECK(t1.retries[0].error.find("rate_limit_error") != std::string::npos);
    CHECK(t1.stop_reason == "end_turn");
    CHECK(t1.tools.empty());

    const TurnRecord& t2 = doc.turns[1];
    REQUIRE(t2.before.size() == 2);
    CHECK(t2.before[0].kind == UserItem::Kind::nudge);
    CHECK(t2.before[0].text.find("submit_result") != std::string::npos);
    CHECK(t2.before[1].kind == UserItem::Kind::status);
    REQUIRE(t2.blocks.size() == 5);
    CHECK(t2.blocks[0].kind == ResponseBlock::Kind::thinking);
    CHECK(t2.blocks[0].text.empty());
    CHECK(t2.blocks[2].kind == ResponseBlock::Kind::fallback);
    CHECK(t2.blocks[2].from_model == "model-a");
    CHECK(t2.blocks[2].to_model == "model-b");
    CHECK(t2.had_fallback);
    CHECK(t2.model == "model-b");
    CHECK(t2.usage.input == 110);  // billed: the sum of the attempts
    CHECK(t2.usage.output == 25);
    CHECK(t2.usage_raw.contains("iterations"));

    const std::string md = to_markdown(doc);
    CHECK(md.find("*Retry 1 after") != std::string::npos);
    CHECK(md.find("(HTTP 429)") != std::string::npos);
    CHECK(md.find("**Reminder:** You ended your turn") != std::string::npos);
    CHECK(md.find("*Thinking (no summary)*") != std::string::npos);
    CHECK(md.find("*Fallback: model-a -> model-b*") != std::string::npos);
    CHECK(md.find("model model-b (fallback)") != std::string::npos);
}

TEST_CASE("transcript: a refusal, and pause markers") {
    {
        Session s;
        const Json details = {{"type", "refusal"}, {"category", "cyber"}, {"explanation", "Declined."}};
        REQUIRE(s.run({replay::refusal(details)}).outcome == "refused");
        const TranscriptDoc doc = parse_transcript(s.text());
        REQUIRE(doc.turns.size() == 1);
        CHECK(doc.turns[0].stop_reason == "refusal");
        CHECK(doc.turns[0].stop_details["category"] == "cyber");
        REQUIRE(doc.outcome);
        CHECK(doc.outcome->outcome == "refused");
        CHECK(doc.outcome->detail.find("cyber") != std::string::npos);
    }
    {
        Session s;
        LoopControl control;
        std::atomic<bool> done{false};
        // Resumes once the paused record is on disk, so the markers come out in order.
        std::thread resumer([&] {
            for (int i = 0; i < 2000 && !done; ++i) {
                if (fs::read_text(s.transcript).value_or("").find("\"type\":\"paused\"") != std::string::npos) {
                    control.resume();
                    return;
                }
                std::this_thread::sleep_for(5ms);
            }
            control.resume();
        });
        const auto r = s.run({tool_turn("toolu_1", "lookup_symbol", {{"query", "add"}}), give_up("toolu_2")}, {}, &control, [&](int n) {
            if (n == 1) control.request_pause();
        });
        done = true;
        resumer.join();
        REQUIRE(r.outcome == "gave_up");
        const TranscriptDoc doc = parse_transcript(s.text());
        REQUIRE(doc.turns.size() == 2);
        REQUIRE(doc.turns[1].before.size() == 4);
        CHECK(doc.turns[1].before[0].kind == UserItem::Kind::paused);
        CHECK(doc.turns[1].before[1].kind == UserItem::Kind::resumed);
        CHECK(*doc.turns[1].before[0].time <= *doc.turns[1].before[1].time);
        CHECK(doc.turns[1].before[2].kind == UserItem::Kind::tool_result);
        const std::string md = to_markdown(doc);
        CHECK(md.find("*Paused at ") != std::string::npos);
        CHECK(md.find("*Resumed at ") != std::string::npos);
    }
}

TEST_CASE("transcript: incremental reading equals reading the whole file") {
    Session s;
    LoopControl control;
    REQUIRE(s.run({tool_turn("toolu_1", "lookup_symbol", {{"query", "g_counter"}}), give_up("toolu_2")}, {}, &control,
                  [&](int n) {
                      if (n == 1) control.inject("More guidance.");
                  })
                .outcome == "gave_up");
    const std::string text = s.text();
    const TranscriptDoc whole = parse_transcript(text);
    const std::string expected = to_markdown(whole);
    for (usize chunk : {usize{1}, usize{7}, usize{64}, usize{4096}}) {
        CAPTURE(chunk);
        TranscriptReader reader;
        u64 version = reader.version();
        for (usize pos = 0; pos < text.size(); pos += chunk) {
            reader.feed(std::string_view(text).substr(pos, chunk));
            CHECK(reader.version() >= version);
            version = reader.version();
        }
        CHECK(reader.doc().records == whole.records);
        CHECK(to_markdown(reader.doc()) == expected);
    }

    // Parse speed, on the transcript repeated to a few MB.
    std::string big;
    while (big.size() < (4u << 20)) big += text;
    const auto start = std::chrono::steady_clock::now();
    const TranscriptDoc parsed = parse_transcript(big);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("parse_transcript: " << big.size() / 1024 << " KB in " << ms << " ms (" << ms / (static_cast<double>(big.size()) / 102400.0)
                                 << " ms per 100 KB)");
    CHECK(parsed.records > whole.records);

    // A growing file: half of it, then the rest; then a rewrite starts over.
    auto dir = fs::TempDir::create("decomp-vm-transcript").value();
    const auto path = dir.path() / "t.jsonl";
    const usize half = text.size() / 2;
    REQUIRE(fs::write_text(path, text.substr(0, half)));
    TranscriptReader reader;
    CHECK(reader.feed_file(path).value());
    CHECK(reader.offset() == half);
    const usize partial_turns = reader.doc().turns.size();
    CHECK_FALSE(reader.feed_file(path).value());  // nothing new
    REQUIRE(fs::append_text(path, text.substr(half)));
    CHECK(reader.feed_file(path).value());
    CHECK(reader.doc().turns.size() >= partial_turns);
    CHECK(to_markdown(reader.doc()) == expected);
    REQUIRE(fs::write_text(path, text.substr(0, 100)));
    CHECK(reader.feed_file(path).value());
    CHECK(reader.offset() == 100);
    CHECK_FALSE(reader.doc().outcome);
    CHECK_FALSE(reader.feed_file(dir.path() / "missing.jsonl"));
}

TEST_CASE("transcript: malformed lines, unknown records and records out of order") {
    const std::string text =
        "{\"type\":\"session\",\"session\":\"s\",\"function\":\"f\",\"va\":4096,\"time\":1000}\n"
        "not json\n"
        "[1,2]\n"
        "\n"
        "{\"type\":\"tool\",\"turn\":2,\"id\":\"t\",\"name\":\"x\",\"result\":[{\"type\":\"text\",\"text\":\"a\"},{\"type\":\"image\"}],\"time\":1}\n"
        "{\"type\":\"telemetry\",\"time\":2}\n"
        "{\"type\":\"response\",\"turn\":2,\"content\":\"oops\",\"usage\":7}\n"
        "{\"type\":\"request\",\"turn\":1}\n"
        "{\"type\":\"guidance\",\"turn\":1,\"text\":\"later\",\"id\":5}\n"
        "{\"type\":\"paused\",\"turn\":1,\"time\":3}";  // no newline yet
    TranscriptReader reader;
    reader.feed(text);
    CHECK(reader.doc().malformed == 2);
    CHECK(reader.doc().unknown == 1);
    REQUIRE(reader.doc().header);
    CHECK(reader.doc().header->va == 4096);
    REQUIRE(reader.doc().turns.size() == 2);  // turn 1 (request) and turn 2 (tool and response first)
    CHECK(reader.doc().turns[0].turn == 1);
    CHECK(reader.doc().turns[1].tools[0].result == "a\n[image]");
    CHECK(reader.doc().turns[1].has_response);
    CHECK(reader.doc().turns[1].blocks.empty());
    REQUIRE(reader.doc().pending.size() == 1);  // the guidance; the pause line is not complete yet
    reader.finish();
    REQUIRE(reader.doc().pending.size() == 2);
    CHECK(reader.doc().pending[1].kind == UserItem::Kind::paused);
    const std::string md = to_markdown(reader.doc());
    CHECK(md.find("## After the last request") != std::string::npos);
    CHECK(md.find("(the session has not ended)") != std::string::npos);
    CHECK(to_markdown(TranscriptDoc{}).starts_with("# Agent session: session"));
}
