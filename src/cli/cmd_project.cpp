#include "cli/common.hpp"
#include "core/fs.hpp"
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
                std::map<project::FunctionStatus, std::pair<usize, u64>> buckets;
                u64 total_bytes = 0, matched_bytes = 0;
                usize total = 0, matched = 0;
                double spend = 0;
                for (const auto* f : program.symbols().functions()) {
                    auto info = p.function_info(f->va);
                    auto& b = buckets[info.status];
                    ++b.first;
                    b.second += f->size;
                    ++total;
                    total_bytes += f->size;
                    spend += info.cost_usd;
                    if (info.status == project::FunctionStatus::matched) {
                        ++matched;
                        matched_bytes += f->size;
                    }
                }
                double pct_fn = total ? 100.0 * matched / total : 0, pct_bytes = total_bytes ? 100.0 * matched_bytes / total_bytes : 0;
                if (g.json) {
                    Json jb = Json::object();
                    for (auto& [st, v] : buckets) jb[std::string(project::to_string(st))] = {{"functions", v.first}, {"bytes", v.second}};
                    print_json({{"functions", total}, {"matched_functions", matched}, {"code_bytes", total_bytes},
                                {"matched_bytes", matched_bytes}, {"percent_functions", pct_fn},
                                {"percent_bytes", pct_bytes}, {"buckets", jb}, {"spend_usd", spend}});
                    return 0;
                }
                std::println("{}  ({})", p.config().target, fs::to_utf8(p.root()));
                std::println("  matched     {}/{} functions ({:.1f}%), {}/{} code bytes ({:.1f}%)", matched, total, pct_fn,
                             matched_bytes, total_bytes, pct_bytes);
                for (auto& [st, v] : buckets) std::println("  {:<12}{:6} functions {:8} bytes", project::to_string(st), v.first, v.second);
                std::println("  spend       ${:.2f}", spend);
                return 0;
            }));
        });
    }
}

} // namespace decomp::cli
