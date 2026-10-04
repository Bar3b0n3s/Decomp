#include "agent/replay_transport.hpp"
#include "agent/runner.hpp"
#include "cli/common.hpp"
#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "events/bus.hpp"
#include "events/progress.hpp"
#include "project/project.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <print>
#include <thread>

namespace decomp::cli {

namespace {

struct AgentArgs {
    std::string function, binary, pdb, toolchain, model, effort, replay, log_dir;
    std::vector<std::string> flags, guidance;
    int max_turns = 0;
    double budget_usd = -1;
    long long max_tokens = -1;
    int max_minutes = -1;
    bool no_fallbacks = false, progress = false, no_progress = false, interactive = false;
};

// Ctrl+C: the first stops after the current turn, the second aborts the request in flight, the third
// exits at once. The handler only bumps a counter; a watcher thread turns it into loop commands.
std::atomic<int> g_interrupts{0};

void on_interrupt(int) {
    if (g_interrupts.fetch_add(1) + 1 >= 3) std::_Exit(130);
    std::signal(SIGINT, on_interrupt);
}

class InterruptWatcher {
public:
    explicit InterruptWatcher(agent::LoopControl& control) : control_(control) {
        g_interrupts = 0;
        previous_ = std::signal(SIGINT, on_interrupt);
        thread_ = std::jthread([this](std::stop_token stop) { watch(stop); });
    }
    ~InterruptWatcher() {
        thread_.request_stop();
        thread_.join();
        if (previous_ != SIG_ERR) std::signal(SIGINT, previous_);
    }
    InterruptWatcher(const InterruptWatcher&) = delete;
    InterruptWatcher& operator=(const InterruptWatcher&) = delete;

private:
    void watch(const std::stop_token& stop) {
        int seen = 0;
        while (!stop.stop_requested()) {
            const int n = g_interrupts.load();
            if (n > seen) {
                seen = n;
                if (n == 1) {
                    log::warn("stopping after the current turn (Ctrl+C again to abort now)");
                    control_.request_stop();
                } else {
                    log::warn("aborting");
                    control_.request_abort();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    agent::LoopControl& control_;
    void (*previous_)(int) = SIG_DFL;
    std::jthread thread_;
};

// --interactive: lines typed on stdin steer the session. The reader blocks on stdin, so it is detached
// and owns a reference to the control block.
void start_stdin_supervisor(std::shared_ptr<agent::LoopControl> control) {
    std::thread([control = std::move(control)] {
        std::string line;
        while (std::getline(std::cin, line)) {
            const std::string cmd(trim(line));
            if (cmd.empty()) continue;
            if (cmd == ":pause") {
                control->request_pause();
                log::info("pausing after the current turn (:resume to continue)");
            } else if (cmd == ":resume") {
                control->resume();
                log::info("resumed");
            } else if (cmd == ":stop") {
                control->request_stop();
                log::info("stopping after the current turn");
            } else if (cmd == ":abort") {
                control->request_abort();
                log::info("aborting");
            } else if (cmd == ":help") {
                log::info("type guidance for the agent, or :pause, :resume, :stop, :abort");
            } else {
                control->inject(cmd);
                log::info("guidance queued for the next turn");
            }
        }
    }).detach();
}

int exit_code(const agent::FunctionRunResult& r) {
    if (r.matched) return 0;
    if (r.outcome == "refused") return 3;
    if (r.outcome == "error" || r.outcome == "aborted") return 1;
    return 2;
}

Json usage_json(const agent::Usage& u) { return u.to_json(); }

Result<int> run_agent(const GlobalOptions& g, const AgentArgs& a) {
    std::optional<project::Project> project;
    if (a.binary.empty()) {
        TRY_ASSIGN(auto p, project::Project::find(g.project));
        project = std::move(p);
    }
    std::optional<std::filesystem::path> pdb_path;
    if (!a.pdb.empty()) pdb_path = fs::from_utf8(a.pdb);
    TRY_ASSIGN(auto program, project ? project->open_program() : Program::open(fs::from_utf8(a.binary), pdb_path));
    TRY_ASSIGN(u64 va, resolve_function(program, a.function));
    // Fail before spending anything on an address that is not a function.
    if (auto extent = program.function_extent(va); !extent)
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

    if (!a.replay.empty()) {
        TRY_ASSIGN(auto replay, agent::ReplayTransport::load(fs::from_utf8(a.replay)));
        config.transport = std::move(replay);
        config.client.api_key = "replay";  // a scripted run never sends the real key anywhere
    } else if (trim(config.client.api_key).empty()) {
        return make_error(ErrorCode::invalid_argument, "ANTHROPIC_API_KEY is not set (export it, or pass --replay <file> for a scripted run)");
    }

    const std::string run_id = events::new_run_id();
    std::filesystem::path run_dir;
    if (!a.log_dir.empty()) run_dir = fs::from_utf8(a.log_dir) / run_id;
    else if (project) run_dir = project->runs_dir() / run_id;

    events::EventBus bus(run_id);
    std::unique_ptr<events::JsonlEventLog> event_log;
    if (!run_dir.empty()) {
        TRY_ASSIGN(auto opened, events::JsonlEventLog::open(run_dir / "events.jsonl"));
        event_log = std::move(opened);
        bus.subscribe([log = event_log.get()](const events::Event& e) { log->write(e); });
    }
    // Warnings and errors logged during the run become events, so the run log and the views keep them.
    const int log_sink = log::add_sink([&bus](log::Level level, std::string_view, std::string_view message) {
        if (level >= log::Level::warn) bus.publish(events::LogLine{std::string(log::to_string(level)), std::string(message)}, -1);
    });
    struct SinkGuard {
        int id;
        ~SinkGuard() { log::remove_sink(id); }
    } sink_guard{log_sink};
    const bool show_progress = a.progress || (!a.no_progress && !g.quiet && !g.json);
    events::ProgressRenderer renderer(is_tty(stderr));
    if (show_progress) renderer.attach(bus);

    const Symbol* sym = program.symbols().at(va);
    const std::string name = sym ? sym->name : std::format("{:#x}", va);
    const std::string display = sym && !sym->display.empty() ? sym->display : name;
    const std::string target_name = project ? fs::to_utf8(project->root().filename()) : fs::to_utf8(fs::from_utf8(a.binary).filename());
    bus.publish(events::RunStarted{target_name, settings.model, settings.effort, 1, {display}}, -1);
    const auto started = std::chrono::system_clock::now();

    config.guidance = a.guidance;
    auto control = std::make_shared<agent::LoopControl>();
    agent::FunctionRunResult result;
    {
        InterruptWatcher watcher(*control);
        if (a.interactive) start_stdin_supervisor(control);
        const std::string file = sym ? project::safe_function_name(*sym) : std::format("sub_{:x}", va);
        const std::filesystem::path transcript = run_dir.empty() ? std::filesystem::path{} : run_dir / "sessions" / fs::from_utf8(file + ".jsonl");
        result = agent::run_function(program, project ? &*project : nullptr, setup, va, config, bus, transcript, control.get(), 0);
    }
    const std::string run_status = result.outcome == "aborted" ? "aborted" : result.outcome == "stopped" ? "stopped" : result.outcome == "error" ? "error" : "completed";
    bus.publish(events::RunFinished{run_status}, -1);
    if (show_progress) {
        renderer.finish();
        renderer.detach(bus);
    }

    Json summary = {{"run", run_id},
                    {"started", std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(started))},
                    {"finished", std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()))},
                    {"status", run_status},
                    {"model", settings.model},
                    {"effort", settings.effort},
                    {"replay", !a.replay.empty()},
                    {"cost_usd", result.cost_usd},
                    {"functions",
                     Json::array({{{"function", name},
                                   {"display", display},
                                   {"va", va},
                                   {"outcome", result.outcome},
                                   {"detail", result.detail},
                                   {"matched", result.matched},
                                   {"best_match", result.best_match},
                                   {"turns", result.turns},
                                   {"cost_usd", result.cost_usd},
                                   {"usage", usage_json(result.usage)}}})}};
    if (!run_dir.empty()) {
        if (auto w = fs::write_text(run_dir / "summary.json", dump_pretty(summary) + "\n"); !w) log::warn("cannot write summary.json: {}", w.error().message);
        summary["run_dir"] = fs::to_utf8(run_dir);
    }
    if (!result.matched_source.empty()) summary["matched_source"] = fs::to_utf8(result.matched_source);

    if (g.json) {
        print_json(summary);
    } else {
        std::string line = std::format("{} {} ({:#x}) after {} turn{}, best {:.1f}%, ${:.4f}", result.outcome, display, va, result.turns,
                                       result.turns == 1 ? "" : "s", result.best_match, result.cost_usd);
        if (!result.detail.empty() && !result.matched) line += ": " + result.detail;
        std::println("{}", line);
        if (!result.matched_source.empty()) std::println("source: {}", fs::to_utf8(result.matched_source));
        else if (result.best_source && !project) std::println("best source so far:\n{}", *result.best_source);
        if (!run_dir.empty()) std::println("run log: {}", fs::to_utf8(run_dir));
    }
    return exit_code(result);
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
    cmd->add_option("--replay", a->replay, "Scripted API responses (JSONL) instead of the live API");
    cmd->add_option("--log-dir", a->log_dir, "Where run logs go (default: <project>/.decomp/runs)");
    cmd->add_flag("--progress", a->progress, "Show the live progress view even with --json or --quiet");
    cmd->add_flag("--no-progress", a->no_progress, "Hide the live progress view");
    cmd->footer("Exit codes: 0 matched, 2 not matched (gave up, budget, turn limit, stopped), 3 refused, 1 error or aborted.\n"
                "Ctrl+C stops after the current turn; press it again to abort the request in flight.");
    cmd->callback([&g, a] { throw CLI::RuntimeError(run(g, [&] { return run_agent(g, *a); })); });
}

} // namespace decomp::cli
