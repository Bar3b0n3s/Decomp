// Run monitor (docs/ui.md#run-monitor): the workers, the live feed, the queue, the worker timeline,
// throughput and rate limits, with the run's live controls.

#include "gui/views/run_monitor_view.hpp"

#include "core/strings.hpp"
#include "gui/views/remembered_tabs.hpp"
#include "gui/widgets.hpp"
#include "viewmodel/common.hpp"
#include "viewmodel/eta.hpp"

#include <implot.h>

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace decomp::gui {

namespace {

using Clock = std::chrono::system_clock;

std::string duration_text(Clock::duration d) {
    const auto s = std::chrono::duration_cast<std::chrono::seconds>(d).count();
    if (s < 0) return "-";
    if (s < 60) return std::format("{}s", s);
    if (s < 3600) return std::format("{}m{:02}s", s / 60, s % 60);
    return std::format("{}h{:02}m", s / 3600, (s / 60) % 60);
}

// Phases group into what the worker waits for: the model, its tools, the supervisor or nothing.
enum class PhaseGroup { model, tools, waiting, idle };

PhaseGroup group_of(std::string_view phase) {
    if (phase == "idle" || phase == "done" || phase == "retired" || phase.empty()) return PhaseGroup::idle;
    if (phase == "paused" || phase == "waiting for approval" || phase == "waiting for rate limit" || phase == "backoff" ||
        phase == "waiting for the first session" || phase == "run budget exhausted")
        return PhaseGroup::waiting;
    if (phase == "compiling" || phase.starts_with("running") || phase == "turn done") return PhaseGroup::tools;
    return PhaseGroup::model;  // starting, waiting for model, thinking, writing
}

ImVec4 group_color(PhaseGroup g, const ThemeColors& c) {
    switch (g) {
    case PhaseGroup::model: return c.info;
    case PhaseGroup::tools: return c.ok;
    case PhaseGroup::waiting: return c.warn;
    case PhaseGroup::idle: return c.muted;
    }
    return c.muted;
}

int max_turns(const events::RunStateData& s) {
    if (s.budget.function.turns > 0) return s.budget.function.turns;
    if (s.config.is_object())
        if (auto l = s.config.find("limits"); l != s.config.end() && l->is_object()) return static_cast<int>(json_int_or(*l, "max_turns", 0));
    return 0;
}

class RunMonitorView final : public View {
public:
    std::string_view id() const override { return "run_monitor"; }
    std::string_view title() const override { return "Run monitor"; }

    void draw(ViewContext& ctx) override {
        const auto* s = ctx.snapshot.get();
        if (!s) {
            ImGui::TextDisabled("No run. Start one with Run > Start run (F5), or open a past run (Run > Runs...).");
            return;
        }
        RunCommands& commands = *ctx.services.commands;
        const bool live = commands.live();
        draw_controls(ctx, *s, commands, live);
        draw_workers(ctx, *s, commands, live);
        ImGui::Spacing();
        if (ImGui::BeginTabBar("##monitor_tabs")) {
            tabs_.begin(ctx, "run_monitor", "activity");
            if (tabs_.item("Activity", "activity")) {
                draw_activity(*s);
                ImGui::EndTabItem();
            }
            if (tabs_.item(std::format("Queue ({})###queue", s->queue_total).c_str(), "queue")) {
                draw_queue(ctx, *s, commands, live);
                ImGui::EndTabItem();
            }
            if (tabs_.item("Timeline", "timeline")) {
                draw_timeline(ctx, *s);
                ImGui::EndTabItem();
            }
            if (tabs_.item("Throughput", "throughput")) {
                draw_throughput(*s);
                ImGui::EndTabItem();
            }
            if (tabs_.item("Rate limits", "rate_limits")) {
                draw_rate_limits(ctx, *s);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

private:
    void draw_controls(ViewContext& ctx, const events::RunStateData& s, RunCommands& commands, bool live) {
        ImGui::BeginDisabled(!live);
        // Concurrency: applied at the next scheduling decision.
        int workers = live ? commands.concurrency() : s.worker_count;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        if (ImGui::InputInt("Workers", &workers, 1, 1) && live) commands.set_concurrency(std::clamp(workers, 1, 64));
        ImGui::SameLine();
        // Run budget: edited, then applied with Enter.
        if (!editing_budget_) budget_ = s.budget.run.usd;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        if (ImGui::InputDouble("Run budget $", &budget_, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue) && live)
            commands.set_run_budget(std::max(budget_, 0.0));
        editing_budget_ = ImGui::IsItemActive();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("0 = unlimited. Press Enter to apply.");
        ImGui::SameLine();
        if (ImGui::Button("Limits...")) ImGui::OpenPopup("##limits");
        if (ImGui::BeginPopup("##limits")) {
            draw_limits_editor(s, commands);
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        if (!live) {
            ImGui::SameLine();
            ImGui::TextDisabled(s.status == "running" ? "(a past run: read-only)" : "(the run has ended)");
        }
        (void)ctx;
    }

    void draw_limits_editor(const events::RunStateData& s, RunCommands& commands) {
        if (ImGui::IsWindowAppearing()) {
            const Json limits = s.config.is_object() ? s.config.value("limits", Json::object()) : Json::object();
            limits_.max_turns = s.budget.function.turns > 0 ? s.budget.function.turns : static_cast<int>(json_int_or(limits, "max_turns", 40));
            limits_.max_cost_usd = s.budget.function.usd > 0 ? s.budget.function.usd : json_number_or(limits, "max_usd", 0);
            limits_.max_total_tokens = s.budget.function.tokens > 0 ? s.budget.function.tokens : json_int_or(limits, "max_tokens", 0);
            limits_.max_wall = std::chrono::seconds(s.budget.function.minutes > 0 ? s.budget.function.minutes * 60LL
                                                                                    : json_int_or(limits, "max_seconds", 0));
        }
        ImGui::TextUnformatted("Limits per function (running and later sessions):");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputInt("Turns", &limits_.max_turns);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputDouble("Dollars (0 = unlimited)", &limits_.max_cost_usd, 0, 0, "%.2f");
        long long tokens = limits_.max_total_tokens;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::InputScalar("Tokens (0 = unlimited)", ImGuiDataType_S64, &tokens)) limits_.max_total_tokens = std::max(0LL, tokens);
        int minutes = static_cast<int>(limits_.max_wall.count() / 60);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::InputInt("Minutes (0 = unlimited)", &minutes)) limits_.max_wall = std::chrono::minutes(std::max(0, minutes));
        if (ImGui::Button("Apply")) {
            limits_.max_turns = std::max(1, limits_.max_turns);
            limits_.max_cost_usd = std::max(0.0, limits_.max_cost_usd);
            commands.set_limits(limits_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    }

    void draw_workers(ViewContext& ctx, const events::RunStateData& s, RunCommands& commands, bool live) {
        const ThemeColors& c = ctx.colors();
        const auto now = Clock::now();
        const int turns_max = max_turns(s);
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
        const float height = ImGui::GetFrameHeightWithSpacing() * static_cast<float>(std::min<usize>(s.workers.size(), 8) + 1) + 4;
        if (!ImGui::BeginTable("##workers", 10, flags, ImVec2(0, height))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Worker");
        ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Phase");
        ImGui::TableSetupColumn("Turn");
        ImGui::TableSetupColumn("Elapsed");
        ImGui::TableSetupColumn("Best");
        ImGui::TableSetupColumn("Tokens");
        ImGui::TableSetupColumn("Dollars");
        ImGui::TableSetupColumn("Last tool", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        if (s.workers.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("No worker has started yet.");
        }
        for (const auto& [id, w] : s.workers) {
            ImGui::PushID(id);
            const events::SessionState* session = w.session.empty() ? nullptr : s.session(w.session);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", id);
            ImGui::TableNextColumn();
            if (session) {
                const std::string name = session->display.empty() ? session->function : session->display;
                if (ImGui::TextLink(name.c_str())) ctx.open("agent_session", NavTarget{.va = session->va, .session = session->id});
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("%s at %s\nSession %s\nOpen the Agent session.", session->function.c_str(), hex(session->va, 8).c_str(),
                                      session->id.c_str());
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            const std::string phase = w.phase.empty() ? "idle" : w.phase;
            colored_text(group_color(group_of(phase), c), phase);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("For %s", duration_text(now - w.phase_since).c_str());
            ImGui::TableNextColumn();
            if (session) {
                if (turns_max > 0) ImGui::Text("%d/%d", session->turn, turns_max);
                else ImGui::Text("%d", session->turn);
            }
            ImGui::TableNextColumn();
            if (session) ImGui::TextUnformatted(duration_text((session->finished ? session->ended : now) - session->started).c_str());
            ImGui::TableNextColumn();
            if (session) ImGui::Text("%.1f%%", session->best_match);
            ImGui::TableNextColumn();
            if (session) ImGui::Text("%lld", session->usage.total());
            ImGui::TableNextColumn();
            if (session) ImGui::Text("$%.3f", session->cost_usd);
            ImGui::TableNextColumn();
            if (session && !session->last_tool.empty()) {
                ImGui::Text("%s", session->last_tool.c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !session->last_tool_summary.empty())
                    ImGui::SetTooltip("%s", session->last_tool_summary.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!live);
            if (phase == "paused") {
                if (ImGui::SmallButton("Resume")) commands.resume_worker(id);
            } else if (ImGui::SmallButton("Pause")) {
                commands.pause_worker(id);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Pause or resume this worker between turns.");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void draw_activity(const events::RunStateData& s) {
        ImGui::Checkbox("Follow", &follow_);
        ImGui::SameLine();
        ImGui::TextDisabled("%zu of %llu lines", s.activity.size(), static_cast<unsigned long long>(s.activity_total));
        if (!ImGui::BeginChild("##feed", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            ImGui::EndChild();
            return;
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(s.activity.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) ImGui::TextUnformatted(s.activity[static_cast<usize>(i)].c_str());
        if (follow_ && s.activity_total != followed_) ImGui::SetScrollHereY(1.0f);
        followed_ = s.activity_total;
        ImGui::EndChild();
    }

    void draw_queue(ViewContext& ctx, const events::RunStateData& s, RunCommands& commands, bool live) {
        const auto& queue = *s.queue;
        if (queue.empty()) ImGui::TextDisabled("Nothing waits: every function of the run has been dispatched.");
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
        // Expected start and finish per queued function (the App's estimate, refreshed once a second).
        if (ctx.eta.get() != eta_source_) {
            eta_source_ = ctx.eta.get();
            eta_by_va_.clear();
            if (ctx.eta)
                for (const auto& item : ctx.eta->items) eta_by_va_[item.va] = {item.start, item.finish};
        }
        if (!queue.empty() && ImGui::BeginTable("##queue", 5, flags)) {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Difficulty");
            ImGui::TableSetupColumn("ETA");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(queue.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const auto& q = queue[static_cast<usize>(i)];
                    ImGui::PushID(static_cast<int>(q.va));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d%s", i + 1, q.pinned ? " (pinned)" : "");
                    ImGui::TableNextColumn();
                    if (ImGui::TextLink(q.function.c_str())) ctx.open("inspector", NavTarget{.va = q.va});
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f", q.difficulty);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                        ImGui::SetTooltip("The queue's estimate, from the function's size: easier functions run first.");
                    ImGui::TableNextColumn();
                    if (auto it = eta_by_va_.find(q.va); it != eta_by_va_.end()) {
                        ImGui::Text("in %s", vm::format_duration(it->second.first).c_str());
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                            ImGui::SetTooltip("Expected to start in %s and finish in %s, from how long functions of its size took.",
                                              vm::format_duration(it->second.first).c_str(), vm::format_duration(it->second.second).c_str());
                    } else {
                        ImGui::TextDisabled("-");
                    }
                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(!live);
                    ImGui::BeginDisabled(i == 0);
                    if (ImGui::SmallButton("Up")) commands.move(q.va, static_cast<usize>(i - 1));
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(static_cast<usize>(i) + 1 >= s.queue_total);
                    if (ImGui::SmallButton("Down")) commands.move(q.va, static_cast<usize>(i + 1));
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::SmallButton(q.pinned ? "Unpin" : "Pin")) commands.pin(q.va, !q.pinned);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Skip")) commands.skip(q.va);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) commands.remove(q.va);
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        if (s.queue_total > queue.size())
            ImGui::TextDisabled("... and %zu more. The first %zu are listed; pin a function in the Function browser to run it sooner.",
                                s.queue_total - queue.size(), queue.size());
        // Finished functions can go back into the queue while the run is live.
        if (s.finished > 0 && ImGui::CollapsingHeader("Finished functions###finished")) {
            if (finished_version_ != s.last_seq || finished_run_ != s.run_id) {
                // Latest session per function, rebuilt only when the run changed.
                std::map<u64, const events::SessionState*> latest;
                for (const auto& [id, session] : s.sessions)
                    if (session->finished) {
                        auto& slot = latest[session->va];
                        if (!slot || session->started > slot->started) slot = session.get();
                    }
                finished_.clear();
                for (const auto& [va, session] : latest)
                    finished_.push_back(FinishedRow{va, session->id, session->display.empty() ? session->function : session->display, session->outcome});
                finished_version_ = s.last_seq;
                finished_run_ = s.run_id;
            }
            if (ImGui::BeginTable("##finished", 3, flags | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 12))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Outcome");
                ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(finished_.size()));
                while (clipper.Step())
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                        const FinishedRow& row = finished_[static_cast<usize>(i)];
                        ImGui::PushID(static_cast<int>(row.va));
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (ImGui::TextLink(row.name.c_str())) ctx.open("agent_session", NavTarget{.va = row.va, .session = row.session});
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(row.outcome.c_str());
                        ImGui::TableNextColumn();
                        ImGui::BeginDisabled(!live);
                        if (ImGui::SmallButton("Requeue")) commands.requeue(row.va);
                        ImGui::EndDisabled();
                        ImGui::PopID();
                    }
                ImGui::EndTable();
            }
        }
    }

    // A Gantt chart of each worker's phases over the last minutes.
    void draw_timeline(ViewContext& ctx, const events::RunStateData& s) {
        const ThemeColors& c = ctx.colors();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::SliderInt("Minutes shown", &window_minutes_, 1, 60);
        ImGui::SameLine();
        for (PhaseGroup g : {PhaseGroup::model, PhaseGroup::tools, PhaseGroup::waiting, PhaseGroup::idle}) {
            const char* label = g == PhaseGroup::model ? "model" : g == PhaseGroup::tools ? "tools and compiles" : g == PhaseGroup::waiting ? "waiting" : "idle";
            status_label(label, group_color(g, c));
            ImGui::SameLine();
        }
        ImGui::NewLine();
        if (s.workers.empty()) {
            ImGui::TextDisabled("No worker has started yet.");
            return;
        }
        const Clock::time_point end = s.ended != Clock::time_point{} ? s.ended : Clock::now();
        Clock::time_point begin = end - std::chrono::minutes(window_minutes_);
        if (s.started != Clock::time_point{} && s.started > begin) begin = s.started;
        const double span = std::max(1.0, std::chrono::duration<double>(end - begin).count());

        const float label_w = ImGui::CalcTextSize("Worker 00").x + ImGui::GetStyle().ItemSpacing.x;
        const float row_h = ImGui::GetFrameHeight();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = std::max(50.0f, ImGui::GetContentRegionAvail().x - label_w);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        int row = 0;
        for (const auto& [id, w] : s.workers) {
            const float y = origin.y + static_cast<float>(row) * row_h;
            dl->AddText(ImVec2(origin.x, y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), std::format("Worker {}", id).c_str());
            for (const auto& span_rec : w.spans) {
                const Clock::time_point a = std::max(span_rec.start, begin);
                const Clock::time_point b = std::min(span_rec.end == Clock::time_point{} ? end : span_rec.end, end);
                if (b <= a) continue;
                const float x0 = origin.x + label_w + static_cast<float>(std::chrono::duration<double>(a - begin).count() / span) * width;
                const float x1 = origin.x + label_w + static_cast<float>(std::chrono::duration<double>(b - begin).count() / span) * width;
                const ImVec2 p0(x0, y + 2), p1(std::max(x1, x0 + 1.0f), y + row_h - 2);
                dl->AddRectFilled(p0, p1, ImGui::GetColorU32(group_color(group_of(span_rec.phase), c)));
                if (ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered())
                    ImGui::SetTooltip("Worker %d: %s\n%s\n%s", id, span_rec.phase.c_str(), span_rec.function.empty() ? "-" : span_rec.function.c_str(),
                                      duration_text(b - a).c_str());
            }
            ++row;
        }
        // Minute ticks under the rows.
        const float axis_y = origin.y + static_cast<float>(row) * row_h;
        const int minutes = static_cast<int>(span / 60.0);
        for (int m = 0; m <= minutes; ++m) {
            const float x = origin.x + label_w + static_cast<float>((span - m * 60.0) / span) * width;
            dl->AddLine(ImVec2(x, axis_y), ImVec2(x, axis_y + 4), ImGui::GetColorU32(ImGuiCol_TextDisabled));
            if (m > 0 && (minutes <= 10 || m % 5 == 0))
                dl->AddText(ImVec2(x - 10, axis_y + 4), ImGui::GetColorU32(ImGuiCol_TextDisabled), std::format("-{}m", m).c_str());
        }
        ImGui::Dummy(ImVec2(label_w + width, static_cast<float>(row) * row_h + ImGui::GetTextLineHeightWithSpacing() + 4));
    }

    // Per-minute throughput from the reducer's minute buckets (x: minutes before the latest one).
    void draw_throughput(const events::RunStateData& s) {
        if (s.minutes.empty()) {
            ImGui::TextDisabled("No turn has finished yet.");
            return;
        }
        const i64 last = s.minutes.rbegin()->first;
        std::vector<double> xs, turns, compiles, retries, tokens_per_s, ttft;
        for (const auto& [minute, m] : s.minutes) {
            xs.push_back(static_cast<double>(minute - last));
            turns.push_back(m.turns);
            compiles.push_back(m.compiles);
            retries.push_back(m.retries);
            tokens_per_s.push_back(static_cast<double>(m.output_tokens) / 60.0);
            ttft.push_back(m.ttft_count ? static_cast<double>(m.ttft_sum_ms) / m.ttft_count : 0.0);
        }
        const int n = static_cast<int>(xs.size());
        const float h = std::max(120.0f, (ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y * 2) / 3);
        if (ImPlot::BeginPlot("Per minute##counts", ImVec2(-1, h))) {
            ImPlot::SetupAxes("minutes", "count", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::PlotLine("turns", xs.data(), turns.data(), n);
            ImPlot::PlotLine("compiles", xs.data(), compiles.data(), n);
            ImPlot::PlotLine("retries", xs.data(), retries.data(), n);
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("Output tokens per second##tps", ImVec2(-1, h))) {
            ImPlot::SetupAxes("minutes", "tokens/s", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::PlotLine("output", xs.data(), tokens_per_s.data(), n);
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("Time to first token##ttft", ImVec2(-1, h))) {
            ImPlot::SetupAxes("minutes", "ms", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::PlotLine("mean", xs.data(), ttft.data(), n);
            ImPlot::EndPlot();
        }
    }

    void draw_rate_limits(ViewContext& ctx, const events::RunStateData& s) {
        if (!s.rate_limit.known) {
            ImGui::TextDisabled("No rate-limit headers seen yet (scripted runs have none).");
        } else {
            const auto& l = s.rate_limit.last;
            ImGui::Text("Requests remaining: %lld of %lld", l.requests_remaining, l.requests_limit);
            if (l.input_tokens_limit > 0) ImGui::Text("Input tokens remaining: %lld of %lld", l.input_tokens_remaining, l.input_tokens_limit);
            if (l.output_tokens_limit > 0)
                ImGui::Text("Output tokens remaining: %lld of %lld", l.output_tokens_remaining, l.output_tokens_limit);
            if (!l.reset.empty()) ImGui::Text("Next reset: %s", l.reset.c_str());
            if (s.rate_history.size() > 1 && ImPlot::BeginPlot("Requests remaining##rate", ImVec2(-1, ImGui::GetFontSize() * 10))) {
                const auto t0 = s.rate_history.front().time;
                std::vector<double> xs, ys;
                for (const auto& r : s.rate_history) {
                    xs.push_back(std::chrono::duration<double>(r.time - t0).count());
                    ys.push_back(static_cast<double>(r.snapshot.requests_remaining));
                }
                ImPlot::SetupAxes("seconds", "requests", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                ImPlot::PlotLine("remaining", xs.data(), ys.data(), static_cast<int>(xs.size()));
                ImPlot::EndPlot();
            }
        }
        ImGui::SeparatorText("Retries");
        std::vector<const events::ErrorRecord*> retries;
        for (const auto& r : s.error_log)
            if (r.kind == "api") retries.push_back(&r);
        if (retries.empty()) {
            ImGui::TextDisabled("No request was retried.");
            return;
        }
        if (ImGui::BeginTable("##retries", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Delay");
            ImGui::TableSetupColumn("Error", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (auto it = retries.rbegin(); it != retries.rend(); ++it) {
                const auto& r = **it;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(local_clock(r.time).c_str());
                ImGui::TableNextColumn();
                if (r.status) ImGui::Text("%d", r.status);
                else ImGui::TextUnformatted("network");
                ImGui::TableNextColumn();
                ImGui::Text("%.1f s", static_cast<double>(r.delay_ms) / 1000.0);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", r.message.c_str());
            }
            ImGui::EndTable();
        }
        (void)ctx;
    }

    bool follow_ = true;
    u64 followed_ = 0;
    double budget_ = 0;
    bool editing_budget_ = false;
    agent::LoopLimits limits_;
    int window_minutes_ = 10;
    struct FinishedRow {
        u64 va = 0;
        std::string session, name, outcome;
    };
    std::vector<FinishedRow> finished_;
    RememberedTabs tabs_;
    const vm::QueueEta* eta_source_ = nullptr;
    std::map<u64, std::pair<double, double>> eta_by_va_;  // start, finish (seconds from the estimate)
    u64 finished_version_ = ~u64{0};
    std::string finished_run_;
};

} // namespace

std::unique_ptr<View> make_run_monitor_view() { return std::make_unique<RunMonitorView>(); }

} // namespace decomp::gui
