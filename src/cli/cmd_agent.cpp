#include "agent/replay_transport.hpp"
#include "agent/runner.hpp"
#include "cli/common.hpp"
#include "cli/live_run.hpp"
#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "events/bus.hpp"
#include "events/progress.hpp"
#include "project/project.hpp"
#include "run/controller.hpp"
#include "run/queue.hpp"
#include "run/store.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <thread>

namespace decomp::cli {

namespace {

struct AgentArgs {
    std::string function, binary, pdb, toolchain, model, effort, replay, log_dir;
    std::vector<std::string> flags, guidance, policies;
    int max_turns = 0;
    double budget_usd = -1;
    long long max_tokens = -1;
    int max_minutes = -1;
    bool no_fallbacks = false, progress = false, no_progress = false, interactive = false;
};

// --interactive: lines typed on stdin steer the session (guidance, or :pause, :resume, :stop, :abort). The
// reader blocks on stdin, so it is detached and shares the run.
void start_stdin_supervisor(std::shared_ptr<LiveRun> live) {
    std::thread([live = std::move(live)] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!live->active) return;
            auto& c = *live->controller;
            const std::string cmd(trim(line));
            if (cmd.empty()) continue;
            if (cmd == ":pause") {
                c.pause();
                log::info("pausing after the current turn (:resume to continue)");
            } else if (cmd == ":resume") {
                c.unpause();
                log::info("resumed");
            } else if (cmd == ":stop") {
                c.stop();
                log::info("stopping after the current turn");
            } else if (cmd == ":abort") {
                c.abort();
                log::info("aborting");
            } else if (cmd == ":help") {
                log::info("type guidance for the agent, or :pause, :resume, :stop, :abort");
            } else {
                const auto sessions = c.running_sessions();
                const std::optional<u64> id = sessions.empty() ? std::nullopt : c.inject(sessions.front(), cmd);
                if (id) log::info("guidance queued for the next turn");
                else log::warn("no session is running: the guidance was not sent");
            }
        }
    }).detach();
}

int exit_code(bool matched, std::string_view outcome) {
    if (matched) return 0;
    if (outcome == "refused") return 3;
    if (outcome == "error" || outcome == "aborted") return 1;
    return 2;
}

// The session's result as the agent returned it (its sources), for the summary.
struct SessionOutcome {
    std::mutex mutex;
    std::optional<agent::FunctionRunResult> result;
};

// A one-function run on the RunController: the same run directory, events, approvals, live limits and
// resumability as `decomp run`, with one worker and no stagger.
Result<int> run_agent(const GlobalOptions& g, const AgentArgs& a) {
    std::optional<project::Project> project;
    if (a.binary.empty()) {
        TRY_ASSIGN(auto p, project::Project::find(g.project));
        project = std::move(p);
    }
    // One live run per project: a second agent or run would interleave writes to the same functions.
    std::optional<FileLock> active_run;
    if (project) {
        TRY_ASSIGN(auto lock, project->try_lock_active_run());
        if (!lock) return make_error(ErrorCode::invalid_argument, "another agent run is active in this project");
        active_run = std::move(lock);
    }
    std::optional<std::filesystem::path> pdb_path;
    if (!a.pdb.empty()) pdb_path = fs::from_utf8(a.pdb);
    TRY_ASSIGN(auto opened_program, project ? project->open_program() : Program::open(fs::from_utf8(a.binary), pdb_path));
    const auto program = std::make_shared<const Program>(std::move(opened_program));
    TRY_ASSIGN(u64 va, resolve_function(*program, a.function));
    // Fail before spending anything on an address that is not a function.
    if (auto extent = program->function_extent(va); !extent)
        return make_error(ErrorCode::invalid_argument, "{:#x} is not a function: {}", va, extent.error().message);
    TRY_ASSIGN(auto setup, make_match_setup(g, a.toolchain, a.flags));

    project::AgentSettings settings = project ? project->config().agent : project::AgentSettings{};
    if (!a.model.empty()) settings.model = a.model;
    if (!a.effort.empty()) settings.effort = a.effort;
    if (a.max_turns > 0) settings.max_turns = a.max_turns;
    if (a.budget_usd >= 0) settings.max_usd_per_function = a.budget_usd;
    if (a.max_tokens >= 0) settings.max_tokens_per_function = a.max_tokens;
    if (a.max_minutes >= 0) settings.max_minutes_per_function = a.max_minutes;
    if (a.no_fallbacks) settings.fallbacks = false;
    // The agent always sends adaptive thinking and an effort level; models without them answer 400.
    if (settings.model.starts_with("claude-haiku"))
        return make_error(ErrorCode::invalid_argument,
                          "model '{}' does not support the adaptive thinking and effort settings the agent sends; use claude-opus-5-5 "
                          "or claude-sonnet-5-5",
                          settings.model);
    agent::AgentRunConfig config = agent::run_config_from(settings);
    config.guidance = a.guidance;
    TRY_ASSIGN(auto approval_policies, agent::resolve_approval_policies(settings.approvals, a.policies, false));

    run::TransportFactory transport;
    if (!a.replay.empty()) {
        const auto script = fs::from_utf8(a.replay);
        // A missing or malformed script fails before the run starts.
        if (auto checked = agent::ReplayTransport::load(script); !checked) return std::unexpected(std::move(checked.error()));
        transport = [script](const Symbol&) -> Result<std::shared_ptr<agent::HttpTransport>> {
            TRY_ASSIGN(auto replay, agent::ReplayTransport::load(script));
            return std::shared_ptr<agent::HttpTransport>(std::move(replay));
        };
        config.client.api_key = "replay";  // a scripted run never sends the real key anywhere
    } else if (trim(config.client.api_key).empty()) {
        return make_error(ErrorCode::invalid_argument, "ANTHROPIC_API_KEY is not set (export it, or pass --replay <file> for a scripted run)");
    } else {
        config.transport = std::shared_ptr<agent::HttpTransport>(agent::make_default_transport());
    }

    // The run's directory: --log-dir, the project's runs, or for a binary without a project a temporary
    // one that is removed afterwards (nothing is persisted).
    std::filesystem::path runs_dir;
    bool temporary = false;
    if (!a.log_dir.empty()) {
        runs_dir = fs::from_utf8(a.log_dir);
    } else if (project) {
        runs_dir = project->runs_dir();
    } else {
        std::error_code ec;
        runs_dir = std::filesystem::temp_directory_path(ec) / "decomp-agent";
        if (ec) return make_error(ErrorCode::io, "no temporary directory for the run: {}", ec.message());
        temporary = true;
    }
    TRY_ASSIGN(auto store, run::RunStore::create(runs_dir, events::new_run_id()));
    const std::filesystem::path run_dir = store.dir();
    const std::string run_id = store.id();

    auto live = std::make_shared<LiveRun>(run_id);
    if (!temporary) {
        TRY_ASSIGN(live->log, events::JsonlEventLog::open(store.events_path()));
        live->bus.subscribe([log = live->log.get()](const events::Event& e) { log->write(e); });
    }
    const RunLogForwarder forwarder(live);
    const bool show_progress = a.progress || (!a.no_progress && !g.quiet && !g.json);
    events::ProgressRenderer renderer(is_tty(stderr));
    if (show_progress) renderer.attach(live->bus);

    auto session = std::make_shared<SessionOutcome>();
    run::RunDeps deps;
    deps.program = [program] { return program; };
    deps.project = project ? &*project : nullptr;
    deps.setup = setup;
    deps.transport = transport;
    deps.run_session = [session](const run::SessionRequest& r, events::EventBus& bus) {
        auto result = agent::run_function(*r.program, r.project, r.setup, r.va, r.config, bus, r.transcript, r.control, r.worker);
        std::lock_guard lock(session->mutex);
        session->result = result;
        return result;
    };
    live->controller = std::make_unique<run::RunController>(std::move(deps), live->bus);

    const Symbol* sym = program->symbols().at(va);
    const FunctionAnalysis analysis = analyze_functions(*program, std::span<const u64>(&va, 1));
    run::QueueItem item = run::make_queue_items(*program, std::span<const u64>(&va, 1), &analysis, false).front();
    run::RunOptions options;
    options.workers = 1;
    options.agent = config;
    options.policies = approval_policies;
    options.stagger_timeout = std::chrono::milliseconds(0);
    options.project_name = project ? fs::to_utf8(project->root().filename()) : fs::to_utf8(fs::from_utf8(a.binary).filename());
    options.replay = !a.replay.empty();
    options.selection = Json{{"functions", Json::array({a.function})}, {"from", "agent"}};
    TRY(live->controller->start(std::move(store), {std::move(item)}, std::move(options)));
    {
        // Ctrl+C: the first stops after the current turn, the second aborts the request in flight.
        InterruptWatcher watcher([live](int presses) {
            if (presses == 1) {
                log::warn("stopping after the current turn (Ctrl+C again to abort now)");
                live->controller->stop();
            } else {
                log::warn("aborting");
                live->controller->abort();
            }
        });
        if (a.interactive) start_stdin_supervisor(live);
        live->controller->wait();
    }
    live->active = false;
    if (show_progress) {
        renderer.finish();
        renderer.detach(live->bus);
    }

    const std::string status = live->controller->status();
    Json summary = parse_json(fs::read_text(run_dir / "summary.json").value_or("{}")).value_or(Json::object());
    summary["replay"] = !a.replay.empty();
    Json function = Json::object();
    if (auto it = summary.find("functions"); it != summary.end() && it->is_array() && !it->empty()) function = (*it)[0];
    // A run stopped before its session started has no function entry: its status is the outcome.
    const std::string outcome = json_string_or(function, "outcome", status == "completed" ? "error" : status);
    const bool matched = json_bool_or(function, "matched", false);
    std::optional<agent::FunctionRunResult> result;
    {
        std::lock_guard lock(session->mutex);
        result = session->result;
    }
    if (result && !result->matched_source.empty()) summary["matched_source"] = fs::to_utf8(result->matched_source);
    if (!temporary) summary["run_dir"] = fs::to_utf8(run_dir);

    if (g.json) {
        print_json(summary);
    } else {
        const int turns = static_cast<int>(json_int_or(function, "turns", 0));
        const std::string detail = json_string_or(function, "detail", "");
        std::string line = std::format("{} {} ({:#x}) after {} turn{}, best {:.1f}%, ${:.4f}", outcome,
                                       json_string_or(function, "display", sym && !sym->display.empty() ? sym->display : a.function), va, turns,
                                       turns == 1 ? "" : "s", json_number_or(function, "best_match", 0), json_number_or(function, "cost_usd", 0));
        if (!detail.empty() && !matched) line += ": " + detail;
        std::println("{}", line);
        if (result && !result->matched_source.empty()) std::println("source: {}", fs::to_utf8(result->matched_source));
        else if (result && result->best_source && !project)
            std::println("{}:\n{}", matched ? "matched source (not saved: there is no project)" : "best source so far", *result->best_source);
        if (!temporary) std::println("run log: {}", fs::to_utf8(run_dir));
        if (project && !temporary && status != "completed") std::println("the run can continue: decomp run --resume {}", run_id);
    }
    if (temporary) {
        std::error_code ec;
        std::filesystem::remove_all(run_dir, ec);
    }
    return exit_code(matched, outcome);
}

} // namespace

void register_agent_commands(CLI::App& app, GlobalOptions& g) {
    auto a = std::make_shared<AgentArgs>();
    auto* cmd = app.add_subcommand("agent", "Let the built-in Claude agent match a function (needs ANTHROPIC_API_KEY)");
    cmd->add_option("function", a->function, "Function name or address")->required();
    cmd->add_option("--binary", a->binary, "Target binary (default: the project's target; without a project nothing is persisted)");
    cmd->add_option("--pdb", a->pdb, "PDB for --binary");
    cmd->add_option("--toolchain", a->toolchain, "Toolchain name (default: the project's)");
    cmd->add_option("--flag", a->flags, "Extra compiler flag (repeatable)")->allow_extra_args(false);
    cmd->add_option("--model", a->model, "Model (default: the project's agent.model, claude-opus-5-5)");
    cmd->add_option("--effort", a->effort, "Effort: low, medium, high, xhigh, max (default: the project's, high)")
        ->check(CLI::IsMember({"low", "medium", "high", "xhigh", "max"}));
    cmd->add_option("--max-turns", a->max_turns, "Turn limit for this function")->check(CLI::PositiveNumber);
    cmd->add_option("--budget-usd", a->budget_usd, "Spend limit for this function in USD (0 = unlimited)")->check(CLI::NonNegativeNumber);
    cmd->add_option("--max-tokens", a->max_tokens, "Token limit for this function, all token types (0 = unlimited)")->check(CLI::NonNegativeNumber);
    cmd->add_option("--max-minutes", a->max_minutes, "Wall-clock limit for this function (0 = unlimited)")->check(CLI::NonNegativeNumber);
    cmd->add_flag("--no-fallbacks", a->no_fallbacks, "Do not let the API retry declined requests on a fallback model");
    cmd->add_option("--guidance", a->guidance, "Guidance for the agent, sent with the first request (repeatable)")->allow_extra_args(false);
    cmd->add_flag("--interactive", a->interactive, "Read guidance from stdin while running (:pause, :resume, :stop, :abort)");
    cmd->add_option("--policy", a->policies, "Approval policy override, e.g. write_source=deny (auto or deny; repeatable)")
        ->allow_extra_args(false);
    cmd->add_option("--replay", a->replay, "Scripted API responses (JSONL) instead of the live API");
    cmd->add_option("--log-dir", a->log_dir, "Where run logs go (default: <project>/.decomp/runs)");
    cmd->add_flag("--progress", a->progress, "Show the live progress view even with --json or --quiet");
    cmd->add_flag("--no-progress", a->no_progress, "Hide the live progress view");
    cmd->footer("Exit codes: 0 matched, 2 not matched (gave up, budget, turn limit, stopped), 3 refused, 1 error or aborted.\n"
                "Ctrl+C stops after the current turn; press it again to abort the request in flight.");
    cmd->callback([&g, a] { throw CLI::RuntimeError(run(g, [&] { return run_agent(g, *a); })); });
}

} // namespace decomp::cli
