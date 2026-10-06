#include "search/apply.hpp"

#include "core/fs.hpp"
#include "project/project.hpp"
#include "project/units.hpp"

#include <algorithm>
#include <format>

namespace decomp::search {

Result<void> apply_configuration(project::Project& project, const std::vector<std::string>& flags, const std::optional<std::string>& toolchain) {
    project.config().flags = flags;
    if (toolchain) project.config().toolchain = *toolchain;
    return project.save_config();
}

Result<AppliedSource> apply_source(project::Project& project, const Program& program, const matching::MatchSetup& setup, const Symbol& fn,
                                   const std::string& source, double match_percent, bool byte_exact, const std::string& reason) {
    AppliedSource out;
    const std::string display = fn.display.empty() ? fn.name : fn.display;
    if (!byte_exact) {
        if (match_percent < project.function_info(fn.va).best_match) {
            out.message = std::format("{} is not byte-exact and no closer than its best attempt: nothing kept", display);
            return out;
        }
        TRY(project.save_best_source(fn, source));
        TRY(project.modify_function(fn.va, [&](project::FunctionInfo& info) {
            info.best_match = std::max(info.best_match, match_percent);
            if (info.status == project::FunctionStatus::unstarted) info.status = project::FunctionStatus::nonmatching;
        }));
        out.best_attempt = true;
        out.message = std::format("{} is not byte-exact ({:.1f}%): kept as its best attempt", display, match_percent);
        return out;
    }
    TRY_ASSIGN(auto saved, project::save_verified_function(project, program, setup, fn, source, project::ChangeOrigin{SymbolSource::user, "", reason}));
    TRY(project.save_best_source(fn, source));
    TRY(project.modify_function(fn.va, [](project::FunctionInfo& info) {
        info.best_match = 100.0;
        info.status = project::FunctionStatus::matched;
    }));
    out.verified = true;
    out.path = fs::to_utf8(saved.path);
    out.message = std::format("{} verified byte-exact and saved to {}", display, out.path);
    return out;
}

} // namespace decomp::search
