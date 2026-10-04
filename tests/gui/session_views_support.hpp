#pragma once

// Helpers for the Agent session and Diff viewer tests: transcripts in the runner's format, a project
// over the x86 fixture, and past runs made from events.

#include "core/fs.hpp"
#include "core/json.hpp"
#include "events/bus.hpp"
#include "gui/views/agent_session_view.hpp"
#include "gui/views/diff_viewer_view.hpp"
#include "gui/workspace.hpp"
#include "harness.hpp"
#include "project/project.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace decomp::gui::test {

// Transcript records as agent::run_function writes them.
struct TranscriptLines {
    std::string text;

    void add(const Json& j) { text += dump_compact(j) + "\n"; }
    void header(const std::string& session, const std::string& function, const std::string& display, u64 va) {
        add({{"type", "session"}, {"session", session}, {"function", function}, {"display", display}, {"va", va},
             {"model", "claude-opus-5-5"}, {"effort", "high"}, {"worker", 0}, {"time", 1'791'108'000'000}});
    }
    void request(int turn, const std::string& brief) {
        add({{"type", "request"}, {"turn", turn}, {"time", 1'791'108'000'000 + turn},
             {"body", {{"messages", Json::array({Json{{"role", "user"}, {"content", Json::array({Json{{"type", "text"}, {"text", brief}}})}}})}}}});
    }
    void request_delta(int turn, const std::string& tool_id, const std::string& status) {
        add({{"type", "request_delta"},
             {"turn", turn},
             {"messages", Json::array({Json{{"role", "assistant"}, {"content", Json::array()}},
                                       Json{{"role", "user"},
                                            {"content", Json::array({Json{{"type", "tool_result"}, {"tool_use_id", tool_id}, {"content", "compile: ok"}},
                                                                     Json{{"type", "text"}, {"text", status}}})}}})}});
    }
    void response(int turn, const std::string& thinking, const std::string& text, const std::string& tool_id, const std::string& source,
                  const std::string& model = "claude-opus-5-5") {
        add({{"type", "response"},
             {"turn", turn},
             {"id", "msg_" + std::to_string(turn)},
             {"model", model},
             {"stop_reason", "tool_use"},
             {"had_fallback", model != "claude-opus-5-5"},
             {"content", Json::array({Json{{"type", "thinking"}, {"thinking", thinking}, {"signature", "sig"}}, Json{{"type", "text"}, {"text", text}},
                                      Json{{"type", "tool_use"}, {"id", tool_id}, {"name", "compile_and_diff"}, {"input", {{"source", source}}}}})},
             {"usage", {{"input_tokens", 1200}, {"output_tokens", 350}, {"cache_creation_input_tokens", 800}, {"cache_read_input_tokens", 5200}}},
             {"cost_usd", 0.031},
             {"latency_ms", 2300},
             {"ttft_ms", 400}});
    }
    void tool(int turn, const std::string& tool_id, const std::string& source, const std::string& result) {
        add({{"type", "tool"}, {"turn", turn}, {"id", tool_id}, {"name", "compile_and_diff"}, {"input", {{"source", source}}},
             {"is_error", false}, {"result", result}, {"elapsed_ms", 850}});
    }
    void guidance(int turn, u64 id, const std::string& text) { add({{"type", "guidance"}, {"turn", turn}, {"id", id}, {"text", text}}); }
    void outcome(const std::string& outcome, double best, int turns) {
        add({{"type", "outcome"}, {"outcome", outcome}, {"detail", ""}, {"best_match", best}, {"turns", turns}, {"cost_usd", 0.42},
             {"usage", {{"input_tokens", 1}, {"output_tokens", 1}, {"cache_creation_input_tokens", 0}, {"cache_read_input_tokens", 0}}}});
    }
};

// A candidate source of about `lines` lines.
inline std::string sample_source(int variant, int lines) {
    std::string s = "extern int g_counter;\n\n";
    for (int i = 0; i < lines - 4; ++i) s += std::format("static const int k{}_{} = {}; // line {} of the candidate\n", variant, i, i * 3 + variant, i);
    s += std::format("__declspec(noinline) int add(int a, int b) {{ return a + b + g_counter + {}; }}\n", variant);
    return s;
}

// A session of `turns` turns, each with a thinking summary, text, a compile of a ~`source_lines`-line
// source and its result.
inline std::string big_transcript(const std::string& session, u64 va, int turns, int source_lines) {
    TranscriptLines t;
    t.header(session, "?add@@YAHHH@Z", "int __cdecl add(int, int)", va);
    std::string brief = "# Target function\nfunction: int __cdecl add(int, int)\n";
    for (int i = 0; i < 300; ++i) brief += std::format("  0040{:04x}  mov eax, dword ptr [esp+0x{:x}]\n", i * 4, i % 16);
    t.request(1, brief);
    for (int turn = 1; turn <= turns; ++turn) {
        const std::string id = std::format("toolu_{}", turn);
        if (turn > 1) t.request_delta(turn, std::format("toolu_{}", turn - 1), std::format("[status] turns left: {}; attempts: {}; best match: 87.5%", 400 - turn, turn - 1));
        std::string thinking;
        for (int k = 0; k < 6; ++k) thinking += std::format("Step {} of the reasoning about operand order and register allocation in turn {}. ", k, turn);
        const std::string source = sample_source(turn, source_lines);
        t.response(turn, thinking, std::format("Attempt {}: reordering the operands.", turn), id, source, turn % 50 == 0 ? "claude-sonnet-5-5" : "claude-opus-5-5");
        std::string result = std::format("compile: ok\nmatch 87.5% (7/8 equal; 1 operand) - not matching\ntarget ?add@@YAHHH@Z (15 bytes) | candidate ?add@@YAHHH@Z (15 bytes)\n");
        for (int r = 0; r < 24; ++r) result += std::format("~ {:4x}: mov eax, dword ptr [esp+0x8]                  | {:4x}: mov eax, dword ptr [esp+0x4]  (op1 stack)\n", r * 4, r * 4);
        result += std::format("\nattempt {}: best so far 87.5%", turn);
        t.tool(turn, id, source, result);
    }
    t.outcome("gave_up", 87.5, turns);
    return t.text;
}

// A project over the committed x86 fixture, with the flags the fixtures were built with.
struct FixtureProject {
    fs::TempDir dir = fs::TempDir::create("decomp-gui-sessions").value();
    std::filesystem::path root = dir.path() / "project";
    project::Project project;

    FixtureProject() {
        auto p = project::Project::init(root, decomp::test::fixture("x86/basic.exe"), std::nullopt, "clang-cl-x86");
        REQUIRE(p);
        project = std::move(*p);
        project.config().flags = {"/O2", "/Gy", "/GS-", "/GR-", "/EHs-c-"};
        REQUIRE(project.save_config());
    }
};

// Writes a finished run (its events and transcripts) into the project's runs directory, as a past run.
struct PastRun {
    std::string id = "2026-10-04T10-00-00-beef";
    std::filesystem::path dir;
    std::unique_ptr<events::EventBus> bus;
    std::unique_ptr<events::JsonlEventLog> log;

    explicit PastRun(const std::filesystem::path& runs_dir) {
        dir = runs_dir / id;
        REQUIRE(fs::create_directories(dir / "sessions"));
        REQUIRE(fs::write_text(dir / "run.json", dump_pretty(Json{{"id", id}, {"status", "completed"}, {"version", 1}}) + "\n"));
        bus = std::make_unique<events::EventBus>(id);
        log = events::JsonlEventLog::open(dir / "events.jsonl").value();
        bus->subscribe([l = log.get()](const events::Event& e) { l->write(e); });
    }
    void publish(events::Payload p, int worker = 0) { bus->publish(std::move(p), worker); }
    void finish() {
        publish(events::RunFinished{"completed"}, -1);
        log.reset();
    }
};

// Polls the workspace and draws frames until `done` (or about ten seconds).
inline bool frames_until(HeadlessContext& gui, App& app, Workspace* ws, const std::function<bool()>& done, int max_frames = 2000) {
    for (int i = 0; i < max_frames; ++i) {
        if (ws) ws->poll();
        gui.frame([&] { app.frame(); });
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

inline AgentSessionControl* session_control(App& app) { return dynamic_cast<AgentSessionControl*>(app.find_view("agent_session")); }
inline DiffViewerControl* diff_control(App& app) { return dynamic_cast<DiffViewerControl*>(app.find_view("diff_viewer")); }

} // namespace decomp::gui::test
