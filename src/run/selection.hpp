#pragma once

#include "analysis/program.hpp"
#include "core/result.hpp"
#include "project/project.hpp"

#include <string>
#include <vector>

namespace decomp::run {

// Which functions a run works on.
struct Selection {
    std::vector<std::string> functions;            // names or addresses; when given, exactly these
    std::vector<project::FunctionStatus> statuses;  // only functions with these statuses
    std::string filter;                            // ECMAScript regex over decorated and readable names
    bool include_finished = false;                 // also matched, refused, skipped and library functions
};

// Statuses a run takes by default: everything not finished or set aside.
bool runnable_by_default(project::FunctionStatus status);

// Resolves a selection to function addresses in address order. Explicitly named functions are
// always included (whatever their status); otherwise every function symbol passes the filters.
Result<std::vector<u64>> select_functions(const Program& program, const project::Project* project, const Selection& selection);

} // namespace decomp::run
