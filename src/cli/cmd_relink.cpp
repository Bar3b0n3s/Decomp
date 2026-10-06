// decomp relink and decomp units check: whole-program verification. Units whose sources compile to their
// code and data are linked from those sources, the rest from split objects of the original bytes, by the
// original linker; the result is compared with the target.

#include "cli/common.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "project/project.hpp"
#include "project/relink.hpp"

#include <format>
#include <memory>
#include <print>
#include <string>
#include <vector>

namespace decomp::cli {

namespace {

void print_check(const Program& program, const project::UnitSourceCheck& c) {
    std::println("{} ({}): {}", c.unit.name, c.unit.source.empty() ? "no source" : c.unit.source, c.summary());
    if (!c.error.empty())
        for (const auto& d : c.compile.diagnostics)
            if (d.severity.find("error") != std::string::npos) std::println("  {}:{}: {}", d.line, d.column, d.message);
    if (!c.missing_functions.empty()) {
        std::vector<std::string> names;
        for (u64 va : c.missing_functions) {
            const auto* s = program.symbols().at(va);
            names.push_back(s ? (s->display.empty() ? s->name : s->display) : std::format("{:#x}", va));
        }
        std::println("  not in the source: {}", join(names, ", "));
        return;
    }
    if (!c.check) return;
    for (const auto& s : c.check->sections) {
        if (s.state == matching::PlacementState::equal) continue;
        std::string where = s.rva ? std::format(" at {:#x}", *s.rva) : std::string();
        std::string detail = s.state != matching::PlacementState::discarded ? s.note
                             : !s.folded_into.empty()                          ? "folded into " + s.folded_into
                                                                               : std::format("the image has {}'s copy", s.unit);
        std::println("  {} {}{}: {}{}", s.name, s.symbol, where, to_string(s.state), detail.empty() ? "" : " (" + detail + ")");
    }
    for (const auto& m : c.check->missing) std::println("  missing: {} at {:#x} ({} bytes)", m.name, m.rva, m.size);
    for (const auto& p : c.check->problems) std::println("  {}", p);
}

} // namespace

void register_relink_commands(CLI::App& app, GlobalOptions& g) {
    auto* cmd = app.add_subcommand("relink", "Link the target again from the units' sources and the original bytes, and compare");
    auto source = std::make_shared<std::vector<std::string>>();
    auto split = std::make_shared<std::vector<std::string>>();
    auto all_split = std::make_shared<bool>(false);
    auto toolchain = std::make_shared<std::string>();
    cmd->add_option("--source", *source, "Link this unit from its source even when its check fails (repeatable)");
    cmd->add_option("--split", *split, "Carry this unit's original bytes even when its source checks (repeatable)");
    cmd->add_flag("--all-split", *all_split, "Carry every unit's original bytes (tests the relink itself)");
    cmd->add_option("--toolchain", *toolchain, "Toolchain to compile and link with (default: the project's)");
    cmd->callback([&g, source, split, all_split, toolchain] {
        throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
            TRY_ASSIGN(auto p, project::Project::find(g.project));
            TRY_ASSIGN(auto program, p.open_program());
            TRY_ASSIGN(auto setup, make_match_setup(g, *toolchain, {}));
            project::RelinkOptions options;
            options.source = *source;
            options.split = *split;
            options.all_split = *all_split;
            if (!g.json && !g.quiet) options.progress = [](const std::string& line) { std::println("{}", line); };
            TRY_ASSIGN(auto result, project::relink_project(p, program, setup, options));
            if (g.json) {
                print_json(project::to_json(result));
                return result.identical() ? 0 : 2;
            }
            for (const auto& u : result.units) {
                if (u.mode == project::LinkMode::linker) continue;
                std::println("  {:<28} {:<7} {}", u.unit.name.empty() ? "(no unit)" : u.unit.name, to_string(u.mode), u.reason);
            }
            for (const auto& n : result.notes) std::println("note: {}", n);
            std::println("linker: {}", result.linker.text);
            if (!result.link.ok) {
                std::println("{}:\n{}", result.error, result.link.output);
                return 2;
            }
            const auto& c = *result.comparison;
            std::vector<std::string> stamped;
            for (const auto& s : c.stamped) stamped.push_back(s.name);
            if (c.identical) {
                std::println("identical to the target: SHA-1 {} ({} units from source, {} split){}", c.relinked_sha1,
                             result.count(project::LinkMode::source), result.count(project::LinkMode::split),
                             stamped.empty() ? std::string() : std::format("; taken over from the original: {}", join(stamped, ", ")));
                return 0;
            }
            std::println("differs from the target: SHA-1 {} (the target's {}), {} bytes differ", c.relinked_sha1, c.original_sha1, c.differing_bytes);
            if (c.first)
                std::println("first difference: {}{}{} ({:#x}): {} -> {}", c.first->where, c.first->unit.empty() ? "" : " in " + c.first->unit,
                             c.first->symbol.empty() ? "" : ", " + c.first->symbol, program.image().image_base() + c.first->rva.value_or(0),
                             c.first->original_bytes, c.first->relinked_bytes);
            for (const auto& s : c.sections)
                std::println("  {}: {} bytes differ{}{}", s.name, s.differing_bytes,
                             s.first_rva ? std::format(", the first at {:#x}", program.image().image_base() + *s.first_rva) : "",
                             s.size_differs ? " (its size or place differs)" : "");
            for (const auto& d : c.differences)
                if (!d.rva) std::println("  {}: {} -> {}", d.where, d.original_bytes, d.relinked_bytes);
            return 2;
        }));
    });

    auto* units = app.get_subcommand("units");
    auto* compose = units->add_subcommand("compose", "Make a unit's source from a whole translation unit: every function of the unit after its marker");
    auto compose_unit = std::make_shared<std::string>();
    auto compose_file = std::make_shared<std::string>();
    auto compose_toolchain = std::make_shared<std::string>();
    auto compose_dry_run = std::make_shared<bool>(false);
    compose->add_option("unit", *compose_unit, "The unit")->required();
    compose->add_option("file", *compose_file, "The translation unit (a C or C++ file)")->required();
    compose->add_option("--toolchain", *compose_toolchain, "Toolchain to compile with (default: the project's)");
    compose->add_flag("--dry-run", *compose_dry_run, "Check it, but write nothing");
    compose->callback([&g, compose_unit, compose_file, compose_toolchain, compose_dry_run] {
        throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
            TRY_ASSIGN(auto p, project::Project::find(g.project));
            TRY_ASSIGN(auto program, p.open_program());
            TRY_ASSIGN(auto setup, make_match_setup(g, *compose_toolchain, {}));
            TRY_ASSIGN(const auto text, fs::read_text(fs::from_utf8(*compose_file)));
            TRY_ASSIGN(auto composed, project::compose_unit_source(p, program, setup, *compose_unit, text));
            if (!*compose_dry_run) {
                TRY_ASSIGN(auto run_lock, p.try_lock_active_run());
                if (!run_lock) return make_error(ErrorCode::invalid_argument, "a run is active in this project; compose units when it is done");
                project::ChangeSubject subject;
                subject.unit = composed.unit.name;
                subject.functions = composed.check.functions;
                TRY(p.write_project_file(fs::from_utf8(composed.unit.source), composed.content,
                                         project::ChangeOrigin{SymbolSource::user, "", "composed from " + *compose_file}, subject));
                // The functions the unit's check found in place are matched.
                if (composed.check.complete())
                    TRY(p.modify_functions(composed.check.functions,
                                           [](u64, project::FunctionInfo& info) { info.status = project::FunctionStatus::matched; }));
            }
            if (g.json) {
                Json rejected = Json::array();
                for (const auto& [va, why] : composed.rejected) rejected.push_back({{"va", va}, {"reason", why}});
                print_json({{"unit", composed.unit.name}, {"source", composed.unit.source}, {"written", !*compose_dry_run},
                            {"rejected", std::move(rejected)}, {"check", project::to_json(composed.check)}});
            } else {
                // Compiler-made functions (SEH filters, funclets) have no definition but come out of the source anyway.
                for (const auto& [va, why] : composed.rejected)
                    if (std::ranges::find(composed.check.missing_functions, va) != composed.check.missing_functions.end())
                        std::println("  {:#010x}: {}", va, why);
                std::println("{} {}: {}", *compose_dry_run ? "would write" : "wrote", composed.unit.source, composed.check.summary());
            }
            return composed.check.complete() ? 0 : 2;
        }));
    });
    auto* check = units->add_subcommand("check", "Compile the unit sources and compare their code and data with the target's");
    auto check_units = std::make_shared<std::vector<std::string>>();
    auto check_toolchain = std::make_shared<std::string>();
    check->add_option("units", *check_units, "Units to check (default: every unit with a source)");
    check->add_option("--toolchain", *check_toolchain, "Toolchain to compile with (default: the project's)");
    check->callback([&g, check_units, check_toolchain] {
        throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
            TRY_ASSIGN(auto p, project::Project::find(g.project));
            TRY_ASSIGN(auto program, p.open_program());
            TRY_ASSIGN(auto setup, make_match_setup(g, *check_toolchain, {}));
            TRY_ASSIGN(auto checks, project::check_unit_sources(p, program, setup, *check_units));
            bool all = true;
            Json arr = Json::array();
            for (const auto& c : checks) {
                all = all && c.complete();
                if (g.json) arr.push_back(project::to_json(c));
                else print_check(program, c);
            }
            if (g.json) print_json(arr);
            else if (checks.empty()) std::println("no unit sources yet: matches go into them, and `decomp units emit` moves older ones there");
            return all ? 0 : 2;
        }));
    });
}

} // namespace decomp::cli
