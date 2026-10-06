#include "search/runner.hpp"

#include "analysis/demangle.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "matching/toolchain.hpp"
#include "matching/unit_source.hpp"
#include "project/project.hpp"
#include "project/units.hpp"
#include "search/probes.hpp"

#include <algorithm>
#include <format>
#include <memory>

namespace decomp::search {

namespace {

std::string function_label(const Program& program, u64 va) {
    const Symbol* s = program.symbols().at(va);
    return s ? qualified_name(s->name) : std::format("{:#x}", va);
}

Json probe_list(const Probes& probes) {
    Json out = Json::array();
    for (const auto& p : probes.probes) out.push_back({{"file", p.file_name}, {"functions", p.functions}});
    return out;
}

// The preset's groups (none when groups are given and no preset is), a given group replacing the
// preset's of the same name.
Result<std::vector<FlagGroup>> request_groups(const SearchRequest& request, matching::ToolchainKind kind, Arch arch, std::string& preset) {
    preset = !request.preset.empty() ? request.preset : request.groups.empty() ? "common" : "none";
    TRY_ASSIGN(auto groups, preset_groups(preset, kind, arch));
    for (const auto& text : request.groups) {
        TRY_ASSIGN(auto group, parse_flag_group(text));
        auto same = std::ranges::find_if(groups, [&](const FlagGroup& x) { return !group.name.empty() && x.name == group.name; });
        if (same != groups.end()) *same = std::move(group);
        else groups.push_back(std::move(group));
    }
    if (groups.empty()) return make_error(ErrorCode::invalid_argument, "no flag groups: give a group (\"/Od | /O1 | /O2\") or a preset");
    return groups;
}

// The toolchains named, else every registered one that compiles for `arch` (or does not say which).
Result<std::vector<matching::Toolchain>> candidate_toolchains(const std::vector<std::string>& names, Arch arch) {
    TRY_ASSIGN(auto registry, matching::ToolchainRegistry::load());
    std::vector<matching::Toolchain> out;
    if (!names.empty()) {
        for (const auto& list : names)
            for (auto name : split(list, ',')) {
                if (trim(name).empty()) continue;
                const matching::Toolchain* t = registry.find(trim(name));
                if (!t) return make_error(ErrorCode::not_found, "unknown toolchain '{}' (see `decomp toolchain list`)", trim(name));
                out.push_back(*t);
            }
    } else {
        for (const auto& t : registry.toolchains())
            if (auto a = toolchain_arch(t); !a || *a == arch) out.push_back(t);
    }
    if (out.empty()) return make_error(ErrorCode::not_found, "no toolchain to try: add some with `decomp toolchain add`");
    return out;
}

} // namespace

Result<Probes> make_probes(const project::Project* project, const Program& program, const ProbeSpec& spec) {
    Probes out;
    std::vector<std::string> what;
    if (!spec.sources.empty()) {
        for (const auto& s : spec.sources) {
            TRY_ASSIGN(auto p, file_probe(program, s, spec.functions));
            what.push_back(p.file_name);
            out.probes.push_back(std::move(p));
        }
    } else {
        for (u64 va : spec.functions) {
            const Symbol* fn = program.symbols().at(va);
            if (!fn) return make_error(ErrorCode::not_found, "no function at {:#x}", va);
            if (!project) return make_error(ErrorCode::invalid_argument, "a function's sources are in a project: give a source file without one");
            auto p = spec.attempt ? best_attempt_probe(*project, *fn) : verified_probe(*project, program, *fn, spec.whole);
            if (!p) return std::unexpected(p.error());
            what.push_back(spec.whole && !spec.attempt ? p->file_name : function_label(program, va));
            out.probes.push_back(std::move(*p));
        }
    }
    if (!spec.units.empty()) {
        if (!project) return make_error(ErrorCode::invalid_argument, "a unit's source needs a project");
        TRY_ASSIGN(auto units, project::load_units(*project));
        for (const auto& name : spec.units) {
            auto it = std::ranges::find(units, name, &Unit::name);
            if (it == units.end()) return make_error(ErrorCode::not_found, "no unit '{}' (see `decomp units`)", name);
            TRY_ASSIGN(auto p, unit_probe(*project, *it));
            what.push_back(name);
            out.probes.push_back(std::move(p));
        }
    }
    if (spec.verified) {
        if (!project) return make_error(ErrorCode::invalid_argument, "the verified sources are in a project");
        auto all = verified_probes(*project, program);
        if (all.empty()) return make_error(ErrorCode::not_found, "no function is verified yet");
        what.push_back("every verified source");
        for (auto& p : all) out.probes.push_back(std::move(p));
    }
    if (out.probes.empty()) return make_error(ErrorCode::invalid_argument, "nothing to search: give functions, units, source files or every verified source");
    for (const auto& p : out.probes) out.functions.insert(out.functions.end(), p.functions.begin(), p.functions.end());
    out.target = what.size() <= 3 ? join(what, ", ") : std::format("{}, {} and {} more", what[0], what[1], what.size() - 2);
    return out;
}

std::string SearchOutcome::headline() const {
    std::string text;
    if (const auto* f = std::get_if<FlagSearchResult>(&detail)) {
        text = std::format("{} with {}", score.text(), f->flags.empty() ? std::string("no flags") : join(f->flags, " "));
    } else if (const auto* p = std::get_if<PermuteResult>(&detail)) {
        if (!error.empty()) text = error;
        else if (score.better_than(start_score)) text = std::format("{} after {} edit{} (from {})", score.text(), p->steps.size(), p->steps.size() == 1 ? "" : "s", start_score.text());
        else text = std::format("nothing better than the start ({})", start_score.text());
    } else if (const auto* i = std::get_if<IdentifyResult>(&detail)) {
        if (i->decided()) text = std::format("{} first: {} with {}", i->ranking.front().toolchain, i->ranking.front().score.text(), join(i->ranking.front().flags, " "));
        else text = "no toolchain comes out ahead";
    }
    return cancelled ? "stopped; " + text : text;
}

Result<SearchOutcome> run_search(const project::Project* project, const Program& program, const matching::MatchSetup& setup,
                                 const SearchRequest& request, const SearchCallbacks& callbacks) {
    const auto& on_entry = callbacks.candidate;
    const auto& cancelled = callbacks.cancelled;
    SearchOutcome out;
    ProbeSpec spec = request.probes;
    if (request.kind == SearchKind::permute) {
        if (spec.sources.size() > 1) return make_error(ErrorCode::invalid_argument, "the permuter takes one source file");
        if (!spec.units.empty() || spec.verified || spec.whole)
            return make_error(ErrorCode::invalid_argument, "the permuter takes a function's best attempt or a source file");
        if (spec.sources.empty()) {
            if (spec.functions.size() != 1) return make_error(ErrorCode::invalid_argument, "give a function (its best attempt is permuted), or a source file");
            spec.attempt = true;
        }
    }
    TRY_ASSIGN(out.probes, make_probes(project, program, spec));
    const auto& probes = out.probes;

    std::unique_ptr<RunWriter> writer;
    auto start_run = [&](Json settings) -> Result<void> {
        settings["probes"] = probe_list(probes);
        if (callbacks.started) callbacks.started(probes);
        if (!project || !request.record) return {};
        TRY_ASSIGN(writer, RunWriter::create(search_dir(*project), request.kind, probes.target, probes.functions, std::move(settings)));
        out.run_dir = writer->dir();
        return {};
    };
    RunStatus status = RunStatus::done;

    switch (request.kind) {
    case SearchKind::flags: {
        std::string preset;
        TRY_ASSIGN(out.groups, request_groups(request, setup.toolchain.kind, program.arch(), preset));
        const usize limit = request.limit ? request.limit : 1000;
        Json groups = Json::array();
        for (const auto& g : out.groups) groups.push_back(to_string(g));
        TRY(start_run(Json{{"toolchain", setup.toolchain.name}, {"start", setup.flags}, {"groups", groups}, {"preset", preset}, {"limit", limit},
                           {"exhaustive_limit", request.exhaustive_limit}, {"restarts", request.restarts}, {"seed", request.seed}}));
        CandidateLog log(writer.get(), on_entry);
        FlagSearchOptions options;
        options.groups = out.groups;
        options.start = setup.flags;
        options.exhaustive_limit = request.exhaustive_limit;
        options.max_candidates = limit;
        options.restarts = request.restarts;
        options.seed = request.seed;
        options.threads = request.threads;
        options.cancelled = cancelled;
        options.log = &log;
        auto r = search_flags(program, setup, probes.probes, options);
        out.result = to_json(r, out.groups);
        out.start_score = r.start_score;
        out.score = r.score;
        out.candidates = r.candidates;
        out.cancelled = r.cancelled;
        out.detail = std::move(r);
        break;
    }
    case SearchKind::permute: {
        const auto& probe = probes.probes.front();
        std::vector<std::string> names;
        for (u64 va : probe.functions)
            if (const Symbol* fn = program.symbols().at(va))
                for (auto& n : matching::definition_names(*fn)) names.push_back(std::move(n));
        const Configuration configuration{setup.toolchain, setup.flags};
        const usize limit = request.limit ? request.limit : 500;
        TRY(start_run(Json{{"toolchain", setup.toolchain.name}, {"flags", setup.flags}, {"file", probe.file_name}, {"limit", limit},
                           {"batch", request.batch}, {"seed", request.seed}, {"seconds", request.seconds}}));
        if (writer) TRY(writer->write_file("start-" + probe.file_name, probe.source));
        CandidateLog log(writer.get(), on_entry);
        PermuteOptions options;
        options.max_candidates = limit;
        options.batch = request.batch;
        options.threads = request.threads;
        options.seed = request.seed;
        options.time_limit = std::chrono::seconds(request.seconds);
        options.cancelled = cancelled;
        options.log = &log;
        auto r = permute(program, setup, configuration, probe, names, options);
        if (writer) TRY(writer->write_file("best-" + probe.file_name, r.source));
        out.result = to_json(r);
        out.start_score = r.start_score;
        out.score = r.score;
        out.candidates = r.candidates;
        out.cancelled = r.cancelled;
        out.error = r.error;
        if (!r.error.empty()) status = RunStatus::failed;
        out.detail = std::move(r);
        break;
    }
    case SearchKind::identify: {
        TRY_ASSIGN(auto toolchains, candidate_toolchains(request.toolchains, program.arch()));
        const std::string preset = request.preset.empty() ? "basic" : request.preset;
        const usize limit = request.limit ? request.limit : 64;
        Json names = Json::array();
        for (const auto& t : toolchains) names.push_back(t.name);
        TRY(start_run(Json{{"toolchains", names}, {"preset", preset}, {"start", setup.flags}, {"limit", limit}}));
        CandidateLog log(writer.get(), on_entry);
        IdentifyOptions options;
        options.preset = preset;
        options.start = setup.flags;
        options.max_per_toolchain = limit;
        options.threads = request.threads;
        options.cancelled = cancelled;
        options.log = &log;
        auto r = identify(program, setup, probes.probes, toolchains, options);
        out.result = to_json(r);
        if (!r.ranking.empty()) out.score = r.ranking.front().score;
        out.candidates = r.candidates;
        out.cancelled = r.cancelled;
        out.detail = std::move(r);
        break;
    }
    }
    if (out.cancelled) status = RunStatus::cancelled;
    if (writer) {
        TRY(writer->finish(status, out.result, out.error));
        out.run = writer->record();
    }
    return out;
}

} // namespace decomp::search
