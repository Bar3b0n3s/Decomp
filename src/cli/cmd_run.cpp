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
#include "project/units.hpp"
#include "run/controller.hpp"
#include "run/selection.hpp"
#include "run/store.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <iostream>
#include <memory>
#include <print>
#include <thread>

namespace decomp::cli {

namespace {

struct RunArgs {
    std::vector<std::string> functions, statuses, policies, flags, units;
    std::string filter, toolchain, model, effort, replay_dir, resume;
    bool all = false, include_finished = false, no_fallbacks = false, interactive = false, progress = false, no_progress = false;
    int workers = 0, max_turns = 0, max_minutes = -1, stagger_seconds = -1;
    double run_budget_usd = -1, budget_usd = -1;
    long long max_tokens = -1;
};

std::optional<u64> session_function(const Program& program, const std::string& text) {
    if (auto va = program.resolve(std::string(trim(text)))) return *va;
    return std::nullopt;
}

// --interactive: run commands typed on stdin.
void start_run_supervisor(std::shared_ptr<LiveRun> live, std::shared_ptr<const Program> program) {
    std::thread([live = std::move(live), program = std::move(program)] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!live->active) return;
            auto& c = *live->controller;
            const std::string text(trim(line));
            if (text.empty()) continue;
            const auto space = text.find(' ');
            const std::string cmd = text.substr(0, space);
            const std::string arg = space == std::string::npos ? std::string() : std::string(trim(std::string_view(text).substr(space + 1)));
            auto worker_arg = [&]() -> std::optional<int> {
                if (arg.empty()) return std::nullopt;
                if (auto n = parse_u64(arg); n && *n < static_cast<u64>(run::RunController::kMaxWorkers)) return static_cast<int>(*n);
                return std::nullopt;
            };
            if (cmd == ":pause") {
                if (auto w = worker_arg()) c.pause_worker(*w);
                else c.pause();
                log::info("paused{}", arg.empty() ? "" : " worker " + arg);
            } else if (cmd == ":resume") {
                if (auto w = worker_arg()) c.unpause_worker(*w);
                else c.unpause();
                log::info("resumed{}", arg.empty() ? "" : " worker " + arg);
            } else if (cmd == ":stop") {
                c.stop();
                log::info("stopping: sessions finish their current turn");
            } else if (cmd == ":abort") {
                c.abort();
                log::info("aborting");
            } else if (cmd == ":skip") {
                auto va = session_function(*program, arg);
                if (!va || !c.skip(*va)) log::warn("cannot skip '{}': not pending or running in this run", arg);
            } else if (cmd == ":requeue") {
                auto va = session_function(*program, arg);
                if (!va || !c.requeue(*va)) log::warn("cannot requeue '{}': not finished in this run", arg);
            } else if (cmd == ":workers") {
                if (auto n = parse_u64(arg); n && *n >= 1) c.set_concurrency(static_cast<int>(*n));
                else log::warn(":workers needs a number of workers");
            } else if (cmd == ":budget") {
                double usd = 0;
                if (auto [p, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), usd); ec == std::errc{} && usd >= 0) c.set_run_budget(usd);
                else log::warn(":budget needs an amount in USD (0 = unlimited)");
            } else if (cmd == ":guide") {
                // :guide <function> <text>
                const auto split = arg.find(' ');
                auto va = split == std::string::npos ? std::nullopt : session_function(*program, arg.substr(0, split));
                std::optional<u64> id;
                if (va)
                    for (const auto& session : c.running_sessions())
                        if (session.find(std::format("-{:x}", *va)) != std::string::npos)
                            id = c.inject(session, std::string(trim(std::string_view(arg).substr(split + 1))));
                if (id) log::info("guidance queued for the next turn (id {})", *id);
                else log::warn(":guide needs a running function and the text: :guide <function> <text>");
            } else if (cmd == ":status") {
                usize pending = 0, done = 0;
                for (const auto& item : c.queue()) {
                    pending += item.state == run::ItemState::pending;
                    done += item.state == run::ItemState::done || item.state == run::ItemState::skipped;
                }
                log::info("{}: {} running, {} pending, {} done, {} worker(s), ${:.2f} spent", c.status(), c.running_sessions().size(), pending,
                          done, c.concurrency(), c.ledger()->spent());
            } else {
                log::info("commands: :pause [worker], :resume [worker], :stop, :abort, :skip <fn>, :requeue <fn>, :workers <n>, "
                          ":budget <usd>, :guide <fn> <text>, :status");
            }
        }
    }).detach();
}

int exit_code_for(const std::string& status) {
    if (status == "completed") return 0;
    if (status == "stopped" || status == "budget_exhausted") return 2;
    return 1;
}

void print_summary(const Json& summary, const std::filesystem::path& run_dir) {
    const auto& functions = summary["functions"];
    std::println("run {}: {} - {} function(s) worked, {} matched, {} session(s), ${:.4f}", json_string_or(summary, "run", ""),
                 json_string_or(summary, "status", ""), functions.size(), json_int_or(summary, "functions_matched", 0),
                 json_int_or(summary, "sessions", 0), json_number_or(summary, "cost_usd", 0));
    for (const auto& f : functions) {
        const std::string display = json_string_or(f, "display", json_string_or(f, "function", ""));
        std::println("  {:<20} {:>6.1f}%  {:>3} turn(s)  ${:<8.4f} {} ({:#x}){}", json_string_or(f, "outcome", ""),
                     json_number_or(f, "best_match", 0), json_int_or(f, "turns", 0), json_number_or(f, "cost_usd", 0), display,
                     json_int_or(f, "va", 0),
                     json_int_or(f, "sessions", 1) > 1 ? std::format(", {} sessions", json_int_or(f, "sessions", 1)) : std::string());
    }
    if (!run_dir.empty()) std::println("run log: {}", fs::to_utf8(run_dir));
}

Result<int> run_run(const GlobalOptions& g, const RunArgs& a) {
    TRY_ASSIGN(auto project, project::Project::find(g.project));
    if (!a.resume.empty() && (!a.functions.empty() || a.all || !a.statuses.empty() || !a.filter.empty() || !a.units.empty()))
        return make_error(ErrorCode::invalid_argument, "--resume continues the run's own functions; do not select functions with it");
    if (a.resume.empty() && a.functions.empty() && !a.all && a.statuses.empty() && a.filter.empty() && a.units.empty())
        return make_error(ErrorCode::invalid_argument, "name the functions to run, or select them with --all, --status, --filter or --unit");
    if (!a.units.empty()) {
        TRY_ASSIGN(const auto units, project::load_units(project));
        for (const auto& name : a.units)
            if (std::ranges::find(units, name, &Unit::name) == units.end())
                return make_error(ErrorCode::not_found, "no unit named '{}' (`decomp units` lists them)", name);
    }

    // One live run per project.
    TRY_ASSIGN(auto active_run, project.try_lock_active_run());
    if (!active_run) return make_error(ErrorCode::invalid_argument, "another agent run is active in this project");
    TRY_ASSIGN(auto opened_program, project.open_program());
    auto program = std::make_shared<const Program>(std::move(opened_program));
    TRY_ASSIGN(auto setup, make_match_setup(g, a.toolchain, a.flags));

    // A resumed run keeps its recorded settings unless the command line changes them.
    std::optional<run::RunStore> store;
    Json recorded;
    if (!a.resume.empty()) {
        TRY_ASSIGN(auto dir, run::find_run(project.runs_dir(), a.resume));
        TRY_ASSIGN(auto opened, run::RunStore::open(dir));
        TRY_ASSIGN(recorded, opened.read_run());
        store = std::move(opened);
    }
    project::AgentSettings settings = project.config().agent;
    std::string replay_dir = a.replay_dir;
    if (recorded.is_object()) {
        run::apply_recorded_settings(recorded, settings);
        if (replay_dir.empty() && json_bool_or(recorded, "replay", false)) {
            replay_dir = json_string_or(recorded, "replay_dir", "");
            if (replay_dir.empty()) return make_error(ErrorCode::invalid_argument, "run {} was a scripted run: pass --replay-dir", store->id());
        }
    }
    if (!a.model.empty()) settings.model = a.model;
    if (!a.effort.empty()) settings.effort = a.effort;
    if (a.workers > 0) settings.workers = a.workers;
    if (a.run_budget_usd >= 0) settings.max_usd_per_run = a.run_budget_usd;
    if (a.budget_usd >= 0) settings.max_usd_per_function = a.budget_usd;
    if (a.max_turns > 0) settings.max_turns = a.max_turns;
    if (a.max_tokens >= 0) settings.max_tokens_per_function = a.max_tokens;
    if (a.max_minutes >= 0) settings.max_minutes_per_function = a.max_minutes;
    if (a.no_fallbacks) settings.fallbacks = false;
    if (settings.model.starts_with("claude-haiku"))
        return make_error(ErrorCode::invalid_argument,
                          "model '{}' does not support the adaptive thinking and effort settings the agent sends; use claude-opus-5-5 "
                          "or claude-sonnet-5-5",
                          settings.model);
    agent::AgentRunConfig config = agent::run_config_from(settings);
    // Nobody can answer an "ask" in a terminal run.
    TRY_ASSIGN(auto policies, agent::resolve_approval_policies(settings.approvals, a.policies, false));

    run::TransportFactory transport;
    if (!replay_dir.empty()) {
        const auto dir = fs::from_utf8(replay_dir);
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return make_error(ErrorCode::not_found, "replay directory {} not found", replay_dir);
        transport = [dir](const Symbol& fn) -> Result<std::shared_ptr<agent::HttpTransport>> {
            auto script = agent::find_replay_script(dir, fn);
            if (!script)
                return make_error(ErrorCode::not_found, "no replay script for {} in {} ({}.jsonl, its name without the address, or default.jsonl)",
                                  fn.name, fs::to_utf8(dir), project::safe_function_name(fn));
            TRY_ASSIGN(auto replay, agent::ReplayTransport::load(*script));
            return std::shared_ptr<agent::HttpTransport>(std::move(replay));
        };
        config.client.api_key = "replay";  // a scripted run never sends the real key anywhere
    } else if (trim(config.client.api_key).empty()) {
        return make_error(ErrorCode::invalid_argument, "ANTHROPIC_API_KEY is not set (export it, or pass --replay-dir <dir> for a scripted run)");
    } else {
        config.transport = std::shared_ptr<agent::HttpTransport>(agent::make_default_transport());  // thread-safe, shared by all sessions
    }

    // The functions of a new run, easy ones first.
    std::vector<run::QueueItem> items;
    if (!store) {
        run::Selection selection;
        selection.functions = a.functions;
        selection.filter = a.filter;
        selection.units = a.units;
        selection.include_finished = a.include_finished;
        for (const auto& list : a.statuses)
            for (const auto& name : split(list, ',')) {
                auto status = project::status_from_string(trim(name));
                if (!status) return make_error(ErrorCode::invalid_argument, "unknown status '{}'", name);
                selection.statuses.push_back(*status);
            }
        TRY_ASSIGN(auto vas, run::select_functions(*program, &project, selection));
        if (vas.empty()) return make_error(ErrorCode::invalid_argument, "no functions match the selection");
        // Scored by their code (about a microsecond per instruction), easy ones first.
        const FunctionAnalysis analysis = analyze_functions(*program, vas);
        items = run::make_queue_items(*program, vas, &analysis, true);
        TRY_ASSIGN(auto created, run::RunStore::create(project.runs_dir(), events::new_run_id()));
        store = std::move(created);
    }
    const std::filesystem::path run_dir = store->dir();

    auto live = std::make_shared<LiveRun>(store->id());
    TRY_ASSIGN(live->log, events::JsonlEventLog::open(store->events_path()));
    live->bus.subscribe([log = live->log.get()](const events::Event& e) { log->write(e); });
    const RunLogForwarder forwarder(live);
    const bool show_progress = a.progress || (!a.no_progress && !g.quiet && !g.json);
    events::ProgressRenderer renderer(is_tty(stderr));
    if (show_progress) renderer.attach(live->bus);

    run::RunDeps deps;
    deps.program = [program] { return program; };
    deps.project = &project;
    deps.setup = setup;
    deps.transport = transport;
    live->controller = std::make_unique<run::RunController>(std::move(deps), live->bus);
    run::RunOptions options;
    options.workers = settings.workers;
    options.agent = config;
    options.run_budget_usd = settings.max_usd_per_run;
    options.policies = policies;
    if (a.stagger_seconds >= 0) options.stagger_timeout = std::chrono::seconds(a.stagger_seconds);
    options.project_name = fs::to_utf8(project.root().filename());
    options.replay = !replay_dir.empty();
    options.replay_dir = replay_dir;
    options.selection = recorded.is_object() ? recorded.value("selection", Json::object())
                                             : Json{{"functions", a.functions},
                                                    {"statuses", a.statuses},
                                                    {"filter", a.filter},
                                                    {"units", a.units},
                                                    {"all", a.all},
                                                    {"include_finished", a.include_finished}};
    if (recorded.is_object()) {
        TRY(live->controller->resume(std::move(*store), std::move(options)));
    } else {
        TRY(live->controller->start(std::move(*store), std::move(items), std::move(options)));
    }
    {
        // Ctrl+C: the first stops after the current turns, the second aborts the requests in flight.
        InterruptWatcher watcher([live](int presses) {
            if (presses == 1) {
                log::warn("stopping: sessions finish their current turn (Ctrl+C again to abort now)");
                live->controller->stop();
            } else {
                log::warn("aborting");
                live->controller->abort();
            }
        });
        if (a.interactive) start_run_supervisor(live, program);
        live->controller->wait();
    }
    live->active = false;
    if (show_progress) {
        renderer.finish();
        renderer.detach(live->bus);
    }

    const std::string status = live->controller->status();
    Json summary = parse_json(fs::read_text(run_dir / "summary.json").value_or("{}")).value_or(Json::object());
    if (g.json) {
        summary["run_dir"] = fs::to_utf8(run_dir);
        print_json(summary);
    } else {
        print_summary(summary, run_dir);
        if (status != "completed")
            std::println("the run can continue: decomp run --resume {}", live->controller->run_id());
    }
    return exit_code_for(status);
}

Result<int> runs_list(const GlobalOptions& g) {
    TRY_ASSIGN(auto project, project::Project::find(g.project));
    const auto runs = run::list_runs(project.runs_dir());
    if (g.json) {
        Json list = Json::array();
        for (const auto& r : runs)
            list.push_back(Json{{"id", r.id},
                                {"status", r.status},
                                {"live", r.live},
                                {"created", r.created},
                                {"updated", r.updated},
                                {"functions", r.functions},
                                {"done", r.done},
                                {"matched", r.matched},
                                {"spent_usd", r.spent_usd},
                                {"model", r.model},
                                {"effort", r.effort},
                                {"workers", r.workers},
                                {"replay", r.replay},
                                {"dir", fs::to_utf8(r.dir)}});
        print_json(list);
        return 0;
    }
    if (runs.empty()) {
        std::println("no runs yet (decomp run starts one)");
        return 0;
    }
    std::println("{:<34} {:<16} {:>9} {:>8} {:>10}  {}", "RUN", "STATUS", "DONE", "MATCHED", "SPENT", "MODEL");
    for (const auto& r : runs)
        std::println("{:<34} {:<16} {:>9} {:>8} {:>10}  {}{}", r.id, r.status, std::format("{}/{}", r.done, r.functions), r.matched,
                     std::format("${:.2f}", r.spent_usd), r.model, r.replay ? " (replay)" : "");
    return 0;
}

Result<int> runs_show(const GlobalOptions& g, const std::string& id) {
    TRY_ASSIGN(auto project, project::Project::find(g.project));
    TRY_ASSIGN(auto dir, run::find_run(project.runs_dir(), id));
    TRY_ASSIGN(auto info, run::read_run_info(dir));
    // Replayed from the event log, so it matches what the live views showed.
    TRY_ASSIGN(auto events, events::read_event_log(dir / "events.jsonl"));
    const auto state = events::RunState::replay(events);
    Json summary = run::run_summary(state.data());
    // A run whose process died has no run_finished event: say so.
    if (info.status == "interrupted") summary["status"] = "interrupted";
    if (g.json) {
        print_json(summary);
        return 0;
    }
    print_summary(summary, dir);
    if (info.status == "interrupted" || info.status == "stopped" || info.status == "budget_exhausted")
        std::println("the run can continue: decomp run --resume {}", info.id);
    return 0;
}

} // namespace

void register_run_commands(CLI::App& app, GlobalOptions& g) {
    auto a = std::make_shared<RunArgs>();
    auto* cmd = app.add_subcommand("run", "Let the agent work on many functions in parallel (needs ANTHROPIC_API_KEY)");
    cmd->add_option("functions", a->functions, "Functions (names or addresses)");
    cmd->add_flag("--all", a->all, "Every function that is not finished (matched, refused, skipped or library)");
    cmd->add_option("--status", a->statuses, "Only functions with these statuses (comma-separated)")->allow_extra_args(false);
    cmd->add_option("--filter", a->filter, "Only functions whose names match this regular expression (case-insensitive)");
    cmd->add_option("--unit", a->units, "Only functions of these translation units (repeatable; `decomp units` lists them)")->allow_extra_args(false);
    cmd->add_flag("--include-finished", a->include_finished, "Also select matched, refused, skipped and library functions");
    cmd->add_option("--workers", a->workers, "Parallel sessions (default: the project's agent.workers, 4)")
        ->check(CLI::Range(1, run::RunController::kMaxWorkers));
    cmd->add_option("--run-budget-usd", a->run_budget_usd, "Spend limit for the whole run in USD (0 = unlimited)")->check(CLI::NonNegativeNumber);
    cmd->add_option("--budget-usd", a->budget_usd, "Spend limit per function in USD (0 = unlimited)")->check(CLI::NonNegativeNumber);
    cmd->add_option("--max-turns", a->max_turns, "Turn limit per function")->check(CLI::PositiveNumber);
    cmd->add_option("--max-tokens", a->max_tokens, "Token limit per function (0 = unlimited)")->check(CLI::NonNegativeNumber);
    cmd->add_option("--max-minutes", a->max_minutes, "Wall-clock limit per function (0 = unlimited)")->check(CLI::NonNegativeNumber);
    cmd->add_option("--model", a->model, "Model (default: the project's agent.model)");
    cmd->add_option("--effort", a->effort, "Effort: low, medium, high, xhigh, max")->check(CLI::IsMember({"low", "medium", "high", "xhigh", "max"}));
    cmd->add_flag("--no-fallbacks", a->no_fallbacks, "Do not let the API retry declined requests on a fallback model");
    cmd->add_option("--toolchain", a->toolchain, "Toolchain name (default: the project's)");
    cmd->add_option("--flag", a->flags, "Extra compiler flag (repeatable)")->allow_extra_args(false);
    cmd->add_option("--policy", a->policies, "Approval policy override, e.g. write_source=deny (auto or deny; repeatable)")
        ->allow_extra_args(false);
    cmd->add_option("--replay-dir", a->replay_dir,
                    "Scripted API responses per function (<safe name>.jsonl, <name>.jsonl or default.jsonl) instead of the live API");
    cmd->add_option("--resume", a->resume, "Continue a stopped, budget-limited or interrupted run (its id or a unique prefix)");
    cmd->add_option("--stagger-seconds", a->stagger_seconds, "How long the first session runs alone before the others start (default 30)")
        ->check(CLI::NonNegativeNumber);
    cmd->add_flag("--interactive", a->interactive, "Read commands from stdin (:pause, :resume, :stop, :skip <fn>, :workers <n>, ...)");
    cmd->add_flag("--progress", a->progress, "Show the live progress view even with --json or --quiet");
    cmd->add_flag("--no-progress", a->no_progress, "Hide the live progress view");
    cmd->footer("Exit codes: 0 completed, 2 stopped or out of run budget (resumable), 1 error or aborted.\n"
                "Ctrl+C stops after the current turns; press it again to abort the requests in flight.");
    cmd->callback([&g, a] { throw CLI::RuntimeError(run(g, [&] { return run_run(g, *a); })); });

    auto* runs = app.add_subcommand("runs", "List and inspect runs");
    runs->require_subcommand(1);
    auto* list = runs->add_subcommand("list", "List the project's runs, newest first");
    list->callback([&g] { throw CLI::RuntimeError(run(g, [&] { return runs_list(g); })); });
    auto show_id = std::make_shared<std::string>();
    auto* show = runs->add_subcommand("show", "Summarize a run from its event log");
    show->add_option("id", *show_id, "Run id (or a unique prefix)")->required();
    show->callback([&g, show_id] { throw CLI::RuntimeError(run(g, [&] { return runs_show(g, *show_id); })); });
}

} // namespace decomp::cli
