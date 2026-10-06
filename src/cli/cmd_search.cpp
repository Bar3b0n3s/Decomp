#include "analysis/demangle.hpp"
#include "cli/common.hpp"
#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "matching/toolchain.hpp"
#include "matching/unit_source.hpp"
#include "project/project.hpp"
#include "project/units.hpp"
#include "viewmodel/line_diff.hpp"
#include "search/apply.hpp"
#include "search/flags.hpp"
#include "search/permute.hpp"
#include "search/probes.hpp"
#include "search/runs.hpp"

#include <atomic>
#include <memory>
#include <print>

namespace decomp::cli {

namespace {

// What a search compiles: functions' verified sources (or best attempts), units' sources, files.
struct ProbeArgs {
    std::vector<std::string> functions, units, sources;
    bool verified = false, attempt = false, whole = false;
    std::string binary, toolchain;
    std::vector<std::string> flags;
};

void add_probe_options(CLI::App* cmd, ProbeArgs& a) {
    cmd->add_option("functions", a.functions, "Functions (names or addresses): their verified sources, or the targets of --source");
    cmd->add_option("--unit", a.units, "A unit's source, with every function it holds (repeatable)");
    cmd->add_option("--source", a.sources, "A C/C++ file, with the target functions it defines (repeatable)");
    cmd->add_flag("--verified", a.verified, "Every verified source of the project");
    cmd->add_flag("--attempt", a.attempt, "The functions' best attempts instead of their verified sources");
    cmd->add_flag("--whole", a.whole, "With a function's verified source, every function that file holds");
    cmd->add_option("--binary", a.binary, "Target PE image (default: the project's target)");
    cmd->add_option("--toolchain", a.toolchain, "Toolchain (default: the project's)");
    cmd->add_option("--flag", a.flags, "Extra compiler flag (repeatable; after the project's)")->allow_extra_args(false);
}

std::string function_label(const Program& program, u64 va) {
    const Symbol* s = program.symbols().at(va);
    return s ? qualified_name(s->name) : std::format("{:#x}", va);
}

struct Probes {
    std::vector<search::Probe> probes;
    std::string target;  // what they are, for the run record
    std::vector<u64> functions;
};

Result<Probes> make_probes(const ProbeArgs& a, const project::Project* project, const Program& program) {
    Probes out;
    std::vector<u64> vas;
    for (const auto& f : a.functions) {
        TRY_ASSIGN(u64 va, resolve_function(program, f));
        vas.push_back(va);
    }
    std::vector<std::string> what;
    if (!a.sources.empty()) {
        for (const auto& s : a.sources) {
            TRY_ASSIGN(auto p, search::file_probe(program, fs::from_utf8(s), vas));
            what.push_back(p.file_name);
            out.probes.push_back(std::move(p));
        }
    } else {
        for (u64 va : vas) {
            const Symbol* fn = program.symbols().at(va);
            if (!fn) return make_error(ErrorCode::not_found, "no function at {:#x}", va);
            if (!project) return make_error(ErrorCode::invalid_argument, "a function's sources are in a project: give --source <file> without one");
            auto p = a.attempt ? search::best_attempt_probe(*project, *fn) : search::verified_probe(*project, program, *fn, a.whole);
            if (!p) return std::unexpected(p.error());
            what.push_back(a.whole && !a.attempt ? p->file_name : function_label(program, va));
            out.probes.push_back(std::move(*p));
        }
    }
    if (!a.units.empty()) {
        if (!project) return make_error(ErrorCode::invalid_argument, "--unit needs a project");
        TRY_ASSIGN(auto units, project::load_units(*project));
        for (const auto& name : a.units) {
            auto it = std::ranges::find(units, name, &Unit::name);
            if (it == units.end()) return make_error(ErrorCode::not_found, "no unit '{}' (see `decomp units`)", name);
            TRY_ASSIGN(auto p, search::unit_probe(*project, *it));
            what.push_back(name);
            out.probes.push_back(std::move(p));
        }
    }
    if (a.verified) {
        if (!project) return make_error(ErrorCode::invalid_argument, "--verified needs a project");
        auto all = search::verified_probes(*project, program);
        if (all.empty()) return make_error(ErrorCode::not_found, "no function is verified yet");
        what.push_back("every verified source");
        for (auto& p : all) out.probes.push_back(std::move(p));
    }
    if (out.probes.empty()) return make_error(ErrorCode::invalid_argument, "nothing to search: give functions, --unit, --source or --verified");
    for (const auto& p : out.probes) out.functions.insert(out.functions.end(), p.functions.begin(), p.functions.end());
    out.target = what.size() <= 3 ? join(what, ", ") : std::format("{}, {} and {} more", what[0], what[1], what.size() - 2);
    return out;
}

// The project of a command, unless it searches another binary.
std::optional<project::Project> command_project(const GlobalOptions& g, const ProbeArgs& a) {
    if (!a.binary.empty()) return std::nullopt;
    auto p = project::Project::find(g.project);
    if (!p) return std::nullopt;
    return std::move(*p);
}

struct FlagArgs {
    ProbeArgs probes;
    std::vector<std::string> groups;
    std::string preset;
    usize limit = 1000, exhaustive_limit = 256;
    int restarts = 2, threads = 0;
    u64 seed = 1;
    bool apply = false, no_record = false;
};

Result<int> search_flags_command(const GlobalOptions& g, const FlagArgs& a) {
    auto project = command_project(g, a.probes);
    TRY_ASSIGN(auto program, open_program(g, a.probes.binary));
    TRY_ASSIGN(auto setup, make_match_setup(g, a.probes.toolchain, a.probes.flags));
    TRY_ASSIGN(auto probes, make_probes(a.probes, project ? &*project : nullptr, program));

    // The preset's groups (none when groups are given and no preset is), a given group replacing the
    // preset's of the same name.
    const std::string preset = !a.preset.empty() ? a.preset : a.groups.empty() ? "common" : "none";
    TRY_ASSIGN(auto groups, search::preset_groups(preset, setup.toolchain.kind, program.arch()));
    for (const auto& text : a.groups) {
        TRY_ASSIGN(auto group, search::parse_flag_group(text));
        auto same = std::ranges::find_if(groups, [&](const search::FlagGroup& x) { return !group.name.empty() && x.name == group.name; });
        if (same != groups.end()) *same = std::move(group);
        else groups.push_back(std::move(group));
    }
    if (groups.empty()) return make_error(ErrorCode::invalid_argument, "no flag groups: give --group \"/Od | /O1 | /O2\" or a --preset");

    search::FlagSearchOptions options;
    options.groups = groups;
    options.start = setup.flags;
    options.exhaustive_limit = a.exhaustive_limit;
    options.max_candidates = a.limit;
    options.restarts = a.restarts;
    options.seed = a.seed;
    options.threads = a.threads;
    auto stop = std::make_shared<std::atomic<bool>>(false);
    options.cancelled = [stop] { return stop->load(); };

    Json group_texts = Json::array();
    for (const auto& gr : groups) group_texts.push_back(search::to_string(gr));
    Json probe_list = Json::array();
    for (const auto& p : probes.probes) probe_list.push_back({{"file", p.file_name}, {"functions", p.functions}});
    const Json settings{{"toolchain", setup.toolchain.name}, {"start", setup.flags},     {"groups", group_texts},
                        {"preset", preset},                  {"limit", a.limit},         {"exhaustive_limit", a.exhaustive_limit},
                        {"restarts", a.restarts},            {"seed", a.seed},           {"probes", probe_list}};
    std::unique_ptr<search::RunWriter> writer;
    if (project && !a.no_record) {
        TRY_ASSIGN(writer, search::RunWriter::create(search::search_dir(*project), search::SearchKind::flags, probes.target, probes.functions, settings));
    }
    const bool show = !g.json && !g.quiet;
    if (show)
        std::println("flag search on {} ({} function{}) with {}: {} group{}, starting from {}", probes.target, probes.functions.size(),
                     probes.functions.size() == 1 ? "" : "s", setup.toolchain.name, groups.size(), groups.size() == 1 ? "" : "s",
                     setup.flags.empty() ? "no flags" : join(setup.flags, " "));
    search::CandidateLog log(writer.get(), [show](const search::LogEntry& e) {
        if (show && e.best) std::println("  #{:<5} {}  {}", e.index, e.score.text(), e.label);
    });
    options.log = &log;

    search::FlagSearchResult result;
    {
        InterruptWatcher watcher([stop](int) {
            log::warn("stopping: the configurations being compiled finish first");
            stop->store(true);
        });
        result = search::search_flags(program, setup, probes.probes, options);
    }
    const Json result_json = search::to_json(result, groups);
    if (writer) TRY(writer->finish(result.cancelled ? search::RunStatus::cancelled : search::RunStatus::done, result_json));

    const bool better = result.score.better_than(result.start_score);
    bool applied = false;
    if (a.apply && project && better) {
        TRY(search::apply_configuration(*project, result.flags));
        applied = true;
    }
    if (g.json) {
        Json j = result_json;
        j["applied"] = applied;
        if (writer) j["run"] = writer->record().id;
        print_json(j);
    } else {
        std::println("{}best: {} after {} of {} configurations{} (start: {})", result.cancelled ? "stopped; " : "", result.score.text(),
                     result.candidates, result.space, result.exhaustive ? ", every one" : "", result.start_score.text());
        std::println("flags: {}", join(result.flags, " "));
        usize width = 0;
        for (const auto& gr : groups) width = std::max(width, (gr.name.empty() ? search::to_string(gr) : gr.name).size());
        for (usize i = 0; i < groups.size(); ++i) {
            const auto& gr = groups[i];
            std::vector<std::string> also;
            for (usize alt : result.equivalent[i])
                if (alt != result.choice[i]) also.push_back(search::alternative_text(gr.alternatives[alt]));
            std::println("  {:<{}}  {:<12} {}", gr.name.empty() ? search::to_string(gr) : gr.name, width,
                         search::alternative_text(gr.alternatives[result.choice[i]]),
                         also.empty() ? "decided" : "also " + join(also, ", "));
        }
        if (writer) std::println("run: {}", writer->record().id);
        if (applied) std::println("decomp.json flags set; `decomp units verify` checks every verified function with them");
        else if (a.apply && !better) std::println("the project's flags do as well: nothing to apply");
        else if (a.apply) std::println("--apply needs a project");
        else if (better && project) std::println("--apply sets them as the project's flags");
    }
    return result.score.complete() ? 0 : 2;
}

struct PermuteArgs {
    ProbeArgs probes;
    usize limit = 500, batch = 0;
    int threads = 0, seconds = 0;
    u64 seed = 1;
    std::string out;
    bool apply = false, no_record = false;
};

Result<int> search_permute_command(const GlobalOptions& g, const PermuteArgs& a) {
    if (a.probes.sources.size() > 1) return make_error(ErrorCode::invalid_argument, "the permuter takes one --source");
    if (!a.probes.units.empty() || a.probes.verified || a.probes.whole)
        return make_error(ErrorCode::invalid_argument, "the permuter takes a function's best attempt or a --source file");
    auto project = command_project(g, a.probes);
    TRY_ASSIGN(auto program, open_program(g, a.probes.binary));
    TRY_ASSIGN(auto setup, make_match_setup(g, a.probes.toolchain, a.probes.flags));
    ProbeArgs pa = a.probes;
    if (pa.sources.empty()) {
        if (pa.functions.size() != 1) return make_error(ErrorCode::invalid_argument, "give a function (its best attempt is permuted), or --source <file>");
        pa.attempt = true;
    }
    TRY_ASSIGN(auto probes, make_probes(pa, project ? &*project : nullptr, program));
    const auto& probe = probes.probes.front();
    std::vector<std::string> names;
    for (u64 va : probe.functions)
        if (const Symbol* fn = program.symbols().at(va))
            for (auto& n : matching::definition_names(*fn)) names.push_back(std::move(n));
    const search::Configuration configuration{setup.toolchain, setup.flags};

    search::PermuteOptions options;
    options.max_candidates = a.limit;
    options.batch = a.batch;
    options.threads = a.threads;
    options.seed = a.seed;
    options.time_limit = std::chrono::seconds(a.seconds);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    options.cancelled = [stop] { return stop->load(); };

    const Json settings{{"toolchain", setup.toolchain.name}, {"flags", setup.flags},   {"file", probe.file_name}, {"limit", a.limit},
                        {"batch", a.batch},                  {"seed", a.seed},        {"seconds", a.seconds}};
    std::unique_ptr<search::RunWriter> writer;
    if (project && !a.no_record) {
        TRY_ASSIGN(writer, search::RunWriter::create(search::search_dir(*project), search::SearchKind::permute, probes.target, probes.functions, settings));
        TRY(writer->write_file("start-" + probe.file_name, probe.source));
    }
    const bool show = !g.json && !g.quiet;
    if (show)
        std::println("permuting {} ({}, {} function{}) with {}", probes.target, probe.file_name, probe.functions.size(), probe.functions.size() == 1 ? "" : "s",
                     configuration.label());
    search::CandidateLog log(writer.get(), [show](const search::LogEntry& e) {
        if (show && e.best) std::println("  #{:<5} {}  {}", e.index, e.score.text(), e.label);
    });
    options.log = &log;

    search::PermuteResult result;
    {
        InterruptWatcher watcher([stop](int) {
            log::warn("stopping: the candidates being compiled finish first");
            stop->store(true);
        });
        result = search::permute(program, setup, configuration, probe, names, options);
    }
    if (!result.error.empty() && !g.json) log::warn("{}", result.error);
    Json result_json = search::to_json(result);
    if (writer) {
        TRY(writer->write_file("best-" + probe.file_name, result.source));
        TRY(writer->finish(result.cancelled ? search::RunStatus::cancelled : !result.error.empty() ? search::RunStatus::failed : search::RunStatus::done,
                           result_json, result.error));
    }
    if (!a.out.empty()) TRY(fs::write_text(fs::from_utf8(a.out), result.source));

    const bool better = result.score.better_than(result.start_score);
    std::string applied;
    if (a.apply) {
        if (!project) return make_error(ErrorCode::invalid_argument, "--apply needs a project");
        if (probe.functions.size() != 1) return make_error(ErrorCode::invalid_argument, "--apply keeps the source of one function: give it");
        if (better || result.score.complete()) {
            const Symbol* fn = program.symbols().at(probe.functions.front());
            TRY_ASSIGN(auto kept, search::apply_source(*project, program, setup, *fn, result.source, result.score.match_percent, result.score.complete(),
                                                       "found by the permuter"));
            applied = kept.message;
        } else {
            applied = "the permuter found nothing better: nothing kept";
        }
    }
    if (g.json) {
        result_json["source"] = result.source;
        if (writer) result_json["run"] = writer->record().id;
        if (!applied.empty()) result_json["applied"] = applied;
        print_json(result_json);
    } else {
        std::println("{}best: {} after {} candidates (start: {})", result.cancelled ? "stopped; " : "", result.score.text(), result.candidates,
                     result.start_score.text());
        if (better) {
            std::println("edits: {}", join(result.steps, "; "));
            std::print("{}", vm::to_unified(vm::diff_lines(probe.source, result.source), "start/" + probe.file_name, "best/" + probe.file_name));
        }
        if (writer) std::println("run: {}", writer->record().id);
        if (!applied.empty()) std::println("{}", applied);
        else if (better && project && probe.functions.size() == 1) std::println("--apply keeps it in the project");
    }
    return result.score.complete() ? 0 : 2;
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
        auto a = std::make_shared<FlagArgs>();
        add_probe_options(cmd, a->probes);
        cmd->add_option("--group", a->groups, "A group of alternatives, e.g. \"opt: /Od | /O1 | /O2\" or \"none | /Oy-\" (repeatable)");
        cmd->add_option("--preset", a->preset, "Groups to start from: common, full or none (default: common, or none with --group)");
        cmd->add_option("--limit", a->limit, "Configurations to compile, at most");
        cmd->add_option("--exhaustive-limit", a->exhaustive_limit, "Try every combination when there are at most this many");
        cmd->add_option("--restarts", a->restarts, "Local searches from random starts after the first");
        cmd->add_option("--seed", a->seed, "Seed of the random starts");
        cmd->add_option("--threads", a->threads, "Configurations compiled at once (default: as many as compiles may run)");
        cmd->add_flag("--apply", a->apply, "Set the best flags as the project's (when they do better than its own)");
        cmd->add_flag("--no-record", a->no_record, "Do not keep the run under .decomp/search/");
        cmd->callback([&g, a] { throw CLI::RuntimeError(run(g, [&] { return search_flags_command(g, *a); })); });
    }
    {
        auto* cmd = search->add_subcommand("permute", "Permute a function's source (its best attempt, or --source) toward the target's bytes "
                                                      "(exit code 0 = every function byte-exact)");
        auto a = std::make_shared<PermuteArgs>();
        add_probe_options(cmd, a->probes);
        cmd->add_option("--limit", a->limit, "Candidates to compile, at most");
        cmd->add_option("--batch", a->batch, "Candidates per round (default: twice the threads, at least 8)");
        cmd->add_option("--seed", a->seed, "Seed of the random edits");
        cmd->add_option("--threads", a->threads, "Candidates compiled at once (default: as many as compiles may run)");
        cmd->add_option("--time", a->seconds, "Seconds to search, at most (default: no limit)");
        cmd->add_option("--out", a->out, "Write the best source to this file");
        cmd->add_flag("--apply", a->apply, "Keep the best source: verified when byte-exact, else as the function's best attempt");
        cmd->add_flag("--no-record", a->no_record, "Do not keep the run under .decomp/search/");
        cmd->callback([&g, a] { throw CLI::RuntimeError(run(g, [&] { return search_permute_command(g, *a); })); });
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
