#include "analysis/annotate.hpp"
#include "analysis/bounds.hpp"
#include "analysis/demangle.hpp"
#include "cli/common.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "matching/suggest.hpp"
#include "project/project.hpp"

#include <format>
#include <print>
#include <regex>

namespace decomp::cli {

namespace {

Json info_json(const Program& p) {
    const auto& img = p.image();
    Json j;
    j["path"] = p.path().string();
    j["format"] = img.is_pe32_plus() ? "PE32+" : "PE32";
    j["arch"] = std::string(to_string(img.arch()));
    j["image_base"] = img.image_base();
    j["entry_point"] = img.entry_point();
    j["timestamp"] = img.timestamp();
    j["dll"] = img.is_dll();
    j["relocations"] = img.base_relocations().size();
    j["relocs_stripped"] = img.relocs_stripped();
    j["linker_version"] = std::format("{}.{:02}", img.linker_major(), img.linker_minor());
    Json sections = Json::array();
    for (const auto& s : img.image_sections())
        sections.push_back({{"name", s.name}, {"va", s.va}, {"size", s.virtual_size}, {"executable", s.executable}, {"writable", s.writable}});
    j["sections"] = sections;
    j["imports"] = img.imports().size();
    j["exports"] = img.exports().size();
    if (const auto& cv = img.codeview()) j["codeview"] = {{"pdb", cv->pdb_path}, {"guid", cv->guid_string()}, {"age", cv->age}};
    j["pdb_loaded"] = p.pdb_path() ? Json(p.pdb_path()->string()) : Json(nullptr);
    Json rich = Json::array();
    for (const auto& r : img.rich_entries())
        rich.push_back({{"product_id", r.product_id}, {"build", r.build}, {"count", r.count}, {"description", pe::describe_rich_product(r.product_id)}});
    j["rich"] = rich;
    j["build"] = nullptr;
    j["suggested_toolchain"] = nullptr;
    if (const auto build = img.build_info()) {
        j["build"] = matching::to_json(*build);
        if (const auto s = matching::suggest_toolchain(*build)) j["suggested_toolchain"] = matching::to_json(*s);
    }
    usize functions = p.symbols().functions().size();
    j["symbols"] = p.symbols().size();
    j["functions"] = functions;
    return j;
}

} // namespace

void register_analysis_commands(CLI::App& app, GlobalOptions& g) {
    {
        auto* cmd = app.add_subcommand("info", "Show the target binary's format, sections, debug info and compiler hints");
        auto binary = std::make_shared<std::string>();
        auto pdb = std::make_shared<std::string>();
        cmd->add_option("binary", *binary, "PE image (default: the project's target)");
        cmd->add_option("--pdb", *pdb, "PDB to use instead of the one found next to the image");
        cmd->callback([&g, binary, pdb] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                TRY_ASSIGN(auto p, open_program(g, *binary, *pdb));
                auto j = info_json(p);
                if (g.json) {
                    print_json(j);
                    return 0;
                }
                const auto& img = p.image();
                std::println("{}", p.path().string());
                std::println("  format      {} {} ({})", j["format"].get<std::string>(), j["arch"].get<std::string>(), img.is_dll() ? "DLL" : "EXE");
                std::println("  image base  {:#x}   entry {:#x}   linker {}", img.image_base(), img.entry_point(), j["linker_version"].get<std::string>());
                std::println("  relocations {}{}", img.base_relocations().size(), img.relocs_stripped() ? " (stripped)" : "");
                for (const auto& s : img.image_sections())
                    std::println("  section     {:<8} {:#010x} size {:#x}{}{}", s.name, s.va, s.virtual_size, s.executable ? " code" : "", s.writable ? " writable" : "");
                std::println("  imports     {}   exports {}", img.imports().size(), img.exports().size());
                if (const auto& cv = img.codeview()) std::println("  codeview    {} {} age {}", cv->pdb_path, cv->guid_string(), cv->age);
                std::println("  pdb         {}", p.pdb_path() ? p.pdb_path()->string() : std::string("(not loaded)"));
                for (const auto& r : img.rich_entries())
                    std::println("  rich        id {:#06x} build {:5} x{:<4} {}", r.product_id, r.build, r.count, pe::describe_rich_product(r.product_id));
                if (const auto build = img.build_info()) {
                    if (!build->checksum_ok) std::println("  rich        the checksum does not match: edited after linking");
                    if (const auto s = matching::suggest_toolchain(*build)) {
                        std::println("  built with  {}: {}", s->visual_studio, s->compiler);
                        if (build->linker) std::println("  linked by   {}", build->linker->description());
                        if (!s->name.empty()) std::println("  toolchain   register it as \"{}\" (`decomp toolchain add {} --kind msvc ...`)", s->name, s->name);
                        for (const auto& note : s->notes) std::println("  note        {}", note);
                    }
                } else {
                    std::println("  rich        none (not linked by Microsoft's linker)");
                }
                std::println("  symbols     {} ({} functions)", p.symbols().size(), p.symbols().functions().size());
                return 0;
            }));
        });
    }
    {
        auto* cmd = app.add_subcommand("funcs", "List the functions: from the PDB, or found by analysis without one");
        auto binary = std::make_shared<std::string>();
        auto filter = std::make_shared<std::string>();
        cmd->add_option("binary", *binary, "PE image (default: the project's target)");
        cmd->add_option("--filter", *filter, "Regular expression matched against names");
        cmd->callback([&g, binary, filter] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                TRY_ASSIGN(auto p, open_program(g, *binary));
                std::optional<std::regex> re;
                if (!filter->empty()) {
                    try {
                        re.emplace(*filter, std::regex::icase);
                    } catch (const std::regex_error& e) {
                        return make_error(ErrorCode::invalid_argument, "bad --filter: {}", e.what());
                    }
                }
                Json arr = Json::array();
                for (const auto* f : p.symbols().functions()) {
                    if (re && !std::regex_search(f->name, *re) && !std::regex_search(f->display, *re)) continue;
                    if (g.json) {
                        arr.push_back({{"va", f->va}, {"name", f->name}, {"display", f->display}, {"size", f->size},
                                       {"source", std::string(to_string(f->source))}, {"static", f->is_static}});
                    } else {
                        std::println("{:#010x} {:6} {:<10} {}", f->va, f->size, to_string(f->source), f->display);
                    }
                }
                if (g.json) print_json(arr);
                return 0;
            }));
        });
    }
    {
        auto* cmd = app.add_subcommand("bounds", "Measure function bounds against the build's PDB or map file");
        auto binary = std::make_shared<std::string>();
        auto truth = std::make_shared<std::string>();
        auto errors = std::make_shared<usize>(20);
        auto min_exact = std::make_shared<double>(0.0);
        cmd->add_option("binary", *binary, "PE image, analyzed as if it had no PDB (default: the project's functions)");
        cmd->add_option("--truth", *truth, "The build's PDB (procedures with sizes) or link map (starts)")->required();
        cmd->add_option("--errors", *errors, "Mismatches to list (default 20)");
        cmd->add_option("--min-exact", *min_exact, "Exit with code 2 when fewer than this percentage of bounds are exact");
        cmd->callback([&g, binary, truth, errors, min_exact] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                std::optional<Program> program;
                if (!binary->empty()) {
                    OpenOptions options;
                    options.use_pdb = false;
                    TRY_ASSIGN(auto p, Program::open(fs::from_utf8(*binary), options));
                    program = std::move(p);
                } else {
                    TRY_ASSIGN(auto project, project::Project::find(g.project));
                    TRY_ASSIGN(auto p, project.open_program());
                    program = std::move(p);
                }
                const auto truth_path = fs::from_utf8(*truth);
                std::string ext = to_lower(fs::to_utf8(truth_path.extension()));
                Result<std::vector<FunctionBounds>> expected = make_error(ErrorCode::invalid_argument, "--truth must be a .pdb or a .map file");
                if (ext == ".pdb") expected = pdb_function_bounds(truth_path, program->image().image_base(), program->image());
                else if (ext == ".map") expected = map_function_bounds(truth_path, program->image());
                if (!expected) return std::unexpected(expected.error());
                const auto found = function_bounds(program->symbols(), program->image());
                const auto c = compare_bounds(*expected, found, program->image(), program->decoder());
                if (g.json) {
                    print_json(to_json(c, *errors));
                } else {
                    std::println("{} functions in the truth ({}), {} found", c.truth, c.truth_has_ends ? "starts and ends" : "starts only", c.found);
                    std::println("  exact      {:6} ({:.1f}%)", c.exact, c.exact_rate() * 100.0);
                    std::println("  start only {:6}", c.start_only);
                    std::println("  missed     {:6}", c.missed);
                    std::println("  extra      {:6}", c.extra);
                    usize shown = 0;
                    for (const auto& m : c.mismatches) {
                        if (shown++ >= *errors) {
                            std::println("  ... {} more", c.mismatches.size() - *errors);
                            break;
                        }
                        std::string ends;
                        if (m.kind == BoundsMismatch::Kind::wrong_end)
                            ends = m.truth_end ? std::format(" ends {:#x}, found {:#x}", m.truth_end, m.found_end) : std::format(" found end {:#x}", m.found_end);
                        else if (m.kind == BoundsMismatch::Kind::missed && m.truth_end)
                            ends = std::format(" ends {:#x}", m.truth_end);
                        else if (m.kind == BoundsMismatch::Kind::extra)
                            ends = std::format(" to {:#x}", m.found_end);
                        std::println("  {:<9} {:#010x}{} {}", to_string(m.kind), m.start, ends, m.name);
                    }
                }
                return c.exact_rate() * 100.0 + 1e-9 < *min_exact ? 2 : 0;
            }));
        });
    }
    {
        auto* cmd = app.add_subcommand("disasm", "Annotated disassembly of a function");
        auto binary = std::make_shared<std::string>();
        auto function = std::make_shared<std::string>();
        auto no_bytes = std::make_shared<bool>(false);
        cmd->add_option("function", *function, "Function name or address")->required();
        cmd->add_option("binary", *binary, "PE image (default: the project's target)");
        cmd->add_flag("--no-bytes", *no_bytes, "Omit instruction bytes");
        cmd->callback([&g, binary, function, no_bytes] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                TRY_ASSIGN(auto p, open_program(g, *binary));
                TRY_ASSIGN(u64 va, resolve_function(p, *function));
                TRY_ASSIGN(auto fn, annotate_function(p, va));
                if (g.json) print_json(to_json(fn));
                else std::print("{}", to_text(fn, !*no_bytes));
                return 0;
            }));
        });
    }
}

} // namespace decomp::cli
