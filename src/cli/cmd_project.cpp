#include "cli/common.hpp"
#include "analysis/signatures.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "formats/archive.hpp"
#include "formats/coff.hpp"
#include "project/analyze.hpp"
#include "project/library.hpp"
#include "project/progress.hpp"
#include "project/project.hpp"

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
        auto* cmd = app.add_subcommand("status", "Progress summary: functions and code bytes matched, status buckets, spend");
        cmd->callback([&g] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                TRY_ASSIGN(auto p, project::Project::find(g.project));
                TRY_ASSIGN(auto program, p.open_program());
                const auto progress = project::compute_progress(program.symbols(), p);
                if (g.json) {
                    print_json(project::to_json(progress));
                    return 0;
                }
                std::println("{}  ({})", p.config().target, fs::to_utf8(p.root()));
                std::println("  matched     {}/{} functions ({:.1f}%), {}/{} code bytes ({:.1f}%)", progress.matched_functions,
                             progress.functions, progress.percent_functions(), progress.matched_bytes, progress.code_bytes,
                             progress.percent_bytes());
                for (const auto& [st, b] : progress.buckets)
                    std::println("  {:<12}{:6} functions {:8} bytes", project::to_string(st), b.functions, b.bytes);
                std::println("  spend       ${:.2f}", progress.spend_usd);
                return 0;
            }));
        });
    }
}

} // namespace decomp::cli
