#include "analysis/annotate.hpp"
#include "analysis/demangle.hpp"
#include "cli/common.hpp"
#include "core/strings.hpp"

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
        rich.push_back({{"product_id", r.product_id}, {"build", r.build}, {"count", r.count},
                        {"description", pe::describe_rich_product(r.product_id).value_or("")}});
    j["rich"] = rich;
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
                    std::println("  rich        id {:#06x} build {:5} x{:<4} {}", r.product_id, r.build, r.count, pe::describe_rich_product(r.product_id).value_or("?"));
                std::println("  symbols     {} ({} functions)", p.symbols().size(), p.symbols().functions().size());
                return 0;
            }));
        });
    }
    {
        auto* cmd = app.add_subcommand("funcs", "List functions known from symbols, exports and unwind data");
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
