#include "viewmodel/symbols_table.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <unordered_map>

namespace decomp::vm {

using project::FunctionStatus;

SymbolState symbol_state(const Symbol& s) { return SymbolState{s.name, s.kind, s.size, s.source}; }

namespace {

std::optional<SymbolState> state_from_json(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    SymbolState s;
    s.name = json_string_or(j, "name", "");
    s.kind = symbol_kind_from_string(json_string_or(j, "kind", "")).value_or(SymbolKind::unknown);
    s.size = static_cast<u32>(std::max<long long>(0, json_int_or(j, "size", 0)));
    s.source = symbol_source_from_string(json_string_or(j, "source", "")).value_or(SymbolSource::user);
    return s;
}

} // namespace

std::vector<SymbolLogRecord> parse_symbol_log(const std::vector<Json>& lines) {
    std::vector<SymbolLogRecord> out;
    for (usize i = 0; i < lines.size(); ++i) {
        const Json& j = lines[i];
        if (!j.is_object() || !j.contains("va") || !j["va"].is_number()) continue;
        SymbolLogRecord r;
        r.index = i;
        r.time_text = json_string_or(j, "time", "");
        r.time = parse_iso8601(r.time_text).value_or(TimePoint{});
        r.va = j["va"].is_number_unsigned() ? j["va"].get<u64>() : static_cast<u64>(std::max<long long>(0, j["va"].get<long long>()));
        r.before = state_from_json(j.value("before", Json()));
        r.after = state_from_json(j.value("after", Json()));
        r.source = symbol_source_from_string(json_string_or(j, "source", "")).value_or(SymbolSource::user);
        r.session = json_string_or(j, "session", "");
        r.reason = json_string_or(j, "reason", "");
        out.push_back(std::move(r));
    }
    return out;
}

std::string describe_edit(const std::optional<SymbolState>& before, const std::optional<SymbolState>& after) {
    if (!before && !after) return "no change";
    if (!before)
        return after->size ? std::format("created {} {} ({} bytes)", to_string(after->kind), after->name, after->size)
                           : std::format("created {} {}", to_string(after->kind), after->name);
    if (!after) return std::format("removed {}", before->name);
    std::vector<std::string> parts;
    if (before->name != after->name) parts.push_back(std::format("renamed {} -> {}", before->name, after->name));
    if (before->kind != after->kind) parts.push_back(std::format("kind {} -> {}", to_string(before->kind), to_string(after->kind)));
    if (before->size != after->size) parts.push_back(std::format("size {} -> {}", before->size, after->size));
    if (parts.empty()) return std::format("{} set by {}", after->name, to_string(after->source));
    return join(parts, "; ");
}

std::vector<SymbolRow> build_symbol_rows(const SymbolDb& symbols, const std::map<u64, project::FunctionInfo>& infos,
                                         const std::vector<SymbolLogRecord>* log) {
    std::unordered_map<u64, std::pair<u32, bool>> edits;  // va -> (records, any by the agent)
    if (log)
        for (const auto& r : *log) {
            auto& e = edits[r.va];
            ++e.first;
            e.second = e.second || r.by_agent();
        }
    std::vector<SymbolRow> rows;
    rows.reserve(symbols.size());
    for (const auto& [va, s] : symbols) {
        SymbolRow& r = rows.emplace_back();
        r.va = va;
        r.name = s.name;
        r.display = s.display.empty() ? s.name : s.display;
        r.kind = s.kind;
        r.size = s.size;
        r.source = s.source;
        r.is_static = s.is_static;
        if (s.kind == SymbolKind::function) {
            auto it = infos.find(va);
            r.status = it == infos.end() ? FunctionStatus::unstarted : it->second.status;
        }
        if (auto it = edits.find(va); it != edits.end()) {
            r.edits = it->second.first;
            r.agent_edited = it->second.second;
        }
    }
    return rows;
}

namespace {

constexpr std::array<std::string_view, 8> kSymbolColumns = {"address", "kind", "name", "display", "size", "source", "status", "edits"};

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

int icompare(std::string_view a, std::string_view b) {
    const usize n = std::min(a.size(), b.size());
    for (usize i = 0; i < n; ++i) {
        const char x = lower(a[i]), y = lower(b[i]);
        if (x != y) return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
    }
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

bool icontains(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size()) return false;
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char h, char n) { return lower(h) == n; }) !=
           haystack.end();
}

template <class T>
int three_way(const T& a, const T& b) {
    return a < b ? -1 : b < a ? 1 : 0;
}

int compare(const SymbolRow& a, const SymbolRow& b, const SymbolSortKey& key) {
    auto directed = [&](int c) { return key.descending ? -c : c; };
    switch (key.column) {
    case SymbolColumn::address: return directed(three_way(a.va, b.va));
    case SymbolColumn::kind: return directed(three_way(a.kind, b.kind));
    case SymbolColumn::name: return directed(icompare(a.name, b.name));
    case SymbolColumn::display: return directed(icompare(a.display, b.display));
    case SymbolColumn::size: return directed(three_way(a.size, b.size));
    case SymbolColumn::source: return directed(three_way(a.source, b.source));
    case SymbolColumn::status:
        // Symbols without a status (not functions) come last in both directions.
        if (a.status.has_value() != b.status.has_value()) return a.status ? -1 : 1;
        return a.status ? directed(three_way(*a.status, *b.status)) : 0;
    case SymbolColumn::edits: return directed(three_way(a.edits, b.edits));
    }
    return 0;
}

} // namespace

std::string_view to_string(SymbolColumn column) { return kSymbolColumns[static_cast<usize>(column)]; }

std::optional<SymbolColumn> symbol_column_from_string(std::string_view text) {
    for (usize i = 0; i < kSymbolColumns.size(); ++i)
        if (kSymbolColumns[i] == text) return static_cast<SymbolColumn>(i);
    return std::nullopt;
}

Result<std::vector<u32>> filter_and_sort_symbols(const std::vector<SymbolRow>& rows, const SymbolFilter& filter,
                                                 std::span<const SymbolSortKey> sort, const std::function<bool()>& cancelled) {
    const std::string needle = to_lower(trim(filter.text));
    std::optional<u64> address;
    if (const std::string_view t = trim(filter.text); t.starts_with("0x") || t.starts_with("0X") || ((t.ends_with('h') || t.ends_with('H')) && t.size() > 1))
        address = parse_u64(t);
    std::vector<u32> out;
    out.reserve(rows.size());
    for (usize i = 0; i < rows.size(); ++i) {
        if (i % 4096 == 0 && cancelled && cancelled()) return make_error(ErrorCode::cancelled, "cancelled");
        const SymbolRow& r = rows[i];
        if (!filter.kinds.empty() && std::ranges::find(filter.kinds, r.kind) == filter.kinds.end()) continue;
        if (!filter.sources.empty() && std::ranges::find(filter.sources, r.source) == filter.sources.end()) continue;
        if (filter.edited_only && r.edits == 0) continue;
        if (!needle.empty()) {
            const bool at = address && (*address == r.va || (*address > r.va && *address < r.va + r.size));
            if (!at && !icontains(r.name, needle) && !icontains(r.display, needle)) continue;
        }
        out.push_back(static_cast<u32>(i));
    }
    std::ranges::sort(out, [&](u32 a, u32 b) {
        for (const auto& key : sort)
            if (const int c = compare(rows[a], rows[b], key)) return c < 0;
        return rows[a].va < rows[b].va;
    });
    return out;
}

std::vector<ProvenanceEntry> symbol_provenance(u64 va, const std::vector<SymbolLogRecord>& log, const events::RunStateData* run) {
    std::vector<ProvenanceEntry> out;
    for (usize i = 0; i < log.size(); ++i) {
        const auto& r = log[i];
        if (r.va != va) continue;
        out.push_back({r.time, describe_edit(r.before, r.after), r.source, r.session, r.reason, i});
    }
    if (run) {
        for (const auto& c : run->symbol_changes) {
            if (c.change.va != va) continue;
            const bool logged = std::ranges::any_of(log, [&](const SymbolLogRecord& r) {
                return r.va == va && r.session == c.change.session && r.after && r.after->name == c.change.new_name;
            });
            if (logged) continue;
            // The event carries the new kind and size only, so the description names the rename.
            const SymbolSource source = symbol_source_from_string(c.change.source).value_or(SymbolSource::agent);
            const SymbolKind kind = symbol_kind_from_string(c.change.kind).value_or(SymbolKind::unknown);
            std::optional<SymbolState> before, after;
            if (!c.change.old_name.empty()) before = SymbolState{c.change.old_name, kind, c.change.size, source};
            if (!c.change.new_name.empty()) after = SymbolState{c.change.new_name, kind, c.change.size, source};
            out.push_back({c.time, describe_edit(before, after), source, c.change.session, "", std::nullopt});
        }
    }
    std::ranges::stable_sort(out, [](const ProvenanceEntry& a, const ProvenanceEntry& b) { return a.time < b.time; });
    return out;
}

std::vector<AgentEditGroup> agent_edit_groups(const std::vector<SymbolLogRecord>& log) {
    std::vector<AgentEditGroup> out;
    std::map<std::string, usize> index;
    for (usize i = 0; i < log.size(); ++i) {
        const auto& r = log[i];
        if (!r.by_agent()) continue;
        auto [it, inserted] = index.emplace(r.session, out.size());
        if (inserted) {
            out.push_back({r.session, {}, r.time, r.time});
        }
        AgentEditGroup& g = out[it->second];
        g.records.push_back(i);
        g.first = std::min(g.first, r.time);
        g.last = std::max(g.last, r.time);
    }
    return out;
}

Result<std::vector<RevertStep>> plan_revert(const std::vector<SymbolLogRecord>& log, std::span<const usize> records,
                                            const std::function<std::optional<SymbolState>(u64)>& current) {
    std::vector<usize> order(records.begin(), records.end());
    std::ranges::sort(order, std::greater<>());
    order.erase(std::unique(order.begin(), order.end()), order.end());
    std::map<u64, std::optional<SymbolState>> simulated;  // the symbol after the steps planned so far
    auto state_of = [&](u64 va) -> std::optional<SymbolState> {
        if (auto it = simulated.find(va); it != simulated.end()) return it->second;
        return current(va);
    };
    std::vector<RevertStep> steps;
    for (usize i : order) {
        if (i >= log.size()) return make_error(ErrorCode::invalid_argument, "no symbol log record {}", i);
        const SymbolLogRecord& r = log[i];
        const auto now = state_of(r.va);
        const bool unchanged = r.after ? (now && now->same_symbol(*r.after)) : !now;
        if (!unchanged)
            return make_error(ErrorCode::invalid_argument, "the symbol at {:#x} has changed since the edit of {} ({}); revert the later change first",
                              r.va, r.time_text, describe_edit(r.before, r.after));
        RevertStep step;
        step.record = i;
        step.edit.va = r.va;
        if (!r.before) {
            if (!r.after) continue;  // nothing to undo
            step.edit.remove = true;
            simulated[r.va] = std::nullopt;
        } else {
            step.edit.name = r.before->name;
            step.edit.kind = r.before->kind;
            step.edit.size = r.before->size;
            SymbolState restored = *r.before;
            restored.source = SymbolSource::user;
            simulated[r.va] = restored;
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

} // namespace decomp::vm
