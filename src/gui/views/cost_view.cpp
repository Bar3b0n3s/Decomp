// Cost and usage (docs/ui.md#cost-and-usage): spend by run, day, model and function, tokens by type,
// the cache-hit rate, per-match figures, the projection for the functions still to do, budgets with
// their alert threshold, and flags for fallback turns and usage priced without a price-table row.

#include "gui/views/cost_view.hpp"

#include "gui/export.hpp"
#include "gui/jobs.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"
#include "viewmodel/cost.hpp"
#include "viewmodel/exports.hpp"
#include "viewmodel/run_history.hpp"
#include "viewmodel/series.hpp"

#include <implot.h>

#include <algorithm>
#include <array>
#include <format>

namespace decomp::gui {

namespace {

using SteadyClock = std::chrono::steady_clock;

struct Report {
    vm::CostReport cost;
    vm::CostProjection projection;
};

std::string tokens_text(long long n) {
    if (n >= 10'000'000) return std::format("{:.0f}M", static_cast<double>(n) / 1e6);
    if (n >= 1'000'000) return std::format("{:.1f}M", static_cast<double>(n) / 1e6);
    if (n >= 10'000) return std::format("{:.0f}k", static_cast<double>(n) / 1e3);
    return std::to_string(n);
}

double cache_rate(const events::TokenUsage& u) {
    const long long prompt = u.input + u.cache_write + u.cache_read;
    return prompt ? static_cast<double>(u.cache_read) / static_cast<double>(prompt) : 0.0;
}

class CostView final : public View {
public:
    std::string_view id() const override { return "cost"; }
    std::string_view title() const override { return "Cost and usage"; }

    void draw(ViewContext& ctx) override {
        Workspace* ws = ctx.services.workspace;
        update(ctx, ws);
        draw_budgets(ctx);
        ImGui::Spacing();
        if (!report_) {
            draw_live_only(ctx);
            return;
        }
        draw_totals(ctx);
        draw_flags(ctx);
        ImGui::Spacing();
        if (ImGui::BeginTabBar("##cost_tabs")) {
            // The shown tab is remembered per project (and can be chosen through the view state).
            Json& state = ctx.view_state("cost");
            const std::string wanted = json_string_or(state, "tab", "runs");
            auto tab = [&](const char* label, const char* key) {
                const ImGuiTabItemFlags flags = wanted != shown_tab_ && wanted == key ? ImGuiTabItemFlags_SetSelected : 0;
                if (!ImGui::BeginTabItem(label, nullptr, flags)) return false;
                if (shown_tab_ != key) {
                    shown_tab_ = key;
                    if (wanted != key) {
                        state["tab"] = key;
                        ctx.mark_settings_dirty();
                    }
                }
                return true;
            };
            if (tab("By run", "runs")) {
                draw_slices(ctx, "##runs", report_->cost.by_run, true);
                ImGui::EndTabItem();
            }
            if (tab("By day", "days")) {
                draw_slices(ctx, "##days", report_->cost.by_day, false);
                ImGui::EndTabItem();
            }
            if (tab("By model", "models")) {
                draw_models();
                ImGui::EndTabItem();
            }
            if (tab("By function", "functions")) {
                draw_functions(ctx);
                ImGui::EndTabItem();
            }
            if (tab("Tokens", "tokens")) {
                draw_tokens();
                ImGui::EndTabItem();
            }
            if (tab("Cache hits", "cache")) {
                draw_cache(ctx);
                ImGui::EndTabItem();
            }
            if (tab("Projection", "projection")) {
                draw_projection();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

private:
    // ---- data ----------------------------------------------------------------------------------------

    void update(ViewContext& ctx, Workspace* ws) {
        project::Project* project = ws ? ws->project() : nullptr;
        auto program = ws ? ws->program() : nullptr;
        if (!project || !program) {
            records_job_.cancel();
            report_job_.cancel();
            records_.reset();
            report_.reset();
            root_.clear();
            return;
        }
        // Run summaries: read again for another project, another run shown, a run that ended, or on request.
        const bool live = ws->run_live();
        if (root_ != project->root() || serial_ != ws->run_serial() || (was_live_ && !live) || refresh_) {
            root_ = project->root();
            serial_ = ws->run_serial();
            refresh_ = false;
            records_job_.cancel();
            records_job_ = ctx.jobs.submit([dir = project->runs_dir()] { return vm::load_run_records(dir); });
        }
        was_live_ = live;
        if (records_job_.ready())
            if (auto records = records_job_.take()) {
                records_ = std::make_shared<const std::vector<vm::RunRecord>>(std::move(*records));
                ++records_version_;
            }
        if (!records_) return;

        // The report: again when the inputs change, at most every two seconds while a run streams in.
        const auto& snap = ctx.snapshot;
        const u64 seq = snap ? snap->last_seq : 0;
        const std::string run = snap ? snap->run_id : std::string();
        const bool inputs_changed = records_version_ != used_records_ || program.get() != used_program_ || project->version() != used_project_version_;
        const bool run_changed = seq != used_seq_ || run != used_run_;
        const auto now = SteadyClock::now();
        if (inputs_changed || (run_changed && now - computed_at_ >= std::chrono::seconds(2))) {
            used_records_ = records_version_;
            used_program_ = program.get();
            used_project_version_ = project->version();
            used_seq_ = seq;
            used_run_ = run;
            computed_at_ = now;
            // Only a live run is merged: a past run's record is already in the summaries.
            auto live_state = live ? snap : nullptr;
            report_job_.cancel();
            report_job_ = ctx.jobs.submit([records = records_, program, infos = project->function_infos(), live_state,
                                           offset = local_utc_offset()](const CancelToken& token) {
                std::vector<vm::RunRecord> all = *records;
                if (live_state) vm::merge_live_run(all, *live_state);
                token.throw_if_cancelled();
                Report r;
                r.cost = vm::cost_report(all, program->symbols(), *infos, live_state.get(), offset);
                token.throw_if_cancelled();
                r.projection = vm::project_remaining_cost(program->symbols(), *infos);
                return r;
            });
        }
        if (report_job_.ready())
            if (auto r = report_job_.take()) report_ = std::make_shared<const Report>(std::move(*r));
    }

    // ---- budgets ---------------------------------------------------------------------------------------

    void draw_budgets(ViewContext& ctx) {
        RunCommands& commands = *ctx.services.commands;
        const auto* s = ctx.snapshot.get();
        const bool live = commands.live();
        const ThemeColors& c = ctx.colors();
        ImGui::SeparatorText("Budgets");
        const double budget = s ? s->budget.run.usd : 0;
        const double spent = s ? s->cost_usd : 0;
        if (budget > 0) {
            const double used = spent / budget;
            const ImVec4& color = used >= 1.0 ? c.error : used >= ctx.settings.budget_alert ? c.warn : c.ok;
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
            ImGui::ProgressBar(static_cast<float>(std::min(used, 1.0)), ImVec2(ImGui::GetFontSize() * 14, 0),
                               std::format("${:.2f} of ${:.2f} ({:.0f}%)", spent, budget, used * 100).c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            status_label(used >= 1.0 ? "run budget spent" : used >= ctx.settings.budget_alert ? "near the run budget" : "within the run budget", color);
        } else if (s) {
            ImGui::Text("This run: %s spent, no run budget", vm::format_usd(spent).c_str());
        } else {
            ImGui::TextDisabled("No run is shown.");
        }

        ImGui::BeginDisabled(!live);
        if (!editing_budget_) budget_edit_ = budget;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        if (ImGui::InputDouble("Run budget $##cost", &budget_edit_, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue))
            commands.set_run_budget(std::max(budget_edit_, 0.0));
        editing_budget_ = ImGui::IsItemActive();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(live ? "0 = unlimited. Press Enter to apply to the live run." : "Applies to a live run.");
        ImGui::SameLine();
        if (ImGui::Button("Per-function limits...")) {
            limits_ = current_limits(s);
            ImGui::OpenPopup("##cost_limits");
        }
        if (ImGui::BeginPopup("##cost_limits")) {
            int turns = limits_.max_turns;
            float usd = static_cast<float>(limits_.max_cost_usd);
            int minutes = static_cast<int>(limits_.max_wall.count() / 60);
            int tokens_k = static_cast<int>(limits_.max_total_tokens / 1000);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
            if (ImGui::InputInt("Turns", &turns)) limits_.max_turns = std::max(turns, 1);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
            if (ImGui::InputFloat("USD (0 = unlimited)", &usd, 0, 0, "%.2f")) limits_.max_cost_usd = std::max(usd, 0.0f);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
            if (ImGui::InputInt("Minutes (0 = unlimited)", &minutes)) limits_.max_wall = std::chrono::minutes(std::max(minutes, 0));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
            if (ImGui::InputInt("Thousand tokens (0 = unlimited)", &tokens_k)) limits_.max_total_tokens = std::max(tokens_k, 0) * 1000LL;
            if (ImGui::Button("Apply to running and later sessions")) {
                commands.set_limits(limits_);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        float alert = static_cast<float>(ctx.settings.budget_alert * 100);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::SliderFloat("Alert at", &alert, 5, 100, "%.0f%%")) {
            ctx.settings.budget_alert = std::clamp(static_cast<double>(alert) / 100, 0.05, 1.0);
            ctx.mark_settings_dirty();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Spend turns amber and a warning is raised at this share of a run or function budget.");
        ImGui::TextDisabled("Project defaults (agent.max_usd_per_run, agent.max_usd_per_function, ...) are in Settings.");
    }

    static agent::LoopLimits current_limits(const events::RunStateData* s) {
        agent::LoopLimits l;
        if (!s) return l;
        if (s->config.is_object())
            if (auto it = s->config.find("limits"); it != s->config.end() && it->is_object()) {
                l.max_turns = static_cast<int>(json_int_or(*it, "max_turns", l.max_turns));
                l.max_cost_usd = json_number_or(*it, "max_usd", l.max_cost_usd);
                l.max_total_tokens = json_int_or(*it, "max_tokens", l.max_total_tokens);
                l.max_wall = std::chrono::seconds(json_int_or(*it, "max_seconds", l.max_wall.count()));
            }
        const auto& f = s->budget.function;
        if (f.turns > 0) l.max_turns = f.turns;
        if (f.usd > 0) l.max_cost_usd = f.usd;
        if (f.tokens > 0) l.max_total_tokens = f.tokens;
        if (f.minutes > 0) l.max_wall = std::chrono::minutes(f.minutes);
        return l;
    }

    // ---- summary -----------------------------------------------------------------------------------------

    void draw_live_only(ViewContext& ctx) {
        Workspace* ws = ctx.services.workspace;
        if (!ws || !ws->project()) {
            ImGui::TextDisabled("Open a project to see what its runs cost.");
            return;
        }
        ImGui::TextDisabled("Reading the project's run summaries...");
    }

    void draw_totals(ViewContext& ctx) {
        const vm::CostSlice& all = report_->cost.total;
        ImGui::SeparatorText("Spend");
        if (ImGui::BeginTable("##totals", 2, ImGuiTableFlags_SizingFixedFit)) {
            auto row = [](const char* label, const std::string& value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", label);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(value.c_str());
            };
            row("All runs", std::format("{} in {} run(s), {} function(s) worked, {} matched", vm::format_usd(all.cost_usd), all.runs, all.worked, all.matched));
            if (const auto* s = ctx.snapshot.get())
                row("This run", std::format("{}, {} matched of {} finished", vm::format_usd(s->cost_usd), s->matched, s->finished));
            row("Per match", all.matched ? std::format("{} and {:.1f} turns", vm::format_usd(all.usd_per_match()), all.turns_per_match()) : std::string("-"));
            row("Tokens", std::format("input {}, output {}, cache write {}, cache read {}", tokens_text(all.usage.input), tokens_text(all.usage.output),
                                      tokens_text(all.usage.cache_write), tokens_text(all.usage.cache_read)));
            row("Cache hits", std::format("{:.0f}% of prompt tokens read from the cache", all.cache_hit_rate() * 100));
            row("Per function", std::format("{} recorded on {} function(s)", vm::format_usd(report_->cost.function_spend_usd),
                                            report_->cost.by_function.size()));
            ImGui::EndTable();
        }
        ImGui::SameLine();
        std::array<ExportFormat, 5> formats{
            ExportFormat{"runs CSV", "csv", [r = report_] { return vm::cost_csv(r->cost, vm::CostTable::runs); }},
            ExportFormat{"days CSV", "csv", [r = report_] { return vm::cost_csv(r->cost, vm::CostTable::days); }},
            ExportFormat{"models CSV", "csv", [r = report_] { return vm::cost_csv(r->cost, vm::CostTable::models); }},
            ExportFormat{"functions CSV", "csv", [r = report_] { return vm::cost_csv(r->cost, vm::CostTable::functions); }},
            ExportFormat{"JSON", "json", [r = report_] { return vm::cost_json(r->cost, &r->projection).dump(2) + "\n"; }},
        };
        export_button(ctx, "##cost_export", "cost-report", formats);
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) refresh_ = true;
    }

    void draw_flags(ViewContext& ctx) {
        const ThemeColors& c = ctx.colors();
        if (report_->cost.fallback_turns > 0)
            status_label(std::format("{} turn(s) of this run were served (in part) by a fallback model", report_->cost.fallback_turns), c.warn);
        if (!report_->cost.unpriced_models.empty()) {
            std::string models;
            for (const auto& m : report_->cost.unpriced_models) models += (models.empty() ? "" : ", ") + m;
            status_label(std::format("No price known for {}: that usage was priced at the configured model's rates", models), c.warn);
        }
    }

    // ---- tables and charts --------------------------------------------------------------------------------

    void draw_slices(ViewContext& ctx, const char* id, const std::vector<vm::CostSlice>& slices, bool runs) {
        if (slices.empty()) {
            ImGui::TextDisabled("No runs yet.");
            return;
        }
        // Spend per slot, newest at the right.
        const float plot_h = ImGui::GetFontSize() * 9;
        if (ImPlot::BeginPlot(std::format("Spend{}", id).c_str(), ImVec2(-1, plot_h), ImPlotFlags_NoLegend)) {
            std::vector<double> xs, ys;
            for (usize i = 0; i < slices.size(); ++i) {
                xs.push_back(static_cast<double>(i));
                ys.push_back(slices[i].cost_usd);
            }
            ImPlot::SetupAxes(runs ? "run" : "day", "USD", 0, ImPlotAxisFlags_AutoFit);
            slot_axis(slices, runs);
            ImPlot::PlotBars("spend", xs.data(), ys.data(), static_cast<int>(xs.size()), 0.67);
            ImPlot::EndPlot();
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable(id, 9, flags, ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFrameHeight() * 6)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            for (const char* h : {runs ? "Run" : "Day", "Spend", "Worked", "Matched", "Success", "$/match", "Turns/match", "Tokens", "Cache hits"})
                ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(slices.size()));
            while (clipper.Step())
                for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
                    const vm::CostSlice& s = slices[slices.size() - 1 - static_cast<usize>(n)];  // newest first
                    ImGui::PushID(n);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (runs) {
                        if (ImGui::TextLink(s.key.c_str())) open_run(ctx, s.key);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Open this run (read-only) in the views.");
                    } else {
                        ImGui::TextUnformatted(s.key.c_str());
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(vm::format_usd(s.cost_usd).c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", s.worked);
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", s.matched);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f%%", s.success_rate() * 100);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(s.matched ? vm::format_usd(s.usd_per_match()).c_str() : "-");
                    ImGui::TableNextColumn();
                    if (s.matched) ImGui::Text("%.1f", s.turns_per_match());
                    else ImGui::TextUnformatted("-");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(tokens_text(s.usage.total()).c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f%%", s.cache_hit_rate() * 100);
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
    }

    // One slot per run or day on the x axis: labeled when there are few, numbered otherwise.
    static void slot_axis(const std::vector<vm::CostSlice>& slices, bool runs) {
        const int n = static_cast<int>(slices.size());
        ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, n - 0.5, ImPlotCond_Always);
        if (n > 12) {
            ImPlot::SetupAxisFormat(ImAxis_X1, "%.0f");
            return;
        }
        std::vector<double> at;
        std::vector<std::string> text;
        for (int i = 0; i < n; ++i) {
            const vm::CostSlice& s = slices[static_cast<usize>(i)];
            at.push_back(i);
            text.push_back(runs ? local_month_day_time(s.time) : s.key.size() > 5 ? s.key.substr(5) : s.key);
        }
        std::vector<const char*> labels;
        for (const auto& t : text) labels.push_back(t.c_str());
        ImPlot::SetupAxisTicks(ImAxis_X1, at.data(), n, labels.data());
    }

    void open_run(ViewContext& ctx, const std::string& id) {
        Workspace* ws = ctx.services.workspace;
        if (!ws || ws->run_id() == id) return;
        if (ws->run_live()) {
            ctx.notify(Severity::warning, "A run is in progress: stop it before opening another.");
            return;
        }
        if (auto r = ws->open_run(id); !r) ctx.notify(Severity::error, std::format("Cannot open the run: {}", r.error().message));
    }

    void draw_models() {
        const auto& models = report_->cost.by_model;
        if (models.empty()) {
            ImGui::TextDisabled("No runs yet.");
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##models", 7, flags)) {
            for (const char* h : {"Model and effort", "Runs", "Spend", "Worked", "Matched", "Success", "$/match"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (const auto& m : models) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.key.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", m.runs);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(vm::format_usd(m.cost_usd).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", m.worked);
                ImGui::TableNextColumn();
                ImGui::Text("%zu", m.matched);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f%%", m.success_rate() * 100);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.matched ? vm::format_usd(m.usd_per_match()).c_str() : "-");
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Success rate: functions matched of those worked on, per model and effort as configured for the run.");
    }

    void draw_functions(ViewContext& ctx) {
        const auto& fns = report_->cost.by_function;
        if (fns.empty()) {
            ImGui::TextDisabled("No function has recorded spend yet.");
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##functions", 7, flags, ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFrameHeight() * 6)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
            for (const char* h : {"Size", "Status", "Spend", "Of which live", "Attempts"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(fns.size()));
            while (clipper.Step())
                for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
                    const vm::FunctionSpend& f = fns[static_cast<usize>(n)];
                    ImGui::PushID(n);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%08llx", static_cast<unsigned long long>(f.va));
                    ImGui::TableNextColumn();
                    if (ImGui::TextLink(f.name.c_str())) ctx.open("inspector", NavTarget{.va = f.va});
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", f.size);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(std::string(project::to_string(f.status)).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(vm::format_usd(f.total_usd()).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(f.live_usd > 0 ? vm::format_usd(f.live_usd).c_str() : "");
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", f.attempts);
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
    }

    void draw_tokens() {
        const auto& runs = report_->cost.by_run;
        if (runs.empty()) {
            ImGui::TextDisabled("No runs yet.");
            return;
        }
        // Tokens by type per run, stacked (cache reads usually dominate; their price is a tenth of input).
        const int n = static_cast<int>(runs.size());
        std::vector<double> values;  // item-major: 4 rows of n
        for (int item = 0; item < 4; ++item)
            for (const auto& r : runs) {
                const auto& u = r.usage;
                const long long v = item == 0 ? u.input : item == 1 ? u.output : item == 2 ? u.cache_write : u.cache_read;
                values.push_back(static_cast<double>(v));
            }
        static const char* const labels[] = {"input", "output", "cache write", "cache read"};
        if (ImPlot::BeginPlot("Tokens per run##tokens", ImVec2(-1, ImGui::GetFontSize() * 14))) {
            ImPlot::SetupAxes("run", "tokens", 0, ImPlotAxisFlags_AutoFit);
            slot_axis(runs, true);
            ImPlot::PlotBarGroups(labels, values.data(), 4, n, 0.67, 0, ImPlotSpec(ImPlotProp_Flags, ImPlotBarGroupsFlags_Stacked));
            ImPlot::EndPlot();
        }
        const auto& u = report_->cost.total.usage;
        ImGui::Text("All runs: input %s, output %s, cache write %s, cache read %s", tokens_text(u.input).c_str(), tokens_text(u.output).c_str(),
                    tokens_text(u.cache_write).c_str(), tokens_text(u.cache_read).c_str());
    }

    void draw_cache(ViewContext& ctx) {
        const auto* s = ctx.snapshot.get();
        if (s && !s->minutes.empty()) {
            const auto minute = vm::cache_hit_rate_over_time(*s, s->started);
            const auto cumulative = vm::cache_hit_rate_over_time(*s, s->started, true);
            if (ImPlot::BeginPlot("Cache-hit rate in this run##cache_run", ImVec2(-1, ImGui::GetFontSize() * 10))) {
                ImPlot::SetupAxes("seconds since the run started", "share", ImPlotAxisFlags_AutoFit, 0);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 1, ImPlotCond_Always);
                ImPlot::PlotLine("per minute", minute.x.data(), minute.y.data(), static_cast<int>(minute.size()));
                ImPlot::PlotLine("so far", cumulative.x.data(), cumulative.y.data(), static_cast<int>(cumulative.size()));
                ImPlot::EndPlot();
            }
        } else {
            ImGui::TextDisabled("The cache-hit rate over time appears while a run is shown.");
        }
        const auto& runs = report_->cost.by_run;
        if (runs.size() > 1 && ImPlot::BeginPlot("Cache-hit rate per run##cache_runs", ImVec2(-1, ImGui::GetFontSize() * 9), ImPlotFlags_NoLegend)) {
            std::vector<double> xs, ys;
            for (usize i = 0; i < runs.size(); ++i) {
                xs.push_back(static_cast<double>(i));
                ys.push_back(cache_rate(runs[i].usage));
            }
            ImPlot::SetupAxes("run", "share", 0, 0);
            slot_axis(runs, true);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 1, ImPlotCond_Always);
            ImPlot::PlotBars("cache hits", xs.data(), ys.data(), static_cast<int>(xs.size()), 0.67);
            ImPlot::EndPlot();
        }
        ImGui::TextDisabled("From the second turn on, a session should read the shared prompt prefix from the cache.");
    }

    void draw_projection() {
        const vm::CostProjection& p = report_->projection;
        if (!p.has_history) {
            ImGui::TextDisabled("No finished function has recorded spend yet, so nothing can be projected.");
            ImGui::Text("%zu function(s) still to do.", p.remaining + p.unsized);
            return;
        }
        ImGui::Text("About %s for the %zu function(s) still to do", vm::format_usd(p.projected_usd).c_str(), p.remaining);
        if (p.unsized) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu more of unknown size not projected)", p.unsized);
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##projection", 5, flags)) {
            for (const char* h : {"Size", "Done with spend", "Mean spend", "Still to do", "Projected"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (const auto& b : p.buckets) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(vm::size_bucket_label(b.bucket).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", b.observed);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(vm::format_usd(b.mean_usd).c_str());
                if (b.from_nearby) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(nearby)");
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                        ImGui::SetTooltip("No function of this size is done yet: the nearest size's mean, scaled by size.");
                }
                ImGui::TableNextColumn();
                ImGui::Text("%zu", b.remaining);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(vm::format_usd(b.projected_usd).c_str());
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("A function's spend is everything its sessions cost; ones partly worked on are projected as if they started over.");
    }

    JobHandle<std::vector<vm::RunRecord>> records_job_;
    std::shared_ptr<const std::vector<vm::RunRecord>> records_;
    u64 records_version_ = 0;
    JobHandle<Report> report_job_;
    std::shared_ptr<const Report> report_;
    std::filesystem::path root_;
    u64 serial_ = ~u64{0};
    bool was_live_ = false;
    bool refresh_ = false;
    u64 used_records_ = ~u64{0};
    const void* used_program_ = nullptr;
    u64 used_project_version_ = ~u64{0};
    u64 used_seq_ = ~u64{0};
    std::string used_run_;
    SteadyClock::time_point computed_at_{};
    double budget_edit_ = 0;
    bool editing_budget_ = false;
    agent::LoopLimits limits_;
    std::string shown_tab_;
};

} // namespace

std::unique_ptr<View> make_cost_view() { return std::make_unique<CostView>(); }

} // namespace decomp::gui
