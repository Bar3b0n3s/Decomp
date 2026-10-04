#pragma once

#include "analysis/symbols.hpp"
#include "core/json.hpp"
#include "project/project.hpp"

#include <map>

namespace decomp::project {

// Matching progress over every function of the target: what `decomp status` prints and the GUI's
// Dashboard shows.
struct Progress {
    struct Bucket {
        usize functions = 0;
        u64 bytes = 0;
    };
    std::map<FunctionStatus, Bucket> buckets;
    usize functions = 0, matched_functions = 0;
    u64 code_bytes = 0, matched_bytes = 0;  // functions of unknown size count 0 bytes
    double spend_usd = 0;                   // the sum of the functions' recorded spend

    double percent_functions() const { return functions ? 100.0 * static_cast<double>(matched_functions) / static_cast<double>(functions) : 0.0; }
    double percent_bytes() const { return code_bytes ? 100.0 * static_cast<double>(matched_bytes) / static_cast<double>(code_bytes) : 0.0; }
};

Progress compute_progress(const SymbolDb& symbols, const Project& project);
// The same from a function-state snapshot (Project::function_infos()), safe on any thread.
Progress compute_progress(const SymbolDb& symbols, const std::map<u64, FunctionInfo>& infos);
Json to_json(const Progress& progress);

} // namespace decomp::project
