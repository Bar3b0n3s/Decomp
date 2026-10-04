#include "cli/common.hpp"

#include "core/log.hpp"
#include "project/project.hpp"

#include <print>

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

void print_json(const Json& value) { std::println("{}", dump_pretty(value)); }

} // namespace decomp::cli
