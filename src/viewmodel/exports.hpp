#pragma once

// The GUI's exports (docs/ui.md "Export"): the progress report (Markdown, JSON), the cost report (CSV,
// JSON), the function list (CSV, JSON) and a diff (text, JSON). Each function returns the file's
// content, and the view writes it (fs::write_text). The transcript's Markdown export is to_markdown()
// in transcript.hpp; its JSONL export is the transcript file itself. All pure and linear.

#include "core/json.hpp"
#include "matching/diff.hpp"
#include "viewmodel/cost.hpp"
#include "viewmodel/function_table.hpp"
#include "viewmodel/progress.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::vm {

// RFC 4180 CSV: a field holding a comma, a double quote, CR or LF is enclosed in double quotes with
// inner quotes doubled; records end with CRLF.
std::string csv_field(std::string_view value);
std::string csv_record(std::span<const std::string> fields);

struct ProgressReport {
    std::string project, target;  // for the title
    TimePoint generated{};
    DashboardProgress progress;
    ProgressHistory history;      // empty: no "over time" sections
};
// Markdown: the totals, the status buckets, the best-match distribution and progress by run and by day.
std::string progress_markdown(const ProgressReport& report);
// JSON: "status" is project::to_json() of the stored numbers, as `decomp --json status` prints it, plus
// the overlay segments, the distribution and the history.
Json progress_json(const ProgressReport& report);

enum class CostTable : u8 { runs, days, models, functions };
// One table of the report as CSV with a header record.
//   runs, days, models: key, start, runs, cost_usd, functions_worked, functions_matched, success_rate,
//     turns, usd_per_match, turns_per_match, input_tokens, output_tokens, cache_write_tokens,
//     cache_read_tokens, cache_hit_rate
//   functions: address, function, size, status, stored_usd, live_usd, total_usd, attempts
std::string cost_csv(const CostReport& report, CostTable table);
// Everything in the report (and the projection when given).
Json cost_json(const CostReport& report, const CostProjection* projection = nullptr);

// The rows in `order` (filter_and_sort()'s permutation, so the export matches the table). Columns:
// address, name, display, size, status, best_match, attempts, cost_usd, last_attempt, source, callers,
// callees, blocks, loops, unknown_callees, difficulty. Columns not computed yet are empty (null in JSON).
std::string function_list_csv(const std::vector<FunctionRow>& rows, std::span<const u32> order);
Json function_list_json(const std::vector<FunctionRow>& rows, std::span<const u32> order);

// `decomp diff`'s options and defaults (its --compact, --context and --bytes).
struct DiffExportOptions {
    bool compact = false;
    usize context = 3;
    bool bytes = false;
};
// Exactly what `decomp diff` prints for the same diff and options (no colors).
std::string diff_text(const matching::FunctionDiff& diff, const DiffExportOptions& options = {});
// Exactly what `decomp --json diff` prints: the pretty JSON report and a newline.
std::string diff_json(const matching::FunctionDiff& diff, const DiffExportOptions& options = {});

} // namespace decomp::vm
