#include "cli/common.hpp"

#include "core/log.hpp"
#include "matching/toolchain.hpp"
#include "project/project.hpp"

#include <print>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace decomp::cli {

int run(const GlobalOptions& g, const std::function<Result<int>()>& body) {
    (void)g;
    auto result = body();
    if (!result) {
        log::error("{}", result.error().message);
        return 1;
    }
    return *result;
}

Result<Program> open_program(const GlobalOptions& g, const std::string& binary, const std::string& pdb) {
    std::optional<std::filesystem::path> pdb_path;
    if (!pdb.empty()) pdb_path = std::filesystem::path(pdb);
    if (!binary.empty()) return Program::open(binary, pdb_path);
    TRY_ASSIGN(auto project, project::Project::find(g.project));
    return project.open_program();
}

Result<u64> resolve_function(const Program& program, const std::string& text) {
    auto va = program.resolve(text);
    if (!va) return make_error(ErrorCode::not_found, "unknown function '{}' (use a name from `decomp funcs` or an address like 0x401000)", text);
    return *va;
}

Result<matching::MatchSetup> make_match_setup(const GlobalOptions& g, const std::string& toolchain, const std::vector<std::string>& extra_flags) {
    matching::MatchSetup setup;
    std::string name = toolchain;
    auto project = project::Project::find(g.project);
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

void print_json(const Json& value) { std::println("{}", dump_pretty(value)); }

bool is_tty(std::FILE* stream) {
#ifdef _WIN32
    return _isatty(_fileno(stream)) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

} // namespace decomp::cli
