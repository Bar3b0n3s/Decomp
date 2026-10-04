#include "agent/replay_transport.hpp"
#include "agent/runner.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "events/bus.hpp"
#include "events/run_state.hpp"
#include "llvm_fixture.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <map>
#include <thread>

using namespace decomp;
using namespace decomp::agent;
using namespace std::chrono_literals;

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
    Result<HttpResponse> send(const HttpRequest& request, const HttpDataCallback& on_data) override {
        on_request_(++count_);
        return inner_->send(request, on_data);
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
    CHECK(lines.front()["type"] == "session");  // the header comes first
    CHECK(lines.front()["va"] == va);
    for (const auto& l : lines) CHECK(l.contains("time"));

    // Events drive the supervision state.
    const auto& session = *rec.state.data().sessions.at("run-match-" + std::format("{:x}", va));
    CHECK(session.finished);
    CHECK(session.outcome == "matched");
    CHECK(session.matched);
    CHECK(session.compiles == 3);
    CHECK(session.turn == 3);
    CHECK(rec.counts["tool_call_started"] == 3);
    CHECK(rec.counts["turn_finished"] == 3);
    CHECK(rec.counts["stream_delta"] > 0);
    CHECK(rec.counts["status_changed"] == 2);  // in_progress, then matched
    CHECK(rec.counts["compile_started"] == 3);
    CHECK(session.transcript == "sessions/add.jsonl");
    CHECK(session.model == "claude-opus-5-5");
    REQUIRE(rec.state.data().recent_compiles.size() == 3);
    for (const auto& c : rec.state.data().recent_compiles) {
        CHECK(c->ok);  // the wrong attempt compiles too; it just does not match
        CHECK(c->exit_code == 0);
        CHECK(c->toolchain == "clang-cl-x86");
    }
    CHECK_FALSE(rec.state.data().recent_compiles.back()->command.empty());

    // The event log replays into the state the live views saw.
    const auto replayed = events::RunState::replay(events::read_event_log(dir.path() / "run" / "events.jsonl").value());
    const auto& again = *replayed.data().sessions.at("run-match-" + std::format("{:x}", va));
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
    CHECK(again.transcript == session.transcript);
    CHECK(again.model == session.model);
    CHECK(replayed.data().recent_compiles.size() == rec.state.data().recent_compiles.size());
    CHECK(replayed.data().minutes.size() == rec.state.data().minutes.size());
    CHECK(replayed.data().workers.at(0).spans.size() == rec.state.data().workers.at(0).spans.size());
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
    const auto& session = *rec.state.data().sessions.at(std::format("run-refusal-{:x}", fx.add));
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
    config.loop.limits.max_cost_usd = 0.01;
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
    bool saved_in_progress = true;
    auto transport = std::make_shared<HookTransport>(replay, [&](int n) {
        if (n != 1) return;
        control.request_stop();
        // While the session runs, symbols.txt keeps the last real status (a crash leaves nothing stale).
        saved_in_progress = fs::read_text(fx.project.root() / "symbols.txt").value().find("in_progress") != std::string::npos;
    });
    events::EventBus bus("run-stop");
    const FunctionRunResult r = run_function(fx.program, &fx.project, fx.setup(), fx.add, replay_config(transport), bus, {}, &control);
    CHECK(r.outcome == "stopped");
    CHECK_FALSE(saved_in_progress);
    CHECK(r.turns == 1);
    CHECK(replay->requests().size() == 1);
    CHECK(fx.project.function_info(fx.add).status == project::FunctionStatus::unstarted);
}

TEST_CASE("agent runner: initial guidance rides in the first message") {
    FixtureProject fx;
    auto replay = std::make_shared<ReplayTransport>(std::vector<Json>{
        tool_turn("toolu_1", "submit_result", {{"outcome", "give_up"}, {"source", ""}, {"reason", "test"}}),
    });
    AgentRunConfig config = replay_config(replay);
    config.guidance = {"Start with the listing.", "  "};
    events::EventBus bus("run-initial");
    Recorder rec;
    rec.attach(bus);
    const FunctionRunResult r = run_function(fx.program, &fx.project, fx.setup(), fx.add, config, bus, {});
    CHECK(r.outcome == "gave_up");
    const auto& reqs = replay->requests();
    REQUIRE(reqs.size() == 1);
    const Json& messages = reqs[0].body["messages"];
    REQUIRE(messages.size() == 1);  // one user message: the brief plus the guidance
    REQUIRE(messages[0]["content"].size() == 2);
    CHECK(messages[0]["content"][1]["text"] == "[Supervisor guidance] Start with the listing.");
    CHECK(rec.counts["guidance"] == 1);  // blank guidance is dropped
}

namespace {

// A project over the fixture program built with the installed LLVM, so candidates can match byte for byte.
struct CompiledProject {
    std::optional<test::LlvmTools> tools = test::find_llvm();
    fs::TempDir dir = fs::TempDir::create("decomp-runner-compiled").value();
    std::optional<project::Project> project;
    std::optional<Program> program;
    u64 add = 0;
    matching::MatchSetup setup;

    CompiledProject() {
        if (!tools || !test::build_fixture_program(Arch::x86, *tools, dir.path())) return;
        project = project::Project::init(dir.path() / "p", dir.path() / "basic.exe", std::nullopt, "clang-cl-x86").value();
        program = project->open_program().value();
        add = *program->resolve("add");
        setup = test::clang_setup(Arch::x86, tools->clang_cl, dir.path() / "work");
    }
    bool ready() const { return program.has_value(); }
};

Json submit_matched(const std::string& id) {
    return replay::message({replay::tool_use(id, "submit_result", {{"outcome", "matched"}, {"source", kRightAdd}, {"reason", ""}})}, "tool_use",
                           replay::usage(900, 200, 0, 5000), {.id = "msg_" + id});
}

Json end_turn(const std::string& id) {
    return replay::message({replay::text("I think that is it.")}, "end_turn", replay::usage(900, 50, 0, 5000), {.id = "msg_" + id});
}

} // namespace

TEST_CASE("agent runner: approvals decide whether a verified match is saved") {
    CompiledProject cp;
    if (!cp.ready()) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    SUBCASE("deny: the agent hears why and the project keeps no file") {
        auto replay = std::make_shared<ReplayTransport>(std::vector<Json>{
            submit_matched("toolu_1"),
            tool_turn("toolu_2", "submit_result", {{"outcome", "give_up"}, {"source", kRightAdd}, {"reason", "declined"}}),
        });
        events::EventBus bus("run-deny");
        Recorder rec;
        rec.attach(bus);
        AgentRunConfig config = replay_config(replay);
        config.approvals = std::make_shared<ApprovalGate>(&bus);
        config.approvals->set_policy(std::string(kWriteSourceAction), ApprovalPolicy::deny);
        const FunctionRunResult r = run_function(*cp.program, &*cp.project, cp.setup, cp.add, config, bus, {});
        CHECK(r.outcome == "gave_up");
        CHECK_FALSE(r.matched);
        CHECK_FALSE(r.auto_submitted);  // a denied match is not saved behind the supervisor's back either
        CHECK_FALSE(std::filesystem::exists(cp.project->matched_source_path(*cp.program->symbols().at(cp.add))));
        CHECK(cp.project->function_info(cp.add).status != project::FunctionStatus::matched);
        CHECK(rec.counts["file_written"] == 0);
        CHECK(rec.counts["approval_decided"] == 1);  // the declined source is not proposed again at the end
        const std::string answer = dump_compact(replay->requests()[1].body["messages"].back());
        CHECK(answer.find("the supervisor declined saving it") != std::string::npos);
    }
    SUBCASE("ask: the session waits for the supervisor's approval") {
        auto replay = std::make_shared<ReplayTransport>(std::vector<Json>{submit_matched("toolu_1")});
        events::EventBus bus("run-ask");
        Recorder rec;
        rec.attach(bus);
        AgentRunConfig config = replay_config(replay);
        config.approvals = std::make_shared<ApprovalGate>(&bus);
        config.approvals->set_policy(std::string(kWriteSourceAction), ApprovalPolicy::ask);
        std::thread supervisor([&] {
            for (int i = 0; i < 3000 && config.approvals->pending().empty(); ++i) std::this_thread::sleep_for(5ms);
            const auto pending = config.approvals->pending();
            REQUIRE(pending.size() == 1);
            CHECK(pending[0].request.content == kRightAdd);
            CHECK(pending[0].request.path.starts_with("src"));
            CHECK(config.approvals->decide(pending[0].id, true));
        });
        const FunctionRunResult r = run_function(*cp.program, &*cp.project, cp.setup, cp.add, config, bus, {});
        supervisor.join();
        CHECK(r.outcome == "matched");
        CHECK(std::filesystem::exists(r.matched_source));
        REQUIRE(rec.state.data().files_written.size() == 1);
        CHECK(rec.state.data().files_written.back().file.approval == "approved by user");
        CHECK(rec.state.data().approvals_pending == 0);
        CHECK(rec.counts["approval_requested"] == 1);
    }
}

TEST_CASE("agent runner: a byte-exact attempt the model never submits is saved at the end") {
    CompiledProject cp;
    if (!cp.ready()) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    // The model compiles the right source, then talks instead of submitting until the nudges run out.
    auto replay = std::make_shared<ReplayTransport>(std::vector<Json>{
        tool_turn("toolu_1", "compile_and_diff", {{"source", kRightAdd}}),
        end_turn("e1"),
        end_turn("e2"),
        end_turn("e3"),
    });
    AgentRunConfig config = replay_config(replay);
    int first_messages = 0;
    config.on_first_message = [&] { ++first_messages; };
    events::EventBus bus("run-auto");
    Recorder rec;
    rec.attach(bus);
    const auto transcript = cp.dir.path() / "auto.jsonl";
    const FunctionRunResult r = run_function(*cp.program, &*cp.project, cp.setup, cp.add, config, bus, transcript);
    CHECK(r.outcome == "matched");
    CHECK(r.matched);
    CHECK(r.auto_submitted);
    CHECK(r.detail.find("did not submit") != std::string::npos);
    CHECK(r.detail.find("no_result") != std::string::npos);
    CHECK(std::filesystem::exists(r.matched_source));
    CHECK(cp.project->function_info(cp.add).status == project::FunctionStatus::matched);
    CHECK(first_messages == 1);
    bool recorded = false;
    for (const auto& l : read_jsonl(transcript))
        if (l["type"] == "auto_submit") recorded = l["accepted"] == true;
    CHECK(recorded);
    CHECK(rec.state.data().matched == 1);
}

TEST_CASE("agent runner: a rate-limit wait shows as the worker's phase; skip ends a session as skipped") {
    FixtureProject fx;
    SUBCASE("rate wait") {
        auto replay = std::make_shared<ReplayTransport>(std::vector<Json>{
            tool_turn("toolu_1", "submit_result", {{"outcome", "give_up"}, {"source", ""}, {"reason", "test"}}),
        });
        AgentRunConfig config = replay_config(replay);
        config.client.gate = std::make_shared<RateGate>();
        config.client.gate->on_throttled(429, 150ms);
        events::EventBus bus("run-rate");
        Recorder rec;
        rec.attach(bus);
        const FunctionRunResult r = run_function(fx.program, &fx.project, fx.setup(), fx.add, config, bus, {}, nullptr, 3);
        CHECK(r.outcome == "gave_up");
        std::vector<std::string> phases;
        for (const auto& e : rec.all)
            if (const auto* p = std::get_if<events::WorkerPhaseChanged>(&e.payload)) phases.push_back(p->phase);
        REQUIRE(phases.size() >= 2);
        CHECK(phases[0] == "waiting for rate limit");
        CHECK(phases[1] == "waiting for model");
        bool spanned = false;
        for (const auto& span : rec.state.data().workers.at(3).spans) spanned |= span.phase == "waiting for rate limit";
        CHECK(spanned);
    }
    SUBCASE("skip") {
        LoopControl control;
        auto replay = std::make_shared<ReplayTransport>(std::vector<Json>{
            tool_turn("toolu_1", "lookup_symbol", {{"query", "add"}}),
            tool_turn("toolu_2", "lookup_symbol", {{"query", "never sent"}}),
        });
        auto transport = std::make_shared<HookTransport>(replay, [&](int n) {
            if (n == 1) control.request_stop(StopReason::skip);
        });
        events::EventBus bus("run-skip");
        Recorder rec;
        rec.attach(bus);
        const FunctionRunResult r = run_function(fx.program, &fx.project, fx.setup(), fx.add, replay_config(transport), bus, {}, &control);
        CHECK(r.outcome == "skipped");
        CHECK(r.stop_reason == StopReason::skip);
        CHECK(r.detail == "skipped by the supervisor");
        CHECK(rec.state.data().session(std::format("run-skip-{:x}", fx.add))->outcome == "skipped");
    }
}
