#include "cli/common.hpp"
#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "matching/toolchain.hpp"
#include "project/project.hpp"
#include "viewmodel/line_diff.hpp"
#include "search/apply.hpp"
#include "search/runner.hpp"

#include <atomic>
#include <memory>
#include <print>

namespace decomp::cli {

namespace {

// What every search takes: what to compile (functions' verified sources or best attempts, units' sources,
// files), with which toolchain and flags, and what to do with the result.
struct SearchArgs {
    std::vector<std::string> functions, units, sources;
    bool verified = false, attempt = false, whole = false;
    std::string binary, toolchain;
    std::vector<std::string> flags;
    bool apply = false, no_record = false;
    int threads = 0;
    // flags and identify
    std::vector<std::string> groups;
    std::string preset;
    usize limit = 0, exhaustive_limit = 256;
    int restarts = 2;
    u64 seed = 1;
    // permute
    usize batch = 0;
    int seconds = 0;
    std::string out;
    // identify
    std::vector<std::string> candidates;
};

void add_probe_options(CLI::App* cmd, SearchArgs& a) {
    cmd->add_option("functions", a.functions, "Functions (names or addresses): their verified sources, or the targets of --source");
    cmd->add_option("--unit", a.units, "A unit's source, with every function it holds (repeatable)");
    cmd->add_option("--source", a.sources, "A C/C++ file, with the target functions it defines (repeatable)");
    cmd->add_flag("--verified", a.verified, "Every verified source of the project");
    cmd->add_flag("--attempt", a.attempt, "The functions' best attempts instead of their verified sources");
    cmd->add_flag("--whole", a.whole, "With a function's verified source, every function that file holds");
    cmd->add_option("--binary", a.binary, "Target PE image (default: the project's target)");
    cmd->add_option("--toolchain", a.toolchain, "Toolchain (default: the project's)");
    cmd->add_option("--flag", a.flags, "Extra compiler flag (repeatable; after the project's)")->allow_extra_args(false);
    cmd->add_option("--threads", a.threads, "Candidates compiled at once (default: as many as compiles may run)");
    cmd->add_flag("--no-record", a.no_record, "Do not keep the run under .decomp/search/");
}

// The first line of a compiler's output.
std::string snippet_line(std::string_view text) {
    const auto line = trim(text.substr(0, text.find('\n')));
    return std::string(line.size() > 100 ? line.substr(0, 97) : line) + (line.size() > 100 ? "..." : "");
}

// The project of a command, unless it searches another binary.
std::optional<project::Project> command_project(const GlobalOptions& g, const SearchArgs& a) {
    if (!a.binary.empty()) return std::nullopt;
    auto p = project::Project::find(g.project);
    if (!p) return std::nullopt;
    return std::move(*p);
}

void print_flags(const search::SearchOutcome& o) {
    const auto& r = std::get<search::FlagSearchResult>(o.detail);
    std::println("{}best: {} after {} of {} configurations{} (start: {})", r.cancelled ? "stopped; " : "", r.score.text(), r.candidates, r.space,
                 r.exhaustive ? ", every one" : "", r.start_score.text());
    std::println("flags: {}", join(r.flags, " "));
    usize width = 0;
    for (const auto& gr : o.groups) width = std::max(width, (gr.name.empty() ? search::to_string(gr) : gr.name).size());
    for (usize i = 0; i < o.groups.size(); ++i) {
        const auto& gr = o.groups[i];
        std::vector<std::string> also;
        for (usize alt : r.equivalent[i])
            if (alt != r.choice[i]) also.push_back(search::alternative_text(gr.alternatives[alt]));
        std::println("  {:<{}}  {:<12} {}", gr.name.empty() ? search::to_string(gr) : gr.name, width, search::alternative_text(gr.alternatives[r.choice[i]]),
                     also.empty() ? "decided" : "also " + join(also, ", "));
    }
}

void print_permute(const search::SearchOutcome& o) {
    const auto& r = std::get<search::PermuteResult>(o.detail);
    const auto& probe = o.probes.probes.front();
    std::println("{}best: {} after {} candidates (start: {})", r.cancelled ? "stopped; " : "", r.score.text(), r.candidates, r.start_score.text());
    if (r.score.better_than(r.start_score)) {
        std::println("edits: {}", join(r.steps, "; "));
        std::print("{}", vm::to_unified(vm::diff_lines(probe.source, r.source), "start/" + probe.file_name, "best/" + probe.file_name));
    }
}

void print_identify(const search::SearchOutcome& o) {
    const auto& r = std::get<search::IdentifyResult>(o.detail);
    usize width = 0;
    for (const auto& t : r.ranking) width = std::max(width, t.toolchain.size());
    for (usize i = 0; i < r.ranking.size(); ++i) {
        const auto& t = r.ranking[i];
        if (!t.error.empty()) std::println("  {}. {:<{}}  could not compile the probes: {}", i + 1, t.toolchain, width, snippet_line(t.error));
        else std::println("  {}. {:<{}}  {}  {}", i + 1, t.toolchain, width, t.score.text(), join(t.flags, " "));
    }
    if (r.decided()) std::println("best: {} with {}", r.ranking.front().toolchain, join(r.ranking.front().flags, " "));
    else std::println("no toolchain comes out ahead");
}

// Keeps what the search found in the project (--apply): what it did, or nothing to say.
Result<std::string> apply_outcome(project::Project& project, const Program& program, const matching::MatchSetup& setup, const search::SearchOutcome& o) {
    if (const auto* f = std::get_if<search::FlagSearchResult>(&o.detail)) {
        if (!f->score.better_than(f->start_score)) return std::string("the project's flags do as well: nothing to apply");
        TRY(search::apply_configuration(project, f->flags));
        return std::string("decomp.json flags set; `decomp units verify` checks every verified function with them");
    }
    if (const auto* p = std::get_if<search::PermuteResult>(&o.detail)) {
        const auto& functions = o.probes.probes.front().functions;
        if (functions.size() != 1) return make_error(ErrorCode::invalid_argument, "--apply keeps the source of one function: give it");
        if (!p->score.better_than(p->start_score) && !p->score.complete()) return std::string("the permuter found nothing better: nothing kept");
        const Symbol* fn = program.symbols().at(functions.front());
        if (!fn) return make_error(ErrorCode::not_found, "no function at {:#x}", functions.front());
        TRY_ASSIGN(auto kept, search::apply_source(project, program, setup, *fn, p->source, p->score.match_percent, p->score.complete(), "found by the permuter"));
        return kept.message;
    }
    if (const auto* i = std::get_if<search::IdentifyResult>(&o.detail)) {
        if (!i->decided()) return std::string("no toolchain comes out ahead: nothing applied");
        const auto& best = i->ranking.front();
        TRY(search::apply_configuration(project, best.flags, best.toolchain));
        return std::format("decomp.json: toolchain {}, flags {}", best.toolchain, join(best.flags, " "));
    }
    return std::string();
}

Result<int> search_command(const GlobalOptions& g, const SearchArgs& a, search::SearchKind kind) {
    auto project = command_project(g, a);
    TRY_ASSIGN(auto program, open_program(g, a.binary));
    // Identification compiles with its candidates; a project without a toolchain still gives the work
    // directories and include paths (its toolchain, when it has one, the style of the flags given).
    std::string toolchain = a.toolchain;
    if (kind == search::SearchKind::identify && toolchain.empty() && (!project || project->config().toolchain.empty())) {
        TRY_ASSIGN(auto registry, matching::ToolchainRegistry::load());
        if (!registry.toolchains().empty()) toolchain = registry.toolchains().front().name;
    }
    TRY_ASSIGN(auto setup, make_match_setup(g, toolchain, a.flags));

    search::SearchRequest request;
    request.kind = kind;
    for (const auto& f : a.functions) {
        TRY_ASSIGN(u64 va, resolve_function(program, f));
        request.probes.functions.push_back(va);
    }
    request.probes.units = a.units;
    for (const auto& s : a.sources) request.probes.sources.push_back(fs::from_utf8(s));
    request.probes.verified = a.verified;
    request.probes.attempt = a.attempt;
    request.probes.whole = a.whole;
    request.groups = a.groups;
    request.preset = a.preset;
    request.limit = a.limit;
    request.exhaustive_limit = a.exhaustive_limit;
    request.restarts = a.restarts;
    request.seed = a.seed;
    request.batch = a.batch;
    request.seconds = a.seconds;
    request.toolchains = a.candidates;
    request.threads = a.threads;
    request.record = !a.no_record;

    const bool show = !g.json && !g.quiet;
    auto stop = std::make_shared<std::atomic<bool>>(false);
    search::SearchCallbacks callbacks;
    callbacks.cancelled = [stop] { return stop->load(); };
    callbacks.started = [&](const search::Probes& p) {
        if (!show) return;
        const std::string functions = std::format("{} function{}", p.functions.size(), p.functions.size() == 1 ? "" : "s");
        if (kind == search::SearchKind::flags)
            std::println("flag search on {} ({}) with {}, starting from {}", p.target, functions, setup.toolchain.name,
                         setup.flags.empty() ? "no flags" : join(setup.flags, " "));
        else if (kind == search::SearchKind::permute)
            std::println("permuting {} ({}, {}) with {}", p.target, p.probes.front().file_name, functions,
                         search::Configuration{setup.toolchain, setup.flags}.label());
        else std::println("identifying the compiler with {} ({})", p.target, functions);
    };
    callbacks.candidate = [show](const search::LogEntry& e) {
        if (show && e.best) std::println("  #{:<5} {}  {}", e.index, e.score.text(), e.label);
    };
    Result<search::SearchOutcome> searched = make_error(ErrorCode::internal, "not run");
    {
        InterruptWatcher watcher([stop](int) {
            log::warn("stopping: the candidates being compiled finish first");
            stop->store(true);
        });
        searched = search::run_search(project ? &*project : nullptr, program, setup, request, callbacks);
    }
    TRY_ASSIGN(auto outcome, std::move(searched));
    if (!outcome.error.empty() && !g.json) log::warn("{}", outcome.error);
    if (!a.out.empty())
        if (const auto* p = std::get_if<search::PermuteResult>(&outcome.detail)) TRY(fs::write_text(fs::from_utf8(a.out), p->source));

    std::string applied;
    if (a.apply) {
        if (!project) return make_error(ErrorCode::invalid_argument, "--apply needs a project");
        TRY_ASSIGN(applied, apply_outcome(*project, program, setup, outcome));
    }
    if (g.json) {
        Json j = outcome.result;
        if (const auto* p = std::get_if<search::PermuteResult>(&outcome.detail)) j["source"] = p->source;
        if (outcome.run) j["run"] = outcome.run->id;
        if (!applied.empty()) j["applied"] = applied;
        print_json(j);
    } else {
        if (kind == search::SearchKind::flags) print_flags(outcome);
        else if (kind == search::SearchKind::permute) print_permute(outcome);
        else print_identify(outcome);
        if (outcome.run) std::println("run: {}", outcome.run->id);
        if (!applied.empty()) std::println("{}", applied);
        else if (project && !a.apply && kind != search::SearchKind::permute && outcome.score.better_than(outcome.start_score))
            std::println("--apply sets {} as the project's", kind == search::SearchKind::flags ? "these flags" : "the best toolchain and its flags");
        else if (project && !a.apply && kind == search::SearchKind::permute && outcome.probes.functions.size() == 1 &&
                 outcome.score.better_than(outcome.start_score))
            std::println("--apply keeps it in the project");
    }
    if (const auto* i = std::get_if<search::IdentifyResult>(&outcome.detail)) return i->decided() && outcome.score.complete() ? 0 : 2;
    return outcome.score.complete() ? 0 : 2;
}

Result<int> search_list(const GlobalOptions& g) {
    TRY_ASSIGN(auto project, project::Project::find(g.project));
    const auto runs = search::list_runs(search::search_dir(project));
    if (g.json) {
        Json arr = Json::array();
        for (const auto& r : runs) arr.push_back(search::to_json(r));
        print_json(arr);
        return 0;
    }
    if (runs.empty()) {
        std::println("no searches yet (decomp search flags|permute|identify)");
        return 0;
    }
    std::println("{:<36} {:<9} {:<10} {:>10}  {:<34} {}", "SEARCH", "KIND", "STATUS", "CANDIDATES", "BEST", "TARGET");
    for (const auto& r : runs)
        std::println("{:<36} {:<9} {:<10} {:>10}  {:<34} {}", r.id, search::to_string(r.kind), search::to_string(r.status), r.candidates,
                     r.best ? r.best->text() : std::string("-"), r.target);
    return 0;
}

Result<int> search_show(const GlobalOptions& g, const std::string& id, bool with_log) {
    TRY_ASSIGN(auto project, project::Project::find(g.project));
    const auto dir = search::search_dir(project) / fs::from_utf8(id);
    TRY_ASSIGN(auto run, search::load_run(dir));
    const auto entries = with_log ? search::load_log(dir) : std::vector<search::LogEntry>{};
    if (g.json) {
        Json j = search::to_json(run);
        if (with_log) {
            Json log = Json::array();
            for (const auto& e : entries) log.push_back(search::to_json(e));
            j["log"] = std::move(log);
        }
        print_json(j);
        return 0;
    }
    std::println("{}: {} search on {}, {} ({} candidates, {:.1f} s)", run.id, search::to_string(run.kind), run.target, search::to_string(run.status),
                 run.candidates, static_cast<double>(run.duration_ms) / 1000.0);
    if (run.best) std::println("best: {}  {}", run.best->text(), run.best_label);
    if (!run.error.empty()) std::println("error: {}", run.error);
    std::println("{}", dump_pretty(run.result));
    for (const auto& e : entries)
        std::println("  #{:<5} {:>8} ms  {}{}  {}", e.index, e.ms, e.score.text(), e.best ? " *" : "", e.label);
    return 0;
}

} // namespace

void register_search_commands(CLI::App& app, GlobalOptions& g) {
    auto* search = app.add_subcommand("search", "Mechanical searches: compiler flags, source permutations, toolchains");
    search->require_subcommand(1);

    {
        auto* cmd = search->add_subcommand("flags", "Search compiler flags that make the sources compile to the target's bytes "
                                                    "(exit code 0 = every function byte-exact)");
        auto a = std::make_shared<SearchArgs>();
        add_probe_options(cmd, *a);
        cmd->add_option("--group", a->groups, "A group of alternatives, e.g. \"opt: /Od | /O1 | /O2\" or \"none | /Oy-\" (repeatable)");
        cmd->add_option("--preset", a->preset, "Groups to start from: basic, common, full or none (default: common, or none with --group)");
        cmd->add_option("--limit", a->limit, "Configurations to compile, at most (default 1000)");
        cmd->add_option("--exhaustive-limit", a->exhaustive_limit, "Try every combination when there are at most this many");
        cmd->add_option("--restarts", a->restarts, "Local searches from random starts after the first");
        cmd->add_option("--seed", a->seed, "Seed of the random starts");
        cmd->add_flag("--apply", a->apply, "Set the best flags as the project's (when they do better than its own)");
        cmd->callback([&g, a] { throw CLI::RuntimeError(run(g, [&] { return search_command(g, *a, search::SearchKind::flags); })); });
    }
    {
        auto* cmd = search->add_subcommand("permute", "Permute a function's source (its best attempt, or --source) toward the target's bytes "
                                                      "(exit code 0 = every function byte-exact)");
        auto a = std::make_shared<SearchArgs>();
        add_probe_options(cmd, *a);
        cmd->add_option("--limit", a->limit, "Candidates to compile, at most (default 500)");
        cmd->add_option("--batch", a->batch, "Candidates per round (default: twice the threads, at least 8)");
        cmd->add_option("--seed", a->seed, "Seed of the random edits");
        cmd->add_option("--time", a->seconds, "Seconds to search, at most (default: no limit)");
        cmd->add_option("--out", a->out, "Write the best source to this file");
        cmd->add_flag("--apply", a->apply, "Keep the best source: verified when byte-exact, else as the function's best attempt");
        cmd->callback([&g, a] { throw CLI::RuntimeError(run(g, [&] { return search_command(g, *a, search::SearchKind::permute); })); });
    }
    {
        auto* cmd = search->add_subcommand("identify", "Rank toolchains by how close they compile the sources to the target's bytes, each with "
                                                       "a small flag search (exit code 0 = one ahead, every function byte-exact)");
        auto a = std::make_shared<SearchArgs>();
        add_probe_options(cmd, *a);
        cmd->add_option("--candidates", a->candidates, "Toolchains to try, comma-separated (default: every one for the target's architecture)");
        cmd->add_option("--preset", a->preset, "Flag groups each toolchain is searched with: basic (default), common, full or none");
        cmd->add_option("--limit", a->limit, "Configurations to compile per toolchain, at most (default 64)");
        cmd->add_flag("--apply", a->apply, "Set the best toolchain and its flags as the project's (when one comes out ahead)");
        cmd->callback([&g, a] { throw CLI::RuntimeError(run(g, [&] { return search_command(g, *a, search::SearchKind::identify); })); });
    }
    search->add_subcommand("list", "List the project's searches, newest first")->callback([&g] {
        throw CLI::RuntimeError(run(g, [&] { return search_list(g); }));
    });
    {
        auto* cmd = search->add_subcommand("show", "Show a search: its settings, result and (with --log) every candidate");
        auto id = std::make_shared<std::string>();
        auto with_log = std::make_shared<bool>(false);
        cmd->add_option("id", *id, "Search id (from `decomp search list`)")->required();
        cmd->add_flag("--log", *with_log, "Every candidate evaluated");
        cmd->callback([&g, id, with_log] { throw CLI::RuntimeError(run(g, [&] { return search_show(g, *id, *with_log); })); });
    }
}

} // namespace decomp::cli
