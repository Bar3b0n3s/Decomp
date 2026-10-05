#include "gui/views/manual_mode.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "matching/match.hpp"
#include "project/setup.hpp"
#include "viewmodel/attempts.hpp"

#include <algorithm>
#include <chrono>
#include <format>

namespace decomp::gui {

Symbol function_symbol(const Program& program, u64 va) {
    if (const Symbol* s = program.symbols().at(va)) return *s;
    Symbol fallback;
    fallback.va = va;
    fallback.name = std::format("sub_{:x}", va);
    fallback.kind = SymbolKind::function;
    return fallback;
}

CompiledSource compile_source(const std::shared_ptr<const Program>& program, const project::Project& project, u64 va, const std::string& source,
                              const CancelToken& token) {
    CompiledSource out;
    out.va = va;
    out.source = source;
    if (!program) {
        out.summary = "no program is loaded";
        return out;
    }
    auto setup = project::make_match_setup(&project, "");
    if (!setup) {
        out.summary = setup.error().message;
        return out;
    }
    setup->cancelled = [token] { return token.cancelled(); };
    Result<matching::CandidateResult> result = make_error(ErrorCode::internal, "not compiled");
    try {
        result = matching::compile_and_diff(*program, *setup, va, source);
    } catch (const std::exception& e) {
        // Shown in place of the diff, so the view says what went wrong instead of staying empty.
        out.summary = std::format("the compile failed: {}", e.what());
        return out;
    }
    if (!result) {
        out.summary = result.error().message;
        return out;
    }
    out.compiled = result->compile.ok;
    out.cached = result->compile.cached;
    out.duration_ms = result->compile.duration.count();
    out.diagnostics = std::move(result->compile.diagnostics);
    out.output = truncate_utf8(result->compile.output, 64 * 1024);
    if (result->diff) {
        out.diff = std::make_shared<const matching::FunctionDiff>(std::move(*result->diff));
        out.summary = matching::summary_line(*out.diff);
    } else {
        out.summary = result->diff_error;
    }
    return out;
}

VerifyResult verify_and_save(const std::shared_ptr<const Program>& program, project::Project project, u64 va, const std::string& source,
                             const std::string& session, const CancelToken& token) {
    VerifyResult r;
    r.compiled = compile_source(program, project, va, source, token);
    if (!program || token.cancelled()) {
        r.message = r.compiled.summary;
        return r;
    }
    const Symbol fn = function_symbol(*program, va);
    const auto& diff = r.compiled.diff;
    const double percent = diff ? diff->match_percent : 0.0;
    const bool exact = diff && diff->byte_exact;
    // Recorded like the agent's verification compile, whatever it shows.
    const auto earlier = vm::parse_attempts(project.attempts(fn));
    const Json attempt = vm::user_attempt_json(session, vm::next_attempt_number(earlier, session), std::chrono::system_clock::now(),
                                               r.compiled.compiled, percent, exact, r.compiled.summary, source);
    if (auto recorded = project.record_attempt(fn, attempt); !recorded) r.error = "cannot record the attempt: " + recorded.error().message;
    if (diff && percent >= project.function_info(va).best_match) (void)project.save_best_source(fn, source);
    const std::string display = fn.display.empty() ? fn.name : fn.display;
    if (!exact) {
        (void)project.modify_function(va, [&](project::FunctionInfo& info) {
            ++info.attempts;
            info.best_match = std::max(info.best_match, percent);
            if (info.status == project::FunctionStatus::unstarted && info.best_match > 0) info.status = project::FunctionStatus::nonmatching;
        });
        r.message = std::format("Not saved: {} is not byte-exact ({}).", display, r.compiled.summary);
        return r;
    }
    auto written = project.write_matched_source(fn, source, project::ChangeOrigin{SymbolSource::user, "", "verified by hand"});
    if (!written) {
        r.error = "cannot write the source: " + written.error().message;
        r.message = r.error;
        return r;
    }
    std::error_code ec;
    r.path = fs::to_utf8(std::filesystem::relative(written->path, project.root(), ec));
    auto updated = project.modify_function(va, [](project::FunctionInfo& info) {
        ++info.attempts;
        info.best_match = 100.0;
        info.status = project::FunctionStatus::matched;
    });
    if (!updated) {
        r.error = "cannot update symbols.txt: " + updated.error().message;
        r.message = r.error;
        return r;
    }
    r.saved = true;
    r.message = std::format("Matched by hand: {} verified byte-exact and saved to {}.", display, r.path);
    return r;
}

void take_over(ViewContext& ctx, u64 va, const std::string& session, TakeOver how) {
    RunCommands& commands = *ctx.services.commands;
    if (commands.live() && ctx.snapshot && !session.empty())
        if (const events::SessionState* s = ctx.snapshot->session(session); s && !s->finished) {
            // A paused session can take the edited source back later; an ended one leaves the function to the user.
            if (how == TakeOver::pause) commands.pause_worker(s->worker);
            else commands.skip(va);
        }
    ctx.open("diff_viewer", NavTarget{.va = va, .session = session, .anchor = "manual"});
}

std::string hand_back_text(std::string_view source, std::string_view summary) {
    std::string fence = "```";
    while (source.find(fence) != std::string_view::npos) fence += '`';
    return std::format("The supervisor edited the source by hand{}. Continue from this version:\n\n{}cpp\n{}{}{}",
                       summary.empty() ? std::string() : std::format(" ({})", summary), fence, source,
                       source.ends_with('\n') ? "" : "\n", fence);
}

} // namespace decomp::gui
