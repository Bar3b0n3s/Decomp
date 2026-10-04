#include "agent/replay_transport.hpp"
#include "agent/runner.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "events/bus.hpp"
#include "events/run_state.hpp"
#include "llvm_fixture.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <map>

using namespace decomp;
using namespace decomp::agent;

namespace {

const char* kWrongAdd = "extern int g_counter;\n__declspec(noinline) int add(int a, int b) { return a - b + g_counter; }\n";
const char* kRightAdd = "extern int g_counter;\n__declspec(noinline) int add(int a, int b) { return a + b + g_counter; }\n";

Json tool_turn(const std::string& id, const std::string& tool, Json input, Json usage = replay::usage(1200, 300, 0, 4000)) {
    return replay::message({replay::thinking("Working on it.", "sig_" + id), replay::tool_use(id, tool, std::move(input))}, "tool_use", usage,
                           {.id = "msg_" + id});
}

AgentRunConfig replay_config(std::shared_ptr<HttpTransport> transport) {
    AgentRunConfig c = run_config_from(project::AgentSettings{});
    c.transport = std::move(transport);
    c.client.api_key = "sk-test-secret";
    return c;
}

std::vector<Json> read_jsonl(const std::filesystem::path& path) {
    std::vector<Json> out;
    const std::string text = fs::read_text(path).value();
    for (const auto& line : split_lines(text))
        if (!trim(line).empty()) out.push_back(parse_json(line).value());
    return out;
}

// Records events by type and folds them into a RunState.
struct Recorder {
    events::RunState state;
    std::map<std::string, int> counts;
    std::vector<events::Event> all;

    void attach(events::EventBus& bus) {
        bus.subscribe([this](const events::Event& e) {
            state.apply(e);
            ++counts[std::string(events::type_name(e.payload))];
            all.push_back(e);
        });
    }
};

// Calls `on_request(n)` before forwarding the n-th request (1-based) to the replay script.
class HookTransport : public HttpTransport {
public:
    HookTransport(std::shared_ptr<ReplayTransport> inner, std::function<void(int)> on_request)
        : inner_(std::move(inner)), on_request_(std::move(on_request)) {}
    Result<HttpResponse> post(const HttpRequest& request, const HttpDataCallback& on_data) override {
        on_request_(++count_);
        return inner_->post(request, on_data);
    }

private:
    std::shared_ptr<ReplayTransport> inner_;
    std::function<void(int)> on_request_;
    int count_ = 0;
};

// A project over the committed x86 fixture (enough for sessions that never compile).
struct FixtureProject {
    fs::TempDir dir = fs::TempDir::create("decomp-runner").value();
    project::Project project = project::Project::init(dir.path() / "p", test::fixture("x86/basic.exe"), std::nullopt, "clang-cl-x86").value();
    Program program = project.open_program().value();
    u64 add = *program.resolve("add");

    matching::MatchSetup setup() const {
        matching::MatchSetup s;
        s.toolchain.name = "unused";
        s.toolchain.compiler = "unused";
        s.work_dir = dir.path() / "work";
        return s;
    }
};

} // namespace

TEST_CASE("agent runner: wrong source, diff, corrected source, submit -> matched") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    auto dir = fs::TempDir::create("decomp-runner-match").value();
    REQUIRE(test::build_fixture_program(Arch::x86, *tools, dir.path()));
    auto proj = project::Project::init(dir.path() / "p", dir.path() / "basic.exe", std::nullopt, "clang-cl-x86").value();
    auto program = proj.open_program().value();
    const u64 va = *program.resolve("add");
    auto setup = test::clang_setup(Arch::x86, tools->clang_cl, dir.path() / "work");

    auto replay = std::make_shared<ReplayTransport>(std::vector<Json>{
        tool_turn("toolu_1", "compile_and_diff", {{"source", kWrongAdd}}),
        tool_turn("toolu_2", "compile_and_diff", {{"source", kRightAdd}}),
        replay::message({replay::text("Byte-exact; submitting."),
                         replay::tool_use("toolu_3", "submit_result", {{"outcome", "matched"}, {"source", kRightAdd}, {"reason", ""}})},
                        "tool_use", replay::usage(900, 200, 0, 5000), {.id = "msg_3"}),
    });
    events::EventBus bus("run-match");
    Recorder rec;
    rec.attach(bus);
    auto event_log = events::JsonlEventLog::open(dir.path() / "run" / "events.jsonl").value();
    bus.subscribe([&](const events::Event& e) { event_log->write(e); });
    const auto transcript = dir.path() / "run" / "sessions" / "add.jsonl";
    const FunctionRunResult r = run_function(program, &proj, setup, va, replay_config(replay), bus, transcript);

    CHECK(r.outcome == "matched");
    CHECK(r.matched);
    CHECK(r.turns == 3);
    CHECK(r.best_match == 100.0);
    CHECK(r.cost_usd > 0);
    REQUIRE(std::filesystem::exists(r.matched_source));
    CHECK(fs::read_text(r.matched_source).value().find("return a + b + g_counter;") != std::string::npos);

    // Status and history are persisted.
    auto reloaded = project::Project::load(dir.path() / "p").value();
    const auto info = reloaded.function_info(va);
    CHECK(info.status == project::FunctionStatus::matched);
    CHECK(info.best_match == 100.0);
    CHECK(info.attempts == 3);  // two compiles + the verification compile of submit_result
    CHECK(info.cost_usd > 0);

    // Every request extends the previous one; system and tools never change; the key is masked.
    const auto& reqs = replay->requests();
    REQUIRE(reqs.size() == 3);
    for (usize i = 1; i < reqs.size(); ++i) {
        CHECK(is_prefix_extension(reqs[i - 1].body, reqs[i].body));
        CHECK(reqs[i].body["system"] == reqs[0].body["system"]);
        CHECK(reqs[i].body["tools"] == reqs[0].body["tools"]);
    }
    CHECK(reqs[0].header("x-api-key") == "***");
    CHECK(reqs[0].body["messages"].size() == 1);  // the brief
    const std::string second = dump_compact(reqs[1].body["messages"].back());
    CHECK(second.find("not matching") != std::string::npos);  // the diff of the wrong attempt
    CHECK(second.find("turns left: 39") != std::string::npos);

    // Transcript: first request in full, then deltas; responses, tool calls, outcome; no secret.
    const auto lines = read_jsonl(transcript);
    std::map<std::string, int> types;
    for (const auto& l : lines) {
        ++types[l["type"].get<std::string>()];
        CHECK(dump_compact(l).find("sk-test-secret") == std::string::npos);
    }
    CHECK(types["request"] == 1);
    CHECK(types["request_delta"] == 2);
    CHECK(types["response"] == 3);
    CHECK(types["tool"] == 3);
    CHECK(lines.back()["type"] == "outcome");
    CHECK(lines.back()["outcome"] == "matched");

    // Events drive the supervision state.
    const auto& session = rec.state.data().sessions.at("run-match-" + std::format("{:x}", va));
    CHECK(session.finished);
    CHECK(session.outcome == "matched");
    CHECK(session.matched);
    CHECK(session.compiles == 3);
    CHECK(session.turn == 3);
    CHECK(rec.counts["tool_call_started"] == 3);
    CHECK(rec.counts["turn_finished"] == 3);
    CHECK(rec.counts["stream_delta"] > 0);
    CHECK(rec.counts["status_changed"] == 2);  // in_progress, then matched

    // The event log replays into the state the live views saw.
    const auto replayed = events::RunState::replay(events::read_event_log(dir.path() / "run" / "events.jsonl").value());
    const auto& again = replayed.data().sessions.at("run-match-" + std::format("{:x}", va));
    CHECK(again.outcome == session.outcome);
    CHECK(again.turn == session.turn);
    CHECK(again.compiles == session.compiles);
    CHECK(again.tool_calls == session.tool_calls);
    CHECK(again.best_match == session.best_match);
    CHECK(again.scores == session.scores);
    CHECK(again.cost_usd == doctest::Approx(session.cost_usd));
    CHECK(again.usage.cache_read == session.usage.cache_read);
    CHECK(session.usage.cache_read > 0);
    CHECK(replayed.data().activity_total == rec.state.data().activity_total);
}

TEST_CASE("agent runner: a refusal marks the function refused") {
    FixtureProject fx;
    const Json details = {{"type", "refusal"}, {"category", "cyber"}, {"explanation", "Declined."}};
    auto replay = std::make_shared<ReplayTransport>(std::vector<Json>{replay::refusal(details)});
    events::EventBus bus("run-refusal");
    Recorder rec;
    rec.attach(bus);
    const FunctionRunResult r = run_function(fx.program, &fx.project, fx.setup(), fx.add, replay_config(replay), bus, {});
    CHECK(r.outcome == "refused");
    CHECK(r.turns == 1);
    CHECK_FALSE(r.matched);
    CHECK(r.detail.find("cyber") != std::string::npos);
    CHECK(fx.project.function_info(fx.add).status == project::FunctionStatus::refused);
    CHECK(rec.counts["refusal"] == 1);
    const auto& session = rec.state.data().sessions.at(std::format("run-refusal-{:x}", fx.add));
    CHECK(session.outcome == "refused");
    CHECK(session.refusal_category == "cyber");
}

TEST_CASE("agent runner: the spend budget ends the session") {
    FixtureProject fx;
    // 20k input + 500 output tokens at $4/$20 per MTok = $0.09, over a $0.01 budget.
    auto replay = std::make_shared<ReplayTransport>(std::vector<Json>{
        tool_turn("toolu_1", "lookup_symbol", {{"query", "g_counter"}}, replay::usage(20000, 500)),
        tool_turn("toolu_2", "lookup_symbol", {{"query", "never sent"}}),
    });
    AgentRunConfig config = replay_config(replay);
    config.loop.max_cost_usd = 0.01;
    events::EventBus bus("run-budget");
    const FunctionRunResult r = run_function(fx.program, &fx.project, fx.setup(), fx.add, config, bus, fx.dir.path() / "t.jsonl");
    CHECK(r.outcome == "budget_exhausted");
    CHECK(r.turns == 1);
    CHECK(replay->requests().size() == 1);
    CHECK(r.cost_usd == doctest::Approx(0.09));
    const auto info = fx.project.function_info(fx.add);
    CHECK(info.status == project::FunctionStatus::unstarted);  // nothing was compiled
    CHECK(info.cost_usd == doctest::Approx(0.09));
    // The lookup still ran and was recorded.
    bool saw_tool = false;
    for (const auto& l : read_jsonl(fx.dir.path() / "t.jsonl"))
        if (l["type"] == "tool") saw_tool = l["result"].get<std::string>().find("g_counter") != std::string::npos;
    CHECK(saw_tool);
}

TEST_CASE("agent runner: supervisor guidance is appended without rewriting history") {
    FixtureProject fx;
    LoopControl control;
    auto replay = std::make_shared<ReplayTransport>(std::vector<Json>{
        tool_turn("toolu_1", "lookup_symbol", {{"query", "g_counter"}}),
        tool_turn("toolu_2", "submit_result", {{"outcome", "give_up"}, {"source", ""}, {"reason", "cannot reproduce the register allocation"}}),
    });
    // The supervisor speaks while the first request is in flight.
    auto transport = std::make_shared<HookTransport>(replay, [&](int n) {
        if (n == 1) control.inject("g_counter is read after the addition; keep that order.");
    });
    events::EventBus bus("run-guidance");
    Recorder rec;
    rec.attach(bus);
    const FunctionRunResult r = run_function(fx.program, &fx.project, fx.setup(), fx.add, replay_config(transport), bus,
                                             fx.dir.path() / "g.jsonl", &control);
    CHECK(r.outcome == "gave_up");
    CHECK(r.detail == "cannot reproduce the register allocation");
    CHECK(fx.project.function_info(fx.add).status == project::FunctionStatus::gave_up);

    const auto& reqs = replay->requests();
    REQUIRE(reqs.size() == 2);
    CHECK(is_prefix_extension(reqs[0].body, reqs[1].body));
    const Json& last = reqs[1].body["messages"].back();
    CHECK(last["role"] == "user");
    REQUIRE(last["content"].size() == 2);
    CHECK(last["content"][0]["type"] == "tool_result");  // tool results come first
    const std::string tail = last["content"][1]["text"].get<std::string>();
    CHECK(tail.find("[Supervisor guidance] g_counter is read after the addition") != std::string::npos);
    CHECK(tail.find("turns left: 39") != std::string::npos);
    CHECK(rec.counts["guidance"] == 1);

    bool logged = false;
    for (const auto& l : read_jsonl(fx.dir.path() / "g.jsonl")) logged |= l["type"] == "guidance";
    CHECK(logged);
}

TEST_CASE("agent runner: a stop request ends the session between turns") {
    FixtureProject fx;
    LoopControl control;
    auto replay = std::make_shared<ReplayTransport>(std::vector<Json>{
        tool_turn("toolu_1", "lookup_symbol", {{"query", "add"}}),
        tool_turn("toolu_2", "lookup_symbol", {{"query", "never sent"}}),
    });
    auto transport = std::make_shared<HookTransport>(replay, [&](int n) {
        if (n == 1) control.request_stop();
    });
    events::EventBus bus("run-stop");
    const FunctionRunResult r = run_function(fx.program, &fx.project, fx.setup(), fx.add, replay_config(transport), bus, {}, &control);
    CHECK(r.outcome == "stopped");
    CHECK(r.turns == 1);
    CHECK(replay->requests().size() == 1);
    CHECK(fx.project.function_info(fx.add).status == project::FunctionStatus::unstarted);
}
