#include "viewmodel/exports.hpp"

#include <format>

namespace decomp::vm {

std::string csv_field(std::string_view value) {
    if (value.find_first_of(",\"\r\n") == std::string_view::npos) return std::string(value);
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

std::string csv_record(std::span<const std::string> fields) {
    std::string out;
    for (usize i = 0; i < fields.size(); ++i) {
        if (i) out += ',';
        out += csv_field(fields[i]);
    }
    return out + "\r\n";
}

namespace {

std::string iso(TimePoint t) {
    if (t == TimePoint{}) return {};
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(t));
}

std::string percent(double share) { return std::format("{:.1f}%", 100.0 * share); }

std::string number(double v) { return std::format("{}", v); }

template <class T>
std::string optional_text(const std::optional<T>& v) {
    return v ? std::format("{}", *v) : std::string();
}

template <class T>
Json optional_json(const std::optional<T>& v) {
    return v ? Json(*v) : Json(nullptr);
}

Json point_json(const ProgressPoint& p) {
    return Json{{"label", p.label},
                {"time", iso(p.time)},
                {"functions", p.functions},
                {"bytes", p.bytes},
                {"new_functions", p.new_functions},
                {"new_bytes", p.new_bytes},
                {"total_functions", p.total_functions},
                {"total_bytes", p.total_bytes},
                {"cost_usd", p.cost_usd}};
}

std::string bin_label(usize i) { return i == 9 ? std::string("90-100%") : std::format("{}-{}%", i * 10, i * 10 + 9); }

} // namespace

std::string progress_markdown(const ProgressReport& report) {
    const DashboardProgress& p = report.progress;
    std::string out = std::format("# Progress: {}{}\n\n", report.project, report.target.empty() ? "" : " (" + report.target + ")");
    if (report.generated != TimePoint{})
        out += std::format("Generated {:%Y-%m-%d %H:%M} UTC.\n\n", std::chrono::floor<std::chrono::seconds>(report.generated));
    out += std::format("- Functions matched: {} of {} ({:.1f}%)\n", p.stored.matched_functions, p.stored.functions, p.stored.percent_functions());
    out += std::format("- Code bytes matched: {} of {} ({:.1f}%)\n", p.stored.matched_bytes, p.stored.code_bytes, p.stored.percent_bytes());
    out += std::format("- Spend: {}\n", format_usd(p.stored.spend_usd));
    if (p.running) out += std::format("- Functions in progress now: {}\n", p.running);
    out += "\n## Status\n\n| Status | Functions | Bytes | Share of functions | Share of bytes |\n|---|---:|---:|---:|---:|\n";
    for (const auto& s : p.segments)
        out += std::format("| {} | {} | {} | {} | {} |\n", project::to_string(s.status), s.functions, s.bytes, percent(s.function_share),
                           percent(s.byte_share));
    out += "\n## Non-matching functions by best match\n\n| Best match | Functions |\n|---|---:|\n";
    for (usize i = 0; i < p.best_match_bins.size(); ++i) out += std::format("| {} | {} |\n", bin_label(i), p.best_match_bins[i]);
    if (!report.history.runs.empty()) {
        out += "\n## Progress by run\n\n| Run | Started | Matched | New | Matched so far | Bytes so far | Spend |\n|---|---|---:|---:|---:|---:|---:|\n";
        for (const auto& r : report.history.runs)
            out += std::format("| {} | {} | {} | {} | {} | {} | {} |\n", r.label, iso(r.time), r.functions, r.new_functions, r.total_functions,
                               r.total_bytes, format_usd(r.cost_usd));
    }
    if (!report.history.days.empty()) {
        out += "\n## Progress by day\n\n| Day | Matched | New | Matched so far | Bytes so far | Spend |\n|---|---:|---:|---:|---:|---:|\n";
        for (const auto& d : report.history.days)
            out += std::format("| {} | {} | {} | {} | {} | {} |\n", d.label, d.functions, d.new_functions, d.total_functions, d.total_bytes,
                               format_usd(d.cost_usd));
    }
    return out;
}

Json progress_json(const ProgressReport& report) {
    const DashboardProgress& p = report.progress;
    Json segments = Json::array();
    for (const auto& s : p.segments)
        segments.push_back(Json{{"status", std::string(project::to_string(s.status))},
                                {"functions", s.functions},
                                {"bytes", s.bytes},
                                {"function_share", s.function_share},
                                {"byte_share", s.byte_share}});
    Json bins = Json::array();
    for (usize i = 0; i < p.best_match_bins.size(); ++i) bins.push_back(Json{{"range", bin_label(i)}, {"functions", p.best_match_bins[i]}});
    Json runs = Json::array(), days = Json::array();
    for (const auto& r : report.history.runs) runs.push_back(point_json(r));
    for (const auto& d : report.history.days) days.push_back(point_json(d));
    return Json{{"project", report.project},
                {"target", report.target},
                {"generated", iso(report.generated)},
                {"status", project::to_json(p.stored)},
                {"segments", std::move(segments)},
                {"in_progress_now", p.running},
                {"best_match_bins", std::move(bins)},
                {"runs", std::move(runs)},
                {"days", std::move(days)}};
}

namespace {

std::vector<std::string> slice_fields(const CostSlice& s) {
    return {s.key,
            iso(s.time),
            std::to_string(s.runs),
            number(s.cost_usd),
            std::to_string(s.worked),
            std::to_string(s.matched),
            number(s.success_rate()),
            std::to_string(s.turns),
            number(s.usd_per_match()),
            number(s.turns_per_match()),
            std::to_string(s.usage.input),
            std::to_string(s.usage.output),
            std::to_string(s.usage.cache_write),
            std::to_string(s.usage.cache_read),
            number(s.cache_hit_rate())};
}

Json slice_json(const CostSlice& s) {
    return Json{{"key", s.key},
                {"start", iso(s.time)},
                {"runs", s.runs},
                {"cost_usd", s.cost_usd},
                {"functions_worked", s.worked},
                {"functions_matched", s.matched},
                {"success_rate", s.success_rate()},
                {"turns", s.turns},
                {"usd_per_match", s.usd_per_match()},
                {"turns_per_match", s.turns_per_match()},
                {"usage",
                 {{"input_tokens", s.usage.input},
                  {"output_tokens", s.usage.output},
                  {"cache_creation_input_tokens", s.usage.cache_write},
                  {"cache_read_input_tokens", s.usage.cache_read}}},
                {"cache_hit_rate", s.cache_hit_rate()}};
}

} // namespace

std::string cost_csv(const CostReport& report, CostTable table) {
    std::string out;
    if (table == CostTable::functions) {
        const std::vector<std::string> header = {"address", "function", "size", "status", "stored_usd", "live_usd", "total_usd", "attempts"};
        out += csv_record(header);
        for (const auto& f : report.by_function) {
            const std::vector<std::string> fields = {std::format("{:#x}", f.va),  f.name,
                                                     std::to_string(f.size),      std::string(project::to_string(f.status)),
                                                     number(f.stored_usd),        number(f.live_usd),
                                                     number(f.total_usd()),       std::to_string(f.attempts)};
            out += csv_record(fields);
        }
        return out;
    }
    const std::vector<std::string> header = {"key", "start", "runs", "cost_usd", "functions_worked", "functions_matched", "success_rate", "turns",
                                             "usd_per_match", "turns_per_match", "input_tokens", "output_tokens", "cache_write_tokens",
                                             "cache_read_tokens", "cache_hit_rate"};
    out += csv_record(header);
    const auto& slices = table == CostTable::runs ? report.by_run : table == CostTable::days ? report.by_day : report.by_model;
    for (const auto& s : slices) out += csv_record(slice_fields(s));
    return out;
}

Json cost_json(const CostReport& report, const CostProjection* projection) {
    auto slices = [](const std::vector<CostSlice>& list) {
        Json arr = Json::array();
        for (const auto& s : list) arr.push_back(slice_json(s));
        return arr;
    };
    Json functions = Json::array();
    for (const auto& f : report.by_function)
        functions.push_back(Json{{"va", f.va},
                                 {"function", f.name},
                                 {"size", f.size},
                                 {"status", std::string(project::to_string(f.status))},
                                 {"stored_usd", f.stored_usd},
                                 {"live_usd", f.live_usd},
                                 {"total_usd", f.total_usd()},
                                 {"attempts", f.attempts}});
    Json j = {{"total", slice_json(report.total)},
              {"by_run", slices(report.by_run)},
              {"by_day", slices(report.by_day)},
              {"by_model", slices(report.by_model)},
              {"by_function", std::move(functions)},
              {"function_spend_usd", report.function_spend_usd},
              {"fallback_turns", report.fallback_turns},
              {"unpriced_models", report.unpriced_models}};
    if (projection) {
        Json buckets = Json::array();
        for (const auto& b : projection->buckets)
            buckets.push_back(Json{{"bucket", b.bucket},
                                   {"sizes", size_bucket_label(b.bucket)},
                                   {"observed", b.observed},
                                   {"mean_usd", b.mean_usd},
                                   {"remaining", b.remaining},
                                   {"projected_usd", b.projected_usd},
                                   {"from_nearby", b.from_nearby}});
        j["projection"] = Json{{"remaining", projection->remaining},
                               {"unsized", projection->unsized},
                               {"projected_usd", projection->projected_usd},
                               {"has_history", projection->has_history},
                               {"buckets", std::move(buckets)}};
    }
    return j;
}

namespace {

std::vector<std::string> row_fields(const FunctionRow& r) {
    return {std::format("{:#x}", r.va),
            r.name,
            r.display,
            std::to_string(r.size),
            std::string(project::to_string(r.status)),
            std::format("{:.1f}", r.best_match),
            std::to_string(r.attempts),
            number(r.cost_usd),
            r.last_attempt ? iso(*r.last_attempt) : std::string(),
            std::string(to_string(r.source)),
            optional_text(r.callers),
            optional_text(r.callees),
            optional_text(r.blocks),
            optional_text(r.loops),
            optional_text(r.unknown_callees),
            r.difficulty ? std::format("{:.2f}", *r.difficulty) : std::string()};
}

} // namespace

std::string function_list_csv(const std::vector<FunctionRow>& rows, std::span<const u32> order) {
    const std::vector<std::string> header = {"address", "name", "display", "size", "status", "best_match", "attempts", "cost_usd", "last_attempt",
                                             "source", "callers", "callees", "blocks", "loops", "unknown_callees", "difficulty"};
    std::string out = csv_record(header);
    for (u32 i : order)
        if (i < rows.size()) out += csv_record(row_fields(rows[i]));
    return out;
}

Json function_list_json(const std::vector<FunctionRow>& rows, std::span<const u32> order) {
    Json arr = Json::array();
    for (u32 i : order) {
        if (i >= rows.size()) continue;
        const FunctionRow& r = rows[i];
        arr.push_back(Json{{"va", r.va},
                           {"name", r.name},
                           {"display", r.display},
                           {"size", r.size},
                           {"status", std::string(project::to_string(r.status))},
                           {"best_match", r.best_match},
                           {"attempts", r.attempts},
                           {"cost_usd", r.cost_usd},
                           {"last_attempt", r.last_attempt ? Json(iso(*r.last_attempt)) : Json(nullptr)},
                           {"source", std::string(to_string(r.source))},
                           {"callers", optional_json(r.callers)},
                           {"callees", optional_json(r.callees)},
                           {"blocks", optional_json(r.blocks)},
                           {"loops", optional_json(r.loops)},
                           {"unknown_callees", optional_json(r.unknown_callees)},
                           {"difficulty", optional_json(r.difficulty)}});
    }
    return arr;
}

namespace {

matching::ReportOptions report_options(const DiffExportOptions& options) {
    matching::ReportOptions ro;
    ro.compact = options.compact;
    ro.context = options.context;
    ro.bytes = options.bytes;
    ro.color = false;
    return ro;
}

} // namespace

std::string diff_text(const matching::FunctionDiff& diff, const DiffExportOptions& options) { return matching::to_text(diff, report_options(options)); }

std::string diff_json(const matching::FunctionDiff& diff, const DiffExportOptions& options) {
    return dump_pretty(matching::to_json(diff, report_options(options))) + "\n";
}

} // namespace decomp::vm
