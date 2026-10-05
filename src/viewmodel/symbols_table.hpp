#pragma once

// Symbols and provenance (docs/ui.md "Symbols and provenance"): one row per symbol, a filter and sort
// over them (an index permutation, like the function table), the symbol log
// (.decomp/symbols.log.jsonl) read back as records, each symbol's provenance, the audit trail of agent
// edits grouped by session, and the plan for reverting agent edits one at a time or per session. Pure.

#include "analysis/symbols.hpp"
#include "core/json.hpp"
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

// A symbol as a log record describes it before or after an edit.
struct SymbolState {
    std::string name;
    SymbolKind kind = SymbolKind::unknown;
    u32 size = 0;
    SymbolSource source = SymbolSource::user;

    // Same name, kind and size (who made it is not compared).
    bool same_symbol(const SymbolState& other) const { return name == other.name && kind == other.kind && size == other.size; }
};
SymbolState symbol_state(const Symbol& symbol);

// One line of symbols.log.jsonl (docs/project-format.md).
struct SymbolLogRecord {
    usize index = 0;  // line number among the records, 0-based
    TimePoint time{};
    std::string time_text;  // as recorded
    u64 va = 0;
    std::optional<SymbolState> before, after;  // nullopt: the symbol did not exist (created / removed)
    SymbolSource source = SymbolSource::user;
    std::string session, reason;

    bool by_agent() const { return source == SymbolSource::agent; }
};
// In file order; malformed lines are skipped (their index is not reused).
std::vector<SymbolLogRecord> parse_symbol_log(const std::vector<Json>& lines);
// "renamed ?a@@YAXXZ -> ?b@@YAXXZ", "created data g_count (4 bytes)", "removed sub_401000", "size 0 -> 32"
std::string describe_edit(const std::optional<SymbolState>& before, const std::optional<SymbolState>& after);

struct SymbolRow {
    u64 va = 0;
    std::string name, display;
    SymbolKind kind = SymbolKind::unknown;
    u32 size = 0;
    SymbolSource source = SymbolSource::analysis;
    bool is_static = false;
    std::optional<project::FunctionStatus> status;  // functions only
    u32 edits = 0;         // log records for this address
    bool agent_edited = false;
};
// Ascending by address. `log` adds the edit counts. Linear.
std::vector<SymbolRow> build_symbol_rows(const SymbolDb& symbols, const std::map<u64, project::FunctionInfo>& infos,
                                         const std::vector<SymbolLogRecord>* log = nullptr);

enum class SymbolColumn : u8 { address, kind, name, display, size, source, status, edits };
std::string_view to_string(SymbolColumn column);  // "address", "kind", ...
std::optional<SymbolColumn> symbol_column_from_string(std::string_view text);

struct SymbolSortKey {
    SymbolColumn column = SymbolColumn::address;
    bool descending = false;
};

struct SymbolFilter {
    std::string text;                     // substring of the name or demangled name, ignoring case; or an address
    std::vector<SymbolKind> kinds = {};   // empty: all
    std::vector<SymbolSource> sources = {};
    bool edited_only = false;             // only symbols with log records
};
// Indices of the rows that pass, sorted by the keys (the address breaks ties). Text that parses as an
// address ("0x401000", "401000h") also keeps the symbol at or containing that address. Fails only when
// `cancelled` returns true. Linear plus the sort; a background job for large tables.
Result<std::vector<u32>> filter_and_sort_symbols(const std::vector<SymbolRow>& rows, const SymbolFilter& filter,
                                                 std::span<const SymbolSortKey> sort, const std::function<bool()>& cancelled = {});

// One entry of a symbol's history.
struct ProvenanceEntry {
    TimePoint time{};
    std::string what;     // describe_edit()
    SymbolSource source = SymbolSource::user;
    std::string session, reason;
    std::optional<usize> record;  // index into the log (nullopt: a symbol_changed event of the shown run)
};
// The log records of `va`, plus the shown run's symbol_changed events that the log does not hold (an
// event matches a record of the same address, session and new name), oldest first.
std::vector<ProvenanceEntry> symbol_provenance(u64 va, const std::vector<SymbolLogRecord>& log, const events::RunStateData* run);

// The agent's edits, grouped by session (sessions in order of their first edit).
struct AgentEditGroup {
    std::string session;
    std::vector<usize> records;  // indices into the log, oldest first
    TimePoint first{}, last{};
};
std::vector<AgentEditGroup> agent_edit_groups(const std::vector<SymbolLogRecord>& log);

// How to undo records: set_symbol() edits that restore each record's `before`, newest record first.
// Each record must still describe the symbol as it is (its `after` equals the current state, applying
// the plan's earlier steps), otherwise a later change would be lost: the plan fails and names the
// record. `current(va)` is the symbol as symbols.txt holds it (nullopt: none).
struct RevertStep {
    usize record = 0;
    project::SymbolEdit edit;
};
Result<std::vector<RevertStep>> plan_revert(const std::vector<SymbolLogRecord>& log, std::span<const usize> records,
                                            const std::function<std::optional<SymbolState>(u64)>& current);

} // namespace decomp::vm
