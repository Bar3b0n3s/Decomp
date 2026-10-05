// Function browser (docs/ui.md#function-browser-and-inspector): every function in a virtualized,
// sortable, filterable table with the live overlay of the run, multi-selection and the actions on it:
// run or queue the selection, mark it skipped or library, reset it, open a function elsewhere, edit
// notes, export the list. Rows, the code analysis and the filtered order are background jobs.

#include "gui/views/function_browser_view.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/export.hpp"
#include "gui/views/function_table_widget.hpp"
#include "gui/views/view_support.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"
#include "viewmodel/browser.hpp"
#include "viewmodel/difficulty.hpp"
#include "viewmodel/exports.hpp"
#include "viewmodel/function_table.hpp"
#include "viewmodel/progress.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <atomic>
#include <format>

namespace decomp::gui {

namespace {

using project::FunctionStatus;

using LastAttempts = std::map<u64, std::pair<int, std::optional<vm::TimePoint>>>;

struct AnalysisKey {
    std::weak_ptr<const Program> program;
    bool operator==(const AnalysisKey& o) const { return !program.owner_before(o.program) && !o.program.owner_before(program); }
};

struct AnalysisProgress {
    std::atomic<usize> done{0}, total{0};
};

struct RowsKey {
    ProjectInputs project;
    u64 overlay = 0;
    u64 analysis = 0;
    bool operator==(const RowsKey&) const = default;
};

struct RowsData {
    std::shared_ptr<const std::vector<vm::FunctionRow>> rows;
    std::shared_ptr<const LastAttempts> attempts;
};

struct OrderKey {
    u64 rows = 0, filter = 0, sort = 0;
    bool operator==(const OrderKey&) const = default;
};

struct OrderData {
    std::shared_ptr<const std::vector<vm::FunctionRow>> rows;
    std::vector<u32> order;
    std::string error;
};

std::string optional_text(const std::optional<u64>& v) { return v ? std::to_string(*v) : std::string(); }

class FunctionBrowserView final : public View {
public:
    std::string_view id() const override { return "function_browser"; }
    std::string_view title() const override { return "Function browser"; }

    void navigate(ViewContext& ctx, const NavTarget& target) override {
        load_state(ctx);
        if (auto filter = vm::browser_filter_from_anchor(target.anchor)) {
            set_filter(*filter);
            save_state(ctx);
        }
        if (target.va) {
            table_.select_only(*target.va);
            table_.scroll_to(*target.va);
            current_ = target.va;
        }
    }

    void draw(ViewContext& ctx) override {
        load_state(ctx);
        const ProjectAccess access = project_access(ctx);
        if (!require_project(ctx, access,
                             "The Function browser lists every function with its status, best match, attempts, spend and code "
                             "analysis, and acts on a selection: run it, mark it skipped or library, reset it, edit notes.")) {
            return;
        }
        poll_writes(ctx);
        update_jobs(ctx, access);
        draw_filters(ctx);
        draw_actions(ctx, access);
        draw_table(ctx, access);
        draw_popups(ctx, access);
    }

private:
    // ---- per-project state -------------------------------------------------------------------------------

    void load_state(ViewContext& ctx) {
        const std::string key = fs::to_utf8(ctx.project.root);
        if (state_loaded_ && key == state_project_) return;
        state_loaded_ = true;
        state_project_ = key;
        const Json& s = ctx.view_state(id());
        set_filter(vm::filter_from_json(s.value("filter", Json::object())));
        table_.set_sort_keys(vm::sort_keys_from_json(s.value("sort", Json::array())));
        table_.set_layout(vm::column_layout_from_json(s.value("columns", Json())));
        table_.clear_selection();
        current_.reset();
        ++sort_version_;
    }

    void save_state(ViewContext& ctx) {
        Json& s = ctx.view_state(id());
        s["filter"] = vm::filter_to_json(filter_);
        s["sort"] = vm::sort_keys_to_json(table_.sort_keys());
        s["columns"] = vm::column_layout_to_json(table_.layout());
        ctx.mark_settings_dirty();
    }

    void set_filter(vm::FunctionFilter filter) {
        filter_ = std::move(filter);
        name_ = filter_.name;
        min_size_ = optional_text(filter_.min_size);
        max_size_ = optional_text(filter_.max_size);
        ++filter_version_;
    }

    // ---- jobs -----------------------------------------------------------------------------------------

    void update_jobs(ViewContext& ctx, const ProjectAccess& access) {
        if (analysis_.poll()) ++analysis_generation_;
        if (rows_.poll()) ++rows_generation_;
        if (order_.poll()) ++order_generation_;

        const auto program = access.program;
        const project::Project& project = *access.project;  // copied into the jobs
        if (const std::string root = fs::to_utf8(project.root()); root != project_root_) {
            // Another project: nothing shown so far applies to it.
            project_root_ = root;
            analysis_.reset();
            rows_.reset();
            order_.reset();
        }
        // The code analysis (callers, callees, blocks, loops, difficulty): once per program generation; the
        // previous generation's results stay applied (by address) until the new ones arrive.
        if (const AnalysisKey key{program}; !analysis_.requested(key)) {
            auto progress = std::make_shared<AnalysisProgress>();
            analysis_progress_ = progress;
            analysis_.update(ctx.jobs, key, [program, progress] {
                return [program, progress](const CancelToken& token) {
                    std::vector<u64> vas;
                    for (const Symbol* s : program->symbols().functions()) vas.push_back(s->va);
                    progress->total = vas.size();
                    auto analysis = vm::analyze_functions(
                        *program, vas, [&token] { return token.cancelled(); },
                        [progress](usize done, usize total) {
                            progress->done = done;
                            progress->total = total;
                        });
                    return std::make_shared<const vm::FunctionAnalysis>(std::move(analysis));
                };
            });
        }

        const events::RunStateData* live = live_run(ctx);
        const std::shared_ptr<const events::RunStateData> live_snapshot = live ? ctx.snapshot : nullptr;
        const u64 overlay = live ? vm::live_overlay_digest(*live) : 0;
        const auto analysis = analysis_.value() ? *analysis_.value() : nullptr;
        const auto previous = rows_.value() ? rows_.value()->attempts : nullptr;
        rows_.update(
            ctx.jobs, RowsKey{project_inputs(access), overlay, analysis_generation_},
            [&] {
                return [=](const CancelToken& token) {
                    auto rows = vm::build_function_rows(program->symbols(), *project.function_infos(), live_snapshot.get());
                    if (analysis) vm::apply_analysis(rows, *analysis);
                    // The time of the last attempt: read again only for functions with new attempts.
                    auto attempts = std::make_shared<LastAttempts>();
                    for (auto& r : rows) {
                        if (r.attempts <= 0 || token.cancelled()) continue;
                        if (previous)
                            if (auto it = previous->find(r.va); it != previous->end() && it->second.first == r.attempts) {
                                r.last_attempt = it->second.second;
                                attempts->emplace(r.va, it->second);
                                continue;
                            }
                        if (const Symbol* s = program->symbols().at(r.va)) r.last_attempt = vm::last_attempt_time(project, *s);
                        attempts->emplace(r.va, std::pair{r.attempts, r.last_attempt});
                    }
                    return RowsData{std::make_shared<const std::vector<vm::FunctionRow>>(std::move(rows)), std::move(attempts)};
                };
            },
            refresh_interval(ctx, std::chrono::milliseconds(500)));

        if (rows_.value()) {
            order_.update(ctx.jobs, OrderKey{rows_generation_, filter_version_, sort_version_}, [&] {
                return [rows = rows_.value()->rows, filter = filter_, sort = table_.sort_keys()](const CancelToken& token) {
                    OrderData d;
                    d.rows = rows;
                    auto order = vm::filter_and_sort(*rows, filter, sort, [&token] { return token.cancelled(); });
                    if (order) d.order = std::move(*order);
                    else d.error = order.error().message;
                    return d;
                };
            });
        }
    }

    // ---- filters --------------------------------------------------------------------------------------

    void draw_filters(ViewContext& ctx) {
        bool changed = false;
        const float font = ImGui::GetFontSize();
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, font * 16));
        if (ImGui::InputTextWithHint("##name", "Name (text or regex)", &name_)) {
            filter_.name = name_;
            changed = true;
        }
        const std::string status_label =
            filter_.statuses.empty() ? std::string("Status: all") : std::format("Status: {} selected", filter_.statuses.size());
        const std::string status_button = std::format("{}###status", status_label);
        same_line_or_wrap(button_width(status_button));
        if (ImGui::Button(status_button.c_str())) ImGui::OpenPopup("##statuses");
        if (ImGui::BeginPopup("##statuses")) {
            for (FunctionStatus s : vm::kStatusOrder) {
                bool on = std::ranges::find(filter_.statuses, s) != filter_.statuses.end();
                ImGui::PushID(static_cast<int>(s));
                status_dot(status_color(ctx, s));
                ImGui::SameLine();
                if (ImGui::Checkbox(std::string(status_text(s)).c_str(), &on)) {
                    if (on) filter_.statuses.push_back(s);
                    else std::erase(filter_.statuses, s);
                    changed = true;
                }
                ImGui::PopID();
            }
            if (ImGui::Button("All statuses")) {
                filter_.statuses.clear();
                changed = true;
            }
            ImGui::EndPopup();
        }
        const float size_field = font * 4.5f;
        same_line_or_wrap(size_field * 2 + ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::SetNextItemWidth(size_field);
        if (ImGui::InputTextWithHint("##min_size", "min B", &min_size_, ImGuiInputTextFlags_CharsDecimal)) {
            filter_.min_size = parse_u64(trim(min_size_));
            changed = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Smallest size in bytes");
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::SetNextItemWidth(size_field);
        if (ImGui::InputTextWithHint("##max_size", "max B", &max_size_, ImGuiInputTextFlags_CharsDecimal)) {
            filter_.max_size = parse_u64(trim(max_size_));
            changed = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Largest size in bytes");
        same_line_or_wrap(checkbox_width("Unknown callees"));
        changed |= ImGui::Checkbox("Unknown callees", &filter_.unknown_callees);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Functions calling something nobody has named (needs the code analysis; functions not analyzed yet are left out).");
        same_line_or_wrap(checkbox_width("Refused"));
        changed |= ImGui::Checkbox("Refused", &filter_.refused);
        if (filter_.min_best || filter_.best_below) {
            const std::string best = std::format("best {}-{}% (clear)", filter_.min_best ? static_cast<int>(*filter_.min_best) : 0,
                                                 filter_.best_below ? static_cast<int>(*filter_.best_below) : 100);
            same_line_or_wrap(button_width(best));
            if (ImGui::SmallButton(best.c_str())) {
                filter_.min_best.reset();
                filter_.best_below.reset();
                changed = true;
            }
        }
        same_line_or_wrap(button_width("Clear"));
        ImGui::BeginDisabled(vm::filter_is_empty(filter_));
        if (ImGui::Button("Clear")) {
            set_filter({});
            changed = true;
        }
        ImGui::EndDisabled();
        if (changed) {
            ++filter_version_;
            save_state(ctx);
        }
    }

    // ---- selection actions -----------------------------------------------------------------------------

    // The selection in table order; recomputed only when the selection or the order changes (a pass over
    // 100,000 rows is too much for every frame).
    const std::vector<u64>& selected() {
        const std::pair key{table_.selection_version(), order_generation_};
        if (key != selected_key_) {
            selected_key_ = key;
            selected_cache_.clear();
            if (order_.value() && order_.value()->rows) selected_cache_ = table_.selected_in_order(*order_.value()->rows, order_.value()->order);
        }
        return selected_cache_;
    }

    void queue(ViewContext& ctx, std::vector<u64> vas) {
        if (vas.empty()) return;
        RunCommands& commands = *ctx.services.commands;
        if (commands.live()) {
            const usize added = commands.enqueue(vas);
            ctx.notify(Severity::info, std::format("Added {} of {} function(s) to the run's queue.", added, vas.size()));
        } else if (commands.available()) {
            ctx.notify(Severity::info, std::format("Starting a run over {} function(s).", vas.size()));
            commands.start_functions(std::move(vas));
        }
    }

    void set_status(ViewContext& ctx, const ProjectAccess& access, std::vector<u64> vas, FunctionStatus status) {
        if (vas.empty() || !access.project) return;
        if (status == FunctionStatus::skipped && ctx.services.commands->live())
            for (u64 va : vas) ctx.services.commands->skip(va);  // ends its session or leaves the queue
        project::Project project = *access.project;
        const usize count = vas.size();
        write_ = ctx.jobs.submit([project, vas = std::move(vas), status]() mutable -> std::string {
            auto r = project.modify_functions(vas, [status](u64, project::FunctionInfo& info) { info.status = status; });
            return r ? std::string() : r.error().message;
        });
        write_message_ = std::format("Marked {} function(s) {}.", count, status_text(status));
    }

    void poll_writes(ViewContext& ctx) {
        if (!write_.valid()) return;
        std::optional<std::string> error;
        try {
            error = write_.take();
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!error) return;
        write_.reset();
        if (error->empty()) ctx.notify(Severity::info, write_message_);
        else ctx.notify(Severity::error, std::format("Cannot change the status: {}", *error));
    }

    // Asks first when a status change touches many functions or matched ones.
    void request_status(ViewContext& ctx, const ProjectAccess& access, std::vector<u64> vas, FunctionStatus status) {
        usize matched = 0;
        const std::vector<u64> lookup = sorted(vas);
        if (order_.value() && order_.value()->rows)
            for (const auto& r : *order_.value()->rows)
                if (r.stored_status == FunctionStatus::matched && std::ranges::binary_search(lookup, r.va)) ++matched;
        if (vas.size() <= 1 && matched == 0) {
            set_status(ctx, access, std::move(vas), status);
            return;
        }
        confirm_vas_ = std::move(vas);
        confirm_status_ = status;
        confirm_matched_ = matched;
        open_confirm_ = true;
    }

    static std::vector<u64> sorted(std::vector<u64> v) {
        std::ranges::sort(v);
        return v;
    }

    void draw_actions(ViewContext& ctx, const ProjectAccess& access) {
        RunCommands& commands = *ctx.services.commands;
        const std::vector<u64>& vas = selected();
        const bool any = !vas.empty();
        const bool live = commands.live();
        const std::string run_label = live ? (any ? std::format("Add {} to the run###queue", vas.size()) : std::string("Add to the run###queue"))
                                           : (any ? std::format("Run {}###queue", vas.size()) : std::string("Run###queue"));
        ImGui::BeginDisabled(!any || (!live && !commands.available()));
        if (ImGui::Button(run_label.c_str())) queue(ctx, vas);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(live ? "Queue the selected functions in the live run." : "Start a run over the selected functions, in this order.");
        same_line_or_wrap(button_width("Mark..."));
        ImGui::BeginDisabled(!any);
        if (ImGui::Button("Mark...")) ImGui::OpenPopup("##mark");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Mark the selection skipped or library, or reset it to unstarted.");
        if (ImGui::BeginPopup("##mark")) {
            if (ImGui::MenuItem("Skipped")) request_status(ctx, access, vas, FunctionStatus::skipped);
            if (ImGui::MenuItem("Library")) request_status(ctx, access, vas, FunctionStatus::library);
            if (ImGui::MenuItem("Unstarted (reset; history kept)")) request_status(ctx, access, vas, FunctionStatus::unstarted);
            ImGui::EndPopup();
        }
        const std::optional<u64> focus = current_function(vas);
        ImGui::BeginDisabled(!focus);
        same_line_or_wrap(button_width("Diff"));
        if (ImGui::Button("Diff")) ctx.open("diff_viewer", {.va = *focus});
        same_line_or_wrap(button_width("Session"));
        if (ImGui::Button("Session")) ctx.open("agent_session", {.va = *focus, .session = session_of(*focus)});
        same_line_or_wrap(button_width("Notes..."));
        if (ImGui::Button("Notes...")) open_notes(access, *focus);
        ImGui::EndDisabled();
        if (any && vas.size() > 1) {
            const std::string bulk = std::format("Note on {}...", vas.size());
            same_line_or_wrap(button_width(bulk));
            if (ImGui::Button(bulk.c_str())) {
                note_targets_ = vas;
                bulk_note_.clear();
                open_bulk_note_ = true;
            }
        }
        if (order_.value() && order_.value()->rows) {
            const auto rows = order_.value()->rows;
            const std::vector<u32>* order = &order_.value()->order;
            const ExportFormat formats[] = {
                {"CSV", "csv", [rows, order] { return vm::function_list_csv(*rows, *order); }},
                {"JSON", "json", [rows, order] { return dump_pretty(vm::function_list_json(*rows, *order)) + "\n"; }},
            };
            same_line_or_wrap(button_width("Export"));
            export_button(ctx, "##functions_export", "functions", formats);  // the list as filtered and sorted
        }

        // What the table shows.
        const usize total = rows_.value() ? rows_.value()->rows->size() : 0;
        if (order_.value()) {
            if (!order_.value()->error.empty()) colored_text(ctx.colors().error, order_.value()->error);
            else ImGui::Text("%zu of %zu functions", order_.value()->order.size(), total);
            if (any) {
                ImGui::SameLine();
                ImGui::TextDisabled("%zu selected", vas.size());
            }
            if (!vm::filter_is_empty(filter_)) ImGui::TextDisabled("Filter: %s", vm::describe_filter(filter_).c_str());
        } else {
            ImGui::TextDisabled("Listing the functions...");
        }
        if (analysis_.busy() && analysis_progress_) {
            ImGui::SameLine();
            const usize t = analysis_progress_->total.load(), d = analysis_progress_->done.load();
            ImGui::TextDisabled("analyzing the code: %zu%%", t ? d * 100 / t : 0);
        }
        busy_marker(ctx, rows_.busy() || order_.busy());
    }

    std::optional<u64> current_function(const std::vector<u64>& selection) const {
        if (current_ && table_.is_selected(*current_)) return current_;
        if (!selection.empty()) return selection.front();
        return current_;
    }

    std::string session_of(u64 va) const {
        if (order_.value() && order_.value()->rows)
            for (const auto& r : *order_.value()->rows)
                if (r.va == va) return r.session;
        return {};
    }

    // ---- table ----------------------------------------------------------------------------------------

    void draw_table(ViewContext& ctx, const ProjectAccess& access) {
        if (!order_.value() || !order_.value()->rows) return;
        const OrderData& d = *order_.value();
        table_.set_analysis_pending(analysis_.busy() || !analysis_.value());
        const auto menu = [&](const vm::FunctionRow& row) { row_menu(ctx, access, row); };
        const FunctionTable::Events ev = table_.draw(ctx, *d.rows, d.order, ImVec2(0, 0), menu);
        if (ev.clicked) {
            current_ = ev.clicked;
            ctx.selection.function_va = ev.clicked;  // the Inspector follows the selection
        }
        if (ev.activated) ctx.open("inspector", {.va = *ev.activated});
        if (ev.sort_changed) {
            ++sort_version_;
            save_state(ctx);
        }
        if (ev.layout_changed) save_state(ctx);
    }

    void row_menu(ViewContext& ctx, const ProjectAccess& access, const vm::FunctionRow& row) {
        if (!table_.is_selected(row.va)) {
            table_.select_only(row.va);
            current_ = row.va;
        }
        ImGui::TextDisabled("%s", row.display.c_str());
        ImGui::Separator();
        function_open_items(ctx, row.va, row.session);
        ImGui::Separator();
        const std::vector<u64> vas = selected();
        RunCommands& commands = *ctx.services.commands;
        if (ImGui::MenuItem(commands.live() ? std::format("Add {} to the run", vas.size()).c_str() : std::format("Run {}", vas.size()).c_str(), nullptr,
                            false, commands.live() || commands.available()))
            queue(ctx, vas);
        if (ImGui::MenuItem("Mark skipped")) request_status(ctx, access, vas, FunctionStatus::skipped);
        if (ImGui::MenuItem("Mark library")) request_status(ctx, access, vas, FunctionStatus::library);
        if (ImGui::MenuItem("Reset to unstarted")) request_status(ctx, access, vas, FunctionStatus::unstarted);
        if (ImGui::MenuItem("Edit notes...")) open_notes(access, row.va);
        ImGui::Separator();
        if (ImGui::MenuItem("Copy name")) ImGui::SetClipboardText(row.name.c_str());
    }

    // ---- notes and confirmations ------------------------------------------------------------------------

    void open_notes(const ProjectAccess& access, u64 va) {
        const Symbol* s = access.program ? access.program->symbols().at(va) : nullptr;
        if (!s || !access.project) return;
        notes_va_ = va;
        notes_name_ = s->display.empty() ? s->name : s->display;
        notes_text_ = access.project->notes(*s);
        open_notes_ = true;
    }

    void draw_popups(ViewContext& ctx, const ProjectAccess& access) {
        if (open_notes_) {
            ImGui::OpenPopup("Notes###function_notes");
            open_notes_ = false;
        }
        if (open_confirm_) {
            ImGui::OpenPopup("Change status###confirm_status");
            open_confirm_ = false;
        }
        if (open_bulk_note_) {
            ImGui::OpenPopup("Add a note###bulk_note");
            open_bulk_note_ = false;
        }
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Notes###function_notes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Notes on %s", notes_name_.c_str());
            ImGui::TextDisabled("Later sessions read them in their brief.");
            multiline_text("##notes", &notes_text_, ImVec2(ImGui::GetFontSize() * 36, ImGui::GetFontSize() * 14));
            if (ImGui::Button("Save")) {
                const Symbol* s = access.program->symbols().at(notes_va_);
                if (s) {
                    if (auto r = access.project->save_notes(*s, notes_text_); !r) ctx.notify(Severity::error, std::format("Cannot save the notes: {}", r.error().message));
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Change status###confirm_status", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Mark %zu function(s) %s?", confirm_vas_.size(), std::string(status_text(confirm_status_)).c_str());
            if (confirm_matched_ > 0)
                colored_text(ctx.colors().warn, std::format("{} of them are matched; their verified sources stay in src/functions/.", confirm_matched_));
            ImGui::TextDisabled("Attempts, best sources and notes are kept.");
            if (ImGui::Button("Change")) {
                set_status(ctx, access, std::move(confirm_vas_), confirm_status_);
                confirm_vas_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Add a note###bulk_note", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("A note for each of the %zu selected functions:", note_targets_.size());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30);
            ImGui::InputText("##bulk_note", &bulk_note_);
            ImGui::BeginDisabled(trim(bulk_note_).empty());
            if (ImGui::Button("Add")) {
                const auto program = access.program;
                project::Project project = *access.project;
                const std::string note(trim(bulk_note_));
                ctx.jobs.submit([program, project, vas = note_targets_, note] {
                    for (u64 va : vas)
                        if (const Symbol* s = program->symbols().at(va))
                            if (auto r = project.append_note(*s, note); !r) log::warn("cannot add a note: {}", r.error().message);
                });
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    FunctionTable table_;
    vm::FunctionFilter filter_;
    std::string name_, min_size_, max_size_;
    u64 filter_version_ = 0, sort_version_ = 0;
    bool state_loaded_ = false;
    std::string state_project_;
    std::optional<u64> current_;
    std::string project_root_;

    KeyedJob<AnalysisKey, std::shared_ptr<const vm::FunctionAnalysis>> analysis_;
    std::shared_ptr<AnalysisProgress> analysis_progress_;
    u64 analysis_generation_ = 0;
    KeyedJob<RowsKey, RowsData> rows_;
    u64 rows_generation_ = 0;
    KeyedJob<OrderKey, OrderData> order_;
    u64 order_generation_ = 0;
    std::pair<u64, u64> selected_key_{~u64{0}, ~u64{0}};
    std::vector<u64> selected_cache_;

    JobHandle<std::string> write_;
    std::string write_message_;
    bool open_notes_ = false, open_confirm_ = false, open_bulk_note_ = false;
    u64 notes_va_ = 0;
    std::string notes_name_, notes_text_, bulk_note_;
    std::vector<u64> note_targets_, confirm_vas_;
    FunctionStatus confirm_status_ = FunctionStatus::skipped;
    usize confirm_matched_ = 0;
};

} // namespace

std::unique_ptr<View> make_function_browser_view() { return std::make_unique<FunctionBrowserView>(); }

} // namespace decomp::gui
