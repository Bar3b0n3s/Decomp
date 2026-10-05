#include "viewmodel/dashboard.hpp"

#include "core/fs.hpp"
#include "core/hash.hpp"

#include <algorithm>
#include <format>

namespace decomp::vm {

std::vector<RichBuild> rich_builds(const std::vector<pe::RichEntry>& entries) {
    std::vector<RichBuild> out;
    for (const auto& e : entries) {
        RichBuild b;
        b.product_id = e.product_id;
        b.build = e.build;
        b.count = e.count;
        if (auto d = pe::describe_rich_product(e.product_id)) b.description = *d;
        else b.description = std::format("product {:#06x}", e.product_id);
        if (b.description.find("compiler") != std::string::npos) b.role = RichBuild::Role::compiler;
        else if (b.description.starts_with("linker")) b.role = RichBuild::Role::linker;
        out.push_back(std::move(b));
    }
    std::ranges::stable_sort(out, {}, &RichBuild::role);
    return out;
}

TargetIdentity target_identity(const Program& program, const project::TargetStatus* status) {
    const pe::Image& image = program.image();
    TargetIdentity t;
    // decomp.json names the target relative to the project ("../bin/game.exe"): show it resolved.
    t.path = fs::to_utf8(program.path().lexically_normal());
    t.file_size = image.data().size();
    if (status) {
        t.sha1 = status->actual_sha1;
        t.expected_sha1 = status->expected_sha1;
        t.sha1_ok = status->sha1_ok;
        t.pdb = status->pdb;
        t.pdb_detail = status->pdb_detail;
    } else {
        t.sha1 = sha1_hex(image.data());
        t.pdb = program.pdb_status();
        t.pdb_detail = program.pdb_detail();
    }
    t.format = image.is_pe32_plus() ? "PE32+" : "PE32";
    t.arch = std::string(to_string(image.arch()));
    t.dll = image.is_dll();
    t.image_base = image.image_base();
    t.entry_point = image.entry_point();
    if (t.entry_point)
        if (const Symbol* s = program.symbols().at(t.entry_point)) t.entry_name = s->display.empty() ? s->name : s->display;
    t.linker_version = std::format("{}.{:02}", image.linker_major(), image.linker_minor());
    t.rich = rich_builds(image.rich_entries());
    if (program.pdb_path()) t.pdb_path = fs::to_utf8(program.pdb_path()->lexically_normal());
    if (const auto& cv = image.codeview()) {
        t.has_codeview = true;
        t.codeview_path = cv->pdb_path;
        if (cv->signature == "RSDS") t.guid = cv->guid_string();
        t.age = cv->age;
    }
    return t;
}

std::string pdb_status_text(PdbStatus status) {
    switch (status) {
    case PdbStatus::matched: return "loaded: matching GUID and age";
    case PdbStatus::mismatch: return "mismatch: the PDB belongs to another build (ignored)";
    case PdbStatus::unsupported: return "unsupported format (ignored)";
    case PdbStatus::absent: return "absent";
    }
    return "absent";
}

double SpendSummary::cache_hit_rate() const {
    const long long prompt = usage.input + usage.cache_write + usage.cache_read;
    return prompt ? static_cast<double>(usage.cache_read) / static_cast<double>(prompt) : 0.0;
}

SpendSummary run_spend(const events::RunStateData& run) {
    SpendSummary s;
    s.usd = run.cost_usd;
    s.usage = run.usage;
    s.matched = static_cast<usize>(std::max(run.matched, 0));
    s.runs = 1;
    return s;
}

SpendSummary slice_spend(const CostSlice& slice) {
    SpendSummary s;
    s.usd = slice.cost_usd;
    s.usage = slice.usage;
    s.matched = slice.matched;
    s.runs = slice.runs;
    return s;
}

std::string_view to_string(ActivityKind kind) {
    switch (kind) {
    case ActivityKind::matched: return "matched";
    case ActivityKind::gave_up: return "gave up";
    case ActivityKind::refused: return "refused";
    case ActivityKind::error: return "error";
    }
    return "matched";
}

namespace {

std::optional<ActivityKind> kind_of_outcome(std::string_view outcome) {
    if (outcome == "matched") return ActivityKind::matched;
    if (outcome == "gave_up") return ActivityKind::gave_up;
    if (outcome == "refused") return ActivityKind::refused;
    if (outcome == "error") return ActivityKind::error;
    return std::nullopt;
}

std::string readable(const std::string& display, const std::string& function) { return display.empty() ? function : display; }

} // namespace

std::vector<ActivityItem> recent_activity(const events::RunStateData* shown, const std::vector<RunRecord>& runs, usize limit) {
    std::vector<ActivityItem> out;
    if (shown) {
        for (const auto& [id, s] : shown->sessions) {
            if (!s->finished) continue;
            auto kind = kind_of_outcome(s->outcome);
            if (!kind) continue;
            ActivityItem item;
            item.time = s->ended;
            item.kind = *kind;
            item.va = s->va;
            item.function = readable(s->display, s->function);
            item.session = s->id;
            item.run = shown->run_id;
            item.detail = s->detail;
            if (*kind == ActivityKind::refused && !s->refusal_category.empty() && item.detail.empty())
                item.detail = "category: " + s->refusal_category;
            out.push_back(std::move(item));
        }
        for (const auto& e : shown->error_log) {
            // A failed session is already in the feed through its outcome.
            if (e.kind == "session") continue;
            ActivityItem item;
            item.time = e.time;
            item.kind = ActivityKind::error;
            item.session = e.session;
            item.run = shown->run_id;
            item.detail = e.kind == "api" && e.status ? std::format("HTTP {}: {}", e.status, e.message) : e.message;
            if (const auto* s = e.session.empty() ? nullptr : shown->session(e.session)) {
                item.va = s->va;
                item.function = readable(s->display, s->function);
            }
            out.push_back(std::move(item));
        }
    }
    for (const auto& r : runs) {
        if (shown && r.id == shown->run_id) continue;
        const TimePoint when = r.finished != TimePoint{} ? r.finished : r.started;
        for (const auto& f : r.functions) {
            auto kind = f.matched ? std::optional(ActivityKind::matched) : kind_of_outcome(f.outcome);
            if (!kind) continue;
            ActivityItem item;
            item.time = when;
            item.kind = *kind;
            item.va = f.va;
            item.function = readable(f.display, f.function);
            item.run = r.id;
            out.push_back(std::move(item));
        }
    }
    std::ranges::stable_sort(out, [](const ActivityItem& a, const ActivityItem& b) { return a.time > b.time; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

} // namespace decomp::vm
