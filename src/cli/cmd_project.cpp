#include "cli/common.hpp"
#include "core/fs.hpp"
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
        auto toolchain = std::make_shared<std::string>();
        auto flags = std::make_shared<std::vector<std::string>>();
        cmd->add_option("binary", *binary, "Target PE image")->required();
        cmd->add_option("--dir", *dir, "Project directory (default: -C, else the current directory)");
        cmd->add_option("--pdb", *pdb, "PDB for the target, if not next to it");
        cmd->add_option("--toolchain", *toolchain, "Toolchain name from the registry (see `decomp toolchain list`)");
        cmd->add_option("--flag", *flags, "Compiler flag the target was built with, e.g. /O2 (repeatable)")->allow_extra_args(false);
        cmd->callback([&g, binary, dir, pdb, toolchain, flags] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                std::optional<std::filesystem::path> pdb_path;
                if (!pdb->empty()) pdb_path = fs::from_utf8(*pdb);
                const std::string root = !dir->empty() ? *dir : !g.project.empty() ? g.project : std::string(".");
                TRY_ASSIGN(auto p, project::Project::init(fs::from_utf8(root), fs::from_utf8(*binary), pdb_path, *toolchain));
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
