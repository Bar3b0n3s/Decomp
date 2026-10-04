#include "run/selection.hpp"

#include <algorithm>
#include <regex>

namespace decomp::run {

bool runnable_by_default(project::FunctionStatus status) {
    using project::FunctionStatus;
    switch (status) {
    case FunctionStatus::matched:
    case FunctionStatus::refused:
    case FunctionStatus::skipped:
    case FunctionStatus::library: return false;
    default: return true;
    }
}

Result<std::vector<u64>> select_functions(const Program& program, const project::Project* project, const Selection& selection) {
    std::vector<u64> out;
    if (!selection.functions.empty()) {
        for (const auto& name : selection.functions) {
            auto va = program.resolve(name);
            if (!va) return make_error(ErrorCode::not_found, "unknown function '{}'", name);
            out.push_back(*va);
        }
    } else {
        std::optional<std::regex> re;
        if (!selection.filter.empty()) {
            try {
                re.emplace(selection.filter, std::regex::ECMAScript | std::regex::icase);
            } catch (const std::regex_error& e) {
                return make_error(ErrorCode::invalid_argument, "invalid filter '{}': {}", selection.filter, e.what());
            }
        }
        for (const auto* f : program.symbols().functions()) {
            const auto status = project ? project->function_info(f->va).status : project::FunctionStatus::unstarted;
            if (!selection.statuses.empty()) {
                if (std::ranges::find(selection.statuses, status) == selection.statuses.end()) continue;
            } else if (!selection.include_finished && !runnable_by_default(status)) {
                continue;
            }
            if (re && !std::regex_search(f->name, *re) && !std::regex_search(f->display, *re) &&
                !(f->pdb_name.size() && std::regex_search(f->pdb_name, *re)))
                continue;
            out.push_back(f->va);
        }
    }
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace decomp::run
