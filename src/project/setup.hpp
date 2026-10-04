#pragma once

#include "core/result.hpp"
#include "matching/match.hpp"
#include "project/project.hpp"

#include <string>
#include <vector>

namespace decomp::project {

// How candidates are compiled: the toolchain (`toolchain`, else the project's), the project's flags
// and include directories (when there is a project) followed by `extra_flags`, and the work and
// cache directories under .decomp/ (a temporary directory without a project).
Result<matching::MatchSetup> make_match_setup(const Project* project, const std::string& toolchain,
                                              const std::vector<std::string>& extra_flags = {});

} // namespace decomp::project
