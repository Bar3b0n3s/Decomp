#include "viewmodel/browser.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <set>

namespace decomp::vm {

using project::FunctionStatus;

namespace {

constexpr std::array<BrowserColumn, 16> kColumns = {{
    {Column::address, "Address", true, false},
    {Column::display, "Name", true, false},
    {Column::name, "Decorated name", false, false},
    {Column::size, "Size", true, true},
    {Column::status, "Status", true, false},
    {Column::best_match, "Best %", true, true},
    {Column::attempts, "Attempts", true, true},
    {Column::cost, "Dollars", true, true},
    {Column::last_attempt, "Last attempt", true, false},
    {Column::source, "Source", true, false},
    {Column::callers, "Callers", true, true},
    {Column::callees, "Callees", true, true},
    {Column::unknown_callees, "Unknown callees", false, true},
    {Column::blocks, "Blocks", true, true},
    {Column::loops, "Loops", true, true},
    {Column::difficulty, "Difficulty", true, true},
}};

std::optional<u64> json_u64(const Json& j, std::string_view key) {
    auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_number()) return std::nullopt;
    if (it->is_number_unsigned()) return it->get<u64>();
    if (it->is_number_integer()) {
        const auto v = it->get<long long>();
        return v < 0 ? std::nullopt : std::optional<u64>(static_cast<u64>(v));
    }
    const double d = it->get<double>();
    return d < 0 ? std::nullopt : std::optional<u64>(static_cast<u64>(d));
}

std::optional<double> json_double(const Json& j, std::string_view key) {
    auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_number()) return std::nullopt;
    return it->get<double>();
}

std::string number_text(double v) {
    std::string s = std::format("{:.3f}", v);
    while (s.ends_with('0')) s.pop_back();
    if (s.ends_with('.')) s.pop_back();
    return s;
}

std::optional<double> parse_number(std::string_view text) {
    text = trim(text);
    if (text.empty()) return std::nullopt;
    double v = 0;
    try {
        usize used = 0;
        v = std::stod(std::string(text), &used);
        if (used != text.size()) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    return v;
}

} // namespace

std::span<const BrowserColumn> browser_columns() { return kColumns; }

const BrowserColumn* find_browser_column(Column column) {
    for (const auto& c : kColumns)
        if (c.column == column) return &c;
    return nullptr;
}

std::vector<ColumnState> default_column_layout() {
    std::vector<ColumnState> out;
    for (const auto& c : kColumns) out.push_back({c.column, c.visible_by_default});
    return out;
}

std::vector<ColumnState> column_layout_from_json(const Json& json) {
    if (!json.is_array()) return default_column_layout();
    std::vector<ColumnState> out;
    std::set<Column> seen;
    for (const auto& item : json) {
        if (!item.is_object()) continue;
        auto column = column_from_string(json_string_or(item, "column", ""));
        if (!column || !find_browser_column(*column) || !seen.insert(*column).second) continue;
        out.push_back({*column, json_bool_or(item, "visible", true)});
    }
    if (out.empty()) return default_column_layout();
    for (const auto& c : kColumns)
        if (!seen.contains(c.column)) out.push_back({c.column, c.visible_by_default});
    for (auto& c : out)
        if (c.column == Column::address) c.visible = true;
    return out;
}

Json column_layout_to_json(std::span<const ColumnState> layout) {
    Json out = Json::array();
    for (const auto& c : layout) out.push_back(Json{{"column", std::string(to_string(c.column))}, {"visible", c.visible}});
    return out;
}

Json filter_to_json(const FunctionFilter& f) {
    Json j = Json::object();
    if (!f.statuses.empty()) {
        Json list = Json::array();
        for (auto s : f.statuses) list.push_back(std::string(project::to_string(s)));
        j["statuses"] = std::move(list);
    }
    if (f.min_size) j["min_size"] = *f.min_size;
    if (f.max_size) j["max_size"] = *f.max_size;
    if (!f.name.empty()) j["name"] = f.name;
    if (f.unknown_callees) j["unknown_callees"] = true;
    if (f.refused) j["refused"] = true;
    if (f.min_best) j["min_best"] = *f.min_best;
    if (f.best_below) j["best_below"] = *f.best_below;
    return j;
}

FunctionFilter filter_from_json(const Json& j) {
    FunctionFilter f;
    if (!j.is_object()) return f;
    if (auto it = j.find("statuses"); it != j.end() && it->is_array())
        for (const auto& s : *it)
            if (s.is_string())
                if (auto status = project::status_from_string(s.get<std::string>());
                    status && std::ranges::find(f.statuses, *status) == f.statuses.end())
                    f.statuses.push_back(*status);
    f.min_size = json_u64(j, "min_size");
    f.max_size = json_u64(j, "max_size");
    f.name = json_string_or(j, "name", "");
    f.unknown_callees = json_bool_or(j, "unknown_callees", false);
    f.refused = json_bool_or(j, "refused", false);
    f.min_best = json_double(j, "min_best");
    f.best_below = json_double(j, "best_below");
    return f;
}

Json sort_keys_to_json(std::span<const SortKey> keys) {
    Json out = Json::array();
    for (const auto& k : keys) out.push_back(Json{{"column", std::string(to_string(k.column))}, {"descending", k.descending}});
    return out;
}

std::vector<SortKey> sort_keys_from_json(const Json& json) {
    std::vector<SortKey> out;
    if (!json.is_array()) return out;
    for (const auto& item : json) {
        if (!item.is_object()) continue;
        auto column = column_from_string(json_string_or(item, "column", ""));
        if (!column || std::ranges::find(out, *column, &SortKey::column) != out.end()) continue;
        out.push_back({*column, json_bool_or(item, "descending", false)});
    }
    return out;
}

namespace {

constexpr std::string_view kAnchorPrefix = "filter:";

} // namespace

std::string browser_anchor(const FunctionFilter& f) {
    std::vector<std::string> parts;
    if (!f.statuses.empty()) {
        std::vector<std::string> names;
        for (auto s : f.statuses) names.emplace_back(project::to_string(s));
        parts.push_back("status=" + join(names, ","));
    }
    if (f.min_size || f.max_size)
        parts.push_back(std::format("size={}-{}", f.min_size ? std::to_string(*f.min_size) : "", f.max_size ? std::to_string(*f.max_size) : ""));
    if (f.min_best || f.best_below)
        parts.push_back(std::format("best={}-{}", f.min_best ? number_text(*f.min_best) : "", f.best_below ? number_text(*f.best_below) : ""));
    if (f.refused) parts.emplace_back("refused");
    if (f.unknown_callees) parts.emplace_back("unknown_callees");
    if (!f.name.empty()) parts.push_back("name=" + f.name);  // last: the name may contain ';'
    return std::string(kAnchorPrefix) + join(parts, ";");
}

std::optional<FunctionFilter> browser_filter_from_anchor(std::string_view anchor) {
    if (!anchor.starts_with(kAnchorPrefix)) return std::nullopt;
    std::string_view rest = anchor.substr(kAnchorPrefix.size());
    FunctionFilter f;
    while (!rest.empty()) {
        if (rest.starts_with("name=")) {
            f.name = std::string(rest.substr(5));
            break;
        }
        const usize end = rest.find(';');
        const std::string_view part = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        const usize eq = part.find('=');
        const std::string_view key = trim(part.substr(0, eq));
        const std::string_view value = eq == std::string_view::npos ? std::string_view{} : trim(part.substr(eq + 1));
        auto range = [&](auto parse, auto& low, auto& high) {
            const usize dash = value.find('-');
            if (dash == std::string_view::npos) return;
            low = parse(value.substr(0, dash));
            high = parse(value.substr(dash + 1));
        };
        if (key == "status") {
            for (auto name : split(value, ','))
                if (auto s = project::status_from_string(trim(name)); s && std::ranges::find(f.statuses, *s) == f.statuses.end())
                    f.statuses.push_back(*s);
        } else if (key == "size") {
            range([](std::string_view t) { return parse_u64(trim(t)); }, f.min_size, f.max_size);
        } else if (key == "best") {
            range(parse_number, f.min_best, f.best_below);
        } else if (key == "refused") {
            f.refused = true;
        } else if (key == "unknown_callees") {
            f.unknown_callees = true;
        }
    }
    return f;
}

u64 live_overlay_digest(const events::RunStateData& run) {
    // FNV-1a over the fields the overlay reads.
    u64 h = 0xcbf29ce484222325ull;
    auto mix = [&](u64 v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 0x100000001b3ull;
        }
    };
    for (const auto& [id, s] : run.sessions) {
        mix(s->va);
        mix(s->finished ? 1 : 0);
        if (s->finished) continue;
        mix(static_cast<u64>(s->compiles));
        mix(std::bit_cast<u64>(s->best_match));
        mix(std::bit_cast<u64>(s->cost_usd));
    }
    return h;
}

bool filter_is_empty(const FunctionFilter& f) {
    return f.statuses.empty() && !f.min_size && !f.max_size && f.name.empty() && !f.unknown_callees && !f.refused && !f.min_best &&
           !f.best_below;
}

std::string describe_filter(const FunctionFilter& f) {
    std::vector<std::string> parts;
    if (!f.statuses.empty()) {
        std::vector<std::string> names;
        for (auto s : f.statuses) names.emplace_back(project::to_string(s));
        parts.push_back("status " + join(names, " or "));
    }
    if (f.min_size && f.max_size) parts.push_back(std::format("{}-{} bytes", *f.min_size, *f.max_size));
    else if (f.min_size) parts.push_back(std::format("at least {} bytes", *f.min_size));
    else if (f.max_size) parts.push_back(std::format("at most {} bytes", *f.max_size));
    if (f.min_best && f.best_below) parts.push_back(std::format("best {}-{}%", number_text(*f.min_best), number_text(*f.best_below)));
    else if (f.min_best) parts.push_back(std::format("best at least {}%", number_text(*f.min_best)));
    else if (f.best_below) parts.push_back(std::format("best below {}%", number_text(*f.best_below)));
    if (f.refused) parts.emplace_back("refused");
    if (f.unknown_callees) parts.emplace_back("with unknown callees");
    if (!f.name.empty()) parts.push_back(std::format("name /{}/", f.name));
    return parts.empty() ? std::string("no filter") : join(parts, ", ");
}

} // namespace decomp::vm
