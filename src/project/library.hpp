#pragma once

// Naming the functions a target took from static libraries (decomp lib match): the libraries'
// signatures (analysis/signatures.hpp) are matched against the project's functions; a function one
// signature fits is named after it (source `library`, unless a better source named it), takes the
// library function's size, and gets the status `library`, so runs leave it alone.

#include "core/json.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace decomp::project {

class Project;

struct LibraryMatchReport {
    usize signatures = 0;  // functions in the libraries
    usize matched = 0;     // target functions named after a library function
    usize ambiguous = 0;   // target functions several library functions fit
    usize resized = 0;     // matched functions whose size changed
    usize removed = 0;     // starts the analysis had found inside a matched function
    std::map<std::string, usize> by_library;  // matched functions per library file
    struct Ambiguous {
        u64 va = 0;
        std::vector<std::string> names;
    };
    std::vector<Ambiguous> ambiguous_functions;
};

// With `apply` false, only reports. Refused while a run is active.
Result<LibraryMatchReport> match_libraries(Project& project, std::span<const std::filesystem::path> libraries, bool apply);
Json to_json(const LibraryMatchReport& report);

} // namespace decomp::project
