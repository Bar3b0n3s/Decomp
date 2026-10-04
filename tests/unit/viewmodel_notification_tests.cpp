#include "viewmodel/notification_rules.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;
using namespace decomp::vm;

namespace {

std::vector<std::string> kinds(const std::vector<Notification>& list) {
    std::vector<std::string> out;
    for (const auto& n : list) out.push_back(n.kind);
    return out;
}

const Notification* find_kind(const std::vector<Notification>& list, std::string_view kind) {
    for (const auto& n : list)
        if (n.kind == kind) return &n;
    return nullptr;
}

void finish_session(test::EventScript& run, const std::string& id, const std::string& name, u64 va, const std::string& outcome,
                    double cost = 0.1, std::string detail = {}) {
    run.add(events::SessionStarted{id, "?" + name + "@@YAXXZ", name, va}, 0);
    run.add(events::TurnFinished{id, 1, "tool_use", {}, cost, 100}, 0);
    run.add(events::SessionFinished{id, outcome, std::move(detail), outcome == "matched" ? 100.0 : 40.0, 4, cost}, 0);
}

} // namespace

TEST_CASE("notifications: matched, gave up and refused, batched when many arrive together") {
    NotificationOptions options;
    options.batch_window = std::chrono::milliseconds(1000);
    NotificationRules rules(options);
    test::EventScript run;
    run.add(events::RunStarted{"p", "m", "high", 2, {}, {}});
    const TimePoint t = run.t0;
    CHECK(rules.update(run.data(), t).empty());

    finish_session(run, "s1", "add", 0x1000, "matched");
    CHECK(rules.update(run.data(), t).empty());  // waits for the batch window
    CHECK(rules.waiting());
    auto out = rules.update(run.data(), t + std::chrono::milliseconds(1000));
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == "matched");
    CHECK(out[0].severity == Severity::info);
    CHECK(out[0].toast);
    CHECK_FALSE(out[0].sticky);
    CHECK(out[0].text == "Matched add (4 turns, $0.1000)");
    CHECK(out[0].link.view == "diff_viewer");
    CHECK(out[0].link.va == 0x1000);
    CHECK(out[0].link.session == "s1");
    CHECK_FALSE(rules.waiting());
    // Nothing repeats: the same snapshot, a copy of it, or the same events replayed.
    CHECK(rules.update(run.data(), t + std::chrono::seconds(5)).empty());
    const events::RunStateData copy = run.snapshot();
    CHECK(rules.update(copy, t + std::chrono::seconds(6)).empty());
    CHECK(rules.update(events::RunState::replay(run.events).data(), t + std::chrono::seconds(7)).empty());

    // Four matches and a refusal at once: one summary for the matches, the refusal on its own.
    for (int i = 0; i < 4; ++i) finish_session(run, std::format("m{}", i), std::format("f{}", i), 0x2000 + i * 16, "matched");
    run.add(events::SessionStarted{"r1", "?g@@YAXXZ", "g", 0x3000}, 1);
    run.add(events::Refusal{"r1", "cyber", "no"}, 1);
    run.add(events::SessionFinished{"r1", "refused", "declined", 0, 1, 0.01}, 1);
    finish_session(run, "q1", "h", 0x4000, "gave_up", 0.2, "stuck on the loop");
    CHECK(rules.update(run.data(), t + std::chrono::seconds(10)).empty());
    out = rules.update(run.data(), t + std::chrono::seconds(12));
    REQUIRE(out.size() == 3);
    const Notification* batch = find_kind(out, "matched");
    REQUIRE(batch);
    CHECK(batch->text == "4 functions matched: f0, f1, f2 and 1 more");
    CHECK(batch->link.view == "function_browser");
    CHECK(batch->link.anchor == "status:matched");
    const Notification* refused = find_kind(out, "refused");
    REQUIRE(refused);
    CHECK(refused->severity == Severity::warning);
    CHECK(refused->text == "g was refused (category: cyber)");
    CHECK(refused->link.view == "agent_session");
    const Notification* gave_up = find_kind(out, "gave_up");
    REQUIRE(gave_up);
    CHECK(gave_up->text == "Gave up on h: stuck on the loop");

    // Without a window, a batch is what one update finds.
    NotificationRules immediate(NotificationOptions{.batch_window = std::chrono::milliseconds(0)});
    const auto all = immediate.update(run.data(), t);
    CHECK(std::ranges::count(kinds(all), "matched") == 1);  // 5 matches: one summary
    CHECK(find_kind(all, "matched")->text.starts_with("5 functions matched"));
}

TEST_CASE("notifications: budgets at 80% and 100%, run and function") {
    NotificationRules rules(NotificationOptions{.batch_window = std::chrono::milliseconds(0)});
    test::EventScript run;
    run.add(events::RunStarted{"p", "m", "high", 2, {}, {}, Json{{"run_budget_usd", 10.0}, {"limits", {{"max_usd", 2.0}}}}});
    run.add(events::SessionStarted{"s1", "?f@@YAXXZ", "f", 0x1000}, 0);
    run.add(events::TurnFinished{"s1", 1, "tool_use", {}, 1.7, 100}, 0);
    auto out = rules.update(run.data(), run.now);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == "budget");
    CHECK(out[0].severity == Severity::warning);
    CHECK(out[0].text == "f has used 85% of its budget ($1.70 of $2.00)");
    CHECK(out[0].link.view == "cost");
    CHECK(out[0].link.session == "s1");

    run.add(events::TurnFinished{"s1", 2, "tool_use", {}, 0.4, 100}, 0);
    run.add(events::SessionFinished{"s1", "budget_exhausted", "cost budget exhausted ($2.1000 of $2.0000)", 50, 2, 2.1}, 0);
    out = rules.update(run.data(), run.now);
    REQUIRE(out.size() == 1);
    CHECK(out[0].severity == Severity::error);
    CHECK(out[0].sticky);
    CHECK(out[0].text == "f reached its budget: cost budget exhausted ($2.1000 of $2.0000)");

    // The run budget (function budgets off), then lowered: 80% fires again for the new limit, then 100%.
    run.add(events::BudgetChanged{"function", 0});
    run.add(events::SessionStarted{"s2", "?g@@YAXXZ", "g", 0x2000}, 1);
    run.add(events::TurnFinished{"s2", 1, "tool_use", {}, 6.0, 100}, 1);  // 8.1 of 10
    out = rules.update(run.data(), run.now);
    REQUIRE(out.size() == 1);
    CHECK(out[0].text == "The run has used 81% of its budget ($8.10 of $10.00)");
    run.add(events::BudgetChanged{"run", 9.0});
    out = rules.update(run.data(), run.now);
    REQUIRE(out.size() == 1);  // a new limit re-arms; 8.1 is 90% of 9
    CHECK(out[0].text.starts_with("The run has used 90%"));
    run.add(events::TurnFinished{"s2", 2, "tool_use", {}, 1.0, 100}, 1);
    out = rules.update(run.data(), run.now);
    REQUIRE(out.size() == 1);
    CHECK(out[0].severity == Severity::error);
    CHECK(out[0].sticky);
    CHECK(out[0].text == "The run budget is spent ($9.10 of $9.00)");
    CHECK(rules.update(run.data(), run.now).empty());
}

TEST_CASE("notifications: authentication errors and rate-limit storms") {
    NotificationRules rules(NotificationOptions{.batch_window = std::chrono::milliseconds(0)});
    test::EventScript run;
    run.add(events::RunStarted{"p", "m", "high", 2, {}, {}});
    // Two sessions fail with the same 401 (different request ids): one sticky notification.
    for (const char* id : {"s1", "s2"}) {
        run.add(events::SessionStarted{id, "?f@@YAXXZ", "f", 0x1000}, 0);
        run.add(events::SessionFinished{id, "error",
                                        std::format("api error: HTTP 401 authentication_error: invalid x-api-key (request-id req_{})", id), 0, 1, 0},
                0);
    }
    auto out = rules.update(run.data(), run.now);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == "auth");
    CHECK(out[0].severity == Severity::error);
    CHECK(out[0].sticky);
    CHECK(out[0].text == "Authentication failed: api error: HTTP 401 authentication_error: invalid x-api-key");
    CHECK(out[0].link.view == "settings");
    // A missing key, logged.
    run.add(events::LogLine{"error", "ANTHROPIC_API_KEY is not set"});
    run.add(events::LogLine{"warn", "HTTP 401 mentioned at warn level counts too"});
    run.add(events::LogLine{"info", "HTTP 403 at info level does not"});
    out = rules.update(run.data(), run.now);
    CHECK(out.size() == 2);

    // 429s: three in one minute and two in the next make a storm; a quiet minute ends it.
    auto retry = [&](double at, int status) {
        run.at(at);
        run.add(events::Retry{"s3", 1, "HTTP 429", 100, status}, 0);
    };
    retry(600, 429);
    retry(610, 529);  // overloaded is not a rate limit
    retry(620, 429);
    CHECK(rules.update(run.data(), run.now).empty());
    retry(630, 429);
    retry(670, 429);
    retry(680, 429);
    out = rules.update(run.data(), run.now);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == "rate_limit");
    CHECK(out[0].severity == Severity::warning);
    CHECK(out[0].toast);
    CHECK(out[0].text == "Rate-limit storm: 5 requests were answered 429 within two minutes");
    CHECK(out[0].link.view == "run_monitor");
    retry(730, 429);  // still the same storm
    retry(740, 429);
    retry(750, 429);
    CHECK(rules.update(run.data(), run.now).empty());
    retry(900, 429);  // after quiet minutes, a new storm
    retry(905, 429);
    retry(910, 429);
    retry(915, 429);
    retry(916, 429);
    out = rules.update(run.data(), run.now);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == "rate_limit");
}

TEST_CASE("notifications: toolchain failures that are not compile errors") {
    NotificationRules rules(NotificationOptions{.batch_window = std::chrono::milliseconds(0)});
    test::EventScript run;
    run.add(events::RunStarted{"p", "m", "high", 1, {}, {}});
    auto compile = [&](bool ok, bool cached, int exit_code, int errors, std::string output) {
        run.add(events::CompileFinished{"s1", ok, cached, 1500, errors, exit_code, "cl /c x.cpp", std::move(output), "msvc6"}, 0);
    };
    compile(true, false, 0, 0, "");
    compile(false, false, 2, 3, "x.cpp(3): error C2065: 'y': undeclared identifier");  // the candidate's fault
    compile(false, true, -1, 0, "cached failure");
    compile(false, false, -1, 0, "\n[compile cancelled]");
    CHECK(rules.update(run.data(), run.now).empty());

    compile(false, false, 1, 0, "\n[compiler timed out after 120s]");
    compile(false, false, 1, 0, "\n[compiler timed out after 120s]");  // the same kind again: no repeat
    auto out = rules.update(run.data(), run.now);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == "toolchain");
    CHECK(out[0].severity == Severity::error);
    CHECK(out[0].sticky);
    CHECK(out[0].text == "Compile timed out (toolchain msvc6, after 2 s)");
    CHECK(out[0].link.view == "toolchains");

    compile(false, false, static_cast<int>(0xC0000005u), 0, "");
    compile(false, false, 139, 0, "");
    compile(false, false, 0, 0, "");
    compile(false, false, 2, 0, "cl : Command line error D8021 : invalid numeric argument '/Wx'");
    out = rules.update(run.data(), run.now);
    REQUIRE(out.size() == 3);  // crash (twice, one kind), no object, no diagnostics
    CHECK(out[0].text == "The compiler crashed (toolchain msvc6, exit code 0xc0000005)");
    CHECK(out[1].text == "The compiler reported success but wrote no object (toolchain msvc6)");
    CHECK(out[2].text.starts_with("The compiler failed without diagnostics (toolchain msvc6, exit code 2): cl : Command line error D8021"));

    // A compiler that cannot start shows up as a failed tool call.
    run.add(events::SessionStarted{"s1", "?f@@YAXXZ", "f", 0x1000}, 0);
    run.add(events::ToolCallFinished{"s1", "t1", "compile_and_diff", true, "internal error: program 'cl.exe' not found in PATH", 3}, 0);
    run.add(events::ToolCallFinished{"s1", "t2", "lookup_symbol", true, "internal error: something else", 3}, 0);
    out = rules.update(run.data(), run.now);
    REQUIRE(out.size() == 1);
    CHECK(out[0].text == "The compiler could not run: program 'cl.exe' not found in PATH");

    matching::HealthReport report;
    report.ok = true;
    CHECK_FALSE(health_check_notification("msvc6", report, run.now));
    report.ok = false;
    report.output = "wine: cannot find CL.EXE\nmore";
    const auto health = health_check_notification("msvc6", report, run.now);
    REQUIRE(health);
    CHECK(health->sticky);
    CHECK(health->severity == Severity::error);
    CHECK(health->text == "Toolchain msvc6 failed its health check: wine: cannot find CL.EXE");
    CHECK(health->link.view == "toolchains");
    CHECK(health->link.anchor == "toolchain:msvc6");
}

TEST_CASE("notifications: approvals, fallback turns and the end of a run, resumed") {
    NotificationRules rules(NotificationOptions{.batch_window = std::chrono::milliseconds(0)});
    test::EventScript run;
    run.add(events::RunStarted{"p", "m", "high", 2, {}, {}});
    run.add(events::SessionStarted{"s1", "?f@@YAXXZ", "f", 0x1000}, 0);
    run.add(events::ApprovalRequested{7, "write_source", "s1", "f", 0x1000, "src/functions/f_1000.cpp", "byte-exact f"});
    run.add(events::TurnStarted{"s1", 3}, 0);
    run.add(events::TurnFinished{"s1", 3, "tool_use", {}, 0.1, 100, "model-b", 10, true}, 0);
    auto out = rules.update(run.data(), run.now);
    REQUIRE(out.size() == 2);
    const Notification* approval = find_kind(out, "approval");
    REQUIRE(approval);
    CHECK(approval->severity == Severity::warning);
    CHECK(approval->text == "Approval requested: write_source for f");
    CHECK(approval->link.view == "changes");
    CHECK(approval->link.anchor == "approval:7");
    const Notification* fallback = find_kind(out, "fallback");
    REQUIRE(fallback);
    CHECK_FALSE(fallback->toast);  // history only
    CHECK(fallback->text == "A fallback model served turn 3 of f (served by model-b)");
    CHECK(fallback->link.anchor == "turn:3");

    run.add(events::ApprovalDecided{7, "approved", "user", ""});
    run.at(3725);
    run.add(events::SessionFinished{"s1", "matched", "", 100, 3, 0.1}, 0);
    run.add(events::RunFinished{"stopped"});
    out = rules.update(run.data(), run.now);
    CHECK(kinds(out) == std::vector<std::string>{"matched", "run_finished"});  // the summary last
    CHECK(out[1].text == "Run stopped: 1 matched, 0 gave up, 0 refused, $0.1000, 1h 02m");
    CHECK(out[1].link.view == "dashboard");

    // Resumed: interrupted sessions are not news; the second end is.
    run.at(4000);
    run.add(events::RunResumed{{}});
    CHECK(rules.update(run.data(), run.now).empty());
    run.add(events::SessionStarted{"s2", "?g@@YAXXZ", "g", 0x2000}, 0);
    run.add(events::SessionFinished{"s2", "gave_up", "", 10, 2, 0.2}, 0);
    run.at(4100);
    run.add(events::RunFinished{"completed"});
    out = rules.update(run.data(), run.now);
    CHECK(kinds(out) == std::vector<std::string>{"gave_up", "run_finished"});
    CHECK(out[1].text.starts_with("Run completed: 1 matched, 1 gave up"));
}

TEST_CASE("notifications: a primed past run stays quiet; another run starts fresh") {
    test::EventScript past;
    past.add(events::RunStarted{"p", "m", "high", 1, {}, {}});
    finish_session(past, "s1", "add", 0x1000, "matched");
    past.add(events::RunFinished{"completed"});
    NotificationRules rules;
    rules.prime(past.data());
    CHECK(rules.update(past.data(), past.now + std::chrono::hours(1)).empty());
    CHECK_FALSE(rules.waiting());

    test::EventScript next;
    next.run = "2026-10-04T12-00-00-0002";
    next.add(events::RunStarted{"p", "m", "high", 1, {}, {}});
    finish_session(next, "s1", "add", 0x1000, "matched");  // same session id, another run
    CHECK(rules.update(next.data(), next.now).empty());  // batching
    // Switching runs posts what was waiting.
    const auto out = rules.update(past.data(), next.now);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == "matched");

    rules.reset();
    CHECK(rules.update(past.data(), past.now).size() == 2);  // matched and run_finished, after a reset
    CHECK(to_string(Severity::warning) == "warning");
}
