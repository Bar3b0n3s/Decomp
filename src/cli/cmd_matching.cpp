#include "cli/common.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "matching/diff.hpp"
#include "matching/match.hpp"
#include "matching/toolchain.hpp"
#include "project/project.hpp"

#include <print>

namespace decomp::cli {

void register_toolchain_commands(CLI::App& app, GlobalOptions& g);

namespace {

struct DiffArgs {
    std::string function, binary, obj, symbol, source, toolchain;
    std::vector<std::string> flags;
    bool compact = false, bytes = false, all = false;
    usize context = 3;
};

int print_diff(const GlobalOptions& g, const matching::FunctionDiff& d, const DiffArgs& a) {
    matching::ReportOptions ro;
    ro.compact = a.compact;
    ro.context = a.context;
    ro.bytes = a.bytes;
    ro.color = !g.json && is_tty(stdout);
    if (g.json) print_json(matching::to_json(d, ro));
    else std::print("{}", matching::to_text(d, ro));
    return d.byte_exact ? 0 : 2;
}

} // namespace

void register_toolchain_commands(CLI::App& app, GlobalOptions& g) {
    auto* tc = app.add_subcommand("toolchain", "Manage the compilers used to build candidates");
    tc->require_subcommand(1);
    tc->add_subcommand("list", "List configured and auto-detected toolchains")->callback([&g] {
        throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
            TRY_ASSIGN(auto reg, matching::ToolchainRegistry::load());
            if (g.json) {
                Json arr = Json::array();
                for (const auto& t : reg.toolchains()) {
                    auto j = t.to_json();
                    j["name"] = t.name;
                    j["builtin"] = t.builtin;
                    arr.push_back(j);
                }
                print_json({{"registry", fs::to_utf8(reg.path())}, {"toolchains", arr}});
                return 0;
            }
            std::println("registry: {}", fs::to_utf8(reg.path()));
            if (reg.toolchains().empty()) std::println("  (none; add one with `decomp toolchain add`)");
            for (const auto& t : reg.toolchains())
                std::println("  {:<16} {:<9} {}{}{}", t.name, matching::to_string(t.kind), t.wrapper.empty() ? "" : join(t.wrapper, " ") + " ",
                             t.compiler, t.builtin ? "  (auto-detected)" : "");
            return 0;
        }));
    });
    {
        auto* cmd = tc->add_subcommand("test", "Compile a probe function with a toolchain");
        auto name = std::make_shared<std::string>();
        cmd->add_option("name", *name, "Toolchain name")->required();
        cmd->callback([&g, name] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                TRY_ASSIGN(auto reg, matching::ToolchainRegistry::load());
                const auto* t = reg.find(*name);
                if (!t) return make_error(ErrorCode::not_found, "unknown toolchain '{}'", *name);
                TRY_ASSIGN(auto tmp, fs::TempDir::create("decomp-probe"));
                matching::Compiler compiler(*t, tmp.path());
                matching::CompileRequest req;
                req.source = "int decomp_probe(int x) { return x * 3 + 1; }\n";
                TRY_ASSIGN(auto r, compiler.compile(req));
                std::string detail;
                if (r.ok) {
                    auto obj = coff::Object::parse(r.object_data);
                    detail = obj ? std::format("COFF {} object, {} functions", to_string(obj->arch()), obj->function_symbols().size())
                                 : "not a COFF object (" + obj.error().message + ")";
                }
                if (g.json) print_json({{"ok", r.ok}, {"command", r.command}, {"output", r.output}, {"duration_ms", r.duration.count()}, {"object", detail}});
                else std::println("{} {} ({} ms){}\n{}", r.ok ? "OK" : "FAILED", join(r.command, " "), r.duration.count(),
                                  detail.empty() ? "" : "\n  " + detail, r.output);
                return r.ok ? 0 : 1;
            }));
        });
    }
    {
        auto* cmd = tc->add_subcommand("add", "Add or replace a toolchain in the user registry");
        auto t = std::make_shared<matching::Toolchain>();
        auto kind = std::make_shared<std::string>("msvc");
        auto env = std::make_shared<std::vector<std::string>>();
        auto prepend = std::make_shared<std::vector<std::string>>();
        cmd->add_option("name", t->name, "Toolchain name, e.g. msvc6")->required();
        cmd->add_option("--kind", *kind, "msvc, clang_cl, gcc or clang");
        cmd->add_option("--compiler", t->compiler, "Compiler executable")->required();
        cmd->add_option("--wrapper", t->wrapper, "Wrapper command, e.g. wine");
        cmd->add_option("--flag", t->flags, "Flag always passed (repeatable)")->allow_extra_args(false);
        cmd->add_option("--include", t->include_dirs, "Include directory (repeatable)")->allow_extra_args(false);
        cmd->add_option("--env", *env, "NAME=VALUE to set (repeatable)")->allow_extra_args(false);
        cmd->add_option("--env-prepend", *prepend, "NAME=VALUE to prepend to a PATH-like variable (repeatable)")->allow_extra_args(false);
        cmd->add_option("--description", t->description, "Free text");
        cmd->callback([&g, t, kind, env, prepend] {
            throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
                auto k = matching::toolchain_kind_from_string(*kind);
                if (!k) return make_error(ErrorCode::invalid_argument, "unknown kind '{}'", *kind);
                t->kind = *k;
                for (auto [list, out] : {std::pair{env.get(), &t->env}, std::pair{prepend.get(), &t->env_prepend}}) {
                    for (const auto& e : *list) {
                        auto eq = e.find('=');
                        if (eq == std::string::npos) return make_error(ErrorCode::invalid_argument, "expected NAME=VALUE, got '{}'", e);
                        out->emplace_back(e.substr(0, eq), e.substr(eq + 1));
                    }
                }
                TRY_ASSIGN(auto reg, matching::ToolchainRegistry::load());
                reg.upsert(*t);
                TRY(reg.save());
                std::println("saved toolchain '{}' to {}", t->name, fs::to_utf8(reg.path()));
                return 0;
            }));
        });
    }
}

void register_matching_commands(CLI::App& app, GlobalOptions& g) {
    auto* cmd = app.add_subcommand("diff", "Compare a target function with a candidate (exit code 0 = byte-exact, 2 = differs)");
    auto a = std::make_shared<DiffArgs>();
    cmd->add_option("function", a->function, "Target function name or address (omit with --all)");
    cmd->add_option("--binary", a->binary, "Target PE image (default: the project's target)");
    auto* obj_opt = cmd->add_option("--obj", a->obj, "Candidate COFF object");
    auto* src_opt = cmd->add_option("--source", a->source, "Candidate C/C++ source to compile with the target's toolchain");
    obj_opt->excludes(src_opt);
    cmd->add_option("--toolchain", a->toolchain, "Toolchain name (default: the project's)");
    cmd->add_option("--flag", a->flags, "Extra compiler flag (repeatable; appended to the project's flags)")->allow_extra_args(false);
    cmd->add_option("--symbol", a->symbol, "Candidate symbol name if it differs from the target's");
    cmd->add_flag("--compact", a->compact, "Only show differences with context");
    cmd->add_option("--context", a->context, "Context rows in compact mode");
    cmd->add_flag("--bytes", a->bytes, "Show instruction bytes");
    cmd->add_flag("--all", a->all, "Diff every target function the object defines and print a summary");
    register_toolchain_commands(app, g);
    cmd->callback([&g, a] {
        throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
            TRY_ASSIGN(auto program, open_program(g, a->binary));
            if (a->obj.empty() && a->source.empty()) return make_error(ErrorCode::invalid_argument, "give --obj <file.obj> or --source <file.cpp>");
            if (!a->source.empty()) {
                if (a->function.empty()) return make_error(ErrorCode::invalid_argument, "--source needs a function name");
                TRY_ASSIGN(u64 va, resolve_function(program, a->function));
                TRY_ASSIGN(auto setup, make_match_setup(g, a->toolchain, a->flags));
                TRY_ASSIGN(auto source_text, fs::read_text(fs::from_utf8(a->source)));
                TRY_ASSIGN(auto r, matching::compile_and_diff(program, setup, va, source_text, a->symbol));
                if (!r.compile.ok) {
                    if (g.json) print_json({{"compiled", false}, {"output", r.compile.output}, {"command", r.compile.command}});
                    else std::print("compile failed ({}):\n{}\n", join(r.compile.command, " "), r.compile.output);
                    return 3;
                }
                if (!r.diff) return make_error(ErrorCode::not_found, "{}", r.diff_error);
                return print_diff(g, *r.diff, *a);
            }
            TRY_ASSIGN(auto obj, coff::Object::load(fs::from_utf8(a->obj)));
            if (a->all) {
                Json arr = Json::array();
                usize matched = 0, total = 0;
                for (const auto* f : program.symbols().functions()) {
                    if (!matching::find_candidate_symbol(obj, *f)) continue;
                    auto d = matching::diff_function(program, f->va, obj);
                    if (!d) continue;
                    ++total;
                    matched += d->byte_exact;
                    if (g.json) arr.push_back({{"function", f->name}, {"va", f->va}, {"match_percent", d->match_percent}, {"byte_exact", d->byte_exact}});
                    else std::println("{:#010x} {:6.1f}% {} {}", f->va, d->match_percent, d->byte_exact ? "OK " : "   ", f->display);
                }
                if (g.json) print_json({{"functions", arr}, {"matched", matched}, {"total", total}});
                else if (total == 0) std::println("no function in {} corresponds to a target function", a->obj);
                else std::println("{}/{} functions byte-exact", matched, total);
                return total > 0 && matched == total ? 0 : 2;
            }
            if (a->function.empty()) return make_error(ErrorCode::invalid_argument, "give a function, or --all");
            TRY_ASSIGN(u64 va, resolve_function(program, a->function));
            TRY_ASSIGN(auto d, matching::diff_function(program, va, obj, a->symbol));
            return print_diff(g, d, *a);
        }));
    });
}

} // namespace decomp::cli
