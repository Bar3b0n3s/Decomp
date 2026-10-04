// Supervision features of the agent layer: live limits, the shared run budget, stop reasons, guidance
// ids, the rate gate, approvals, the model list and replay script lookup.

#include "agent/approvals.hpp"
#include "agent/client.hpp"
#include "agent/loop.hpp"
#include "agent/rate_gate.hpp"
#include "agent/replay_transport.hpp"
#include "core/fs.hpp"
#include "project/project.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <format>
#include <memory>
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

Json object_schema(std::string_view property) {
    return Json{{"type", "object"},
                {"properties", Json{{std::string(property), Json{{"type", "string"}}}}},
                {"required", Json::array({std::string(property)})},
                {"additionalProperties", false}};
}

struct Env {
    std::shared_ptr<ReplayTransport> transport;
    Client client;
    ToolRegistry tools;

    explicit Env(std::vector<Json> script, ClientConfig config = test_config())
        : transport(std::make_shared<ReplayTransport>(std::move(script))), client(std::move(config), transport, [](std::chrono::milliseconds) {}) {
        Tool lookup;
        lookup.name = "lookup_symbol";
        lookup.description = "Looks up a symbol.";
        lookup.input_schema = object_schema("query");
        lookup.handler = [](const ToolCall&) { return ToolResult::text("found"); };
        tools.add(lookup);
        Tool submit;
        submit.name = "submit_result";
        submit.description = "Finishes the session.";
        submit.input_schema = object_schema("outcome");
        submit.handler = [](const ToolCall& call) {
            ToolResult r = ToolResult::text("recorded");
            r.end_session = true;
            r.outcome = call.input;
            return r;
        };
        tools.add(submit);
    }

    Conversation conversation() {
        Conversation c(ConversationSettings{}, "You are a test agent.", tools.definitions());
        c.append_user_text("Match it.");
        return c;
    }
};

Json lookup_turn(std::string id, Json usage = replay::usage(10, 10)) {
    return replay::message({replay::tool_use(std::move(id), "lookup_symbol", {{"query", "x"}})}, "tool_use", std::move(usage));
}

Json submit_turn() {
    return replay::message({replay::tool_use("toolu_submit", "submit_result", {{"outcome", "matched"}})}, "tool_use", replay::usage(10, 10));
}

LoopConfig finish_config() {
    LoopConfig config;
    config.finish_tool = "submit_result";
    return config;
}

struct Hooks : LoopObserver {
    std::function<void(int)> after_response;
    std::vector<Injected> injected;
    std::vector<int> waits;  // ms
    void on_response(int turn, const Response&) override {
        if (after_response) after_response(turn);
    }
    void on_injected(const Injected& g) override { injected.push_back(g); }
    void on_rate_wait(std::chrono::milliseconds expected) override { waits.push_back(static_cast<int>(expected.count())); }
};

std::string rfc3339(std::chrono::system_clock::time_point t) {
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(t));
}

} // namespace

TEST_CASE("run_loop: limits changed mid-session apply before the next request") {
    Env env({lookup_turn("toolu_1"), lookup_turn("toolu_2"), lookup_turn("toolu_3"), submit_turn()});
    Conversation conversation = env.conversation();
    LoopControl control;
    Hooks hooks;
    hooks.after_response = [&](int turn) {
        if (turn == 1) control.set_limits(LoopLimits{.max_turns = 2});
    };
    std::vector<LoopProgress> seen;
    LoopConfig config = finish_config();
    config.status_line = [&](const LoopProgress& p) {
        seen.push_back(p);
        return std::format("turns left: {}", p.turns_left());
    };
    const LoopOutcome outcome = run_loop(env.client, conversation, env.tools, config, &control, &hooks);
    CHECK(outcome.status == LoopStatus::max_turns);
    CHECK(outcome.turns == 2);
    CHECK(outcome.detail == "reached the turn limit (2)");
    REQUIRE(seen.size() == 1);  // with turn 2's request
    CHECK(seen[0].turns == 1);
    CHECK(seen[0].limits.max_turns == 2);
    CHECK(seen[0].turns_left() == 1);
    const auto requests = env.transport->requests();
    REQUIRE(requests.size() == 2);
    CHECK(dump_compact(requests[1].body["messages"].back()).find("turns left: 1") != std::string::npos);
    REQUIRE(control.limits());
    CHECK(control.limits()->max_turns == 2);

    SUBCASE("a raised limit lets the session go on") {
        Env more({lookup_turn("toolu_1"), lookup_turn("toolu_2"), submit_turn()});
        Conversation c = more.conversation();
        LoopControl raise;
        Hooks h;
        h.after_response = [&](int turn) {
            if (turn == 1) raise.set_limits(LoopLimits{.max_turns = 10});
        };
        LoopConfig tight = finish_config();
        tight.limits.max_turns = 1;
        CHECK(run_loop(more.client, c, more.tools, tight, &raise, &h).status == LoopStatus::finished);
    }
}

TEST_CASE("run_loop: the run budget is shared by sessions and can be raised") {
    auto ledger = std::make_shared<SpendLedger>(1.0);
    LoopConfig config = finish_config();
    config.ledger = ledger;

    // 300k input tokens on claude-opus-5-5 = $1.20: the first session crosses the run budget.
    Env first({lookup_turn("toolu_1", replay::usage(300000, 10)), submit_turn()});
    Conversation c1 = first.conversation();
    const LoopOutcome o1 = run_loop(first.client, c1, first.tools, config);
    CHECK(o1.status == LoopStatus::run_budget_exhausted);
    CHECK(o1.detail.find("run budget exhausted") != std::string::npos);
    CHECK(o1.turns == 1);
    CHECK(ledger->spent() == doctest::Approx(1.2002));
    CHECK(ledger->exhausted());

    // The next session does not send anything.
    Env second({submit_turn()});
    Conversation c2 = second.conversation();
    const LoopOutcome o2 = run_loop(second.client, c2, second.tools, config);
    CHECK(o2.status == LoopStatus::run_budget_exhausted);
    CHECK(o2.turns == 0);
    CHECK(second.transport->requests().empty());

    ledger->set_limit(5.0);
    CHECK_FALSE(ledger->exhausted());
    Env third({submit_turn()});
    Conversation c3 = third.conversation();
    CHECK(run_loop(third.client, c3, third.tools, config).status == LoopStatus::finished);
    ledger->set_limit(0);  // unlimited
    CHECK_FALSE(ledger->exhausted());
}

TEST_CASE("run_loop: stop reasons name why the supervisor ended a session") {
    SUBCASE("skip") {
        Env env({submit_turn()});
        Conversation c = env.conversation();
        LoopControl control;
        control.request_stop(StopReason::skip);
        const LoopOutcome o = run_loop(env.client, c, env.tools, finish_config(), &control);
        CHECK(o.status == LoopStatus::stopped);
        CHECK(o.stop_reason == StopReason::skip);
        CHECK(o.detail == "skipped by the supervisor");
        CHECK(o.turns == 0);
    }
    SUBCASE("an abort overrides a stop's reason") {
        Env env({submit_turn()});
        Conversation c = env.conversation();
        LoopControl control;
        control.request_stop(StopReason::user);
        control.request_abort(StopReason::shutdown);
        const LoopOutcome o = run_loop(env.client, c, env.tools, finish_config(), &control);
        CHECK(o.status == LoopStatus::aborted);
        CHECK(o.stop_reason == StopReason::shutdown);
        CHECK(o.detail == "the run is shutting down");
    }
    SUBCASE("the first stop names the reason") {
        LoopControl control;
        control.request_stop(StopReason::run_budget);
        control.request_stop(StopReason::user);
        CHECK(control.stop_reason() == StopReason::run_budget);
        CHECK(to_string(StopReason::run_budget) == "run_budget");
    }
}

TEST_CASE("LoopControl: guidance has ids and can be retracted until it is sent") {
    LoopControl control;
    const u64 a = control.inject("Use a for loop.");
    const u64 b = control.inject("Check the calling convention.");
    CHECK(a != 0);
    CHECK(b > a);
    CHECK(control.retract(a));
    CHECK_FALSE(control.retract(a));
    REQUIRE(control.pending_injected().size() == 1);
    CHECK(control.pending_injected()[0].id == b);

    // In a loop: guidance retracted before the next request never reaches the model.
    Env env({lookup_turn("toolu_1"), submit_turn()});
    Conversation c = env.conversation();
    LoopControl live;
    Hooks hooks;
    u64 kept = 0, dropped = 0;
    hooks.after_response = [&](int turn) {
        if (turn != 1) return;
        kept = live.inject("Keep the counter in a register.");
        dropped = live.inject("Ignore this.");
        CHECK(live.retract(dropped));
    };
    CHECK(run_loop(env.client, c, env.tools, finish_config(), &live, &hooks).status == LoopStatus::finished);
    REQUIRE(hooks.injected.size() == 1);
    CHECK(hooks.injected[0].id == kept);
    CHECK(hooks.injected[0].text == "Keep the counter in a register.");
    CHECK_FALSE(live.retract(kept));  // already sent
    const std::string sent = dump_compact(env.transport->requests()[1].body["messages"].back());
    CHECK(sent.find("Keep the counter in a register.") != std::string::npos);
    CHECK(sent.find("Ignore this.") == std::string::npos);
}

TEST_CASE("parse_rfc3339") {
    using namespace std::chrono;
    const auto base = sys_days{2026y / October / 4} + 12h;
    CHECK(parse_rfc3339("2026-10-04T12:00:00Z") == base);
    CHECK(parse_rfc3339(" 2026-10-04t12:00:00z ") == base);
    CHECK(parse_rfc3339("2026-10-04T12:00:00.25Z") == base + 250ms);
    CHECK(parse_rfc3339("2026-10-04T14:00:00+02:00") == base);
    CHECK(parse_rfc3339("2026-10-04T11:30:00-00:30") == base);
    CHECK_FALSE(parse_rfc3339("2026-10-04T12:00:00"));    // no zone
    CHECK_FALSE(parse_rfc3339("2026-13-04T12:00:00Z"));   // month
    CHECK_FALSE(parse_rfc3339("2026-10-04T12:00:00.Z"));  // empty fraction
    CHECK_FALSE(parse_rfc3339("yesterday"));
}

TEST_CASE("RateGate: headers, throttling and waits") {
    SUBCASE("observes the reported limits and tells the listener") {
        RateGate gate;
        std::vector<RateLimitSnapshot> heard;
        gate.set_listener([&](const RateLimitSnapshot& s) { heard.push_back(s); });
        const std::string reset = rfc3339(std::chrono::system_clock::now() + 30s);
        gate.observe({{"anthropic-ratelimit-requests-limit", "50"},
                      {"anthropic-ratelimit-requests-remaining", "49"},
                      {"anthropic-ratelimit-requests-reset", reset},
                      {"anthropic-ratelimit-input-tokens-limit", "200000"},
                      {"anthropic-ratelimit-input-tokens-remaining", "150000"},
                      {"anthropic-ratelimit-output-tokens-limit", "40000"},
                      {"anthropic-ratelimit-output-tokens-remaining", "39000"}});
        gate.observe({{"request-id", "req_1"}});  // nothing to report: no notification
        REQUIRE(heard.size() == 1);
        CHECK(heard[0].requests_limit == 50);
        CHECK(heard[0].requests_remaining == 49);
        CHECK(heard[0].input_remaining == 150000);
        CHECK(heard[0].output_limit == 40000);
        CHECK(heard[0].reset == reset);
        CHECK(heard[0].backoff_ms == 0);
        CHECK(gate.wait_time() == 0ms);
    }
    SUBCASE("a 429 holds every request back") {
        RateGate gate(RateGate::Options{.throttle_default = 300ms});
        std::vector<RateLimitSnapshot> heard;
        gate.set_listener([&](const RateLimitSnapshot& s) { heard.push_back(s); });
        gate.on_throttled(429, std::nullopt);
        REQUIRE(heard.size() == 1);
        CHECK(heard[0].throttled_status == 429);
        CHECK(heard[0].backoff_ms > 200);
        std::vector<long long> waits;
        const auto start = std::chrono::steady_clock::now();
        CHECK(gate.acquire({}, [&](std::chrono::milliseconds w) { waits.push_back(w.count()); }));
        CHECK(std::chrono::steady_clock::now() - start >= 250ms);
        REQUIRE(waits.size() == 2);
        CHECK(waits[0] > 0);
        CHECK(waits[1] == 0);
        // A 529 with retry-after uses the server's value.
        gate.on_throttled(529, 150ms);
        CHECK(gate.wait_time() > 100ms);
        CHECK(gate.wait_time() <= 150ms);
    }
    SUBCASE("used-up requests wait for the reset; local accounting between header updates") {
        RateGate gate;
        gate.observe({{"anthropic-ratelimit-requests-limit", "50"},
                      {"anthropic-ratelimit-requests-remaining", "2"},
                      {"anthropic-ratelimit-requests-reset", rfc3339(std::chrono::system_clock::now() + 400ms)}});
        const auto start = std::chrono::steady_clock::now();
        CHECK(gate.acquire());
        CHECK(gate.acquire());
        CHECK(std::chrono::steady_clock::now() - start < 200ms);
        CHECK(gate.wait_time() > 0ms);  // none left until the reset
        CHECK(gate.acquire());
        CHECK(std::chrono::steady_clock::now() - start >= 250ms);
    }
    SUBCASE("nearly used-up tokens wait for the token reset") {
        RateGate gate;
        gate.observe({{"anthropic-ratelimit-input-tokens-limit", "100000"},
                      {"anthropic-ratelimit-input-tokens-remaining", "1000"},  // 1% < 2% reserve
                      {"anthropic-ratelimit-input-tokens-reset", rfc3339(std::chrono::system_clock::now() + 300ms)}});
        CHECK(gate.wait_time() > 100ms);
        gate.observe({{"anthropic-ratelimit-input-tokens-remaining", "90000"}});
        CHECK(gate.wait_time() == 0ms);
    }
    SUBCASE("a reset far away waits at most the cap (clock skew)") {
        RateGate gate(RateGate::Options{.max_reset_wait = 200ms});
        gate.observe({{"anthropic-ratelimit-requests-limit", "50"},
                      {"anthropic-ratelimit-requests-remaining", "0"},
                      {"anthropic-ratelimit-requests-reset", rfc3339(std::chrono::system_clock::now() + 1h)}});
        CHECK(gate.wait_time() <= 200ms);
    }
    SUBCASE("cancellation ends a wait") {
        RateGate gate;
        gate.on_throttled(429, 10s);
        std::atomic<bool> cancel{false};
        std::thread canceller([&] {
            std::this_thread::sleep_for(150ms);
            cancel = true;
        });
        const auto start = std::chrono::steady_clock::now();
        CHECK_FALSE(gate.acquire([&] { return cancel.load(); }));
        CHECK(std::chrono::steady_clock::now() - start < 2s);
        canceller.join();
    }
}

TEST_CASE("Client feeds the rate gate from every answer and reports retries in detail") {
    auto gate = std::make_shared<RateGate>();
    std::vector<RateLimitSnapshot> heard;
    gate->set_listener([&](const RateLimitSnapshot& s) { heard.push_back(s); });
    Json throttled = replay::http_error(429, "rate_limit_error", "slow down", "0.2");
    throttled["headers"]["anthropic-ratelimit-requests-limit"] = "50";
    throttled["headers"]["anthropic-ratelimit-requests-remaining"] = "0";
    Json ok = replay::message({replay::text("hi")}, "end_turn", replay::usage(10, 10));
    ok["headers"]["anthropic-ratelimit-requests-limit"] = "50";
    ok["headers"]["anthropic-ratelimit-requests-remaining"] = "49";

    ClientConfig config = test_config();
    config.gate = gate;
    auto transport = std::make_shared<ReplayTransport>(std::vector<Json>{throttled, ok});
    Client client(config, transport, [](std::chrono::milliseconds) {});  // the client's own backoff is not waited
    struct Observer : StreamObserver {
        std::vector<RetryInfo> retries;
        std::vector<long long> waits;
        void on_retry(const RetryInfo& r) override { retries.push_back(r); }
        void on_rate_wait(std::chrono::milliseconds w) override { waits.push_back(w.count()); }
    } observer;
    const Json request = {{"model", "claude-opus-5-5"}, {"max_tokens", 100}, {"stream", true},
                          {"messages", Json::array({Json{{"role", "user"}, {"content", "hi"}}})}};
    const auto start = std::chrono::steady_clock::now();
    auto response = client.create_message(request, &observer);
    REQUIRE(response);
    CHECK(std::chrono::steady_clock::now() - start >= 150ms);  // the gate held the retry back
    REQUIRE(observer.retries.size() == 1);
    CHECK(observer.retries[0].status == 429);
    REQUIRE(observer.retries[0].retry_after);
    CHECK(*observer.retries[0].retry_after == 200ms);
    REQUIRE(observer.waits.size() == 2);
    CHECK(observer.waits[0] > 0);
    CHECK(observer.waits[1] == 0);
    REQUIRE(heard.size() >= 3);  // the 429's headers, the throttle, the success's headers
    CHECK(heard[0].requests_remaining == 0);
    bool throttle_heard = false;
    for (const auto& s : heard) throttle_heard = throttle_heard || s.throttled_status == 429;
    CHECK(throttle_heard);
    CHECK(heard.back().requests_remaining == 49);
}

TEST_CASE("ApprovalGate: policies, waiting for a decision, cancellation") {
    events::EventBus bus("r");
    std::vector<events::Event> seen;
    bus.subscribe([&](const events::Event& e) { seen.push_back(e); });
    ApprovalGate gate(&bus);
    ApprovalRequest request{std::string(kWriteSourceAction), "s1", "add", 0x401060, "src/functions/add_401060.cpp", "byte-exact add",
                            "int add(int a, int b) { return a + b; }", ""};

    SUBCASE("auto by default, deny by policy") {
        CHECK(gate.policy(kWriteSourceAction) == ApprovalPolicy::automatic);
        auto d = gate.request(request);
        CHECK(d.approved());
        CHECK(d.by == "policy");
        gate.set_policy(std::string(kWriteSourceAction), ApprovalPolicy::deny);
        d = gate.request(request);
        CHECK(d.verdict == "denied");
        CHECK_FALSE(d.reason.empty());
        REQUIRE(seen.size() == 2);
        CHECK(std::holds_alternative<events::ApprovalDecided>(seen[0].payload));
        CHECK(std::get<events::ApprovalDecided>(seen[1].payload).verdict == "denied");
        CHECK(parse_approval_policy("ask") == ApprovalPolicy::ask);
        CHECK_FALSE(parse_approval_policy("maybe"));
        CHECK(to_string(ApprovalPolicy::automatic) == "auto");
    }
    SUBCASE("ask waits for the supervisor") {
        gate.set_policy(std::string(kWriteSourceAction), ApprovalPolicy::ask);
        std::optional<ApprovalDecision> decision;
        std::thread worker([&] { decision = gate.request(request, {}, 2); });
        while (gate.pending().empty()) std::this_thread::sleep_for(5ms);
        const auto pending = gate.pending();
        REQUIRE(pending.size() == 1);
        CHECK(pending[0].request.content == request.content);
        CHECK_FALSE(gate.decide(pending[0].id + 100, true));  // unknown id
        CHECK(gate.decide(pending[0].id, true, "looks right"));
        worker.join();
        REQUIRE(decision);
        CHECK(decision->approved());
        CHECK(decision->by == "user");
        CHECK(decision->reason == "looks right");
        CHECK_FALSE(gate.decide(pending[0].id, false));  // already decided
        CHECK(gate.pending().empty());
        REQUIRE(seen.size() == 3);
        const auto& asked = std::get<events::ApprovalRequested>(seen[0].payload);
        CHECK(asked.id == pending[0].id);
        CHECK(asked.path == request.path);
        CHECK(seen[0].worker == 2);
        CHECK(std::get<events::WorkerPhaseChanged>(seen[1].payload).phase == "waiting for approval");
        CHECK(std::get<events::ApprovalDecided>(seen[2].payload).verdict == "approved");
    }
    SUBCASE("an abort or shutdown cancels the wait") {
        gate.set_policy(std::string(kWriteSourceAction), ApprovalPolicy::ask);
        std::atomic<bool> aborted{false};
        std::optional<ApprovalDecision> decision;
        std::thread worker([&] { decision = gate.request(request, [&] { return aborted.load(); }); });
        while (gate.pending().empty()) std::this_thread::sleep_for(5ms);
        aborted = true;
        worker.join();
        REQUIRE(decision);
        CHECK(decision->verdict == "cancelled");
        CHECK(decision->by == "abort");

        std::thread second([&] { decision = gate.request(request); });
        while (gate.pending().empty()) std::this_thread::sleep_for(5ms);
        gate.cancel_all("the run is shutting down");
        second.join();
        CHECK(decision->verdict == "cancelled");
        CHECK(decision->by == "shutdown");
    }
}

TEST_CASE("Client::list_models pages through GET /v1/models") {
    auto model = [](std::string id) { return Json{{"type", "model"}, {"id", id}, {"display_name", "Model " + id}, {"created_at", "2026-01-01T00:00:00Z"}}; };
    auto transport = std::make_shared<ReplayTransport>(std::vector<Json>{
        Json{{"status", 200}, {"body", Json{{"data", Json::array({model("a"), model("b")})}, {"has_more", true}, {"last_id", "b"}}}},
        Json{{"status", 200}, {"body", Json{{"data", Json::array({model("c")})}, {"has_more", false}, {"last_id", "c"}}}},
        replay::http_error(401, "authentication_error", "invalid x-api-key"),
    });
    Client client(test_config(), transport, [](std::chrono::milliseconds) {});
    auto models = client.list_models();
    REQUIRE(models);
    REQUIRE(models->size() == 3);
    CHECK((*models)[0].id == "a");
    CHECK((*models)[2].display_name == "Model c");
    const auto requests = transport->requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].method == "GET");
    CHECK(requests[0].url == "https://api.example.test/v1/models?limit=1000");
    CHECK(requests[1].url == "https://api.example.test/v1/models?limit=1000&after_id=b");
    CHECK(requests[0].header("x-api-key") == "***");
    CHECK_FALSE(requests[0].header("content-type"));
    CHECK(requests[0].raw_body.empty());

    auto denied = client.list_models();
    REQUIRE_FALSE(denied);
    CHECK(denied.error().message.find("authentication_error") != std::string::npos);

    ClientConfig no_key = test_config();
    no_key.api_key.clear();
    Client keyless(no_key, transport);
    CHECK_FALSE(keyless.list_models());
}

TEST_CASE("find_replay_script picks the function's script, then the plain name, then default.jsonl") {
    auto dir = fs::TempDir::create("decomp-replay-dir").value();
    Symbol fn;
    fn.name = "add";
    fn.va = 0x401060;
    CHECK_FALSE(find_replay_script(dir.path(), fn));
    REQUIRE(fs::write_text(dir.path() / "default.jsonl", "\n"));
    CHECK(find_replay_script(dir.path(), fn) == dir.path() / "default.jsonl");
    REQUIRE(fs::write_text(dir.path() / "add.jsonl", "\n"));
    CHECK(find_replay_script(dir.path(), fn) == dir.path() / "add.jsonl");
    REQUIRE(fs::write_text(dir.path() / "add_401060.jsonl", "\n"));
    CHECK(find_replay_script(dir.path(), fn) == dir.path() / "add_401060.jsonl");
}

TEST_CASE("decomp.json: run budget, workers and approval policies") {
    Json j = {{"version", 1},
              {"target", {{"path", "game.exe"}}},
              {"agent", {{"max_usd_per_run", 25.0}, {"workers", 6}, {"approvals", {{"write_source", "ask"}}}}}};
    auto config = project::Config::from_json(j);
    REQUIRE(config);
    CHECK(config->agent.max_usd_per_run == 25.0);
    CHECK(config->agent.workers == 6);
    CHECK(config->agent.approvals.at("write_source") == "ask");
    auto again = project::Config::from_json(config->to_json());
    REQUIRE(again);
    CHECK(again->agent.approvals == config->agent.approvals);
    CHECK(again->agent.workers == 6);

    j["agent"]["approvals"]["write_source"] = "maybe";
    CHECK_FALSE(project::Config::from_json(j));
    j["agent"]["approvals"]["write_source"] = "deny";
    j["agent"]["workers"] = 0;
    CHECK_FALSE(project::Config::from_json(j));
}

TEST_CASE("resolve_approval_policies: decomp.json, overrides and where ask is allowed") {
    auto r = resolve_approval_policies({{"write_source", "ask"}}, {}, true);
    REQUIRE(r);
    CHECK(r->at("write_source") == ApprovalPolicy::ask);
    CHECK_FALSE(resolve_approval_policies({{"write_source", "ask"}}, {}, false));  // nobody can answer on the CLI
    r = resolve_approval_policies({{"write_source", "ask"}}, {"write_source=deny"}, false);
    REQUIRE(r);
    CHECK(r->at("write_source") == ApprovalPolicy::deny);
    CHECK_FALSE(resolve_approval_policies({}, {"write_source"}, false));
    CHECK_FALSE(resolve_approval_policies({}, {"write_everything=auto"}, false));
    CHECK_FALSE(resolve_approval_policies({}, {"write_source=sometimes"}, false));
    CHECK(resolve_approval_policies({}, {}, false)->empty());
}
