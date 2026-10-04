#include "cli/common.hpp"
#include "core/fs.hpp"
#include "matching/diff.hpp"

#include <print>

#ifdef _WIN32
#include <io.h>
#define DECOMP_ISATTY _isatty
#define DECOMP_FILENO _fileno
#else
#include <unistd.h>
#define DECOMP_ISATTY isatty
#define DECOMP_FILENO fileno
#endif

namespace decomp::cli {

namespace {

struct DiffArgs {
    std::string function, binary, obj, symbol;
    bool compact = false, bytes = false, all = false;
    usize context = 3;
};

int print_diff(const GlobalOptions& g, const matching::FunctionDiff& d, const DiffArgs& a) {
    matching::ReportOptions ro;
    ro.compact = a.compact;
    ro.context = a.context;
    ro.bytes = a.bytes;
    ro.color = !g.json && DECOMP_ISATTY(DECOMP_FILENO(stdout));
    if (g.json) print_json(matching::to_json(d, ro));
    else std::print("{}", matching::to_text(d, ro));
    return d.byte_exact ? 0 : 2;
}

} // namespace

void register_matching_commands(CLI::App& app, GlobalOptions& g) {
    auto* cmd = app.add_subcommand("diff", "Compare a target function with a candidate (exit code 0 = byte-exact, 2 = differs)");
    auto a = std::make_shared<DiffArgs>();
    cmd->add_option("function", a->function, "Target function name or address (omit with --all)");
    cmd->add_option("--binary", a->binary, "Target PE image (default: the project's target)");
    cmd->add_option("--obj", a->obj, "Candidate COFF object")->required();
    cmd->add_option("--symbol", a->symbol, "Candidate symbol name if it differs from the target's");
    cmd->add_flag("--compact", a->compact, "Only show differences with context");
    cmd->add_option("--context", a->context, "Context rows in compact mode");
    cmd->add_flag("--bytes", a->bytes, "Show instruction bytes");
    cmd->add_flag("--all", a->all, "Diff every target function the object defines and print a summary");
    cmd->callback([&g, a] {
        throw CLI::RuntimeError(run(g, [&]() -> Result<int> {
            TRY_ASSIGN(auto program, open_program(g, a->binary));
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
                else std::println("{}/{} functions byte-exact", matched, total);
                return matched == total ? 0 : 2;
            }
            if (a->function.empty()) return make_error(ErrorCode::invalid_argument, "give a function, or --all");
            TRY_ASSIGN(u64 va, resolve_function(program, a->function));
            TRY_ASSIGN(auto d, matching::diff_function(program, va, obj, a->symbol));
            return print_diff(g, d, *a);
        }));
    });
}

} // namespace decomp::cli
