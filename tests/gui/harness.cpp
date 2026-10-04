#include "harness.hpp"

#include "gui/views/views.hpp"

#include <imgui_impl_null.h>
#include <imgui_internal.h>
#include <implot.h>

#include <chrono>
#include <cstdio>
#include <format>

namespace decomp::gui::test {

namespace {

void throwing_handler(const char* expr, const char* file, int line) {
    throw AssertionFailure(std::format("IM_ASSERT({}) failed at {}:{}", expr ? expr : "?", file ? file : "?", line));
}

// While a context is torn down after a failure, assertions are reported instead of thrown (no
// exceptions out of destructors).
void reporting_handler(const char* expr, const char* file, int line) {
    std::fprintf(stderr, "ImGui assertion during teardown: %s (%s:%d)\n", expr ? expr : "?", file ? file : "?", line);
}

} // namespace

HeadlessContext::HeadlessContext() {
    previous_handler_ = set_assert_handler(&throwing_handler);
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_);
    implot_ = ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // never touch the user's imgui.ini
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(kWidth, kHeight);
    io.ConfigDebugHighlightIdConflicts = true;
    ImGui_ImplNull_Init();
    detector_ = std::make_unique<debug::IdConflictDetector>(imgui_);
}

HeadlessContext::~HeadlessContext() {
    set_assert_handler(&reporting_handler);
    ImGui::SetCurrentContext(imgui_);  // another context may have become current meanwhile
    ImPlot::SetCurrentContext(implot_);
    detector_.reset();
    ImGui_ImplNull_Shutdown();
    ImPlot::DestroyContext(implot_);
    ImGui::DestroyContext(imgui_);
    set_assert_handler(previous_handler_);
}

void HeadlessContext::frame(const std::function<void()>& body) {
    ImGui::SetCurrentContext(imgui_);
    ImPlot::SetCurrentContext(implot_);
    ImGui_ImplNull_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2(kWidth, kHeight);  // the null platform sets 1920x1080
    ImGui::NewFrame();
    body();
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    ImGui_ImplNullRender_RenderDrawData(draw_data);
    // The null renderer creates and destroys textures but leaves updates pending; acknowledge them as a
    // real renderer would after uploading.
    if (draw_data->Textures)
        for (ImTextureData* tex : *draw_data->Textures)
            if (tex->Status == ImTextureStatus_WantUpdates) tex->SetStatus(ImTextureStatus_OK);
}

void HeadlessContext::frames(int count, const std::function<void()>& body) {
    for (int i = 0; i < count; ++i) frame(body);
}

void HeadlessContext::key(ImGuiKeyChord chord, bool down) {
    ImGuiIO& io = ImGui::GetIO();
    for (ImGuiKey mod : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt, ImGuiMod_Super})
        if (chord & mod) io.AddKeyEvent(mod, down);
    io.AddKeyEvent(static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_), down);
}

void HeadlessContext::tap(ImGuiKeyChord chord, const std::function<void()>& body) {
    key(chord, true);
    frame(body);
    key(chord, false);
    frame(body);
}

void HeadlessContext::type(const char* text, const std::function<void()>& body) {
    ImGui::GetIO().AddInputCharactersUTF8(text);
    frame(body);
}

int HeadlessContext::id_conflicts() const {
    return static_cast<int>(detector_->conflicts().size()) + imgui_->DebugDrawIdConflictsCount;
}

std::string HeadlessContext::describe_conflicts() const { return detector_->describe(); }

namespace {

using namespace decomp::events;

// Events with consecutive sequence numbers, one second apart, folded like a live run.
class Script {
public:
    template <class P>
    void add(P payload, int worker = -1) {
        Event e;
        e.seq = ++seq_;
        e.time = std::chrono::system_clock::time_point(std::chrono::seconds(1791079713 + static_cast<long long>(seq_)));
        e.run = "2026-10-04T02-08-33-test";
        e.worker = worker;
        e.payload = std::move(payload);
        events_.push_back(std::move(e));
    }

    void run_started(int workers, std::vector<std::string> functions) {
        RunStarted p;
        p.project = "/projects/demo";
        p.model = "claude-opus-4-1";
        p.effort = "high";
        p.workers = workers;
        p.functions = std::move(functions);
        add(p);
    }
    void session(int worker, const std::string& id, const std::string& function, const std::string& display, u64 va) {
        SessionStarted p;
        p.session = id;
        p.function = function;
        p.display = display;
        p.va = va;
        add(p, worker);
    }
    void turn(int worker, const std::string& id, int n) {
        TurnStarted p;
        p.session = id;
        p.turn = n;
        add(p, worker);
    }
    void stream(int worker, const std::string& id, const std::string& kind, const std::string& text) {
        StreamDelta p;
        p.session = id;
        p.kind = kind;
        p.text = text;
        add(p, worker);
    }
    void turn_done(int worker, const std::string& id, int n, const std::string& stop_reason, double cost) {
        TurnFinished p;
        p.session = id;
        p.turn = n;
        p.stop_reason = stop_reason;
        p.usage = {1200, 350, 800, 5200};
        p.cost_usd = cost;
        p.latency_ms = 2300;
        add(p, worker);
    }
    void tool(int worker, const std::string& id, const std::string& call, const std::string& name, int turn, const std::string& summary,
              bool error = false) {
        ToolCallStarted s;
        s.session = id;
        s.id = call;
        s.tool = name;
        s.turn = turn;
        s.input = Json{{"source", "int add(int a, int b) { return a + b; }"}};
        add(s, worker);
        ToolCallFinished f;
        f.session = id;
        f.id = call;
        f.tool = name;
        f.is_error = error;
        f.summary = summary;
        f.duration_ms = 900;
        add(f, worker);
    }
    void compile(int worker, const std::string& id, bool ok, int errors) {
        CompileFinished p;
        p.session = id;
        p.ok = ok;
        p.cached = false;
        p.duration_ms = 850;
        p.errors = errors;
        add(p, worker);
    }
    void diff(int worker, const std::string& id, double percent, bool exact, const std::string& summary) {
        DiffComputed p;
        p.session = id;
        p.match_percent = percent;
        p.byte_exact = exact;
        p.summary = summary;
        add(p, worker);
    }
    void finished(int worker, const std::string& id, const std::string& outcome, const std::string& detail, double best) {
        SessionFinished p;
        p.session = id;
        p.outcome = outcome;
        p.detail = detail;
        p.best_match = best;
        p.turns = 3;
        p.cost_usd = 0.42;
        add(p, worker);
    }
    void status(const std::string& function, u64 va, const std::string& status) {
        StatusChanged p;
        p.function = function;
        p.va = va;
        p.status = status;
        add(p);
    }
    void log(const std::string& level, const std::string& message) {
        LogLine p;
        p.level = level;
        p.message = message;
        add(p);
    }

    std::shared_ptr<const RunStateData> fold() const { return std::make_shared<const RunStateData>(RunState::replay(events_).data()); }

private:
    std::vector<Event> events_;
    u64 seq_ = 0;
};

} // namespace

std::shared_ptr<const events::RunStateData> empty_snapshot() { return std::make_shared<const events::RunStateData>(); }

std::shared_ptr<const events::RunStateData> mid_run_snapshot() {
    Script s;
    s.run_started(4, {"?add@@YAHHH@Z", "?sub@@YAHHH@Z", "?mul@@YAHHH@Z", "?div@@YAHHH@Z", "_main"});
    s.session(0, "run-401060", "?add@@YAHHH@Z", "int __cdecl add(int, int)", 0x401060);
    s.session(1, "run-401080", "?sub@@YAHHH@Z", "int __cdecl sub(int, int)", 0x401080);
    s.session(2, "run-4010a0", "?mul@@YAHHH@Z", "int __cdecl mul(int, int)", 0x4010a0);
    s.turn(0, "run-401060", 1);
    s.stream(0, "run-401060", "thinking", "The prologue saves ebp; the body adds the two arguments.");
    s.stream(0, "run-401060", "text", "I'll start from a direct translation.");
    s.turn_done(0, "run-401060", 1, "tool_use", 0.031);
    s.tool(0, "run-401060", "toolu_01", "compile_and_diff", 1, "87.5% (3 rows differ)");
    s.compile(0, "run-401060", true, 0);
    s.diff(0, "run-401060", 87.5, false, "3 rows differ: operand x2, opcode x1");
    s.turn(1, "run-401080", 1);
    {
        events::Retry r;
        r.session = "run-401080";
        r.attempt = 1;
        r.error = "overloaded (529)";
        r.delay_ms = 2000;
        s.add(r, 1);
    }
    s.turn(2, "run-4010a0", 1);
    s.tool(2, "run-4010a0", "toolu_02", "compile_and_diff", 1, "100% (byte-exact)");
    s.compile(2, "run-4010a0", true, 0);
    s.diff(2, "run-4010a0", 100.0, true, "byte-exact");
    s.turn_done(2, "run-4010a0", 1, "tool_use", 0.044);
    s.tool(2, "run-4010a0", "toolu_03", "submit_result", 2, "verified: byte-exact");
    s.finished(2, "run-4010a0", "matched", "", 100.0);
    s.status("?mul@@YAHHH@Z", 0x4010a0, "matched");
    {
        events::FileWritten f;
        f.path = "src/functions/mul_4010a0.cpp";
        f.reason = "verified match";
        s.add(f);
    }
    {
        events::Guidance g;
        g.session = "run-401060";
        g.text = "Try swapping the operands of the add.";
        s.add(g, 0);
    }
    s.turn(0, "run-401060", 2);
    s.stream(0, "run-401060", "text", "Swapping the operands as suggested.");
    s.log("warn", "compile cache miss for ?add@@YAHHH@Z");
    return s.fold();
}

std::shared_ptr<const events::RunStateData> error_snapshot() {
    Script s;
    s.run_started(2, {"?add@@YAHHH@Z", "?parse@@YAHPBD@Z"});
    s.session(0, "run-401060", "?add@@YAHHH@Z", "int __cdecl add(int, int)", 0x401060);
    s.session(1, "run-401200", "?parse@@YAHPBD@Z", "int __cdecl parse(char const *)", 0x401200);
    s.turn(0, "run-401060", 1);
    for (int attempt = 1; attempt <= 3; ++attempt) {
        events::Retry r;
        r.session = "run-401060";
        r.attempt = attempt;
        r.error = "HTTP 401: invalid x-api-key";
        r.delay_ms = 1000 * attempt;
        s.add(r, 0);
    }
    s.turn(1, "run-401200", 1);
    s.tool(1, "run-401200", "toolu_09", "compile_and_diff", 1, "compile failed: error C2065: 'buf': undeclared identifier", true);
    s.compile(1, "run-401200", false, 3);
    {
        events::Refusal r;
        r.session = "run-401200";
        r.category = "cyber";
        r.explanation = "The request was declined.";
        s.add(r, 1);
    }
    s.finished(1, "run-401200", "refused", "request declined (category: cyber)", 0.0);
    s.finished(0, "run-401060", "error", "API error 401 after 3 retries: invalid x-api-key", 0.0);
    s.log("error", "authentication failed (401): check ANTHROPIC_API_KEY");
    s.log("warn", "toolchain msvc6 health check failed: CL.EXE not found");
    {
        events::RunFinished f;
        f.status = "error";
        s.add(f);
    }
    return s.fold();
}

std::shared_ptr<const events::RunStateData> snapshot_with_status(const std::string& status) {
    auto data = std::make_shared<events::RunStateData>(*mid_run_snapshot());
    data->status = status;
    return data;
}

AppServices make_services(std::shared_ptr<const events::RunStateData> snapshot, std::shared_ptr<RunCommands> commands, ProjectInfo project) {
    AppServices services;
    services.snapshot = [snapshot] { return snapshot; };
    services.commands = commands ? std::move(commands) : std::make_shared<RunCommands>();
    services.project = [project] { return project; };
    return services;
}

std::vector<std::string> all_view_ids() {
    std::vector<std::string> ids;
    for (const auto& view : make_all_views()) ids.emplace_back(view->id());
    return ids;
}

} // namespace decomp::gui::test
