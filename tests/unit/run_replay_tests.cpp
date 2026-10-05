// A whole scripted run: the x86 fixture's functions on 4 workers with the real agent session, compiler
// and diff, and API responses from tests/replay/run/ (8 functions match, the others give up).

#include "agent/replay_transport.hpp"
#include "agent/runner.hpp"
#include "core/fs.hpp"
#include "events/bus.hpp"
#include "events/run_state.hpp"
#include "llvm_fixture.hpp"
#include "matching/unit_source.hpp"
#include "project/units.hpp"
#include "run/controller.hpp"
#include "run/selection.hpp"
#include "run/store.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <set>

using namespace decomp;

TEST_CASE("run: the x86 fixture on 4 workers with scripted API responses") {
    auto tools = test::find_llvm();
    if (!tools) {
        MESSAGE("clang-cl or lld-link not found; skipping");
        return;
    }
    // Built with the installed LLVM, so the scripts' sources (the fixture's own) match byte for byte.
    auto dir = fs::TempDir::create("decomp-run-replay").value();
    REQUIRE(test::build_fixture_program(Arch::x86, *tools, dir.path()));
    auto project = project::Project::init(dir.path() / "p", dir.path() / "basic.exe", std::nullopt, "clang-cl-x86").value();
    auto program = std::make_shared<const Program>(project.open_program().value());
    const auto vas = run::select_functions(*program, &project, {}).value();
    REQUIRE(vas.size() == 13);  // every function but the ExitProcess import thunk

    std::vector<run::QueueItem> items;
    for (u64 va : vas) {
        const Symbol* s = program->symbols().at(va);
        items.push_back(run::QueueItem{.va = va, .name = s->name, .display = s->display.empty() ? s->name : s->display,
                                       .difficulty = run::estimate_difficulty(*s)});
    }
    const std::string run_id = "2026-10-04T13-00-00-replay";
    events::EventBus bus(run_id);
    events::RunStateStore state;
    state.attach(bus);
    auto log = events::JsonlEventLog::open(project.runs_dir() / run_id / "events.jsonl").value();
    bus.subscribe([&](const events::Event& e) { log->write(e); });

    const auto scripts = test::source_dir() / "tests" / "replay" / "run";
    run::RunDeps deps;
    deps.program = [program] { return program; };
    deps.project = &project;
    deps.setup = test::clang_setup(Arch::x86, tools->clang_cl, dir.path() / "work");
    deps.transport = [scripts](const Symbol& fn) -> Result<std::shared_ptr<agent::HttpTransport>> {
        auto script = agent::find_replay_script(scripts, fn);
        if (!script) return make_error(ErrorCode::not_found, "no script for {}", fn.name);
        TRY_ASSIGN(auto replay, agent::ReplayTransport::load(*script));
        return std::shared_ptr<agent::HttpTransport>(std::move(replay));
    };
    run::RunController controller(std::move(deps), bus);
    run::RunOptions options;
    options.workers = 4;
    options.agent = agent::run_config_from(project::AgentSettings{});
    options.agent.client.api_key = "replay";
    options.project_name = "fixture";
    options.replay = true;
    REQUIRE(controller.start(run::RunStore::create(project.runs_dir(), run_id).value(), std::move(items), std::move(options)));
    controller.wait();
    CHECK(controller.status() == "completed");

    const auto snap = state.snapshot();
    CHECK(snap->status == "completed");
    CHECK(snap->finished == 13);
    CHECK(snap->matched == 8);
    std::set<int> workers;
    for (const auto& [id, s] : snap->sessions) workers.insert(s->worker);
    CHECK(workers.size() > 1);  // the work spread over several workers
    // Every scripted match landed in the project.
    const std::set<std::string> matched = {"add", "read_counter", "sum_array", "message", "scale", "mix", "Player::Hit", "other_value"};
    const auto units = project::load_units(project).value();
    for (const auto& name : matched) {
        const u64 va = *program->resolve(name);
        CAPTURE(name);
        CHECK(project.function_info(va).status == project::FunctionStatus::matched);
        CHECK(project::has_matched_source(project, *program->symbols().at(va), units));
    }
    CHECK(project.function_info(*program->resolve("dispatch")).status == project::FunctionStatus::gave_up);
    // The fixture's PDB gives the functions their units: the four workers composed their matches into
    // the two units' sources, which verify byte-exact as a whole.
    const auto verified = project::verify_unit_sources(project, *program, test::clang_setup(Arch::x86, tools->clang_cl, dir.path() / "verify")).value();
    REQUIRE(verified.size() == 2);
    CHECK(verified[0].unit.source == "src/basic.cpp");
    CHECK(verified[0].verification.functions.size() == 7);
    CHECK(verified[1].verification.functions.size() == 1);
    for (const auto& v : verified) CHECK(v.verification.all_byte_exact());
    CHECK(snap->files_written.size() == 8);
    CHECK(snap->compiles >= 16);  // a compile and a verification per match
    // The run's record: run.json, summary.json (= a replay of the log) and one transcript per session.
    const auto run_dir = project.runs_dir() / run_id;
    const Json run_json = parse_json(fs::read_text(run_dir / "run.json").value()).value();
    CHECK(run_json["status"] == "completed");
    CHECK(run_json["counts"]["matched"] == 8);
    const Json summary = parse_json(fs::read_text(run_dir / "summary.json").value()).value();
    CHECK(summary["functions_matched"] == 8);
    CHECK(run::run_summary(events::RunState::replay(events::read_event_log(run_dir / "events.jsonl").value()).data()) == summary);
    usize transcripts = 0;
    for (const auto& entry : std::filesystem::directory_iterator(run_dir / "sessions")) transcripts += entry.path().extension() == ".jsonl";
    CHECK(transcripts == 13);
    // The prompt cache warmed by the first session served the others (the stagger let it go first).
    CHECK(snap->usage.cache_read > 0);
}
