// The Agent session's timeline, C++ highlighting, attempt histories and the manual mode's recompile
// schedule.

#include "viewmodel/attempts.hpp"
#include "viewmodel/highlight.hpp"
#include "viewmodel/recompile.hpp"
#include "viewmodel/timeline.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <random>

using namespace decomp;
using namespace decomp::vm;
using namespace std::chrono_literals;

namespace {

std::string line(const Json& j) { return dump_compact(j) + "\n"; }

Json text_block(const std::string& text) { return Json{{"type", "text"}, {"text", text}}; }

// A session in the shape agent::run_function writes: brief and first guidance, a compile, guidance
// sent through the controller, a fallback, and a match.
std::string sample_transcript() {
    std::string t;
    t += line({{"type", "session"}, {"session", "run-401060"}, {"function", "?add@@YAHHH@Z"}, {"display", "int __cdecl add(int, int)"},
               {"va", 0x401060}, {"model", "claude-opus-5-5"}, {"effort", "high"}, {"worker", 0}, {"time", 1000}});
    t += line({{"type", "guidance"}, {"turn", 0}, {"text", "Start with the listing."}, {"time", 1001}});
    t += line({{"type", "request"},
               {"turn", 1},
               {"time", 1002},
               {"body",
                {{"messages", Json::array({Json{{"role", "user"},
                                                {"content", Json::array({text_block("# Target function\nadd"),
                                                                         text_block("[Supervisor guidance] Start with the listing.")})}}})}}}});
    t += line({{"type", "response"},
               {"turn", 1},
               {"id", "msg_1"},
               {"model", "claude-opus-5-5"},
               {"stop_reason", "tool_use"},
               {"time", 2000},
               {"content", Json::array({Json{{"type", "thinking"}, {"thinking", ""}, {"signature", "s"}},
                                        Json{{"type", "text"}, {"text", "A first try."}},
                                        Json{{"type", "tool_use"},
                                             {"id", "toolu_1"},
                                             {"name", "compile_and_diff"},
                                             {"input", {{"source", "int add(int a, int b) { return a - b; }\n"}}}}})},
               {"usage", {{"input_tokens", 100}, {"output_tokens", 50}, {"cache_creation_input_tokens", 0}, {"cache_read_input_tokens", 0}}},
               {"cost_usd", 0.01},
               {"latency_ms", 900},
               {"ttft_ms", 120}});
    t += line({{"type", "tool"},
               {"turn", 1},
               {"id", "toolu_1"},
               {"name", "compile_and_diff"},
               {"input", {{"source", "int add(int a, int b) { return a - b; }\n"}}},
               {"is_error", false},
               {"result", "compile: ok\nmatch 68.8% (2/4 equal; 1 operand, 1 opcode) - not matching\ntarget ...\n\nattempt 1: best so far 68.8%"},
               {"elapsed_ms", 800},
               {"time", 2100}});
    t += line({{"type", "guidance"}, {"turn", 1}, {"id", 7}, {"text", "Swap the operands."}, {"time", 2200}});
    t += line({{"type", "request_delta"},
               {"turn", 2},
               {"time", 2201},
               {"messages",
                Json::array({Json{{"role", "assistant"}, {"content", Json::array()}},
                             Json{{"role", "user"},
                                  {"content", Json::array({Json{{"type", "tool_result"}, {"tool_use_id", "toolu_1"}, {"content", "compile: ok"}},
                                                           text_block("[Supervisor guidance] Swap the operands.\n\n[status] turns left: 38; "
                                                                      "attempts: 1; best match: 68.8%")})}}})}});
    t += line({{"type", "retry"}, {"turn", 2}, {"attempt", 1}, {"error", "overloaded"}, {"delay_ms", 2000}, {"status", 529}, {"time", 2300}});
    t += line({{"type", "response"},
               {"turn", 2},
               {"id", "msg_2"},
               {"model", "claude-sonnet-5-5"},
               {"stop_reason", "tool_use"},
               {"had_fallback", true},
               {"time", 3000},
               {"content", Json::array({Json{{"type", "fallback"}, {"from", {{"model", "claude-opus-5-5"}}}, {"to", {{"model", "claude-sonnet-5-5"}}}},
                                        Json{{"type", "thinking"}, {"thinking", "Swap them."}, {"signature", "s2"}},
                                        Json{{"type", "tool_use"},
                                             {"id", "toolu_2"},
                                             {"name", "submit_result"},
                                             {"input", {{"outcome", "matched"}, {"reason", ""}, {"source", "int add(int a, int b) { return a + b; }\n"}}}}})},
               {"usage", {{"input_tokens", 10}, {"output_tokens", 40}, {"cache_creation_input_tokens", 0}, {"cache_read_input_tokens", 90}}},
               {"cost_usd", 0.02}});
    t += line({{"type", "tool"},
               {"turn", 2},
               {"id", "toolu_2"},
               {"name", "submit_result"},
               {"input", Json::object()},
               {"is_error", false},
               {"result", "accepted: byte-exact match verified."},
               {"elapsed_ms", 700},
               {"time", 3100}});
    t += line({{"type", "outcome"}, {"outcome", "matched"}, {"detail", ""}, {"best_match", 100.0}, {"turns", 2}, {"cost_usd", 0.03}, {"time", 3200}});
    return t;
}

std::vector<TimelineItem::Kind> kinds(const std::vector<TimelineItem>& items) {
    std::vector<TimelineItem::Kind> out;
    for (const auto& i : items) out.push_back(i.kind);
    return out;
}

} // namespace

TEST_CASE("timeline: turns, their requests, responses and results in reading order") {
    const TranscriptDoc doc = parse_transcript(sample_transcript());
    REQUIRE(doc.turns.size() == 2);
    using K = TimelineItem::Kind;
    const auto items = build_timeline(doc, false);
    CHECK(kinds(items) == std::vector<K>{
                              K::brief,
                              K::turn, K::user,  // guidance sent with the brief
                              K::thinking, K::text, K::tool_call, K::tool_result,
                              K::turn, K::user, K::user,  // guidance and status line; the tool result was shown with its call
                              K::retry, K::fallback, K::thinking, K::tool_call, K::tool_result,
                              K::outcome,
                          });
    // The tool result follows its call and names the exchange.
    const auto result = std::ranges::find(items, K::tool_result, &TimelineItem::kind);
    REQUIRE(result != items.end());
    CHECK(doc.turns[static_cast<usize>(result->turn)].tools[static_cast<usize>(result->index)].id == "toolu_1");

    // Both candidate sources, numbered in order.
    const auto sources = transcript_sources(doc);
    REQUIRE(sources.size() == 2);
    CHECK(sources[0].tool == "compile_and_diff");
    CHECK(sources[1].tool == "submit_result");
    CHECK(block_source(doc, sources[0]).find("a - b") != std::string_view::npos);
    CHECK(block_source(doc, sources[1]).find("a + b") != std::string_view::npos);
    CHECK(block_source(doc, SourceRef{5, 0, ""}).empty());
    std::vector<i32> numbered;
    for (const auto& i : items)
        if (i.kind == K::tool_call) numbered.push_back(i.source);
    CHECK(numbered == std::vector<i32>{0, 1});

    // Guidance 7 was sent; an id the transcript does not have is still pending.
    CHECK(guidance_sent(doc, 7));
    CHECK_FALSE(guidance_sent(doc, 8));
    CHECK_FALSE(guidance_sent(doc, 0));

    // With the live buffer and a pending pause marker.
    TranscriptReader reader;
    reader.feed(sample_transcript());
    reader.feed(line({{"type", "paused"}, {"turn", 2}, {"time", 3300}}));
    const auto live = build_timeline(reader.doc(), true);
    CHECK(live.back().kind == K::outcome);
    CHECK(std::ranges::count(kinds(live), K::live) == 1);
    CHECK(std::ranges::count(kinds(live), K::pending) == 1);
}

TEST_CASE("timeline: the live buffer shows only what the transcript does not have yet") {
    TranscriptReader reader;
    const std::string all = sample_transcript();
    // Up to the first request: turn 1 is in flight.
    const usize first_response = all.rfind('\n', all.find("\"type\":\"response\"")) + 1;
    reader.feed(all.substr(0, first_response));
    const TranscriptDoc& doc = reader.doc();
    REQUIRE(doc.turns.size() == 1);
    CHECK_FALSE(doc.turns[0].has_response);
    CHECK(show_live(doc, 1, false, true));
    CHECK_FALSE(show_live(doc, 1, false, false));  // nothing streamed yet
    CHECK_FALSE(show_live(doc, 1, true, true));    // the session ended
    CHECK(show_live(doc, 2, false, true));         // a turn the transcript has not reached
    const auto items = build_timeline(doc, true);
    CHECK(items.back().kind == TimelineItem::Kind::live);
    CHECK(std::ranges::count(kinds(items), TimelineItem::Kind::no_response) == 1);

    const TranscriptDoc whole = parse_transcript(all);
    CHECK_FALSE(show_live(whole, 2, false, true));  // turn 2's response is recorded
}

TEST_CASE("timeline: what tool results say") {
    auto ok = summarize_tool_result("compile: ok (cached)\nmatch 87.5% (3/4 equal; 1 operand) - not matching\ntarget x | candidate x\n"
                                    "~    4: fmul ...\n\nattempt 3: best so far 87.5%");
    CHECK(ok.compile);
    CHECK(ok.compiled);
    CHECK(ok.cached);
    CHECK(ok.headline == "match 87.5% (3/4 equal; 1 operand) - not matching");
    CHECK(ok.attempt == "attempt 3: best so far 87.5%");
    CHECK(ok.errors.empty());

    auto failed = summarize_tool_result("compile: FAILED\nline 1:40: error: use of undeclared identifier 'nope'\nline 2:1: error: x\n\n"
                                        "attempt 1: best so far 100.0%");
    CHECK(failed.compile);
    CHECK_FALSE(failed.compiled);
    CHECK(failed.headline == "compile failed");
    CHECK(failed.errors == std::vector<std::string>{"line 1:40: error: use of undeclared identifier 'nope'", "line 2:1: error: x"});
    CHECK(failed.attempt == "attempt 1: best so far 100.0%");
    CHECK(summarize_tool_result("compile: FAILED (timed out)\n...").headline == "compile failed (timed out)");

    auto missing = summarize_tool_result("compile: ok\ndiff: the candidate object does not define '?add@@YAHHH@Z'\nattempt 2: best so far 0.0%");
    CHECK(missing.compiled);
    CHECK(missing.headline == "the candidate object does not define '?add@@YAHHH@Z'");

    auto rejected = summarize_tool_result("not accepted: the submitted source is not byte-exact.\ncompile: ok\nmatch 75.0% (...) - not matching\n");
    CHECK(rejected.compile);
    CHECK(rejected.headline.starts_with("match 75.0%"));
    auto plain = summarize_tool_result("0x403000 data size 4 int g_counter\nmore");
    CHECK_FALSE(plain.compile);
    CHECK(plain.headline == "0x403000 data size 4 int g_counter");
    CHECK(summarize_tool_result("").headline.empty());
}

TEST_CASE("item heights: offsets and lookups match the running sums") {
    ItemHeights h;
    CHECK(h.total() == 0.0f);
    CHECK(h.find(10.0f) == 0);
    h.resize(1000, 20.0f);
    CHECK(h.total() == doctest::Approx(20000.0f));
    CHECK(h.find(0.0f) == 0);
    CHECK(h.find(19.9f) == 0);
    CHECK(h.find(20.0f) == 1);
    CHECK(h.find(20000.0f) == 1000);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> height(1.0f, 400.0f);
    std::vector<float> naive(1000, 20.0f);
    for (int k = 0; k < 3000; ++k) {
        const usize i = rng() % 1000;
        const float v = std::round(height(rng));  // whole pixels keep the float sums exact
        naive[i] = v;
        h.set(i, v);
    }
    float sum = 0;
    for (usize i = 0; i < naive.size(); ++i) {
        CHECK(h.offset(i) == doctest::Approx(sum));
        CHECK(h.find(sum) == i);
        CHECK(h.find(sum + naive[i] - 0.5f) == i);
        sum += naive[i];
    }
    CHECK(h.total() == doctest::Approx(sum));
    // Growing keeps the measured heights.
    h.resize(1100, 30.0f);
    CHECK(h.height(5) == naive[5]);
    CHECK(h.total() == doctest::Approx(sum + 100 * 30.0f));
    h.resize(10, 0.0f);
    CHECK(h.size() == 10);
}

TEST_CASE("highlight: comments, literals, numbers, keywords, types and directives") {
    const std::string src = "#include \"x.h\"\n"
                            "#define TWICE(x) \\\n"
                            "    ((x) * 2)\n"
                            "/* a comment\n"
                            "   over lines */ static int f(unsigned n) {\n"
                            "    const char* s = \"a \\\"quoted\\\" // not a comment\"; // a comment\r\n"
                            "    return n + 0x1F + 1.5e-3f + 'c' + L\"w\"[0];\n"
                            "}";
    const auto lines = highlight_cpp(src);
    REQUIRE(lines.size() == 8);
    auto spans_of = [&](usize i) {
        std::vector<std::pair<std::string, CodeToken>> out;
        for (const auto& s : lines[i].spans) out.emplace_back(src.substr(s.begin, s.end - s.begin), s.kind);
        return out;
    };
    using T = CodeToken;
    CHECK(spans_of(0) == std::vector<std::pair<std::string, T>>{{"#include \"x.h\"", T::preprocessor}});
    CHECK(spans_of(2) == std::vector<std::pair<std::string, T>>{{"((x) * 2)", T::preprocessor}});  // continued directive
    CHECK(spans_of(3) == std::vector<std::pair<std::string, T>>{{"/* a comment", T::comment}});
    CHECK(spans_of(4) == std::vector<std::pair<std::string, T>>{
                             {"   over lines */", T::comment}, {"static", T::keyword}, {"int", T::type}, {"unsigned", T::type}});
    CHECK(spans_of(5) == std::vector<std::pair<std::string, T>>{
                             {"const", T::keyword}, {"char", T::type}, {"\"a \\\"quoted\\\" // not a comment\"", T::string}, {"// a comment", T::comment}});
    CHECK(src.substr(lines[5].begin, lines[5].end - lines[5].begin).ends_with("comment"));  // without the \r
    CHECK(spans_of(6) == std::vector<std::pair<std::string, T>>{
                             {"return", T::keyword}, {"0x1F", T::number}, {"1.5e-3f", T::number}, {"'c'", T::string}, {"L\"w\"", T::string}, {"0", T::number}});
    CHECK(spans_of(7).empty());
    CHECK(highlight_cpp("").empty());
    CHECK(highlight_cpp("int\n").size() == 1);
    CHECK(highlight_cpp("\"unterminated\nint").at(1).spans.size() == 1);  // a literal never runs past its line
}

TEST_CASE("attempts: parsed history, the best attempt and attempts made by hand") {
    const auto t = from_unix_ms(1'791'108'000'123);
    std::vector<Json> lines = {
        Json{{"attempt", 1}, {"session", "s1"}, {"origin", "agent"}, {"compiled", true}, {"match_percent", 68.75}, {"byte_exact", false},
             {"summary", "match 68.8%"}, {"source", "a"}, {"time", "2026-10-04T10:00:00.000Z"}},
        Json("not an object"),
        Json{{"attempt", 2}, {"session", "s1"}, {"compiled", false}, {"source", "b"}},
        Json{{"attempt", 1}, {"session", "s2"}, {"compiled", true}, {"match_percent", 100.0}, {"byte_exact", true}, {"source", "c"}},
        Json{{"attempt", 2}, {"session", "s2"}, {"compiled", true}, {"match_percent", 100.0}, {"byte_exact", true}, {"source", "a"}},
    };
    const auto attempts = parse_attempts(lines);
    REQUIRE(attempts.size() == 4);
    CHECK(attempts[0].index == 0);
    CHECK(attempts[1].index == 2);  // the line that was not an object is skipped, positions kept
    CHECK(attempts[1].origin == "agent");  // older records have no origin
    CHECK(attempts[0].match_percent == 68.75);
    CHECK(attempts[2].byte_exact);
    CHECK(best_attempt(attempts, std::string("a")) == 3u);  // the latest with that source
    CHECK(best_attempt(attempts, std::string("c")) == 2u);
    CHECK_FALSE(best_attempt(attempts, std::string("zzz")));
    CHECK_FALSE(best_attempt(attempts, std::nullopt));
    CHECK(session_attempts(attempts, "s2") == std::vector<usize>{2, 3});
    CHECK(next_attempt_number(attempts, "s1") == 3);
    CHECK(next_attempt_number(attempts, "user-x") == 1);

    const std::string session = manual_session_id(t);
    CHECK(session == "user-2026-10-04T10-00-00");
    const Json user = user_attempt_json(session, 1, t, true, 100.0, true, "match 100.0% - MATCHING (byte-exact)", "int x;");
    // The same fields as the agent's attempts.
    std::vector<std::string> keys;
    for (auto it = user.begin(); it != user.end(); ++it) keys.push_back(it.key());
    std::vector<std::string> expected = {"attempt", "byte_exact", "compiled", "match_percent", "origin", "session", "source", "summary", "time"};
    CHECK(keys == expected);
    CHECK(user["origin"] == "user");
    CHECK(user["time"] == "2026-10-04T10:00:00.123Z");
    const auto parsed = parse_attempts(std::vector<Json>{user});
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0].origin == "user");
    CHECK(parsed[0].source == "int x;");
}

TEST_CASE("recompile schedule: debounced, latest edit wins, stale while newer text waits") {
    const TimePoint t0 = from_unix_ms(1'791'108'000'000);
    RecompileSchedule s(500ms);
    s.reset();  // a loaded attempt is shown
    CHECK_FALSE(s.stale());
    CHECK_FALSE(s.due(t0 + 1h));
    CHECK_FALSE(s.due_at());

    // Typing: each edit pushes the compile back.
    s.edited(t0);
    s.edited(t0 + 200ms);
    CHECK(s.stale());
    CHECK_FALSE(s.due(t0 + 600ms));
    CHECK(s.due_at() == t0 + 700ms);
    CHECK(s.due(t0 + 700ms));
    const u64 first = s.start();
    CHECK(s.compiling());
    CHECK_FALSE(s.due(t0 + 10s));  // already compiling that text

    // An edit while it compiles: its result still shows (newest available), marked stale.
    s.edited(t0 + 800ms);
    CHECK(s.finished(first));
    CHECK_FALSE(s.compiling());
    CHECK(s.stale());
    CHECK(s.shown() == first);
    CHECK(s.due(t0 + 1300ms));
    const u64 second = s.start();
    CHECK(second > first);

    // Another edit, and its compile starts before the previous one ends: the older result is dropped.
    s.edited(t0 + 1400ms);
    REQUIRE(s.due(t0 + 1900ms));
    const u64 third = s.start();
    CHECK_FALSE(s.finished(second));
    CHECK(s.compiling());
    CHECK(s.shown() == first);
    CHECK(s.finished(third));
    CHECK_FALSE(s.stale());
    CHECK(s.shown() == third);
    CHECK_FALSE(s.finished(third));  // reported twice: shown once

    // Loading another attempt starts over.
    s.edited(t0 + 2s);
    s.reset();
    CHECK_FALSE(s.stale());
    CHECK_FALSE(s.due(t0 + 1h));
}
