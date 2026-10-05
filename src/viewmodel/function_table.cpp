#include "viewmodel/function_table.hpp"

#include "core/strings.hpp"
#include "viewmodel/progress.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <regex>

namespace decomp::vm {

using project::FunctionStatus;

std::vector<FunctionRow> build_function_rows(const SymbolDb& symbols, const std::map<u64, project::FunctionInfo>& infos,
                                             const events::RunStateData* live) {
    std::vector<FunctionRow> rows;
    std::map<u64, const events::SessionState*> sessions;
    if (live) sessions = live_sessions(*live);
    // Symbols, states and sessions are all ordered by address: walk them together.
    auto info = infos.begin();
    auto session = sessions.begin();
    for (const auto& [va, s] : symbols) {
        if (s.kind != SymbolKind::function) continue;
        FunctionRow& r = rows.emplace_back();
        r.va = va;
        r.name = s.name;
        r.display = s.display.empty() ? s.name : s.display;
        r.size = s.size;
        r.source = s.source;
        r.is_static = s.is_static;
        while (info != infos.end() && info->first < va) ++info;
        if (info != infos.end() && info->first == va) {
            r.stored_status = info->second.status;
            r.best_match = info->second.best_match;
            r.attempts = info->second.attempts;
            r.cost_usd = info->second.cost_usd;
        }
        r.status = r.stored_status;
        while (session != sessions.end() && session->first < va) ++session;
        if (session != sessions.end() && session->first == va) {
            const events::SessionState& live_session = *session->second;
            r.session = live_session.id;
            if (r.stored_status != FunctionStatus::matched) r.status = FunctionStatus::in_progress;
            r.best_match = std::max(r.best_match, live_session.best_match);
            r.attempts += live_session.compiles;
            r.cost_usd += live_session.cost_usd;
        }
    }
    return rows;
}

void apply_analysis(std::vector<FunctionRow>& rows, const FunctionAnalysis& analysis) {
    auto f = analysis.functions.begin();
    for (auto& r : rows) {
        while (f != analysis.functions.end() && f->va < r.va) ++f;
        if (f == analysis.functions.end()) break;
        if (f->va != r.va) continue;
        r.callers = f->callers;
        r.callees = f->callees;
        r.blocks = f->blocks;
        r.loops = f->loops;
        r.unknown_callees = f->unknown_callees;
        r.difficulty = difficulty(*f);
    }
}

namespace {

// The last non-empty line of a file, read backwards from its end: at most its last 16 KB, unless
// `whole` (then the chunks grow until the line's start is found).
struct LineTail {
    std::string text;
    bool complete = false;  // `text` is the whole line
};
std::optional<LineTail> last_line(const std::filesystem::path& path, bool whole) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const std::streamoff size = in.tellg();
    if (size <= 0) return std::nullopt;
    std::streamoff chunk = 16 * 1024;
    while (true) {
        const std::streamoff start = std::max<std::streamoff>(0, size - chunk);
        std::string tail(static_cast<usize>(size - start), '\0');
        in.seekg(start);
        if (!in.read(tail.data(), static_cast<std::streamsize>(tail.size()))) return std::nullopt;
        while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) tail.pop_back();
        if (const usize nl = tail.rfind('\n'); nl != std::string::npos) return LineTail{tail.substr(nl + 1), true};
        if (start == 0) return tail.empty() ? std::nullopt : std::optional<LineTail>(LineTail{std::move(tail), true});
        if (!whole) return LineTail{std::move(tail), false};
        chunk *= 4;
    }
}

} // namespace

std::optional<TimePoint> last_attempt_time(const project::Project& project, const Symbol& fn) {
    const auto path = project.function_dir(fn) / "attempts.jsonl";
    auto tail = last_line(path, false);
    if (!tail) return std::nullopt;
    // Records are written with sorted keys, so "time" closes the line: finding it there spares
    // parsing the candidate source the record carries. (Inside a JSON string the quotes would be
    // escaped, so the pattern only matches the key itself.)
    constexpr std::string_view kKey = "\"time\":\"";
    if (const usize at = tail->text.rfind(kKey); at != std::string::npos) {
        const usize begin = at + kKey.size();
        if (const usize end = tail->text.find('"', begin); end != std::string::npos)
            return parse_iso8601(std::string_view(tail->text).substr(begin, end - begin));
    }
    if (!tail->complete) tail = last_line(path, true);
    if (!tail) return std::nullopt;
    auto json = parse_json(tail->text);
    if (!json || !json->is_object()) return std::nullopt;
    return parse_iso8601(json_string_or(*json, "time", ""));
}

void fill_last_attempts(std::vector<FunctionRow>& rows, const project::Project& project, const SymbolDb& symbols,
                        const std::function<bool()>& cancelled) {
    for (auto& r : rows) {
        if (r.attempts <= 0) continue;
        if (cancelled && cancelled()) return;
        if (const Symbol* s = symbols.at(r.va)) r.last_attempt = last_attempt_time(project, *s);
    }
}

namespace {

constexpr std::array<std::string_view, 16> kColumnNames = {
    "address", "name", "display", "size", "status", "best_match", "attempts", "cost", "last_attempt", "source",
    "callers", "callees", "blocks", "loops", "unknown_callees", "difficulty",
};

int status_rank(FunctionStatus status) {
    for (usize i = 0; i < kStatusOrder.size(); ++i)
        if (kStatusOrder[i] == status) return static_cast<int>(i);
    return static_cast<int>(kStatusOrder.size());
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

int icompare(std::string_view a, std::string_view b) {
    const usize n = std::min(a.size(), b.size());
    for (usize i = 0; i < n; ++i) {
        const char x = lower(a[i]), y = lower(b[i]);
        if (x != y) return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
    }
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

// `needle` is lower case.
bool icontains(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size()) return false;
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char h, char n) { return lower(h) == n; }) !=
           haystack.end();
}

bool has_regex_syntax(std::string_view text) { return text.find_first_of("\\^$.|?*+()[]{}") != std::string_view::npos; }

template <class T>
int three_way(const T& a, const T& b) {
    return a < b ? -1 : b < a ? 1 : 0;
}

// Present values first in either direction; nullopt: both present and equal is 0.
template <class T>
std::optional<int> presence(const std::optional<T>& a, const std::optional<T>& b) {
    if (a.has_value() == b.has_value()) return a.has_value() ? std::nullopt : std::optional<int>(0);
    return a.has_value() ? -1 : 1;
}

// Lower-case copies of the text columns being sorted on, by row index (folding case once is several
// times faster than in every comparison).
struct TextKeys {
    std::vector<std::string> name, display;
};

// Ordering of rows `ia` and `ib` by one key: negative when `ia` goes first.
int compare_rows(const std::vector<FunctionRow>& rows, const TextKeys& text, u32 ia, u32 ib, const SortKey& key) {
    const FunctionRow& a = rows[ia];
    const FunctionRow& b = rows[ib];
    auto directed = [&](int c) { return key.descending ? -c : c; };
    auto optional_key = [&](const auto& x, const auto& y) {
        if (auto p = presence(x, y)) return *p;
        return directed(three_way(*x, *y));
    };
    switch (key.column) {
    case Column::address: return directed(three_way(a.va, b.va));
    case Column::name: return directed(text.name.empty() ? icompare(a.name, b.name) : text.name[ia].compare(text.name[ib]));
    case Column::display: return directed(text.display.empty() ? icompare(a.display, b.display) : text.display[ia].compare(text.display[ib]));
    case Column::size: return directed(three_way(a.size, b.size));
    case Column::status: return directed(three_way(status_rank(a.status), status_rank(b.status)));
    case Column::best_match: return directed(three_way(a.best_match, b.best_match));
    case Column::attempts: return directed(three_way(a.attempts, b.attempts));
    case Column::cost: return directed(three_way(a.cost_usd, b.cost_usd));
    case Column::last_attempt: return optional_key(a.last_attempt, b.last_attempt);
    case Column::source: return directed(three_way(a.source, b.source));
    case Column::callers: return optional_key(a.callers, b.callers);
    case Column::callees: return optional_key(a.callees, b.callees);
    case Column::blocks: return optional_key(a.blocks, b.blocks);
    case Column::loops: return optional_key(a.loops, b.loops);
    case Column::unknown_callees: return optional_key(a.unknown_callees, b.unknown_callees);
    case Column::difficulty: return optional_key(a.difficulty, b.difficulty);
    }
    return 0;
}

} // namespace

std::string_view to_string(Column column) { return kColumnNames[static_cast<usize>(column)]; }

std::optional<Column> column_from_string(std::string_view text) {
    for (usize i = 0; i < kColumnNames.size(); ++i)
        if (kColumnNames[i] == text) return static_cast<Column>(i);
    return std::nullopt;
}

Result<std::vector<u32>> filter_and_sort(const std::vector<FunctionRow>& rows, const FunctionFilter& filter, std::span<const SortKey> sort,
                                         const std::function<bool()>& cancelled) {
    u32 statuses = 0;
    for (auto s : filter.statuses) statuses |= 1u << static_cast<unsigned>(s);
    std::optional<std::regex> pattern;
    std::string needle;
    if (!filter.name.empty()) {
        if (has_regex_syntax(filter.name)) {
            try {
                pattern.emplace(filter.name, std::regex::ECMAScript | std::regex::icase);
            } catch (const std::regex_error& e) {
                return make_error(ErrorCode::invalid_argument, "invalid pattern '{}': {}", filter.name, e.what());
            }
        } else {
            needle = to_lower(filter.name);
        }
    }

    std::vector<u32> out;
    out.reserve(rows.size());
    for (usize i = 0; i < rows.size(); ++i) {
        if (i % 4096 == 0 && cancelled && cancelled()) return make_error(ErrorCode::cancelled, "cancelled");
        const FunctionRow& r = rows[i];
        if (statuses && !(statuses & (1u << static_cast<unsigned>(r.status)))) continue;
        if (filter.min_size && r.size < *filter.min_size) continue;
        if (filter.max_size && r.size > *filter.max_size) continue;
        if (filter.refused && r.stored_status != FunctionStatus::refused) continue;
        if (filter.min_best && r.best_match < *filter.min_best) continue;
        if (filter.best_below && r.best_match >= *filter.best_below) continue;
        if (filter.unknown_callees && r.unknown_callees.value_or(0) == 0) continue;
        if (!needle.empty() && !icontains(r.name, needle) && !icontains(r.display, needle)) continue;
        if (pattern && !std::regex_search(r.name, *pattern) && !std::regex_search(r.display, *pattern)) continue;
        out.push_back(static_cast<u32>(i));
    }
    TextKeys text;
    auto fold = [&](std::vector<std::string>& keys, std::string FunctionRow::*field) {
        if (!keys.empty()) return;
        keys.resize(rows.size());
        for (u32 i : out) keys[i] = to_lower(rows[i].*field);
    };
    for (const SortKey& key : sort) {
        if (key.column == Column::name) fold(text.name, &FunctionRow::name);
        if (key.column == Column::display) fold(text.display, &FunctionRow::display);
    }
    std::ranges::sort(out, [&](u32 a, u32 b) {
        for (const SortKey& key : sort)
            if (const int c = compare_rows(rows, text, a, b, key)) return c < 0;
        return rows[a].va < rows[b].va;
    });
    return out;
}

} // namespace decomp::vm
