// decomp types: the program's types. The project's headers under include/ are the source of truth;
// compiled with the project's toolchain, their layouts read back are compared with the target's PDB.

#include "cli/common.hpp"
#include "core/strings.hpp"
#include "project/project.hpp"
#include "project/setup.hpp"
#include "project/types.hpp"

#include <format>
#include <memory>
#include <print>
#include <string>
#include <vector>

namespace decomp::cli {

namespace {

std::string summary(const TypeLayout& layout) {
    return std::format("{} {}, {} byte{}", to_string(layout.kind), layout.name, layout.size, layout.size == 1 ? "" : "s");
}

// The project, its program and its header types.
struct TypesContext {
    project::Project project;
    Program program;
    project::HeaderTypes headers;
};

Result<TypesContext> open_types(const GlobalOptions& g) {
    TRY_ASSIGN(auto p, project::Project::find(g.project));
    TRY_ASSIGN(auto program, p.open_program());
    TRY_ASSIGN(const auto setup, project::make_match_setup(&p, ""));
    TRY_ASSIGN(auto headers, project::compile_header_types(p, setup, program.arch()));
    return TypesContext{std::move(p), std::move(program), std::move(headers)};
}

} // namespace

void register_types_commands(CLI::App& app, GlobalOptions& g) {
    auto* cmd = app.add_subcommand("types", "The program's types: the project's headers as its compiler lays them out, and the target's PDB");
    auto pdb = std::make_shared<bool>(false);
    auto filter = std::make_shared<std::string>();
    cmd->add_flag("--pdb", *pdb, "List the types the target's PDB defines instead");
    cmd->add_option("--filter", *filter, "Only types whose name contains this text");

    auto* show = cmd->add_subcommand("show", "A type's layout: from the project's headers, else (or with --pdb) from the target's PDB");
    auto name = std::make_shared<std::string>();
    auto show_pdb = std::make_shared<bool>(false);
    show->add_option("name", *name, "The type's name (\"Player\", \"game::Shape\")")->required();
    show->add_flag("--pdb", *show_pdb, "The PDB's layout, not the headers'");
    show->callback([&g, name, show_pdb] {
        throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
            TRY_ASSIGN(auto ctx, open_types(g));
            const TypeLayout* header = *show_pdb ? nullptr : ctx.headers.catalog.find(*name);
            const TypeLayout* target = ctx.program.pdb_types().catalog.find(*name);
            const TypeLayout* shown = header ? header : target;
            if (!shown)
                return make_error(ErrorCode::not_found, "no type {} in {}", *name,
                                  *show_pdb ? "the target's PDB" : "the project's headers or the target's PDB");
            const auto differences = header && target ? compare_layouts(*header, *target) : std::vector<std::string>{};
            if (g.json) {
                Json j = to_json(*shown);
                j["source"] = header ? "headers" : "pdb";
                if (const auto* declared = ctx.headers.header_of(*name); declared && header) j["header"] = declared->header;
                if (header && target) j["pdb_differences"] = differences;
                print_json(j);
                return 0;
            }
            if (const auto* declared = ctx.headers.header_of(*name); declared && header) std::println("// {}", declared->header);
            else if (!header) std::println("// the target's PDB");
            std::print("{}", to_text(*shown));
            if (header && target) {
                if (differences.empty()) std::println("// the same in the target's PDB");
                else {
                    std::println("// differs from the target's PDB:");
                    for (const auto& d : differences) std::println("//   {}", d);
                }
            }
            return 0;
        }));
    });

    auto* check = cmd->add_subcommand("check", "Compare the layout of each type the project's headers declare with the target's PDB");
    check->callback([&g] {
        throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
            TRY_ASSIGN(auto ctx, open_types(g));
            const TypeCatalog& target = ctx.program.pdb_types().catalog;
            if (target.empty()) return make_error(ErrorCode::not_found, "the target has no PDB with types to compare with");
            usize equal = 0, differ = 0, absent = 0, undefined = 0;
            Json arr = Json::array();
            for (const auto& declared : ctx.headers.declared) {
                const TypeLayout* header = ctx.headers.catalog.find(declared.name);
                const TypeLayout* expected = target.find(declared.name);
                Json j = {{"name", declared.name}, {"header", declared.header}};
                if (!header) {
                    // A typedef of a built-in type, or a type declared but not defined.
                    ++undefined;
                    j["status"] = "no layout";
                } else if (!expected) {
                    ++absent;
                    j["status"] = "not in the PDB";
                } else if (const auto differences = compare_layouts(*header, *expected); differences.empty()) {
                    ++equal;
                    j["status"] = "equal";
                } else {
                    ++differ;
                    j["status"] = "differs";
                    j["differences"] = differences;
                }
                if (!g.json) {
                    std::println("{:<14} {}  ({})", j["status"].get<std::string>(), declared.name, declared.header);
                    if (j.contains("differences"))
                        for (const auto& d : j["differences"]) std::println("    {}", d.get<std::string>());
                }
                arr.push_back(std::move(j));
            }
            if (g.json) print_json({{"types", arr}, {"equal", equal}, {"differ", differ}, {"not_in_pdb", absent}, {"no_layout", undefined}});
            else
                std::println("{} equal, {} differ, {} not in the PDB, {} without a layout (of {} declared in {})", equal, differ, absent, undefined,
                             ctx.headers.declared.size(), "include/");
            return differ > 0 ? 1 : 0;
        }));
    });

    cmd->callback([&g, cmd, pdb, filter] {
        if (!cmd->get_subcommands().empty()) return;
        throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
            if (*pdb) {
                TRY_ASSIGN(auto program, open_program(g, ""));
                const TypeCatalog& catalog = program.pdb_types().catalog;
                if (catalog.empty()) return make_error(ErrorCode::not_found, "the target has no PDB with types");
                Json arr = Json::array();
                for (const TypeLayout& t : catalog.types()) {
                    if (!filter->empty() && t.name.find(*filter) == std::string::npos) continue;
                    if (g.json) arr.push_back({{"name", t.name}, {"kind", to_string(t.kind)}, {"size", t.size}});
                    else std::println("{}", summary(t));
                }
                if (g.json) print_json(arr);
                return 0;
            }
            TRY_ASSIGN(auto ctx, open_types(g));
            const TypeCatalog& target = ctx.program.pdb_types().catalog;
            Json arr = Json::array();
            if (ctx.headers.declared.empty() && !g.json) std::println("the project's headers (include/) declare no types");
            for (const auto& declared : ctx.headers.declared) {
                if (!filter->empty() && declared.name.find(*filter) == std::string::npos) continue;
                const TypeLayout* layout = ctx.headers.catalog.find(declared.name);
                const TypeLayout* expected = target.find(declared.name);
                std::string pdb_status = target.empty() ? "" : !layout || !expected ? "not in the PDB" : compare_layouts(*layout, *expected).empty() ? "= PDB" : "differs from the PDB";
                if (g.json) {
                    Json j = {{"name", declared.name}, {"header", declared.header}};
                    if (layout) {
                        j["kind"] = to_string(layout->kind);
                        j["size"] = layout->size;
                    }
                    if (!pdb_status.empty()) j["pdb"] = pdb_status;
                    arr.push_back(std::move(j));
                    continue;
                }
                std::println("{:<40} {:<24} {}{}", layout ? summary(*layout) : declared.name + " (no layout)", declared.header,
                             pdb_status.empty() ? "" : "  ", pdb_status);
            }
            if (g.json) print_json(arr);
            return 0;
        }));
    });
}

} // namespace decomp::cli
