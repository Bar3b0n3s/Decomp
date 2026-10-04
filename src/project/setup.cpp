#include "project/setup.hpp"

#include "matching/toolchain.hpp"

namespace decomp::project {

Result<matching::MatchSetup> make_match_setup(const Project* project, const std::string& toolchain,
                                              const std::vector<std::string>& extra_flags) {
    matching::MatchSetup setup;
    std::string name = toolchain;
    if (project) {
        if (name.empty()) name = project->config().toolchain;
        setup.flags = project->config().flags;
        setup.include_dirs = project->include_paths();
        setup.work_dir = project->build_dir();
        setup.cache_dir = project->cache_dir() / "objects";
    } else {
        setup.work_dir = std::filesystem::temp_directory_path() / "decomp-build";
    }
    if (name.empty()) return make_error(ErrorCode::invalid_argument, "no toolchain: pass --toolchain or set \"toolchain\" in decomp.json");
    TRY_ASSIGN(auto registry, matching::ToolchainRegistry::load());
    const matching::Toolchain* t = registry.find(name);
    if (!t) return make_error(ErrorCode::not_found, "unknown toolchain '{}' (see `decomp toolchain list`)", name);
    setup.toolchain = *t;
    setup.flags.insert(setup.flags.end(), extra_flags.begin(), extra_flags.end());
    return setup;
}

} // namespace decomp::project
