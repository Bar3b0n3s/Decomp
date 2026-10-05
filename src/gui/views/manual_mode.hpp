#pragma once

// Manual mode (docs/ui.md "Manual mode") and the background compiles the Diff viewer and the Agent session
// share. Sources are compiled with the project's toolchain, flags and compile cache and diffed against
// the target; candidate code is only ever compiled, never executed.

#include "analysis/program.hpp"
#include "gui/jobs.hpp"
#include "gui/view.hpp"
#include "matching/diff.hpp"
#include "matching/toolchain.hpp"
#include "project/project.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::gui {

// A source compiled and diffed against one function.
struct CompiledSource {
    u64 va = 0;
    std::string source;
    std::shared_ptr<const matching::FunctionDiff> diff;  // null: the compile failed or found no function to diff
    bool compiled = false;
    bool cached = false;
    long long duration_ms = 0;
    std::vector<matching::Diagnostic> diagnostics;
    std::string output;   // the compiler's output
    std::string summary;  // the diff's one-line summary, or why there is none
};

// For a job: cancelling the job's token stops the compile. `project` is a copy (copies share state).
CompiledSource compile_source(const std::shared_ptr<const Program>& program, const project::Project& project, u64 va, const std::string& source,
                              const CancelToken& token);

// "Verify and save": the check submit_result makes (compile, diff, require byte_exact; in the function's
// unit source when it has one), recorded as an attempt with origin "user" in `session`; when it passes,
// the source is saved (ChangeOrigin user, "verified by hand": into the unit source, which must keep its
// other functions byte-exact, else to src/functions/) and the function becomes matched. `project` is a
// copy (copies share state), so a job may hold it.
struct VerifyResult {
    CompiledSource compiled;
    bool saved = false;
    std::string path;     // the written source, relative to the project
    std::string message;  // for the notification
    std::string error;    // a write failed
};
VerifyResult verify_and_save(const std::shared_ptr<const Program>& program, project::Project project, u64 va, const std::string& source,
                             const std::string& session, const CancelToken& token);

// Takes a function over from the agent: its live session is paused (the user may hand back) or ended,
// then the Diff viewer opens on the best attempt, editing.
enum class TakeOver { pause, end };
void take_over(ViewContext& ctx, u64 va, const std::string& session, TakeOver how);

// The guidance that hands an edited source back to the agent.
std::string hand_back_text(std::string_view source, std::string_view summary);

// The function's symbol in `program` (a placeholder named sub_<va> when it has none).
Symbol function_symbol(const Program& program, u64 va);

} // namespace decomp::gui
