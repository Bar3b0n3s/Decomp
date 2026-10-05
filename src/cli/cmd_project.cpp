#include "cli/common.hpp"
#include "analysis/signatures.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "formats/archive.hpp"
#include "formats/coff.hpp"
#include "matching/diff.hpp"
#include "project/analyze.hpp"
#include "project/library.hpp"
#include "project/progress.hpp"
#include "project/project.hpp"
#include "project/units.hpp"

#include <format>
#include <map>
#include <print>

namespace decomp::cli {

void register_project_commands(CLI::App& app, GlobalOptions& g) {
    {
        auto* cmd = app.add_subcommand("init", "Create a decomp project for a binary (writes decomp.json and symbols.txt)");
        auto binary = std::make_shared<std::string>();
        auto dir = std::make_shared<std::string>();
        auto pdb = std::make_shared<std::string>();
        auto map = std::make_shared<std::string>();
        auto toolchain = std::make_shared<std::string>();
        auto flags = std::make_shared<std::vector<std::string>>();
        cmd->add_option("binary", *binary, "Target PE image")->required();
        cmd->add_option("--dir", *dir, "Project directory (default: -C, else the current directory)");
        cmd->add_option("--pdb", *pdb, "PDB for the target, if not next to it");
        cmd->add_option("--map", *map, "The build's link map (link.exe /MAP): function names, starts and object files");
        cmd->add_option("--toolchain", *toolchain, "Toolchain name from the registry (see `decomp toolchain list`)");
        cmd->add_option("--flag", *flags, "Compiler flag the target was built with, e.g. /O2 (repeatable)")->allow_extra_args(false);
        cmd->callback([&g, binary, dir, pdb, map, toolchain, flags] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                std::optional<std::filesystem::path> pdb_path, map_path;
                if (!pdb->empty()) pdb_path = fs::from_utf8(*pdb);
                if (!map->empty()) map_path = fs::from_utf8(*map);
                const std::string root = !dir->empty() ? *dir : !g.project.empty() ? g.project : std::string(".");
                TRY_ASSIGN(auto p, project::Project::init(fs::from_utf8(root), fs::from_utf8(*binary), pdb_path, *toolchain, map_path));
                if (!flags->empty()) {
                    p.config().flags = *flags;
                    TRY(p.save_config());
                }
                TRY_ASSIGN(auto program, p.open_program());
                if (g.json) {
                    print_json({{"root", fs::to_utf8(p.root())}, {"symbols", program.symbols().size()},
                                {"functions", program.symbols().functions().size()}});
                } else {
                    std::println("created {} for {} ({} symbols, {} functions)", fs::to_utf8(p.root() / project::Project::kConfigFile),
                                 p.config().target, program.symbols().size(), program.symbols().functions().size());
                    if (p.config().toolchain.empty())
                        std::println("next: set \"toolchain\" in decomp.json to a name from `decomp toolchain list`");
                    if (p.config().flags.empty())
                        std::println("next: set \"flags\" in decomp.json to the flags the target was built with (e.g. /O2 /Gy)");
                }
                return 0;
            }));
        });
    }
    {
        // decomp analyze and decomp map import: the same re-analysis, with or without a map.
        auto analyze = [&g](const std::optional<std::filesystem::path>& map) -> Result<int> {
            TRY_ASSIGN(auto p, project::Project::find(g.project));
            TRY_ASSIGN(auto s, project::analyze(p, project::AnalyzeOptions{.map = map}));
            if (g.json) {
                print_json(project::to_json(s));
                return 0;
            }
            if (map) std::println("map: {} symbols added or named", s.map_symbols);
            std::println("functions: {} (was {}): {} added, {} removed, {} resized, {} renamed", s.functions, s.functions_before, s.added,
                         s.removed, s.resized, s.renamed);
            if (s.moved) std::println("moved {} work directories and matched sources with their renamed functions", s.moved);
            if (s.units) std::println("units: {} from the {}", s.units, s.units_from);
            return 0;
        };
        auto* cmd = app.add_subcommand("analyze", "Find the project's functions again, keeping named functions and recorded work");
        auto map = std::make_shared<std::string>();
        cmd->add_option("--map", *map, "The build's link map (link.exe /MAP): function names, starts and object files");
        cmd->callback([&g, analyze, map] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                std::optional<std::filesystem::path> map_path;
                if (!map->empty()) map_path = fs::from_utf8(*map);
                return analyze(map_path);
            }));
        });
        auto* map_cmd = app.add_subcommand("map", "Use the build's link map file");
        map_cmd->require_subcommand(1);
        auto* import = map_cmd->add_subcommand("import", "Name the project's functions from a link map and find their bounds again");
        auto import_path = std::make_shared<std::string>();
        import->add_option("file", *import_path, "The link map (link.exe /MAP or lld-link /map)")->required();
        import->callback([&g, analyze, import_path] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> { return analyze(fs::from_utf8(*import_path)); }));
        });
    }
    {
        auto* lib = app.add_subcommand("lib", "Static libraries: name the functions the target took from them");
        lib->require_subcommand(1);
        auto* match = lib->add_subcommand("match", "Match libraries' functions against the target; matched ones are named and marked library");
        auto libraries = std::make_shared<std::vector<std::string>>();
        auto dry_run = std::make_shared<bool>(false);
        auto binary = std::make_shared<std::string>();
        match->add_option("libraries", *libraries, "Static libraries (.lib) the target was linked with, e.g. VC6's LIBC.LIB")->required();
        match->add_flag("--dry-run", *dry_run, "Report what would change, change nothing");
        match->add_option("--binary", *binary, "Match against this image instead of the project's target (reports only)");
        match->callback([&g, libraries, dry_run, binary] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                std::vector<std::filesystem::path> paths;
                for (const auto& l : *libraries) paths.push_back(fs::from_utf8(l));
                if (!binary->empty()) {
                    TRY_ASSIGN(auto program, Program::open(fs::from_utf8(*binary)));
                    std::vector<FunctionSignature> signatures;
                    for (const auto& path : paths) {
                        TRY_ASSIGN(auto sigs, library_signatures(path, program.arch()));
                        for (auto& s : sigs) signatures.push_back(std::move(s));
                    }
                    const auto matches = match_library_functions(program, signatures);
                    Json arr = Json::array();
                    for (const auto& m : matches) {
                        std::vector<std::string> names;
                        for (const auto* s : m.candidates)
                            if (std::ranges::find(names, s->name) == names.end()) names.push_back(s->name);
                        if (g.json) arr.push_back({{"va", m.va}, {"names", names}, {"matched", m.chosen != nullptr}});
                        else std::println("{:#010x} {} {}", m.va, m.chosen ? "matched  " : "ambiguous", join(names, ", "));
                    }
                    if (g.json) print_json(arr);
                    else std::println("{} library functions, {} of the target's functions fit them", signatures.size(), matches.size());
                    return 0;
                }
                TRY_ASSIGN(auto p, project::Project::find(g.project));
                TRY_ASSIGN(auto r, project::match_libraries(p, paths, !*dry_run));
                if (g.json) {
                    print_json(project::to_json(r));
                    return 0;
                }
                std::println("{} library functions; {} of the target's functions matched{}, {} ambiguous", r.signatures, r.matched,
                             *dry_run ? " (dry run: nothing changed)" : " and marked library", r.ambiguous);
                for (const auto& [lib, n] : r.by_library) std::println("  {:<24} {}", lib, n);
                if (r.resized || r.removed) std::println("  {} sizes corrected, {} false starts inside them removed", r.resized, r.removed);
                for (const auto& a : r.ambiguous_functions) std::println("  ambiguous {:#010x}: {}", a.va, join(a.names, ", "));
                return 0;
            }));
        });
        auto* list = lib->add_subcommand("list", "List a library's objects and the functions they define");
        auto list_path = std::make_shared<std::string>();
        list->add_option("library", *list_path, "A static or import library (.lib)")->required();
        list->callback([&g, list_path] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                TRY_ASSIGN(auto archive, archive::Archive::load(fs::from_utf8(*list_path)));
                Json arr = Json::array();
                usize imports = 0;
                for (const auto& m : archive.members()) {
                    if (m.import) {
                        ++imports;
                        if (g.json) arr.push_back({{"member", m.name}, {"import", m.import->symbol}, {"dll", m.import->dll}});
                        continue;
                    }
                    auto obj = coff::Object::parse(m.data);
                    std::vector<std::string> functions;
                    if (obj)
                        for (const auto* s : obj->function_symbols()) functions.push_back(s->name);
                    if (g.json) arr.push_back({{"member", m.name}, {"functions", functions}, {"coff", obj.has_value()}});
                    else std::println("{}{}  {}", m.name, obj ? "" : " (not a COFF object)", join(functions, " "));
                }
                if (g.json) print_json(arr);
                else if (imports) std::println("{} import objects", imports);
                return 0;
            }));
        });
    }
    {
        auto* cmd = app.add_subcommand("units", "Translation units: kind, source, functions and bytes matched, spend");
        auto* derive = cmd->add_subcommand("derive", "Derive the units from the PDB's modules, the link map's object files or the analysis");
        auto force = std::make_shared<bool>(false);
        derive->add_flag("--force", *force, "Derive them again although the project has units (units added by hand stay)");
        derive->callback([&g, force] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                TRY_ASSIGN(auto p, project::Project::find(g.project));
                TRY_ASSIGN(auto run_lock, p.try_lock_active_run());
                if (!run_lock) return make_error(ErrorCode::invalid_argument, "a run is active in this project; stop it before deriving units");
                TRY_ASSIGN(auto r, project::derive_project_units(p, *force));
                const auto& [derived, applied] = r;
                if (g.json) {
                    print_json({{"from", std::string(to_string(derived.from))},
                                {"units", applied.units},
                                {"functions", applied.functions},
                                {"unassigned", applied.unassigned}});
                    return 0;
                }
                std::println("{} units from the {}: {} functions in a unit, {} in none", applied.units,
                             derived.from == UnitOrigin::pdb ? "PDB" : derived.from == UnitOrigin::map ? "link map" : "analysis",
                             applied.functions, applied.unassigned);
                return 0;
            }));
        });
        auto* verify = cmd->add_subcommand("verify", "Compile the unit sources and diff every function they hold against the target");
        auto verify_units = std::make_shared<std::vector<std::string>>();
        auto verify_toolchain = std::make_shared<std::string>();
        verify->add_option("units", *verify_units, "Units to verify (default: every unit with a source)");
        verify->add_option("--toolchain", *verify_toolchain, "Toolchain to compile with (default: the project's)");
        verify->callback([&g, verify_units, verify_toolchain] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                TRY_ASSIGN(auto p, project::Project::find(g.project));
                TRY_ASSIGN(auto program, p.open_program());
                TRY_ASSIGN(auto setup, make_match_setup(g, *verify_toolchain, {}));
                TRY_ASSIGN(auto reports, project::verify_unit_sources(p, program, setup, *verify_units));
                bool all = true;
                Json arr = Json::array();
                for (const auto& r : reports) {
                    const auto& v = r.verification;
                    all = all && v.all_byte_exact();
                    usize exact = 0;
                    for (const auto& f : v.functions) exact += f.byte_exact() ? 1 : 0;
                    if (g.json) {
                        Json fns = Json::array();
                        for (const auto& f : v.functions)
                            fns.push_back({{"va", f.va},
                                           {"byte_exact", f.byte_exact()},
                                           {"match_percent", f.diff ? f.diff->match_percent : 0.0},
                                           {"error", f.error}});
                        arr.push_back({{"unit", r.unit.name}, {"source", r.unit.source}, {"error", v.error}, {"functions", fns}});
                        continue;
                    }
                    if (!v.error.empty()) {
                        std::println("{} ({}): {}", r.unit.name, r.unit.source, v.error);
                        for (const auto& d : v.compile.diagnostics)
                            if (d.severity.find("error") != std::string::npos) std::println("  {}:{}: {}", d.line, d.column, d.message);
                        continue;
                    }
                    std::println("{} ({}): {}/{} functions byte-exact", r.unit.name, r.unit.source, exact, v.functions.size());
                    for (const auto& f : v.functions) {
                        if (f.byte_exact()) continue;
                        const Symbol* s = program.symbols().at(f.va);
                        std::println("  {:#010x} {}: {}", f.va, s ? s->display : std::string(), f.diff ? matching::summary_line(*f.diff) : f.error);
                    }
                }
                if (g.json) print_json(arr);
                else if (reports.empty()) std::println("no unit sources yet: `decomp units emit` writes them from matched functions");
                return all ? 0 : 2;
            }));
        });
        auto* emit = cmd->add_subcommand("emit", "Move matched functions' own sources (src/functions/) into their units' sources");
        auto emit_units = std::make_shared<std::vector<std::string>>();
        auto emit_toolchain = std::make_shared<std::string>();
        emit->add_option("units", *emit_units, "Units to emit (default: every unit)");
        emit->add_option("--toolchain", *emit_toolchain, "Toolchain to compile with (default: the project's)");
        emit->callback([&g, emit_units, emit_toolchain] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                TRY_ASSIGN(auto p, project::Project::find(g.project));
                TRY_ASSIGN(auto run_lock, p.try_lock_active_run());
                if (!run_lock) return make_error(ErrorCode::invalid_argument, "a run is active in this project; stop it before emitting unit sources");
                TRY_ASSIGN(auto program, p.open_program());
                TRY_ASSIGN(auto setup, make_match_setup(g, *emit_toolchain, {}));
                TRY_ASSIGN(auto report, project::emit_unit_sources(p, program, setup,
                                                                   project::ChangeOrigin{SymbolSource::user, "", "emitted into the unit source"},
                                                                   *emit_units));
                TRY_ASSIGN(const auto units, project::load_units(p));
                if (g.json) {
                    Json arr = Json::array();
                    for (const auto& u : report.units) {
                        Json kept = Json::array();
                        for (const auto& [va, why] : u.kept) kept.push_back({{"va", va}, {"reason", why}});
                        arr.push_back({{"unit", u.name}, {"emitted", u.emitted}, {"kept", kept}});
                    }
                    print_json(arr);
                    return 0;
                }
                if (report.units.empty()) std::println("no matched function has its own source file in a unit with a source");
                for (const auto& u : report.units) {
                    auto unit = std::ranges::find(units, u.name, &Unit::name);
                    std::println("{}: {} function{} moved into {}{}", u.name, u.emitted.size(), u.emitted.size() == 1 ? "" : "s",
                                 unit != units.end() ? unit->source : std::string("its source"),
                                 u.kept.empty() ? "" : std::format("; {} kept in {} own file{}", u.kept.size(), u.kept.size() == 1 ? "its" : "their",
                                                                   u.kept.size() == 1 ? "" : "s"));
                    for (const auto& [va, why] : u.kept) {
                        const Symbol* s = program.symbols().at(va);
                        std::println("  {:#010x} {}: {}", va, s ? s->display : std::string(), why);
                    }
                }
                return 0;
            }));
        });
        cmd->callback([&g, cmd] {
            if (!cmd->get_subcommands().empty()) return;
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                TRY_ASSIGN(auto p, project::Project::find(g.project));
                TRY_ASSIGN(auto units, project::load_units(p));
                TRY_ASSIGN(auto program, p.open_program());
                const auto progress = project::compute_unit_progress(units, program.symbols(), *p.function_infos());
                if (g.json) {
                    Json arr = Json::array();
                    for (const auto& u : progress) arr.push_back(project::to_json(u));
                    print_json(arr);
                    return 0;
                }
                if (units.empty()) {
                    std::println("no units yet: `decomp units derive` finds them");
                    return 0;
                }
                std::println("{:<28} {:<8} {:>11} {:>17} {:>7} {:>9}  {}", "unit", "kind", "functions", "bytes", "matched", "spend", "source");
                for (const auto& u : progress) {
                    if (u.unit.name.empty() && !u.functions) continue;
                    std::println("{:<28} {:<8} {:>11} {:>17} {:>6.1f}% {:>9}  {}", u.unit.name.empty() ? "(no unit)" : u.unit.name,
                                 u.unit.name.empty() ? "" : to_string(u.unit.kind), std::format("{}/{}", u.matched, u.functions),
                                 std::format("{}/{}", u.matched_bytes, u.bytes), u.percent_bytes(), std::format("${:.2f}", u.cost_usd),
                                 u.unit.source);
                }
                return 0;
            }));
        });
    }
    {
        auto* cmd = app.add_subcommand("status", "Progress summary: functions and code bytes matched, status buckets, spend, per unit");
        cmd->callback([&g] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                TRY_ASSIGN(auto p, project::Project::find(g.project));
                TRY_ASSIGN(auto program, p.open_program());
                const auto progress = project::compute_progress(program.symbols(), p);
                TRY_ASSIGN(const auto units, project::load_units(p));
                const auto unit_progress = project::compute_unit_progress(units, program.symbols(), *p.function_infos());
                if (g.json) {
                    Json j = project::to_json(progress);
                    Json per_unit = Json::array();
                    for (const auto& u : unit_progress)
                        if (!u.unit.name.empty() || u.functions) per_unit.push_back(project::to_json(u));
                    j["units"] = std::move(per_unit);
                    print_json(j);
                    return 0;
                }
                std::println("{}  ({})", p.config().target, fs::to_utf8(p.root()));
                std::println("  matched     {}/{} functions ({:.1f}%), {}/{} code bytes ({:.1f}%)", progress.matched_functions,
                             progress.functions, progress.percent_functions(), progress.matched_bytes, progress.code_bytes,
                             progress.percent_bytes());
                for (const auto& [st, b] : progress.buckets)
                    std::println("  {:<12}{:6} functions {:8} bytes", project::to_string(st), b.functions, b.bytes);
                std::println("  spend       ${:.2f}", progress.spend_usd);
                // Per unit, in link order: the units with functions (`decomp units` lists them all).
                if (!units.empty()) {
                    std::println("  {:<28} {:>11} {:>17} {:>8} {:>9}", "unit", "functions", "bytes", "matched", "spend");
                    for (const auto& u : unit_progress) {
                        if (u.functions == 0) continue;
                        std::println("  {:<28} {:>11} {:>17} {:>7.1f}% {:>9}", u.unit.name.empty() ? "(no unit)" : u.unit.name,
                                     std::format("{}/{}", u.matched, u.functions), std::format("{}/{}", u.matched_bytes, u.bytes),
                                     u.percent_bytes(), std::format("${:.2f}", u.cost_usd));
                    }
                }
                return 0;
            }));
        });
    }
}

} // namespace decomp::cli
