#pragma once

// Finding a project's functions again (decomp analyze, decomp map import). The analysis runs over the
// target with what the project knows now, optionally with the build's link map, and symbols.txt takes
// the result:
//
// - Functions only the analysis knew, with no recorded work, are found again from scratch, so a start
//   the map shows to be inside another function goes away.
// - Every other function is kept: named by the PDB, the map, the agent or the user, or with work
//   recorded. Their starts guide the analysis; their sizes are measured again (except the PDB's).
// - A function renamed by the map keeps its work: its .decomp/functions/ directory and its matched
//   source in src/functions/ are renamed with it.
//
// Refused while a run is active (a session could be writing to the functions it renames).

#include "core/json.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <filesystem>
#include <optional>

namespace decomp::project {

class Project;

struct AnalyzeOptions {
    std::optional<std::filesystem::path> map;  // the build's link map: names, object files, function starts
};

struct AnalyzeSummary {
    usize functions_before = 0;
    usize functions = 0;
    usize added = 0;        // functions symbols.txt did not have
    usize removed = 0;      // functions the analysis no longer finds (none had work recorded)
    usize resized = 0;      // functions whose size changed
    usize renamed = 0;      // functions whose name changed
    usize map_symbols = 0;  // symbols the map added or named
    usize moved = 0;        // work directories and matched sources renamed with their function
};

Result<AnalyzeSummary> analyze(Project& project, const AnalyzeOptions& options = {});
Json to_json(const AnalyzeSummary& summary);

} // namespace decomp::project
