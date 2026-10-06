// The Dashboard, Function browser, Inspector, Binary explorer, Symbols, Units, Types and Relink views against a
// real project (the x86 fixture): with no run, a live run, a past run; their background jobs run to the end.
// The Relink view with a relink's result. Plus the Function browser's table with 100,000 rows (frame time)
// and the Dashboard's treemap of 100,000 functions.

#include "analysis/layout.hpp"
#include "core/bytes.hpp"
#include "core/fs.hpp"
#include "formats/pdb.hpp"
#include "gui/views/function_table_widget.hpp"
#include "gui/views/treemap_widget.hpp"
#include "gui/workspace.hpp"
#include "harness.hpp"
#include "project/relink.hpp"
#include "project/units.hpp"
#include "relink/compare.hpp"
#include "test_util.hpp"
#include "viewmodel/browser.hpp"
#include "viewmodel/treemap.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <numeric>
#include <thread>

using namespace decomp;
using namespace decomp::gui;
using namespace decomp::gui::test;
using namespace std::chrono_literals;

namespace {

const std::vector<std::string> kMyViews = {"dashboard", "function_browser", "inspector", "binary_explorer", "symbols", "units", "types", "relink"};

// Sessions that finish at once (every other one matches) unless held.
struct HeldSessions {
    std::mutex mutex;
    std::condition_variable cv;
    bool hold = false;
    std::atomic<int> started{0};

    void release() {
        std::lock_guard lock(mutex);
        hold = false;
        cv.notify_all();
    }

    run::SessionFn fn() {
        return [this](const run::SessionRequest& r, events::EventBus& bus) {
            bus.publish(events::SessionStarted{r.session_id, std::format("f_{:x}", r.va), std::format("f_{:x}", r.va), r.va, ""}, r.worker);
            bus.publish(events::CompileFinished{r.session_id, true, false, 5, 0}, r.worker);
            bus.publish(events::DiffComputed{r.session_id, 50, false, "50%"}, r.worker);
            ++started;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return !hold || r.control->stop_requested() || r.control->abort_requested(); });
            }
            agent::FunctionRunResult result;
            if (r.control->abort_requested()) result.outcome = "aborted";
            else if (r.control->stop_requested()) result.outcome = "stopped";
            else result.outcome = (r.va / 0x10) % 2 ? "gave_up" : "matched";
            result.turns = 1;
            bus.publish(events::TurnFinished{r.session_id, 1, "end_turn", events::TokenUsage{100, 10, 0, 0}, 0.01, 5}, r.worker);
            bus.publish(events::SessionFinished{r.session_id, result.outcome, "", 0, 1, 0.01}, r.worker);
            return result;
        };
    }
};

struct ProjectFixture {
    fs::TempDir dir = fs::TempDir::create("decomp-gui-views").value();
    std::filesystem::path root = dir.path() / "project";
    HeldSessions sessions;
    std::unique_ptr<Workspace> workspace;

    ProjectFixture() {
        REQUIRE(project::Project::init(root, decomp::test::fixture("x86/basic.exe"), std::nullopt, "clang-cl-x86"));
        Workspace::Options options;
        options.stagger = 0ms;
        options.session_override = sessions.fn();
        workspace = std::make_unique<Workspace>(std::move(options));
        REQUIRE(workspace->open_project(root));
        workspace->wait_loaded();
        REQUIRE(workspace->project_state().phase == ProjectPhase::open);
    }

    u64 va(std::string_view name) const { return *workspace->program()->resolve(name); }

    bool until(const std::function<bool()>& done) {
        for (int i = 0; i < 2000; ++i) {
            workspace->poll();
            if (done()) return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }
};

// Renders until the views' background jobs are done (three quiet frames in a row).
void settle(HeadlessContext& gui, App& app) {
    int quiet = 0;
    for (int i = 0; i < 1000 && quiet < 3; ++i) {
        gui.frame([&] { app.frame(); });
        quiet = app.jobs().pending() == 0 ? quiet + 1 : 0;
        if (quiet == 0) std::this_thread::sleep_for(2ms);
    }
    CHECK(quiet >= 3);
}

void render_my_views(HeadlessContext& gui, App& app, ProjectFixture& fx, const char* state) {
    CAPTURE(state);
    for (const auto& view : kMyViews) {
        CAPTURE(view);
        REQUIRE(app.focus_view(view));
        settle(gui, app);
        CHECK(app.view_visible(view));
    }
    // The Inspector's tabs and the Binary explorer's hex view and strings, with something selected.
    for (const char* anchor : {"", "xrefs", "attempts", "notes", "history"}) {
        app.context().open("inspector", {.va = fx.va("dispatch"), .anchor = anchor});
        settle(gui, app);
    }
    app.context().open("binary_explorer", {.anchor = "0x401060"});
    settle(gui, app);
    app.context().open("binary_explorer", {.anchor = "string:0x402010"});
    settle(gui, app);
    app.context().open("symbols", {.va = fx.va("add")});
    settle(gui, app);
    app.context().open("symbols", {.anchor = "agent_edits"});
    settle(gui, app);
    // Units, with a unit selected: its functions and actions.
    for (const char* unit : {"basic.obj", "other.obj", "* Linker *"}) {
        app.context().open("units", {.anchor = unit});
        settle(gui, app);
    }
    // Types, with the PDB's Player selected, and a name no source has.
    for (const char* type : {"Player", "Missing"}) {
        app.context().open("types", {.anchor = type});
        settle(gui, app);
    }
    // Relink before any relink: the units and their tabs.
    for (const char* anchor : {"basic.obj", "tab:comparison", "tab:link"}) {
        app.context().open("relink", {.anchor = anchor});
        settle(gui, app);
    }
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}

} // namespace

TEST_CASE("the project views render with a project and no run, a live run and a past run") {
    ProjectFixture fx;
    // A project header for the Types view: Player, with another type for speed than the PDB's.
    REQUIRE(fs::write_text(fx.root / "include" / "game.h", "#pragma once\nstruct Player { int hp; double speed; };\n"));
    // History for the views to show: a symbol the agent renamed, notes and an attempt.
    auto* project = fx.workspace->project();
    REQUIRE(project->set_symbol(project::SymbolEdit{.va = fx.va("helper"), .name = std::string("helper_renamed")},
                                project::ChangeOrigin{SymbolSource::agent, "s-1", "binding"}));
    fx.workspace->reload_symbols();
    const Symbol& dispatch = *fx.workspace->program()->symbols().at(fx.va("dispatch"));
    REQUIRE(project->append_note(dispatch, "the switch has six cases"));
    REQUIRE(project->record_attempt(dispatch, Json{{"attempt", 1}, {"session", "s-0"}, {"time", "2026-10-04T10:00:00Z"}, {"compiled", true},
                                                   {"match_percent", 75.0}, {"byte_exact", false}, {"summary", "75%"}}));

    HeadlessContext gui;
    Settings settings;
    App app(fx.workspace->services(), settings);
    render_my_views(gui, app, fx, "no run");

    fx.sessions.hold = true;
    RunRequest request;
    request.workers = 2;
    auto id = fx.workspace->start_run(request);
    REQUIRE(id);
    REQUIRE(fx.until([&] { return fx.sessions.started >= 2; }));
    render_my_views(gui, app, fx, "live run");
    fx.sessions.release();
    REQUIRE(fx.until([&] { return !fx.workspace->run_live(); }));
    render_my_views(gui, app, fx, "finished run");

    fx.workspace->close_run();
    REQUIRE(fx.workspace->open_run(*id));
    fx.workspace->wait_loaded();
    render_my_views(gui, app, fx, "past run");
}

TEST_CASE("the Relink view shows a relink: its units, the first difference and the bytes there, a unit's check, the link") {
    ProjectFixture fx;
    auto* project = fx.workspace->project();
    auto program = fx.workspace->program();
    const auto& image = program->image();
    // A relink as relink_project() writes it, of an image with one byte of add() changed and another link time.
    auto relinked = std::vector<std::byte>(image.data().begin(), image.data().end());
    const u32 rva = static_cast<u32>(fx.va("add") - image.image_base() + 2);
    const u32 offset = image.rva_to_offset(rva).value();
    relinked[offset] = static_cast<std::byte>(static_cast<u8>(relinked[offset]) ^ 0xFF);
    write_le<u32>(relinked, image.pe_offset() + 8, 0x12345678);
    project::RelinkResult result;
    result.time = "2026-10-06T01:00:00Z";
    const auto units = project::load_units(*project).value();
    for (const auto& unit : units) {
        project::UnitLink link;
        link.unit = unit;
        link.mode = unit.kind == UnitKind::code ? project::LinkMode::split : project::LinkMode::linker;
        link.reason = unit.kind == UnitKind::code ? "no source" : "made by the linker";
        if (unit.name == "basic.obj") {
            link.mode = project::LinkMode::source;
            link.reason = "from its source on request";
            project::UnitSourceCheck check;
            check.unit = unit;
            check.functions = {fx.va("add")};
            check.missing_functions = {fx.va("dispatch")};
            check.check = matching::UnitCheckResult{};
            matching::PlacedSection text;
            text.name = ".text$mn";
            text.symbol = "?add@@YAHHH@Z";
            text.size = 16;
            text.rva = rva - 2;
            text.state = matching::PlacementState::differs;
            text.first_difference = 2;
            text.differing_bytes = 1;
            check.check->sections.push_back(text);
            matching::PlacedSection folded = text;
            folded.symbol = "?add_again@@YAHHH@Z";
            folded.state = matching::PlacementState::discarded;
            folded.folded_into = "?add@@YAHHH@Z";
            check.check->sections.push_back(folded);
            Contribution missing;
            missing.rva = 0x3000;
            missing.size = 4;
            missing.section = ".data";
            check.check->missing.push_back(missing);
            check.check->problems.push_back("a gap before .text$mn ?dispatch@@YAHHH@Z");
            check.compile.output = "basic.cpp(3): warning: something";
            link.check = std::move(check);
        }
        result.units.push_back(std::move(link));
    }
    result.notes = {"a note"};
    result.libraries = {"libs/kernel32.lib"};
    result.linker = relink::linker_fit(relink::OriginalLinker{relink::LinkerKind::lld, 18, 1, 3}, relink::LinkerKind::lld, "LLD 20.1.8");
    result.link.ok = true;
    result.link.command = {"lld-link", "/nologo", "/out:basic.exe"};
    result.link.output = "lld-link: warning: something";
    result.image = ".decomp/relink/out/basic.exe";
    REQUIRE(fs::create_directories(fx.root / ".decomp" / "relink" / "out"));
    REQUIRE(fs::write_file(fx.root / result.image, relinked));
    const auto reader = pdb::Reader::load(*program->pdb_path()).value();
    auto compared = relinked;
    result.comparison = relink::compare_images(image, compared, layout_from_pdb(reader, image), program->symbols());
    REQUIRE(fs::write_text(project::relink_dir(*project) / "result.json", project::to_json(result).dump(2)));

    HeadlessContext gui;
    Settings settings;
    App app(fx.workspace->services(), settings);
    REQUIRE(app.focus_view("relink"));
    settle(gui, app);
    CHECK(app.view_visible("relink"));
    for (const char* anchor : {"tab:comparison", "basic.obj", "other.obj", "* Linker *", "no_such.obj", "tab:link"}) {
        CAPTURE(anchor);
        app.context().open("relink", {.anchor = anchor});
        settle(gui, app);
    }
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}

TEST_CASE("the Dashboard's buckets open the Function browser filtered, and the filter persists per project") {
    ProjectFixture fx;
    REQUIRE(fx.workspace->project()->update_function(fx.va("add"), project::FunctionInfo{project::FunctionStatus::matched, 100, 1, 0.1}));
    HeadlessContext gui;
    Settings settings;
    {
        App app(fx.workspace->services(), settings);
        settle(gui, app);
        vm::FunctionFilter filter;
        filter.statuses = {project::FunctionStatus::matched};
        app.context().open("function_browser", {.anchor = vm::browser_anchor(filter)});
        settle(gui, app);
        CHECK(app.view_visible("function_browser"));
        const Json& state = settings.project_state(fx.root).views["function_browser"];
        CHECK(state["filter"]["statuses"] == Json::array({"matched"}));
        CHECK(state["columns"].is_array());
        CHECK(state["sort"].is_array());
        CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
    }
    // A new App with the same settings starts from the saved state.
    HeadlessContext gui2;
    App app2(fx.workspace->services(), settings);
    settle(gui2, app2);
    CHECK(settings.project_state(fx.root).views["function_browser"]["filter"]["statuses"] == Json::array({"matched"}));
    // The Dashboard's export and the browser's selection do not need a run.
    app2.context().open("function_browser", {.va = fx.va("add")});
    settle(gui2, app2);
    CHECK(app2.context().selection.function_va == fx.va("add"));
}

namespace {

std::vector<vm::FunctionRow> synthetic_rows(usize count) {
    std::vector<vm::FunctionRow> rows(count);
    u64 va = 0x10000000;
    u32 state = 12345;
    for (usize i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        vm::FunctionRow& r = rows[i];
        r.va = va;
        r.size = 4 + (state >> 20) % 2000;
        va += r.size;
        r.name = std::format("?function_{}@Namespace{}@@QAEXH@Z", i, i % 97);
        r.display = std::format("void Namespace{}::function_{}(int)", i % 97, i);
        r.status = static_cast<project::FunctionStatus>(state % 8);
        r.stored_status = r.status;
        r.best_match = static_cast<double>(state % 100);
        r.attempts = static_cast<int>(state % 9);
        r.cost_usd = (state % 1000) / 100.0;
        if (i % 2 == 0) {
            r.callers = state % 13;
            r.callees = state % 7;
            r.blocks = 1 + state % 40;
            r.loops = state % 3;
            r.unknown_callees = state % 2;
            r.difficulty = 4.0 + (state % 200) / 10.0;
        }
    }
    return rows;
}

double elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

} // namespace

TEST_CASE("the Function browser's table stays responsive with 100,000 functions") {
    HeadlessContext gui;
    Settings settings;
    App app(make_services(nullptr), settings);
    const auto rows = synthetic_rows(100'000);
    std::vector<u32> order(rows.size());
    std::iota(order.begin(), order.end(), u32{0});
    FunctionTable table;
    table.set_sort_keys({{vm::Column::best_match, true}, {vm::Column::address, false}});
    FunctionTable::Events events;
    auto draw = [&] {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(HeadlessContext::kWidth, HeadlessContext::kHeight));
        ImGui::Begin("Functions");
        events = table.draw(app.context(), rows, order);
        ImGui::End();
    };
    gui.frames(3, draw);  // the table's first frames set up its columns
    CHECK(table.sort_keys().size() == 2);
    CHECK(table.sort_keys()[0].column == vm::Column::best_match);

    auto measure = [&](const char* what, int frames) {
        double total = 0, worst = 0;
        for (int i = 0; i < frames; ++i) {
            const auto start = std::chrono::steady_clock::now();
            gui.frame(draw);
            const double ms = elapsed_ms(start);
            total += ms;
            worst = std::max(worst, ms);
        }
        const double mean = total / frames;
        MESSAGE("100,000 functions, " << std::string(what) << ": " << mean << " ms per frame on average, " << worst << " ms at worst");
#ifdef NDEBUG
        // Budget: 8 ms per frame; only a regression of more than five times fails.
        CHECK(mean < 40.0);
#endif
    };
    measure("at the top", 20);
    // A selected function in the middle of the table, brought into view.
    table.select_only(rows[50'000].va);
    table.scroll_to(rows[50'000].va);
    gui.frame(draw);
    measure("in the middle", 20);
    CHECK(table.selected_in_order(rows, order) == std::vector<u64>{rows[50'000].va});
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}

TEST_CASE("the Dashboard's treemap draws 100,000 functions") {
    HeadlessContext gui;
    Settings settings;
    App app(make_services(nullptr), settings);
    const auto rows = synthetic_rows(100'000);
    struct Text : BinaryImage {
        std::vector<ImageSection> sections{ImageSection{".text", 0x10000000, 0x10000000, 0x10000000, true, false, true}};
        Arch arch() const override { return Arch::x86; }
        u64 image_base() const override { return 0x0fff0000; }
        u64 image_size() const override { return 0x10010000; }
        u64 entry_point() const override { return 0; }
        const std::vector<ImageSection>& image_sections() const override { return sections; }
        std::optional<ByteSpan> view(u64, usize) const override { return std::nullopt; }
        bool has_relocations() const override { return false; }
        bool is_relocated(u64) const override { return false; }
    } image;
    auto start = std::chrono::steady_clock::now();
    const vm::Treemap map = vm::build_text_treemap(rows, image, vm::Rect{0, 0, 1200, 700}, vm::TreemapOptions{2, 18});
    const double layout_ms = elapsed_ms(start);
    REQUIRE(map.cells.size() == rows.size());
    TreemapWidget widget;
    TreemapEvents events;
    TreemapColoring coloring = TreemapColoring::status;
    auto draw = [&] {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1300, 800));
        ImGui::Begin("Treemap");
        events = widget.draw(app.context(), map, rows, ImVec2(1200, 700), coloring, rows[500].va);
        ImGui::End();
    };
    start = std::chrono::steady_clock::now();
    CHECK_NOTHROW(gui.frame(draw));
    const double draw_ms = elapsed_ms(start);
    coloring = TreemapColoring::best_match;
    CHECK_NOTHROW(gui.frame(draw));
    MESSAGE("treemap of 100,000 functions: layout " << layout_ms << " ms, first frame " << draw_ms << " ms");
    CHECK_FALSE(events.clicked);
    CHECK(describe_cell(map.cells[*map.find(rows[500].va)], rows).find("function_500") != std::string::npos);
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}

TEST_CASE("the palette finds the project's functions, symbols and strings") {
    ProjectFixture fx;
    HeadlessContext gui;
    Settings settings;
    App app(fx.workspace->services(), settings);
    settle(gui, app);
    // The searches run in the background: ask again until the item shows up.
    auto find = [&](const std::string& input, const std::function<bool(const PaletteItem&)>& match) -> std::optional<PaletteItem> {
        for (int i = 0; i < 1000; ++i) {
            for (auto& item : app.palette().items(input, app.context()))
                if (match(item)) return item;
            std::this_thread::sleep_for(2ms);
        }
        return std::nullopt;
    };
    auto add = find("add", [](const PaletteItem& item) { return item.label == "int __cdecl add(int, int)"; });
    REQUIRE(add);
    CHECK(add->detail == "function 0x00401060");
    add->run();
    gui.frames(3, [&] { app.frame(); });
    CHECK(app.context().selection.function_va == 0x401060u);
    CHECK(app.view_visible("inspector"));

    auto counter = find("g_counter", [](const PaletteItem& item) { return item.detail == "data 0x00403000"; });
    REQUIRE(counter);
    counter->run();
    gui.frames(3, [&] { app.frame(); });
    CHECK(app.view_visible("binary_explorer"));

    auto hello = find("\"HELLO", [](const PaletteItem& item) { return item.label == "\"hello world\""; });
    REQUIRE(hello);
    CHECK(hello->detail == "string 0x00402010");
    hello->run();
    settle(gui, app);
    CHECK(app.view_visible("binary_explorer"));
    CHECK_MESSAGE(gui.id_conflicts() == 0, gui.describe_conflicts());
}
