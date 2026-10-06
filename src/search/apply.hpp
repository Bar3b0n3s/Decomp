#pragma once

// Keeping what a search found in the project (docs/matching.md#searching): flags and a toolchain in
// decomp.json, a permuted source as a function's verified source or its best attempt. The CLI's
// --apply and the GUI's Search view both go through these.

#include "analysis/program.hpp"
#include "core/result.hpp"
#include "matching/match.hpp"

#include <optional>
#include <string>
#include <vector>

namespace decomp::project {
class Project;
}

namespace decomp::search {

// Sets the project's compiler flags (and, when given, its toolchain) in decomp.json.
Result<void> apply_configuration(project::Project& project, const std::vector<std::string>& flags, const std::optional<std::string>& toolchain = {});

struct AppliedSource {
    bool verified = false;     // saved as the function's verified source
    bool best_attempt = false; // saved as its best attempt
    std::string path;          // the verified source written (project-relative)
    std::string message;
};
// Keeps a source of `fn` a search made: its verified source when it is byte-exact (into its unit's
// source or its own file, as a match by hand is), else its best attempt when it matches at least as
// well as the best one so far.
Result<AppliedSource> apply_source(project::Project& project, const Program& program, const matching::MatchSetup& setup, const Symbol& fn,
                                   const std::string& source, double match_percent, bool byte_exact, const std::string& reason);

} // namespace decomp::search
