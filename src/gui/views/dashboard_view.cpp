// Dashboard (docs/ui.md#dashboard): the target's identity, progress by code bytes and functions with the
// status buckets, progress over time, the treemap of the code, spend and recent activity. Every count
// opens the Function browser filtered to it; every function opens the Inspector.

#include "gui/views/dashboard_view.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/export.hpp"
#include "gui/views/treemap_widget.hpp"
#include "gui/views/view_support.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"
#include "viewmodel/browser.hpp"
#include "viewmodel/cost.hpp"
#include "viewmodel/dashboard.hpp"
#include "viewmodel/exports.hpp"
#include "viewmodel/progress.hpp"
#include "viewmodel/run_history.hpp"
#include "viewmodel/treemap.hpp"

#include <implot.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace decomp::gui {

namespace {

using project::FunctionStatus;
using Clock = std::chrono::steady_clock;

struct ProgressKey {
    ProjectInputs project;
    u64 overlay = 0;
    bool operator==(const ProgressKey&) const = default;
};

struct MapKey {
    ProjectInputs project;
    u64 overlay = 0;
    int width = 0, height = 0;
    bool operator==(const MapKey&) const = default;
};

struct MapData {
    std::vector<vm::FunctionRow> rows;
    vm::Treemap map;
};

struct RunsKey {
    ProjectInputs project;
    std::string run, status;
    int finished = 0;
    bool operator==(const RunsKey&) const = default;
};

struct RunsData {
    std::shared_ptr<const std::vector<vm::RunRecord>> runs;
    vm::ProgressHistory history;
    vm::SpendSummary all_time;
};

struct ActivityKey {
    u64 runs = 0;  // generation of the run records it read
    std::string run;
    u64 seq = 0;
    bool operator==(const ActivityKey&) const = default;
};

struct IdentityKey {
    std::string root;
    std::weak_ptr<const Program> program;
    bool operator==(const IdentityKey& o) const {
        return root == o.root && !program.owner_before(o.program) && !o.program.owner_before(program);
    }
};

std::string with_commas(u64 v) {
    std::string digits = std::to_string(v), out;
    for (usize i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

std::string bin_label(usize i) { return i == 9 ? std::string("90-100%") : std::format("{}-{}%", i * 10, i * 10 + 9); }

void open_filtered(ViewContext& ctx, const vm::FunctionFilter& filter) { ctx.open("function_browser", {.anchor = vm::browser_anchor(filter)}); }

void open_status(ViewContext& ctx, FunctionStatus status) {
    vm::FunctionFilter f;
    f.statuses = {status};
    open_filtered(ctx, f);
}

void open_bin(ViewContext& ctx, usize bin) {
    vm::FunctionFilter f;
    f.statuses = {FunctionStatus::nonmatching};
    f.min_best = static_cast<double>(bin * 10);
    if (bin < 9) f.best_below = static_cast<double>(bin * 10 + 10);
    open_filtered(ctx, f);
}

class DashboardView final : public View {
public:
    std::string_view id() const override { return "dashboard"; }
    std::string_view title() const override { return "Dashboard"; }

    void draw(ViewContext& ctx) override {
        load_state(ctx);
        const ProjectAccess access = project_access(ctx);
        if (!access.ready()) {
            require_project(ctx, access,
                            "The Dashboard shows the target, progress by code bytes and functions, the status buckets, progress over "
                            "time, a treemap of the code, spend and recent activity.");
            draw_run_only(ctx);
            return;
        }
        update_jobs(ctx, access);
        draw_header(ctx, access);
        const float width = ImGui::GetContentRegionAvail().x;
        const bool wide = width >= ImGui::GetFontSize() * 52;
        if (wide && ImGui::BeginTable("##top", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            draw_identity(ctx);
            ImGui::TableNextColumn();
            draw_spend(ctx);
            ImGui::EndTable();
        } else {
            draw_identity(ctx);
            draw_spend(ctx);
        }
        draw_progress(ctx);
        if (wide && ImGui::BeginTable("##buckets_row", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            draw_buckets(ctx);
            ImGui::TableNextColumn();
            draw_distribution(ctx);
            ImGui::EndTable();
        } else {
            draw_buckets(ctx);
            draw_distribution(ctx);
        }
        draw_treemap(ctx);
        draw_history(ctx);
        draw_activity(ctx);
    }

private:
    // ---- state ----------------------------------------------------------------------------------------

    void load_state(ViewContext& ctx) {
        const std::string key = fs::to_utf8(ctx.project.root);
        if (state_loaded_ && key == state_project_) return;
        state_loaded_ = true;
        state_project_ = key;
        const Json& s = ctx.view_state(id());
        coloring_ = json_string_or(s, "treemap", "status") == "best_match" ? TreemapColoring::best_match : TreemapColoring::status;
        per_day_ = json_bool_or(s, "per_day", false);
        history_bytes_ = json_bool_or(s, "history_bytes", false);
    }

    void save_state(ViewContext& ctx) {
        Json& s = ctx.view_state(id());
        s["treemap"] = coloring_ == TreemapColoring::best_match ? "best_match" : "status";
        s["per_day"] = per_day_;
        s["history_bytes"] = history_bytes_;
        ctx.mark_settings_dirty();
    }

    // ---- jobs -----------------------------------------------------------------------------------------

    void update_jobs(ViewContext& ctx, const ProjectAccess& access) {
        progress_.poll();
        map_.poll();
        if (runs_.poll()) ++runs_generation_;
        activity_.poll();
        identity_.poll();
        const ProjectInputs inputs = project_inputs(access);
        if (inputs.root != project_root_) {
            // Another project: nothing shown so far applies to it.
            project_root_ = inputs.root;
            progress_.reset();
            map_.reset();
            runs_.reset();
            activity_.reset();
            identity_.reset();
        }
        const events::RunStateData* live = live_run(ctx);
        const u64 overlay = live ? vm::live_overlay_digest(*live) : 0;
        const std::shared_ptr<const events::RunStateData> live_snapshot = live ? ctx.snapshot : nullptr;
        const auto program = access.program;
        const project::Project& project = *access.project;  // copied into the jobs

        progress_.update(
            ctx.jobs, ProgressKey{inputs, overlay},
            [&] { return [=] { return vm::dashboard_progress(program->symbols(), project, live_snapshot.get()); }; },
            refresh_interval(ctx, std::chrono::milliseconds(400)));

        if (map_size_.x > 0) {
            const MapKey key{inputs, overlay, static_cast<int>(map_size_.x), static_cast<int>(map_size_.y)};
            const double header = std::floor(ImGui::GetFontSize() + 4);
            map_.update(
                ctx.jobs, key,
                [&] {
                    return [=] {
                        MapData d;
                        d.rows = vm::build_function_rows(program->symbols(), *project.function_infos(), live_snapshot.get());
                        d.map = vm::build_text_treemap(d.rows, program->image(), vm::Rect{0, 0, static_cast<double>(key.width), static_cast<double>(key.height)},
                                                       vm::TreemapOptions{2, header});
                        return d;
                    };
                },
                refresh_interval(ctx, std::chrono::milliseconds(500)));
        }

        const auto* shown = ctx.snapshot.get();
        const RunsKey runs_key{inputs, shown ? shown->run_id : std::string(), shown ? shown->status : std::string(), shown ? shown->finished : 0};
        const std::shared_ptr<const events::RunStateData> shown_snapshot = ctx.snapshot;
        runs_.update(
            ctx.jobs, runs_key,
            [&] {
                return [=, offset = local_utc_offset()] {
                    RunsData d;
                    auto records = vm::load_run_records(project.runs_dir());
                    if (shown_snapshot && !shown_snapshot->run_id.empty()) vm::merge_live_run(records, *shown_snapshot);
                    d.history = vm::progress_history(records, program->symbols(), offset);
                    d.all_time = vm::slice_spend(vm::cost_report(records, program->symbols(), *project.function_infos(), nullptr, offset).total);
                    d.runs = std::make_shared<const std::vector<vm::RunRecord>>(std::move(records));
                    return d;
                };
            },
            refresh_interval(ctx, std::chrono::milliseconds(1500)));

        if (runs_.value()) {
            const auto records = runs_.value()->runs;
            const ActivityKey activity_key{runs_generation_, shown ? shown->run_id : std::string(), shown ? shown->last_seq : 0};
            activity_.update(
                ctx.jobs, activity_key, [&] { return [=] { return vm::recent_activity(shown_snapshot.get(), *records, 100); }; },
                refresh_interval(ctx, std::chrono::milliseconds(750)));
        }

        std::optional<project::TargetStatus> status = access.workspace ? access.workspace->target_status() : std::nullopt;
        identity_.update(ctx.jobs, IdentityKey{inputs.root, program},
                         [&] { return [=] { return vm::target_identity(*program, status ? &*status : nullptr); }; });
    }

    // ---- sections -------------------------------------------------------------------------------------

    void draw_header(ViewContext& ctx, const ProjectAccess& access) {
        const std::string name = fs::to_utf8(access.project->root().filename());
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
        ImGui::TextUnformatted(std::format("{}  {}", name, ctx.project.target).c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        const vm::DashboardProgress* p = progress_.value() ? &*progress_.value() : nullptr;
        const vm::ProgressHistory* h = runs_.value() ? &runs_.value()->history : nullptr;
        const std::string target = ctx.project.target;
        auto report = [p, h, name, target] {
            vm::ProgressReport r;
            r.project = name;
            r.target = target;
            r.generated = std::chrono::system_clock::now();
            if (p) r.progress = *p;
            if (h) r.history = *h;
            return r;
        };
        const ExportFormat formats[] = {
            {"Markdown", "md", [report] { return vm::progress_markdown(report()); }},
            {"JSON", "json", [report] { return dump_pretty(vm::progress_json(report())) + "\n"; }},
        };
        align_right(ImGui::CalcTextSize("Export").x + ImGui::GetStyle().FramePadding.x * 2);
        ImGui::BeginDisabled(!p);
        export_button(ctx, "##progress_export", "progress", formats);
        ImGui::EndDisabled();
    }

    void draw_identity(ViewContext& ctx) {
        ImGui::SeparatorText("Target");
        if (!identity_.value()) {
            ImGui::TextDisabled("Reading the target...");
            return;
        }
        const vm::TargetIdentity& t = *identity_.value();
        const ThemeColors& c = ctx.colors();
        if (!ImGui::BeginTable("##identity", 2, ImGuiTableFlags_SizingFixedFit)) return;
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);
        auto row = [](const char* key) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", key);
            ImGui::TableNextColumn();
        };
        row("Path");
        ImGui::TextWrapped("%s", t.path.c_str());
        row("Size");
        ImGui::Text("%s (%s bytes)", bytes_text(t.file_size).c_str(), with_commas(t.file_size).c_str());
        row("SHA-1");
        ImGui::PushFont(ctx.fonts.mono, 0.0f);
        ImGui::TextUnformatted(t.sha1.c_str());
        ImGui::PopFont();
        if (t.expected_sha1.empty()) {
            colored_text(c.warn, "not recorded in decomp.json: unverified");
        } else if (t.sha1_ok) {
            status_label("verified against decomp.json", c.ok);
        } else {
            status_label("MISMATCH: decomp.json expects another binary; runs are blocked", c.error);
            ImGui::PushFont(ctx.fonts.mono, 0.0f);
            colored_text(c.error, "expected " + t.expected_sha1);
            ImGui::PopFont();
        }
        row("Format");
        ImGui::Text("%s %s %s, linker %s", t.format.c_str(), t.arch.c_str(), t.dll ? "DLL" : "EXE", t.linker_version.c_str());
        row("Image base");
        ImGui::TextUnformatted(hex(t.image_base, 8).c_str());
        row("Entry point");
        if (t.entry_point) {
            ImGui::PushID("entry");
            function_link(ctx, t.entry_name.empty() ? hex(t.entry_point, 8) : std::format("{} ({})", t.entry_name, hex(t.entry_point, 8)), t.entry_point);
            ImGui::PopID();
        } else {
            ImGui::TextDisabled("none");
        }
        row("Built with");
        if (t.built_with.empty()) {
            ImGui::TextDisabled(t.rich.empty() ? "unknown (no Rich header: not linked by Microsoft's linker)" : "unknown (the Rich header names no compiler)");
        } else {
            ImGui::TextWrapped("%s", t.built_with.c_str());
            if (!t.suggested_toolchain.empty()) {
                ImGui::TextDisabled("toolchain name for this release: %s", t.suggested_toolchain.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Toolchains")) ctx.open("toolchains");
            }
            for (const auto& note : t.build_notes) ImGui::BulletText("%s", note.c_str());
        }
        row("Rich header");
        if (t.rich.empty()) {
            ImGui::TextDisabled("none (not linked by Microsoft's linker)");
        } else {
            if (!t.rich_checksum_ok) colored_text(c.warn, "checksum mismatch: edited after linking");
            usize other = 0;
            for (const auto& r : t.rich) {
                if (r.role == vm::RichBuild::Role::other) {
                    ++other;
                    continue;
                }
                ImGui::Text("%s (%u objects)", r.text.c_str(), r.count);
            }
            if (other > 0 && ImGui::TreeNode("##rich_other", "%zu other entries", other)) {
                for (const auto& r : t.rich)
                    if (r.role == vm::RichBuild::Role::other) ImGui::BulletText("%s (%u)", r.text.c_str(), r.count);
                ImGui::TreePop();
            }
        }
        row("PDB");
        const ImVec4& pdb_color = t.pdb == PdbStatus::matched ? c.ok : t.pdb == PdbStatus::absent ? c.muted : c.warn;
        status_label(vm::pdb_status_text(t.pdb), pdb_color);
        if (!t.pdb_path.empty()) ImGui::TextWrapped("%s", t.pdb_path.c_str());
        if (!t.pdb_detail.empty()) ImGui::TextWrapped("%s", t.pdb_detail.c_str());
        if (t.has_codeview) {
            row("Debug record");
            ImGui::TextWrapped("%s", t.codeview_path.c_str());
            if (!t.guid.empty()) {
                ImGui::PushFont(ctx.fonts.mono, 0.0f);
                ImGui::TextWrapped("%s age %u", t.guid.c_str(), t.age);
                ImGui::PopFont();
            }
        }
        ImGui::EndTable();
    }

    // One segmented bar: a segment per status in Dashboard order. Returns the status clicked.
    std::optional<FunctionStatus> segmented_bar(ViewContext& ctx, const char* id, const vm::DashboardProgress& p, bool bytes) {
        const float width = std::max(ImGui::GetContentRegionAvail().x, 50.0f);
        const float height = ImGui::GetFrameHeight();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg));
        std::optional<FunctionStatus> under;
        float x = pos.x;
        const float mouse = ImGui::GetIO().MousePos.x;
        for (const auto& s : p.segments) {
            const double share = bytes ? s.byte_share : s.function_share;
            if (share <= 0) continue;
            const float w = static_cast<float>(share) * width;
            dl->AddRectFilled(ImVec2(x, pos.y), ImVec2(x + w, pos.y + height), ImGui::GetColorU32(status_color(ctx, s.status)));
            if (hovered && mouse >= x && mouse < x + w) {
                under = s.status;
                ImGui::SetTooltip("%s: %zu functions, %s bytes (%.1f%% of the %s)\nClick to list them in the Function browser.",
                                  std::string(status_text(s.status)).c_str(), s.functions, with_commas(s.bytes).c_str(), share * 100.0,
                                  bytes ? "code bytes" : "functions");
            }
            x += w;
        }
        dl->AddRect(pos, ImVec2(pos.x + width, pos.y + height), ImGui::GetColorU32(ImGuiCol_Border));
        return clicked ? under : std::nullopt;
    }

    void draw_progress(ViewContext& ctx) {
        ImGui::SeparatorText("Progress");
        if (!progress_.value()) {
            ImGui::TextDisabled("Counting...");
            return;
        }
        const vm::DashboardProgress& p = *progress_.value();
        const project::Progress& s = p.stored;
        // The same figures and wording as `decomp status`.
        ImGui::Text("Code bytes: %.1f%% matched (%s of %s bytes)", s.percent_bytes(), with_commas(s.matched_bytes).c_str(),
                    with_commas(s.code_bytes).c_str());
        if (auto st = segmented_bar(ctx, "##bytes_bar", p, true)) open_status(ctx, *st);
        ImGui::Text("Functions: %.1f%% matched (%zu of %zu)", s.percent_functions(), s.matched_functions, s.functions);
        if (auto st = segmented_bar(ctx, "##functions_bar", p, false)) open_status(ctx, *st);
        // The legend names every segment (color is never the only cue) and opens it.
        for (usize i = 0; i < p.segments.size(); ++i) {
            const auto& seg = p.segments[i];
            const std::string text = std::format("{} {}", status_text(seg.status), seg.functions);
            const float w = ImGui::GetTextLineHeight() + ImGui::CalcTextSize(text.c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2;
            if (i > 0) {
                ImGui::SameLine();
                if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
            }
            ImGui::PushID(static_cast<int>(i));
            status_dot(status_color(ctx, seg.status));
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            if (ImGui::TextLink(text.c_str())) open_status(ctx, seg.status);
            ImGui::PopID();
        }
        if (p.running > 0) ImGui::TextDisabled("%zu function(s) in progress in the live run.", p.running);
        ImGui::TextDisabled("Spend recorded in symbols.txt: %s", vm::format_usd(s.spend_usd).c_str());
    }

    void draw_buckets(ViewContext& ctx) {
        ImGui::SeparatorText("Status buckets");
        if (!progress_.value()) return;
        const vm::DashboardProgress& p = *progress_.value();
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("##buckets", 4, flags)) return;
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Functions");
        ImGui::TableSetupColumn("Bytes");
        ImGui::TableSetupColumn("Share");
        ImGui::TableHeadersRow();
        for (usize i = 0; i < p.segments.size(); ++i) {
            const auto& seg = p.segments[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable("##bucket", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                open_status(ctx, seg.status);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("List these functions in the Function browser.");
            ImGui::SameLine();
            status_cell(ctx, seg.status);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", seg.functions);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(with_commas(seg.bytes).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.1f%% / %.1f%%", seg.function_share * 100.0, seg.byte_share * 100.0);
            ImGui::PopID();
        }
        ImGui::EndTable();
        ImGui::TextDisabled("Share: of all functions / of all code bytes.");
    }

    void draw_distribution(ViewContext& ctx) {
        ImGui::SeparatorText("Non-matching functions by best match");
        if (!progress_.value()) return;
        const vm::DashboardProgress& p = *progress_.value();
        double xs[10], ys[10];
        usize total = 0;
        for (usize i = 0; i < 10; ++i) {
            xs[i] = static_cast<double>(i) * 10.0 + 5.0;
            ys[i] = static_cast<double>(p.best_match_bins[i]);
            total += p.best_match_bins[i];
        }
        if (total == 0) {
            ImGui::TextDisabled("No non-matching function.");
            return;
        }
        if (ImPlot::BeginPlot("##best_bins", ImVec2(-1, ImGui::GetFontSize() * 11), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
            ImPlot::SetupAxes("best match %", "functions", ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, 100, ImPlotCond_Always);
            ImPlot::PlotBars("functions", xs, ys, 10, 8.0, {ImPlotProp_FillColor, status_color(ctx, FunctionStatus::nonmatching)});
            if (ImPlot::IsPlotHovered()) {
                const ImPlotPoint m = ImPlot::GetPlotMousePos();
                const auto bin = static_cast<usize>(std::clamp(static_cast<int>(m.x / 10.0), 0, 9));
                ImGui::SetTooltip("%s: %zu function(s)\nClick to list them.", bin_label(bin).c_str(), p.best_match_bins[bin]);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) open_bin(ctx, bin);
            }
            ImPlot::EndPlot();
        }
        // The same numbers as text, each a link (nothing is available only on hover).
        for (usize i = 0; i < 10; ++i) {
            if (p.best_match_bins[i] == 0) continue;
            ImGui::PushID(static_cast<int>(i));
            const std::string text = std::format("{}: {}", bin_label(i), p.best_match_bins[i]);
            if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(text.c_str()).x + ImGui::GetStyle().ItemSpacing.x) ImGui::NewLine();
            if (ImGui::TextLink(text.c_str())) open_bin(ctx, i);
            ImGui::SameLine();
            ImGui::PopID();
        }
        ImGui::NewLine();
    }

    void draw_treemap(ViewContext& ctx) {
        ImGui::SeparatorText("Code map");
        int coloring = coloring_ == TreemapColoring::status ? 0 : 1;
        ImGui::TextUnformatted("Color by");
        ImGui::SameLine();
        bool changed = ImGui::RadioButton("status", &coloring, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("best match", &coloring, 1);
        if (changed) {
            coloring_ = coloring == 0 ? TreemapColoring::status : TreemapColoring::best_match;
            save_state(ctx);
        }
        ImGui::SameLine();
        draw_treemap_legend(ctx);

        // The layout follows the box once a resize has settled.
        const float width = std::floor(std::max(ImGui::GetContentRegionAvail().x, 100.0f));
        const float height = std::floor(std::clamp(width * 0.42f, ImGui::GetFontSize() * 10, ImGui::GetFontSize() * 26));
        const ImVec2 want(width, height);
        if (want.x != map_size_.x || want.y != map_size_.y) {
            if (want.x != pending_size_.x || want.y != pending_size_.y) {
                pending_size_ = want;
                pending_since_ = Clock::now();
            }
            const bool settled = map_size_.x == 0 || !ImGui::IsMouseDown(ImGuiMouseButton_Left) || Clock::now() - pending_since_ > std::chrono::milliseconds(300);
            if (settled) map_size_ = want;
        }
        if (!map_.value()) {
            ImGui::Dummy(want);
            ImGui::TextDisabled("Laying out the code...");
            return;
        }
        const MapData& d = *map_.value();
        if (d.map.cells.empty()) {
            ImGui::TextDisabled("No function has a known size yet.");
            return;
        }
        const TreemapEvents ev = treemap_.draw(ctx, d.map, d.rows, want, coloring_, ctx.selection.function_va);
        if (ev.clicked) ctx.open("inspector", {.va = d.map.cells[*ev.clicked].va});
        // The details pane mirrors the tooltip (nothing only on hover): the hovered or the selected cell.
        std::optional<usize> shown = ev.hovered;
        if (!shown && ctx.selection.function_va) shown = d.map.find(*ctx.selection.function_va);
        if (shown) ImGui::TextWrapped("%s", describe_cell(d.map.cells[*shown], d.rows).c_str());
        else ImGui::TextDisabled("Hover a cell for its function; click to open it in the Inspector.");
    }

    void draw_treemap_legend(ViewContext& ctx) {
        if (coloring_ == TreemapColoring::status) {
            for (FunctionStatus s : vm::kStatusOrder) {
                ImGui::SameLine();
                status_label(status_text(s), status_color(ctx, s));
            }
            return;
        }
        for (int v : {0, 25, 50, 75, 100}) {
            ImGui::SameLine();
            status_label(std::format("{}%", v), best_match_color(v));
        }
        ImGui::SameLine();
        status_label("not worked on", status_color(ctx, FunctionStatus::unstarted));
    }

    void draw_history(ViewContext& ctx) {
        ImGui::SeparatorText("Progress over time");
        if (!runs_.value()) {
            ImGui::TextDisabled("Reading the run summaries...");
            return;
        }
        const vm::ProgressHistory& h = runs_.value()->history;
        int mode = per_day_ ? 1 : 0, unit = history_bytes_ ? 1 : 0;
        bool changed = ImGui::RadioButton("per run", &mode, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("per day", &mode, 1);
        ImGui::SameLine(0, ImGui::GetFontSize() * 2);
        changed |= ImGui::RadioButton("functions", &unit, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("bytes", &unit, 1);
        if (changed) {
            per_day_ = mode == 1;
            history_bytes_ = unit == 1;
            save_state(ctx);
        }
        const auto& points = per_day_ ? h.days : h.runs;
        if (points.empty()) {
            ImGui::TextDisabled("No run has been recorded yet. Runs started from the GUI or with decomp run appear here.");
            return;
        }
        const vm::Series& series = per_day_ ? (history_bytes_ ? h.day_bytes : h.day_functions) : (history_bytes_ ? h.run_bytes : h.run_functions);
        if (ImPlot::BeginPlot("##history", ImVec2(-1, ImGui::GetFontSize() * 12), ImPlotFlags_NoMenus)) {
            ImPlot::SetupAxes(nullptr, history_bytes_ ? "bytes matched" : "functions matched", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupLegend(ImPlotLocation_NorthWest);
            ImPlot::PlotStairs("matched so far", series.x.data(), series.y.data(), static_cast<int>(series.size()),
                               {ImPlotProp_LineColor, status_color(ctx, FunctionStatus::matched), ImPlotProp_LineWeight, 2.0f});
            ImPlot::PlotScatter("##points", series.x.data(), series.y.data(), static_cast<int>(series.size()),
                                {ImPlotProp_MarkerFillColor, status_color(ctx, FunctionStatus::matched), ImPlotProp_Flags, ImPlotItemFlags_NoLegend});
            ImPlot::EndPlot();
        }
        if (ImGui::TreeNode("##history_table", "%s (%zu)", per_day_ ? "Days" : "Runs", points.size())) {
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
            const float height = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(std::min<usize>(points.size(), 10) + 2);
            if (ImGui::BeginTable("##points_table", 6, flags, ImVec2(0, height))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                for (const char* label : {per_day_ ? "Day" : "Run", "Matched", "New", "Matched so far", "Bytes so far", "Spend"}) ImGui::TableSetupColumn(label);
                ImGui::TableHeadersRow();
                for (auto it = points.rbegin(); it != points.rend(); ++it) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(it->label.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", it->functions);
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", it->new_functions);
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", it->total_functions);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(with_commas(it->total_bytes).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(vm::format_usd(it->cost_usd).c_str());
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
    }

    void draw_spend(ViewContext& ctx) {
        ImGui::SeparatorText("Spend");
        const auto* shown = ctx.snapshot.get();
        std::optional<vm::SpendSummary> run;
        if (shown) run = vm::run_spend(*shown);
        const vm::SpendSummary* all = runs_.value() ? &runs_.value()->all_time : nullptr;
        spend_table(run ? &*run : nullptr, all);
    }

    static void spend_table(const vm::SpendSummary* run, const vm::SpendSummary* all) {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("##spend", 3, flags)) return;
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("This run");
        ImGui::TableSetupColumn("All runs");
        ImGui::TableHeadersRow();
        auto row = [&](const char* label, auto value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            if (run) ImGui::TextUnformatted(value(*run).c_str());
            else ImGui::TextDisabled("no run");
            ImGui::TableNextColumn();
            if (all) ImGui::TextUnformatted(value(*all).c_str());
            else ImGui::TextDisabled("...");
        };
        row("Dollars", [](const vm::SpendSummary& s) { return vm::format_usd(s.usd); });
        row("Input tokens", [](const vm::SpendSummary& s) { return with_commas(static_cast<u64>(s.usage.input)); });
        row("Output tokens", [](const vm::SpendSummary& s) { return with_commas(static_cast<u64>(s.usage.output)); });
        row("Cache write tokens", [](const vm::SpendSummary& s) { return with_commas(static_cast<u64>(s.usage.cache_write)); });
        row("Cache read tokens", [](const vm::SpendSummary& s) { return with_commas(static_cast<u64>(s.usage.cache_read)); });
        row("Cache hit", [](const vm::SpendSummary& s) { return std::format("{:.1f}%", s.cache_hit_rate() * 100.0); });
        row("Functions matched", [](const vm::SpendSummary& s) { return std::to_string(s.matched); });
        row("Dollars per match", [](const vm::SpendSummary& s) { return s.matched ? vm::format_usd(s.usd_per_match()) : std::string("-"); });
        ImGui::EndTable();
    }

    void draw_activity(ViewContext& ctx) {
        ImGui::SeparatorText("Recent activity");
        if (!activity_.value()) {
            ImGui::TextDisabled("...");
            return;
        }
        draw_activity_items(ctx, *activity_.value());
    }

    static void draw_activity_items(ViewContext& ctx, const std::vector<vm::ActivityItem>& items) {
        if (items.empty()) {
            ImGui::TextDisabled("No matches, give-ups, refusals or errors yet.");
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
        const float height = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(std::min<usize>(items.size(), 12) + 2);
        if (!ImGui::BeginTable("##activity", 4, flags, ImVec2(0, height))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Event");
        ImGui::TableSetupColumn("Function");
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        const ThemeColors& c = ctx.colors();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(items.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const vm::ActivityItem& item = items[static_cast<usize>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(local_date_time(item.time).c_str());
                ImGui::TableNextColumn();
                const ImVec4& color = item.kind == vm::ActivityKind::matched   ? c.ok
                                      : item.kind == vm::ActivityKind::error   ? c.error
                                      : item.kind == vm::ActivityKind::refused ? c.error
                                                                               : c.warn;
                status_label(vm::to_string(item.kind), color);
                ImGui::TableNextColumn();
                if (item.va) function_link(ctx, item.function.empty() ? hex(item.va, 8) : item.function, item.va, item.session);
                else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                if (!item.detail.empty()) ImGui::TextUnformatted(item.detail.c_str());
                else ImGui::TextDisabled("run %s", item.run.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    // Without a project (headless shells, or before one loads), the shown run's own figures.
    void draw_run_only(ViewContext& ctx) {
        const auto* shown = ctx.snapshot.get();
        if (!shown) return;
        ImGui::SeparatorText("Spend");
        const vm::SpendSummary run = vm::run_spend(*shown);
        spend_table(&run, nullptr);
        ImGui::SeparatorText("Recent activity");
        if (run_only_key_ != std::pair{shown->run_id, shown->last_seq}) {
            run_only_ = vm::recent_activity(shown, {}, 100);
            run_only_key_ = {shown->run_id, shown->last_seq};
        }
        draw_activity_items(ctx, run_only_);
    }

    KeyedJob<ProgressKey, vm::DashboardProgress> progress_;
    KeyedJob<MapKey, MapData> map_;
    KeyedJob<RunsKey, RunsData> runs_;
    u64 runs_generation_ = 0;
    KeyedJob<ActivityKey, std::vector<vm::ActivityItem>> activity_;
    KeyedJob<IdentityKey, vm::TargetIdentity> identity_;
    TreemapWidget treemap_;
    std::string project_root_;
    ImVec2 map_size_{0, 0}, pending_size_{0, 0};
    Clock::time_point pending_since_{};
    TreemapColoring coloring_ = TreemapColoring::status;
    bool per_day_ = false, history_bytes_ = false;
    bool state_loaded_ = false;
    std::string state_project_;
    std::vector<vm::ActivityItem> run_only_;
    std::pair<std::string, u64> run_only_key_{"", ~u64{0}};
};

} // namespace

std::unique_ptr<View> make_dashboard_view() { return std::make_unique<DashboardView>(); }

} // namespace decomp::gui
