// Inspector (docs/ui.md#function-browser-and-inspector): the selected function's annotated disassembly
// with block and loop hints, its cross-references, attempt history with a score chart, notes and status
// history. It follows the shared selection (ctx.selection.function_va).

#include "gui/views/inspector_view.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/views/view_support.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"
#include "viewmodel/common.hpp"
#include "viewmodel/inspector.hpp"

#include <implot.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <format>
#include <map>

namespace decomp::gui {

namespace {

using project::FunctionStatus;

struct FunctionKey {
    u64 va = 0;
    std::weak_ptr<const Program> program;
    bool operator==(const FunctionKey& o) const { return va == o.va && !program.owner_before(o.program) && !o.program.owner_before(program); }
};

struct AttemptsKey {
    u64 va = 0;
    std::string root;
    u64 version = 0;
    int live_compiles = -1;  // the running session's compiles add attempts before symbols.txt changes
    bool operator==(const AttemptsKey&) const = default;
};

struct HistoryKey {
    std::string root;
    u64 version = 0;
    std::string run, status;
    int finished = 0;
    bool operator==(const HistoryKey&) const = default;
};

// A row of the listing: an instruction, or the label line before one.
struct DisplayRow {
    u32 line = 0;
    bool label = false;
};

ImVec4 flow_color(const ViewContext& ctx, x86::Flow flow) {
    const ThemeColors& c = ctx.colors();
    switch (flow) {
    case x86::Flow::call:
    case x86::Flow::indirect_call: return c.info;
    case x86::Flow::jump:
    case x86::Flow::cond_jump:
    case x86::Flow::indirect_jump: return c.warn;
    case x86::Flow::ret:
    case x86::Flow::trap:
    case x86::Flow::halt: return c.error;
    case x86::Flow::none: break;
    }
    return c.text;
}

std::string_view xref_kind_text(XrefKind kind) { return to_string(kind); }

class InspectorView final : public View {
public:
    std::string_view id() const override { return "inspector"; }
    std::string_view title() const override { return "Inspector"; }

    void navigate(ViewContext& /*ctx*/, const NavTarget& target) override {
        if (target.anchor == "notes") tab_request_ = 3;
        else if (target.anchor == "xrefs") tab_request_ = 1;
        else if (target.anchor == "attempts") tab_request_ = 2;
        else if (target.anchor == "history") tab_request_ = 4;
    }

    void draw(ViewContext& ctx) override {
        const ProjectAccess access = project_access(ctx);
        if (!require_project(ctx, access,
                             "The Inspector shows the selected function: annotated disassembly with block and loop hints, "
                             "cross-references, attempt history, notes and status history.")) {
            if (ctx.selection.function_va) ImGui::TextDisabled("Selected: %s", hex(*ctx.selection.function_va, 8).c_str());
            return;
        }
        if (!ctx.selection.function_va) {
            ImGui::TextDisabled("No function selected. Select one in the Function browser, the Dashboard's code map, or type its address in "
                                "the palette (Ctrl+P, 0x...).");
            return;
        }
        const u64 va = *ctx.selection.function_va;
        if (va != shown_va_) {
            shown_va_ = va;
            scroll_row_.reset();
            notes_loaded_ = false;
        }
        update_jobs(ctx, access, va);
        draw_header(ctx, access, va);
        if (ImGui::BeginTabBar("##inspector_tabs")) {
            const char* tabs[] = {"Disassembly", "Cross-references", "Attempts", "Notes", "Status history"};
            for (int t = 0; t < 5; ++t) {
                const ImGuiTabItemFlags flags = tab_request_ == t ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (!ImGui::BeginTabItem(tabs[t], nullptr, flags)) continue;
                switch (t) {
                case 0: draw_listing(ctx, access); break;
                case 1: draw_xrefs(ctx); break;
                case 2: draw_attempts(ctx, va); break;
                case 3: draw_notes(ctx, access, va); break;
                case 4: draw_history(ctx, va); break;
                default: break;
                }
                ImGui::EndTabItem();
            }
            tab_request_ = -1;
            ImGui::EndTabBar();
        }
    }

private:
    // ---- jobs -----------------------------------------------------------------------------------------

    void update_jobs(ViewContext& ctx, const ProjectAccess& access, u64 va) {
        if (listing_.poll()) rebuild_rows();
        xrefs_.poll();
        attempts_.poll();
        history_.poll();
        const auto program = access.program;
        const project::Project& project = *access.project;  // copied into the jobs
        if (const std::string root = fs::to_utf8(project.root()); root != project_root_) {
            project_root_ = root;
            listing_.reset();
            rows_.clear();
            label_rows_.clear();
            xrefs_.reset();
            attempts_.reset();
            history_.reset();
            notes_loaded_ = false;
        }
        const FunctionKey key{va, program};
        key_ = key;
        listing_.update(ctx.jobs, key, [&] { return [=] { return vm::build_listing(*program, va); }; });
        xrefs_.update(ctx.jobs, key, [&] { return [=] { return vm::function_xrefs(*program, va); }; });

        const events::SessionState* session = live_session(ctx, va);
        attempts_.update(ctx.jobs, AttemptsKey{va, fs::to_utf8(project.root()), project.version(), session ? session->compiles : -1},
                         [&] {
                             return [=] {
                                 const Symbol* s = program->symbols().at(va);
                                 return s ? vm::parse_attempts(project.attempts(*s)) : std::vector<vm::AttemptRecord>{};
                             };
                         });
        const auto* shown = ctx.snapshot.get();
        history_.update(
            ctx.jobs,
            HistoryKey{fs::to_utf8(project.root()), project.version(), shown ? shown->run_id : std::string(), shown ? shown->status : std::string(),
                       shown ? shown->finished : 0},
            [&] { return [=](const CancelToken& token) {
                return std::make_shared<const vm::StatusHistory>(vm::load_status_history(project.runs_dir(), [&token] { return token.cancelled(); }));
            }; },
            refresh_interval(ctx, std::chrono::milliseconds(2000)));
    }

    static const events::SessionState* live_session(ViewContext& ctx, u64 va) {
        const events::RunStateData* live = live_run(ctx);
        if (!live) return nullptr;
        const events::SessionState* found = nullptr;
        for (const auto& [id, s] : live->sessions)
            if (s->va == va && !s->finished && (!found || s->started > found->started)) found = s.get();
        return found;
    }

    void rebuild_rows() {
        rows_.clear();
        label_rows_.clear();
        if (!listing_.value() || !*listing_.value()) return;
        const vm::Listing& l = **listing_.value();
        for (u32 i = 0; i < l.lines.size(); ++i) {
            if (!l.lines[i].label.empty()) {
                label_rows_[l.lines[i].address] = rows_.size();
                rows_.push_back({i, true});
            }
            rows_.push_back({i, false});
        }
    }

    // ---- header ---------------------------------------------------------------------------------------

    void draw_header(ViewContext& ctx, const ProjectAccess& access, u64 va) {
        const Symbol* s = access.program->symbols().at(va);
        const std::string name = s ? (s->display.empty() ? s->name : s->display) : hex(va, 8);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
        ImGui::TextWrapped("%s", name.c_str());
        ImGui::PopFont();
        if (s && s->name != name) {
            ImGui::PushFont(ctx.fonts.mono, 0.0f);
            ImGui::TextDisabled("%s", s->name.c_str());
            ImGui::PopFont();
        }
        const project::FunctionInfo info = access.project->function_info(va);
        const events::SessionState* session = live_session(ctx, va);
        std::string range = hex(va, 8);
        if (listing_.value() && *listing_.value() && (**listing_.value()).function.start == va) {
            const auto& f = (**listing_.value()).function;
            range = std::format("{}-{}, {} bytes", hex(f.start, 8), hex(f.end, 8), f.end - f.start);
        } else if (s && s->size) {
            range = std::format("{}, {} bytes", hex(va, 8), s->size);
        }
        ImGui::TextUnformatted(range.c_str());
        const FunctionStatus shown = session && info.status != FunctionStatus::matched ? FunctionStatus::in_progress : info.status;
        same_line_or_wrap(ImGui::GetTextLineHeight() + ImGui::CalcTextSize(std::string(status_text(shown)).c_str()).x);
        status_cell(ctx, shown);
        const double best = std::max(info.best_match, session ? session->best_match : 0.0);
        const std::string figures = std::format("best {:.1f}%, {} attempt(s), {}", best, info.attempts + (session ? session->compiles : 0),
                                                vm::format_usd(info.cost_usd + (session ? session->cost_usd : 0.0)));
        same_line_or_wrap(ImGui::CalcTextSize(figures.c_str()).x);
        ImGui::TextDisabled("%s", figures.c_str());
        if (session) {
            ImGui::TextUnformatted("A session is working on it:");
            ImGui::SameLine();
            if (ImGui::TextLink(std::format("{} (turn {}, {})", session->id, session->turn, session->phase).c_str()))
                ctx.open("agent_session", {.va = va, .session = session->id});
        }
        auto small_width = [](const char* label) { return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2; };
        if (ImGui::SmallButton("Diff viewer")) ctx.open("diff_viewer", {.va = va});
        same_line_or_wrap(small_width("Agent session"));
        if (ImGui::SmallButton("Agent session")) ctx.open("agent_session", {.va = va, .session = session ? session->id : std::string()});
        same_line_or_wrap(small_width("Binary explorer"));
        if (ImGui::SmallButton("Binary explorer")) ctx.open("binary_explorer", {.va = va, .anchor = hex(va)});
        same_line_or_wrap(small_width("Symbols"));
        if (ImGui::SmallButton("Symbols")) ctx.open("symbols", {.va = va});
        RunCommands& commands = *ctx.services.commands;
        const char* run_label = commands.live() ? "Add to the run" : "Run it";
        same_line_or_wrap(small_width(run_label));
        ImGui::BeginDisabled(!commands.live() && !commands.available());
        if (ImGui::SmallButton(run_label)) {
            if (commands.live()) {
                const usize added = commands.enqueue({va});
                ctx.notify(Severity::info, added ? std::format("{} queued.", name) : std::format("{} is already in the run.", name));
            } else {
                commands.start_functions({va});
            }
        }
        ImGui::EndDisabled();
    }

    // ---- listing --------------------------------------------------------------------------------------

    void draw_listing(ViewContext& ctx, const ProjectAccess& access) {
        if (!listing_.value() || !listing_.current(key_)) {
            ImGui::TextDisabled("Disassembling...");
            return;
        }
        const Result<vm::Listing>& result = *listing_.value();
        if (!result) {
            colored_text(ctx.colors().error, "Cannot disassemble: " + result.error().message);
            if (ImGui::SmallButton("Show the bytes in the Binary explorer")) ctx.open("binary_explorer", {.anchor = hex(shown_va_)});
            return;
        }
        const vm::Listing& l = *result;
        ImGui::Checkbox("Bytes", &show_bytes_);
        ImGui::SameLine();
        ImGui::TextDisabled("%zu instructions, %zu blocks, %zu loops", l.function.instruction_count, l.function.block_count, l.function.loop_count);
        busy_marker(ctx, listing_.busy());
        if (!ImGui::BeginChild("##listing", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::EndChild();
            return;
        }
        ImGui::PushFont(ctx.fonts.mono, 0.0f);
        const ThemeColors& c = ctx.colors();
        const float cw = ImGui::CalcTextSize("0").x;
        const float base = ImGui::GetCursorPosX();
        const float gutter = cw * (0.6f * static_cast<float>(std::max(l.max_loop_depth, 1)) + 0.8f);
        const float x_address = base + gutter;
        const float x_bytes = x_address + cw * 10;
        const float x_mnemonic = x_bytes + (show_bytes_ ? cw * 22 : 0);
        const float x_operands = x_mnemonic + cw * 8;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 loop_color = ImGui::GetColorU32(c.info);
        const ImU32 separator = ImGui::GetColorU32(ImGuiCol_Separator);
        std::optional<usize> jump;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows_.size()));
        if (scroll_row_) clipper.IncludeItemByIndex(static_cast<int>(*scroll_row_));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const DisplayRow& row = rows_[static_cast<usize>(i)];
                const vm::ListingLine& line = l.lines[row.line];
                ImGui::PushID(i);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                // A line above every block that starts without a label of its own.
                const bool starts_block = line.block_start && (row.label || line.label.empty());
                if (starts_block && i > 0)
                    dl->AddLine(ImVec2(pos.x, pos.y), ImVec2(pos.x + ImGui::GetContentRegionAvail().x + ImGui::GetScrollMaxX(), pos.y), separator);
                // Loop depth as bars in the gutter.
                for (int d = 0; d < line.loop_depth; ++d) {
                    const float x = pos.x + cw * 0.6f * static_cast<float>(d) + 1;
                    dl->AddRectFilled(ImVec2(x, pos.y), ImVec2(x + 2, pos.y + ImGui::GetTextLineHeightWithSpacing()), loop_color);
                }
                if (row.label) {
                    ImGui::SetCursorPosX(x_address);
                    colored_text(c.accent, line.label + ":");
                    if (line.loop_header) {
                        ImGui::SameLine();
                        colored_text(c.info, "; loop");
                    }
                } else {
                    ImGui::SetCursorPosX(x_address);
                    colored_text(c.muted, std::format("{:08x}", line.address));
                    if (show_bytes_) {
                        ImGui::SameLine(x_bytes);
                        colored_text(c.muted, line.bytes.size() > 21 ? line.bytes.substr(0, 20) + "+" : line.bytes);
                    }
                    ImGui::SameLine(x_mnemonic);
                    colored_text(flow_color(ctx, line.flow), line.mnemonic);
                    if (!line.operands.empty()) {
                        ImGui::SameLine(x_operands);
                        if (line.target) {
                            if (ImGui::TextLink(line.operands.c_str())) jump = follow(ctx, access, line);
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                                ImGui::SetTooltip("%s\n%s", hex(*line.target, 8).c_str(),
                                                  line.inside ? "Go to the label" : "Open (a function in the Inspector, data in the Binary explorer)");
                        } else {
                            ImGui::TextUnformatted(line.operands.c_str());
                        }
                    }
                    if (!line.comment.empty()) {
                        ImGui::SameLine(0, cw * 2);
                        colored_text(line.loop_header ? c.info : c.muted, "; " + line.comment);
                    }
                }
                if (scroll_row_ && *scroll_row_ == static_cast<usize>(i)) {
                    ImGui::SetScrollHereY(0.3f);
                    scroll_row_.reset();
                }
                ImGui::PopID();
            }
        }
        ImGui::PopFont();
        ImGui::EndChild();
        if (jump) scroll_row_ = jump;
    }

    // An operand link: a label inside the function scrolls to it; a function opens in the Inspector; data
    // opens in the Binary explorer.
    std::optional<usize> follow(ViewContext& ctx, const ProjectAccess& access, const vm::ListingLine& line) {
        const u64 target = *line.target;
        if (line.inside) {
            if (auto it = label_rows_.find(target); it != label_rows_.end()) return it->second;
            for (usize r = 0; r < rows_.size(); ++r)
                if (!rows_[r].label && (**listing_.value()).lines[rows_[r].line].address == target) return r;
            return std::nullopt;
        }
        const u64 destination = access.program->thunk_destination(target).value_or(target);
        const Symbol* s = access.program->symbols().at(destination);
        if (s && s->kind == SymbolKind::function) ctx.open("inspector", {.va = destination});
        else ctx.open("binary_explorer", {.anchor = hex(target)});
        return std::nullopt;
    }

    // ---- cross-references -----------------------------------------------------------------------------

    void draw_xrefs(ViewContext& ctx) {
        if (!xrefs_.value() || !xrefs_.current(key_)) {
            ImGui::TextDisabled("Finding cross-references (the first time scans every function)...");
            return;
        }
        const vm::FunctionXrefs& x = *xrefs_.value();
        xref_table(ctx, "Callers", "##callers", x.callers, true);
        xref_table(ctx, "Callees", "##callees", x.callees, false);
        xref_table(ctx, "Data references", "##data", x.data, false);
        xref_table(ctx, "Stored in data", "##pointers", x.pointers, true);
    }

    static void xref_table(ViewContext& ctx, const char* title, const char* id, const std::vector<vm::XrefRow>& rows, bool callers) {
        if (!ImGui::CollapsingHeader(std::format("{} ({})###{}", title, rows.size(), id).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
        if (rows.empty()) {
            ImGui::TextDisabled("None.");
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable(id, 3, flags)) return;
        ImGui::TableSetupColumn(callers ? "From" : "At");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn(callers ? "Function" : "Target", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const vm::XrefRow& r = rows[static_cast<usize>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushFont(ctx.fonts.mono, 0.0f);
                address_link(ctx, std::format("{:08x}", r.at), r.at);
                ImGui::PopFont();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::string(xref_kind_text(r.kind)).c_str());
                ImGui::TableNextColumn();
                if (callers) {
                    if (r.function) function_link(ctx, r.name, r.function);
                    else ImGui::TextUnformatted(r.name.c_str());
                } else {
                    address_link(ctx, r.name, r.target, r.is_function);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    // ---- attempts -------------------------------------------------------------------------------------

    void draw_attempts(ViewContext& ctx, u64 va) {
        if (!attempts_.value() || !attempts_.value_key() || attempts_.value_key()->va != va) {
            ImGui::TextDisabled("Reading the attempts...");
            return;
        }
        const auto& attempts = *attempts_.value();
        busy_marker(ctx, attempts_.busy());
        if (attempts.empty()) {
            ImGui::TextDisabled("No attempt has been recorded for this function.");
            return;
        }
        const vm::AttemptSeries series = vm::attempt_series(attempts);
        if (ImPlot::BeginPlot("Score per attempt##scores", ImVec2(-1, ImGui::GetFontSize() * 11), ImPlotFlags_NoMenus)) {
            ImPlot::SetupAxes("attempt", "match %", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 105, ImPlotCond_Always);
            ImPlot::SetupLegend(ImPlotLocation_SouthEast);
            ImPlot::PlotScatter("score", series.score.x.data(), series.score.y.data(), static_cast<int>(series.score.size()),
                                {ImPlotProp_MarkerFillColor, ctx.colors().info});
            ImPlot::PlotStairs("best so far", series.best.x.data(), series.best.y.data(), static_cast<int>(series.best.size()),
                               {ImPlotProp_LineColor, status_color(ctx, FunctionStatus::matched), ImPlotProp_LineWeight, 2.0f});
            ImPlot::EndPlot();
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable("##attempts", 6, flags, ImVec2(0, 0))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Match");
        ImGui::TableSetupColumn("Session");
        ImGui::TableSetupColumn("By");
        ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(attempts.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const usize index = attempts.size() - 1 - static_cast<usize>(i);  // newest first
                const vm::AttemptRecord& a = attempts[index];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(std::to_string(index + 1).c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    ctx.open("diff_viewer", {.va = va, .session = a.session, .anchor = std::format("attempt:{}", a.attempt)});
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Open attempt %d of its session in the Diff viewer", a.attempt);
                ImGui::TableNextColumn();
                const auto when = vm::parse_iso8601(a.time);
                ImGui::TextUnformatted(when ? local_date_time(*when).c_str() : "-");
                ImGui::TableNextColumn();
                if (!a.compiled) colored_text(ctx.colors().error, "no compile");
                else if (a.byte_exact) colored_text(ctx.colors().ok, "byte-exact");
                else ImGui::Text("%.1f%%", a.match_percent);
                ImGui::TableNextColumn();
                if (!a.session.empty()) {
                    if (ImGui::TextLink(std::format("{}##s", a.session).c_str())) ctx.open("agent_session", {.va = va, .session = a.session});
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(a.origin.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(a.summary.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    // ---- notes ----------------------------------------------------------------------------------------

    void draw_notes(ViewContext& ctx, const ProjectAccess& access, u64 va) {
        const Symbol* s = access.program->symbols().at(va);
        if (!s) {
            ImGui::TextDisabled("Notes belong to functions with a symbol.");
            return;
        }
        // notes.md is a small file: read on the UI thread when the function changes.
        if (!notes_loaded_) {
            notes_ = access.project->notes(*s);
            saved_notes_ = notes_;
            notes_loaded_ = true;
        }
        ImGui::TextDisabled("Later sessions read these notes in their brief.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputTextWithHint("##add_note", "Add a note (Enter)", &new_note_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::BeginDisabled(trim(new_note_).empty());
        if (ImGui::Button("Add note") || (enter && !trim(new_note_).empty())) {
            if (auto r = access.project->append_note(*s, std::string(trim(new_note_))); !r)
                ctx.notify(Severity::error, std::format("Cannot add the note: {}", r.error().message));
            new_note_.clear();
            notes_loaded_ = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(notes_ == saved_notes_);
        if (ImGui::Button("Save edits")) {
            if (auto r = access.project->save_notes(*s, notes_); !r) ctx.notify(Severity::error, std::format("Cannot save the notes: {}", r.error().message));
            notes_loaded_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) notes_ = saved_notes_;
        ImGui::EndDisabled();
        multiline_text("##notes", &notes_, ImVec2(-FLT_MIN, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFontSize() * 6)));
    }

    // ---- status history -------------------------------------------------------------------------------

    void draw_history(ViewContext& ctx, u64 va) {
        ImGui::SeparatorText("Status changes recorded by runs");
        if (!history_.value()) {
            ImGui::TextDisabled("Reading the run logs...");
        } else {
            busy_marker(ctx, history_.busy());
            const auto& history = **history_.value();
            auto it = history.find(va);
            if (it == history.end() || it->second.empty()) {
                ImGui::TextDisabled("No run has changed this function's status (edits to symbols.txt are not logged).");
            } else if (ImGui::BeginTable("##status_history", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Time");
                ImGui::TableSetupColumn("Change");
                ImGui::TableSetupColumn("Best");
                ImGui::TableSetupColumn("Run", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (auto r = it->second.rbegin(); r != it->second.rend(); ++r) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(local_date_time(r->time).c_str());
                    ImGui::TableNextColumn();
                    const auto to = project::status_from_string(r->status);
                    ImGui::TextUnformatted(std::format("{} ->", r->old_status.empty() ? "?" : r->old_status).c_str());
                    ImGui::SameLine();
                    if (to) status_cell(ctx, *to);
                    else ImGui::TextUnformatted(r->status.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f%%", r->best);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r->run.c_str());
                }
                ImGui::EndTable();
            }
        }
        ImGui::SeparatorText("Sessions in the shown run");
        const auto* s = ctx.snapshot.get();
        std::vector<const events::SessionState*> sessions;
        if (s)
            for (const auto& [id, session] : s->sessions)
                if (session->va == va) sessions.push_back(session.get());
        if (sessions.empty()) {
            ImGui::TextDisabled(s ? "This function has no session in the shown run." : "No run is shown.");
            return;
        }
        std::ranges::sort(sessions, [](const auto* a, const auto* b) { return a->started > b->started; });
        if (!ImGui::BeginTable("##sessions", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) return;
        ImGui::TableSetupColumn("Started");
        ImGui::TableSetupColumn("Session");
        ImGui::TableSetupColumn("Outcome");
        ImGui::TableSetupColumn("Best");
        ImGui::TableSetupColumn("Spend");
        ImGui::TableHeadersRow();
        for (const auto* session : sessions) {
            ImGui::PushID(session->id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(local_date_time(session->started).c_str());
            ImGui::TableNextColumn();
            if (ImGui::TextLink(session->id.c_str())) ctx.open("agent_session", {.va = va, .session = session->id});
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(session->finished ? session->outcome.c_str() : "running");
            ImGui::TableNextColumn();
            ImGui::Text("%.1f%%", session->best_match);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(vm::format_usd(session->cost_usd).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    u64 shown_va_ = 0;
    std::string project_root_;
    FunctionKey key_;
    int tab_request_ = -1;
    KeyedJob<FunctionKey, Result<vm::Listing>> listing_;
    std::vector<DisplayRow> rows_;
    std::map<u64, usize> label_rows_;
    std::optional<usize> scroll_row_;
    bool show_bytes_ = false;
    KeyedJob<FunctionKey, vm::FunctionXrefs> xrefs_;
    KeyedJob<AttemptsKey, std::vector<vm::AttemptRecord>> attempts_;
    KeyedJob<HistoryKey, std::shared_ptr<const vm::StatusHistory>> history_;
    bool notes_loaded_ = false;
    std::string notes_, saved_notes_, new_note_;
};

} // namespace

std::unique_ptr<View> make_inspector_view() { return std::make_unique<InspectorView>(); }

} // namespace decomp::gui
