#pragma once

// The Function browser's table: one row per function, a filter and a multi-column sort that produce
// an index permutation over the rows (rows are never copied or reordered).
//
// Typical use from a view: build_function_rows() when the project or the snapshot changes (fast), a
// background job with analyze_functions() + apply_analysis() for the analysis columns, and
// filter_and_sort() as a background job whenever the filter, the sort or the rows change.

#include "analysis/difficulty.hpp"
#include "analysis/symbols.hpp"
#include "core/result.hpp"
#include "events/run_state.hpp"
#include "project/project.hpp"
#include "viewmodel/common.hpp"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::vm {

struct FunctionRow {
    u64 va = 0;
    std::string name;     // decorated (as in symbols.txt)
    std::string display;  // demangled; the name when there is no demangled form
    u32 size = 0;         // bytes; 0 when unknown
    // Shown status: the stored one, or in_progress while a session works on the function (a matched
    // function stays matched).
    project::FunctionStatus status = project::FunctionStatus::unstarted;
    project::FunctionStatus stored_status = project::FunctionStatus::unstarted;  // as in symbols.txt
    double best_match = 0;  // percent: the stored best, or the running session's when that is higher
    int attempts = 0;       // stored attempts plus the running session's compiles
    double cost_usd = 0;    // stored spend plus the running session's
    SymbolSource source = SymbolSource::analysis;
    bool is_static = false;
    std::string session;  // the session working on it now (empty: none)
    std::optional<TimePoint> last_attempt;  // fill_last_attempts()
    // Analysis columns, empty until apply_analysis() (they need every function decoded):
    std::optional<u32> callers, callees, blocks, loops, unknown_callees;
    std::optional<double> difficulty;
};

// One row per function symbol, ascending by address, from the symbols, the stored function states
// (Project::function_infos()) and, when given, the live run (see FunctionRow for the overlay). Linear:
// about 50 ms for 100,000 functions in a Release build (mostly copying names), measured in
// tests/unit/viewmodel_function_table_tests.cpp.
std::vector<FunctionRow> build_function_rows(const SymbolDb& symbols, const std::map<u64, project::FunctionInfo>& infos,
                                             const events::RunStateData* live = nullptr);

// Fills the analysis columns (callers, callees, blocks, loops, unknown callees, difficulty) of the rows
// the analysis covers. Linear.
void apply_analysis(std::vector<FunctionRow>& rows, const FunctionAnalysis& analysis);

// When the function's last attempt was recorded: the "time" of the last line of its attempts.jsonl.
// Only the end of the file is read (the key closes each record), so it takes about 10 microseconds
// whatever the size of the sources the file holds. nullopt without attempts.
std::optional<TimePoint> last_attempt_time(const project::Project& project, const Symbol& fn);
// The same for every row with attempts > 0: one small read per such function (about a second for
// 100,000 of them), so run it in a background job when many functions have attempts. Rows without
// attempts are left empty. `cancelled` is polled between files.
void fill_last_attempts(std::vector<FunctionRow>& rows, const project::Project& project, const SymbolDb& symbols,
                        const std::function<bool()>& cancelled = {});

enum class Column : u8 {
    address, name, display, size, status, best_match, attempts, cost, last_attempt, source,
    callers, callees, blocks, loops, unknown_callees, difficulty,
};
std::string_view to_string(Column column);  // "address", "name", ..., "unknown_callees", "difficulty"
std::optional<Column> column_from_string(std::string_view text);

struct SortKey {
    Column column = Column::address;
    bool descending = false;
};

struct FunctionFilter {
    std::vector<project::FunctionStatus> statuses;  // shown statuses to keep; empty: all
    std::optional<u64> min_size, max_size;          // bytes, inclusive
    // Searched (not anchored) in the name and the demangled name, ignoring case. Text without regex
    // metacharacters is a plain substring search; anything else is an ECMAScript regular expression.
    std::string name;
    bool unknown_callees = false;  // only functions with an unknown callee (rows not analyzed yet are left out)
    bool refused = false;          // only refused functions
    // Best match in percent: at least min_best, and below best_below (the Dashboard's distribution bins
    // are half-open, [50, 60) for instance).
    std::optional<double> min_best = std::nullopt, best_below = std::nullopt;
};

// Indices of the rows that pass `filter`, ordered by `sort` (each key breaks the ties of the previous
// ones; the address breaks the remaining ties, so the result is deterministic). Rows whose sort value
// is missing (analysis columns not filled, no last attempt) come last in both directions. Text sorts
// ignore ASCII case. Fails on an invalid regular expression (ErrorCode::invalid_argument) and when
// `cancelled` (polled while filtering) returns true. For 100,000 rows in a Release build: about 10 ms
// to filter by status or by a substring, 200 ms by a regular expression, 50 ms to sort by a number and
// 60 ms by a name (measured in tests/unit/viewmodel_function_table_tests.cpp); run it as a background
// job.
Result<std::vector<u32>> filter_and_sort(const std::vector<FunctionRow>& rows, const FunctionFilter& filter, std::span<const SortKey> sort,
                                         const std::function<bool()>& cancelled = {});

} // namespace decomp::vm
