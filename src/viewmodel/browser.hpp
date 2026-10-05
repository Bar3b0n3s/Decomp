#pragma once

// The Function browser's columns, and the state it keeps per project and receives through navigation:
// filters, sort keys and the column layout as JSON (gui.json's per-project view state), and filters as
// navigation anchors (the Dashboard opens the browser filtered by a status bucket or a best-match bin).
// Pure and cheap.

#include "core/json.hpp"
#include "viewmodel/function_table.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::vm {

struct BrowserColumn {
    Column column = Column::address;
    std::string_view label;
    bool visible_by_default = true;
    bool numeric = false;  // right-aligned
};

// Every column the browser offers, in the default display order.
std::span<const BrowserColumn> browser_columns();
const BrowserColumn* find_browser_column(Column column);

// A column's place and visibility in the user's layout.
struct ColumnState {
    Column column = Column::address;
    bool visible = true;
};

// The layout saved by column_layout_to_json(): known columns in the saved order, then the columns the
// saved layout lacks (newer ones) in their default order and visibility. Anything that is not a valid
// layout gives the default one. The address column stays visible.
std::vector<ColumnState> column_layout_from_json(const Json& json);
Json column_layout_to_json(std::span<const ColumnState> layout);
std::vector<ColumnState> default_column_layout();

// {"statuses": ["matched", ...], "min_size": 16, "max_size": 64, "name": "...", "unknown_callees": true,
//  "refused": false, "min_best": 50, "best_below": 60}; absent keys are unset. Invalid values are ignored.
Json filter_to_json(const FunctionFilter& filter);
FunctionFilter filter_from_json(const Json& json);
// [{"column": "best_match", "descending": true}, ...]
Json sort_keys_to_json(std::span<const SortKey> keys);
std::vector<SortKey> sort_keys_from_json(const Json& json);

// Navigation anchors for a filtered browser: "filter:status=nonmatching;best=50-60". Keys: status (a
// comma-separated list), size (min-max, inclusive; either side may be empty), best (min-below), refused,
// unknown_callees and name (the rest of the anchor, so it may hold ';'). nullopt for an anchor that is
// not a filter.
std::string browser_anchor(const FunctionFilter& filter);
std::optional<FunctionFilter> browser_filter_from_anchor(std::string_view anchor);

// Changes whenever the live overlay of build_function_rows() would: a session starts or finishes, or a
// running session's best match, compiles or spend change. O(sessions), cheap enough for every frame.
u64 live_overlay_digest(const events::RunStateData& run);

// Whether a filter keeps every row.
bool filter_is_empty(const FunctionFilter& filter);
// "status nonmatching, best 50-60%, name /add/": a short description for the browser's header.
std::string describe_filter(const FunctionFilter& filter);

} // namespace decomp::vm
