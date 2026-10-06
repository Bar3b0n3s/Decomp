#pragma once

// Where searches get their sources from (docs/matching.md#searching): a function's verified source (the
// unit source or the file of its own that holds it), a unit's source, a function's best attempt, or a
// file given by name.

#include "analysis/program.hpp"
#include "analysis/units.hpp"
#include "core/result.hpp"
#include "search/evaluate.hpp"

#include <filesystem>
#include <string_view>
#include <vector>

namespace decomp::project {
class Project;
}

namespace decomp::search {

// The target functions a translation unit defines: those with a definition of one of their names
// (matching::definition_names()) at its top level, in address order.
std::vector<u64> defined_functions(const Program& program, std::string_view source);

// A function's verified source, with the function its target (or, with `whole`, every function the
// file holds: the unit source's functions).
Result<Probe> verified_probe(const project::Project& project, const Program& program, const Symbol& fn, bool whole = false);
// Every verified source of the project (unit sources and functions' own files), with the matched
// functions each holds.
std::vector<Probe> verified_probes(const project::Project& project, const Program& program);
// A unit's source, with all its functions.
Result<Probe> unit_probe(const project::Project& project, const Unit& unit);
// A function's best attempt: the source its sessions came closest with.
Result<Probe> best_attempt_probe(const project::Project& project, const Symbol& fn);
// A file: with `functions` its targets, else every target function it defines.
Result<Probe> file_probe(const Program& program, const std::filesystem::path& file, std::vector<u64> functions = {});

} // namespace decomp::search
