#pragma once

// The Inspector's data for one function (docs/ui.md "Function browser and inspector"): the annotated
// listing split into the parts the view colors and links, the cross-references, the attempt history and
// the status history recorded in the runs' event logs. Pure.

#include "analysis/annotate.hpp"
#include "analysis/program.hpp"
#include "core/json.hpp"
#include "core/result.hpp"
#include "viewmodel/attempts.hpp"
#include "viewmodel/common.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::vm {

// One instruction of the annotated listing.
struct ListingLine {
    u64 address = 0;
    std::string label;      // "loc_401020" when something branches here
    std::string bytes;      // hex
    std::string mnemonic;   // with its prefix ("rep movsd")
    std::string operands;   // symbolized
    std::string comment;    // strings, floats, frame slots, switch tables, loop markers
    usize block = 0;
    bool block_start = false;
    bool loop_header = false;  // the block starts a loop (target of a back edge)
    int loop_depth = 0;        // of the block (0: not in a loop)
    x86::Flow flow = x86::Flow::none;
    // What the line links to: a branch or call destination, or the address a memory operand or an
    // address-valued immediate holds. `inside`: the destination is in this function (a label).
    std::optional<u64> target;
    bool inside = false;
};

struct Listing {
    AnnotatedFunction function;  // header data; its lines are moved into `lines`
    std::vector<ListingLine> lines;
    int max_loop_depth = 0;
};

// annotate_function() plus the decoded instructions and the CFG for the parts it does not keep (flow,
// link targets, loop depths). Decodes the function twice (the annotation and the flow); callers are
// not listed (Inspector shows cross-references separately). Background job for large functions.
Result<Listing> build_listing(const Program& program, u64 va);

// A cross-reference row: the instruction `at` in function `function` refers to `target`.
struct XrefRow {
    u64 at = 0;
    u64 function = 0;  // function holding `at` (0 when unknown)
    u64 target = 0;
    XrefKind kind = XrefKind::read;
    std::string name;  // readable name of the other end: the calling function, the callee, the data
    bool is_function = false;  // the other end is a function (link to the Inspector, else the Binary explorer)
};

struct FunctionXrefs {
    std::vector<XrefRow> callers;  // calls and jumps into the function (also through linker thunks)
    std::vector<XrefRow> callees;  // calls and jumps out of it
    std::vector<XrefRow> data;     // reads, writes and addresses it takes
};
// Program::xrefs_to() builds the program's cross-reference index on first use (seconds on a large
// binary), so call this from a background job. Each list is in address order of `at`.
FunctionXrefs function_xrefs(const Program& program, u64 va);

// Attempts are read with parse_attempts() (viewmodel/attempts.hpp).
// Score chart series: x = 1..n over all attempts in order, y = match percent (0 for attempts that did
// not compile), and the best so far.
struct AttemptSeries {
    Series score, best;
};
AttemptSeries attempt_series(const std::vector<AttemptRecord>& attempts);

// A status change recorded in a run's event log (status_changed).
struct StatusChangeRecord {
    TimePoint time{};
    std::string run;
    std::string function;  // as the event named it
    std::string old_status, status;
    double best = 0;
};
using StatusHistory = std::map<u64, std::vector<StatusChangeRecord>>;

// Adds the status_changed events of one events.jsonl (its text) to `history`; other lines are skipped
// by a substring test before any parsing.
void add_status_changes(StatusHistory& history, std::string_view events_jsonl, std::string_view run_id);
// Every run under `runs_dir` (each <id>/events.jsonl), in time order per function. Reads every event log:
// a background job. `cancelled` is polled between runs.
StatusHistory load_status_history(const std::filesystem::path& runs_dir, const std::function<bool()>& cancelled = {});

} // namespace decomp::vm
