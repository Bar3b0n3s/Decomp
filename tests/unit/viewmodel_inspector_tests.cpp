#include "events/bus.hpp"
#include "viewmodel/inspector.hpp"
#include "viewmodel_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace decomp;
using namespace decomp::vm;

namespace {

const XrefRow* find_row(const std::vector<XrefRow>& rows, auto pred) {
    auto it = std::ranges::find_if(rows, pred);
    return it == rows.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("inspector: the listing splits annotated lines into what the view colors and links") {
    test::FixtureProject fx;
    const u64 sum = fx.va("sum_array");
    auto listing = build_listing(fx.program, sum);
    REQUIRE(listing);
    const AnnotatedFunction direct = annotate_function(fx.program, sum, false).value();
    REQUIRE(listing->lines.size() == direct.lines.size());
    CHECK(listing->function.lines.empty());
    CHECK(listing->function.block_count == direct.block_count);
    CHECK(listing->max_loop_depth >= 1);
    for (usize i = 0; i < listing->lines.size(); ++i) {
        const ListingLine& l = listing->lines[i];
        CHECK(l.address == direct.lines[i].address);
        // The mnemonic and the operands are the annotated text.
        CHECK((l.operands.empty() ? l.mnemonic : l.mnemonic + " " + l.operands) == direct.lines[i].text);
    }
    CHECK(listing->lines.front().block_start);
    CHECK(std::ranges::any_of(listing->lines, &ListingLine::loop_header));
    // The loop's back edge jumps to a label inside the function.
    const auto back = std::ranges::find_if(listing->lines, [](const ListingLine& l) { return l.flow == x86::Flow::cond_jump && l.inside; });
    REQUIRE(back != listing->lines.end());
    REQUIRE(back->target);
    CHECK(std::ranges::any_of(listing->lines, [&](const ListingLine& l) { return l.address == *back->target && !l.label.empty(); }));
    CHECK(listing->lines.back().flow == x86::Flow::ret);

    // dispatch calls other functions (links that leave it) and reads globals.
    const auto dispatch = build_listing(fx.program, fx.va("dispatch")).value();
    const auto call = std::ranges::find_if(dispatch.lines, [&](const ListingLine& l) { return l.flow == x86::Flow::call && l.target == fx.va("add"); });
    REQUIRE(call != dispatch.lines.end());
    CHECK_FALSE(call->inside);
    CHECK(call->operands == "add");
    CHECK(std::ranges::any_of(dispatch.lines, [&](const ListingLine& l) { return l.target == fx.va("g_counter") && !l.inside; }));
    CHECK_FALSE(build_listing(fx.program, 0x12345));
}

TEST_CASE("inspector: callers, callees and data references") {
    test::FixtureProject fx;
    const FunctionXrefs add = function_xrefs(fx.program, fx.va("add"));
    const XrefRow* from_dispatch = find_row(add.callers, [&](const XrefRow& r) { return r.function == fx.va("dispatch"); });
    REQUIRE(from_dispatch);
    CHECK(from_dispatch->is_function);
    CHECK(from_dispatch->kind == XrefKind::call);
    CHECK(from_dispatch->name == "int __cdecl dispatch(int, int)");
    CHECK(find_row(add.callers, [&](const XrefRow& r) { return r.function == fx.va("entry"); }));
    CHECK(add.callees.empty());
    CHECK(find_row(add.data, [&](const XrefRow& r) { return r.target == fx.va("g_counter"); }));

    const FunctionXrefs dispatch = function_xrefs(fx.program, fx.va("dispatch"));
    for (const char* callee : {"add", "other_value", "sum_array", "helper"}) {
        CAPTURE(callee);
        const XrefRow* row = find_row(dispatch.callees, [&](const XrefRow& r) { return r.target == fx.va(callee); });
        REQUIRE(row);
        CHECK(row->is_function);
    }
    // helper is reached by a tail jump as well as a call.
    CHECK(find_row(dispatch.callees, [&](const XrefRow& r) { return r.target == fx.va("helper") && r.kind == XrefKind::jump; }));
    const XrefRow* table = find_row(dispatch.data, [&](const XrefRow& r) { return r.target == fx.va("g_table"); });
    REQUIRE(table);
    CHECK_FALSE(table->is_function);
    CHECK(std::ranges::is_sorted(dispatch.callees, {}, &XrefRow::at));
    // entry calls ExitProcess through its import slot: not a function, so a link to the Binary explorer.
    const FunctionXrefs entry = function_xrefs(fx.program, fx.va("entry"));
    CHECK(std::ranges::any_of(entry.callees, [](const XrefRow& r) { return !r.is_function && r.name.find("ExitProcess") != std::string::npos; }));
}

TEST_CASE("inspector: attempts and their score series") {
    const std::vector<Json> lines = {
        Json{{"attempt", 1}, {"session", "s1"}, {"time", "2026-10-04T10:00:00Z"}, {"compiled", true}, {"match_percent", 62.5}, {"byte_exact", false},
             {"summary", "62.5%"}, {"source", "int f();"}},
        Json("not an object"),
        Json{{"attempt", 2}, {"session", "s1"}, {"compiled", false}, {"summary", "error C2065"}},
        Json{{"attempt", 1}, {"session", "s2"}, {"origin", "user"}, {"time", "2026-10-04T11:00:00.250Z"}, {"compiled", true}, {"match_percent", 100.0},
             {"byte_exact", true}},
    };
    const auto attempts = parse_attempts(lines);
    REQUIRE(attempts.size() == 3);
    CHECK(attempts[0].session == "s1");
    CHECK(attempts[0].origin == "agent");  // older records have no origin
    CHECK(parse_iso8601(attempts[0].time) == parse_iso8601("2026-10-04T10:00:00Z"));
    CHECK(attempts[1].match_percent == 0);
    CHECK(attempts[1].time.empty());
    CHECK(attempts[2].origin == "user");
    CHECK(attempts[2].byte_exact);

    const AttemptSeries s = attempt_series(attempts);
    CHECK(s.score.x == std::vector<double>{1, 2, 3});
    CHECK(s.score.y == std::vector<double>{62.5, 0, 100});
    CHECK(s.best.y == std::vector<double>{62.5, 62.5, 100});
    CHECK(attempt_series({}).score.empty());
}

TEST_CASE("inspector: status history from the runs' event logs") {
    auto dir = fs::TempDir::create("decomp-vm-status").value();
    auto write_run = [&](const std::string& id, std::vector<events::Event> list) {
        const auto path = dir.path() / id / "events.jsonl";
        auto log = events::JsonlEventLog::open(path).value();
        for (const auto& e : list) log->write(e);
    };
    auto event = [](u64 seq, const std::string& run, long long seconds, events::Payload payload) {
        events::Event e;
        e.seq = seq;
        e.run = run;
        e.time = from_unix_ms(1'791'108'000'000 + seconds * 1000);
        e.payload = std::move(payload);
        return e;
    };
    write_run("2026-10-04T10-00-00-aaaa",
              {event(1, "2026-10-04T10-00-00-aaaa", 0, events::RunStarted{"/p", "m", "high", 1, {}}),
               event(2, "2026-10-04T10-00-00-aaaa", 5, events::StatusChanged{"add", 0x401060, "in_progress", "unstarted", 0}),
               event(3, "2026-10-04T10-00-00-aaaa", 9, events::LogLine{"warn", "status_changed mentioned in a message", ""}),
               event(4, "2026-10-04T10-00-00-aaaa", 10, events::StatusChanged{"add", 0x401060, "nonmatching", "in_progress", 62.5})});
    write_run("2026-10-04T12-00-00-bbbb",
              {event(1, "2026-10-04T12-00-00-bbbb", 7200, events::StatusChanged{"add", 0x401060, "matched", "nonmatching", 100}),
               event(2, "2026-10-04T12-00-00-bbbb", 7201, events::StatusChanged{"mix", 0x4011a0, "refused", "unstarted", 0})});
    std::filesystem::create_directories(dir.path() / "empty-run");

    const StatusHistory history = load_status_history(dir.path());
    REQUIRE(history.contains(0x401060));
    const auto& add = history.at(0x401060);
    REQUIRE(add.size() == 3);
    CHECK(add[0].status == "in_progress");
    CHECK(add[1].old_status == "in_progress");
    CHECK(add[1].best == 62.5);
    CHECK(add[2].status == "matched");
    CHECK(add[2].run == "2026-10-04T12-00-00-bbbb");
    CHECK(std::ranges::is_sorted(add, {}, &StatusChangeRecord::time));
    CHECK(history.at(0x4011a0).front().status == "refused");
    CHECK(load_status_history(dir.path() / "missing").empty());
    int polls = 0;
    CHECK(load_status_history(dir.path(), [&] { return ++polls > 0; }).empty());
}
