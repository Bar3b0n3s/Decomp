// Units (docs/ui.md#units): the program's translation units in link order with their functions, bytes,
// matched share and spend; the functions of the selected unit; and what can be done with a unit: run or
// queue its unfinished functions, verify its source, move its matched functions' own files into it.

#include "gui/views/units_view.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/views/view_support.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"
#include "matching/diff.hpp"
#include "project/setup.hpp"
#include "project/units.hpp"
#include "run/selection.hpp"

#include <format>

namespace decomp::gui {

namespace {

struct UnitsKey {
    ProjectInputs project;
    u64 refresh = 0;  // the Refresh button (units.txt edited elsewhere)
    bool operator==(const UnitsKey&) const = default;
};

struct UnitsData {
    std::vector<project::UnitProgress> progress;  // units.txt order; functions in no unit last, under ""
    std::string error;
};

// What a verification or an emit of one unit found, for the details pane.
struct UnitReport {
    std::string unit;
    std::string summary;
    std::vector<std::string> lines;  // one per function that is not byte-exact, or kept in its own file
    bool ok = false;
};

class UnitsView final : public View {
public:
    std::string_view id() const override { return "units"; }
    std::string_view title() const override { return "Units"; }

    void navigate(ViewContext&, const NavTarget& target) override {
        if (!target.anchor.empty()) selected_ = target.anchor;
    }

    void draw(ViewContext& ctx) override {
        const ProjectAccess access = project_access(ctx);
        if (!require_project(ctx, access,
                             "Units lists the object files the program was linked from, in link order, with each unit's "
                             "functions, bytes, matched share and spend, and runs, verifies or emits a unit.")) {
            return;
        }
        update(ctx, access);
        if (!data_.value()) {
            ImGui::TextDisabled("Reading the units...");
            return;
        }
        const UnitsData& d = *data_.value();
        if (!d.error.empty()) {
            colored_text(ctx.colors().error, "The units cannot be read: " + d.error);
            if (ImGui::SmallButton("Refresh")) ++refresh_;
            return;
        }
        draw_summary(ctx, d);
        draw_table(d);
        draw_details(ctx, access, d);
    }

private:
    void update(ViewContext& ctx, const ProjectAccess& access) {
        data_.poll();
        if (auto r = report_job_.take()) {
            report_ = std::move(*r);
            if (report_->ok) ctx.notify(Severity::info, report_->summary);
            else ctx.notify(Severity::warning, report_->summary);
            ++refresh_;  // an emit changes the unit sources
        }
        const auto program = access.program;
        const project::Project& project = *access.project;  // copied into the job
        data_.update(
            ctx.jobs, UnitsKey{project_inputs(access), refresh_},
            [&] {
                return [program, project]() {
                    UnitsData out;
                    auto units = project::load_units(project);
                    if (!units) {
                        out.error = units.error().message;
                        return out;
                    }
                    out.progress = project::compute_unit_progress(*units, program->symbols(), *project.function_infos());
                    return out;
                };
            },
            refresh_interval(ctx, std::chrono::milliseconds(1000)));
    }

    void draw_summary(ViewContext& ctx, const UnitsData& d) {
        usize units = 0, code = 0;
        double spend = 0;
        for (const auto& u : d.progress) {
            if (u.unit.name.empty()) continue;
            ++units;
            code += u.unit.kind == UnitKind::code ? 1 : 0;
            spend += u.cost_usd;
        }
        if (units == 0) {
            ImGui::TextDisabled("The project has no units yet: `decomp units derive` finds them.");
        } else {
            const auto origin = d.progress.front().unit.origin;
            ImGui::Text("%zu units (%zu of the program's own code), from %s; $%.2f spent on their functions", units, code,
                        origin == UnitOrigin::pdb ? "the PDB" : origin == UnitOrigin::map ? "the link map"
                                                              : origin == UnitOrigin::user ? "units.txt" : "the analysis (a guess)",
                        spend);
        }
        same_line_or_wrap(button_width("Refresh"));
        if (ImGui::SmallButton("Refresh")) ++refresh_;
        busy_marker(ctx, data_.busy() || report_job_.pending(), report_job_.pending() ? "working on the unit..." : "updating...");
    }

    void draw_table(const UnitsData& d) {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
        const float height = std::max(ImGui::GetContentRegionAvail().y * 0.45f, ImGui::GetFrameHeight() * 5);
        if (!ImGui::BeginTable("##units", 8, flags, ImVec2(0, height))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : {"Unit", "Kind", "Functions", "Matched", "Bytes", "Bytes matched", "Spend", "Source"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (const auto& u : d.progress) {
            if (u.unit.name.empty() && u.functions == 0) continue;
            const std::string name = u.unit.name.empty() ? std::string("(no unit)") : u.unit.name;
            ImGui::PushID(name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(name.c_str(), selected_ == u.unit.name, ImGuiSelectableFlags_SpanAllColumns)) selected_ = u.unit.name;
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(u.unit.name.empty() ? "" : std::string(to_string(u.unit.kind)).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%zu", u.functions);
            ImGui::TableNextColumn();
            ImGui::Text("%zu (%.0f%%)", u.matched, u.percent_functions());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(bytes_text(u.bytes).c_str());
            ImGui::TableNextColumn();
            status_label(std::format("{:.1f}%", u.percent_bytes()), best_match_color(u.percent_bytes()));
            ImGui::TableNextColumn();
            ImGui::Text("$%.2f", u.cost_usd);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(u.unit.source.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void draw_details(ViewContext& ctx, const ProjectAccess& access, const UnitsData& d) {
        if (!selected_) {
            ImGui::TextDisabled("Select a unit to see its functions and act on it.");
            return;
        }
        auto it = std::ranges::find_if(d.progress, [&](const project::UnitProgress& u) { return u.unit.name == *selected_; });
        if (it == d.progress.end()) {
            ImGui::TextDisabled("The selected unit is no longer listed.");
            return;
        }
        const project::UnitProgress& u = *it;
        ImGui::SeparatorText(u.unit.name.empty() ? "Functions in no unit" : u.unit.name.c_str());
        if (!u.unit.name.empty()) {
            std::error_code ec;
            const bool exists = !u.unit.source.empty() && std::filesystem::exists(access.project->root() / fs::from_utf8(u.unit.source), ec);
            ImGui::TextDisabled("%s unit from %s%s%s", std::string(to_string(u.unit.kind)).c_str(), std::string(to_string(u.unit.origin)).c_str(),
                                u.unit.source.empty() ? "" : std::format(", source {}", u.unit.source).c_str(),
                                u.unit.source.empty() ? "" : exists ? "" : " (not written yet)");
        }

        // The functions, with their state, and how many are not finished.
        std::vector<u64> functions;
        usize unfinished = 0;
        const auto infos = access.project->function_infos();
        for (const Symbol* f : access.program->symbols().functions()) {
            if (f->object != u.unit.name) continue;
            functions.push_back(f->va);
            const auto info = infos->find(f->va);
            unfinished += run::runnable_by_default(info == infos->end() ? project::FunctionStatus::unstarted : info->second.status) ? 1 : 0;
        }
        draw_actions(ctx, access, u, unfinished);

        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
        const float height = std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFrameHeight() * 4);
        if (ImGui::BeginTable("##unit_functions", 5, flags, ImVec2(0, height))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            for (const char* h : {"Address", "Function", "Status", "Best", "Spend"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(functions.size()));
            while (clipper.Step())
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const u64 va = functions[static_cast<usize>(row)];
                    const Symbol* s = access.program->symbols().at(va);
                    const auto info = infos->find(va);
                    const project::FunctionInfo fi = info == infos->end() ? project::FunctionInfo{} : info->second;
                    ImGui::PushID(row);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    address_link(ctx, std::format("{:#010x}", va), va, true);
                    ImGui::TableNextColumn();
                    function_link(ctx, s && !s->display.empty() ? s->display : std::format("sub_{:x}", va), va);
                    ImGui::TableNextColumn();
                    status_cell(ctx, fi.status);
                    ImGui::TableNextColumn();
                    if (fi.attempts > 0) ImGui::Text("%.1f%%", fi.best_match);
                    ImGui::TableNextColumn();
                    if (fi.cost_usd > 0) ImGui::Text("$%.2f", fi.cost_usd);
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
    }

    void draw_actions(ViewContext& ctx, const ProjectAccess& access, const project::UnitProgress& u, usize unfinished) {
        RunCommands& commands = *ctx.services.commands;
        const bool live = commands.live();
        ImGui::BeginDisabled(unfinished == 0 || !commands.available());
        if (ImGui::Button(live ? "Queue its functions" : "Run its functions")) {
            // What `decomp run --unit` takes: the unit's functions that are not finished and have code to match.
            run::Selection selection;
            selection.units = {u.unit.name};
            auto vas = run::select_functions(*access.program, access.project, selection);
            if (!vas || vas->empty()) {
                ctx.notify(Severity::warning, vas ? std::format("{} has no function to run.", u.unit.name) : vas.error().message);
            } else if (live) {
                const usize added = commands.enqueue(*vas);
                ctx.notify(Severity::info, std::format("{} function(s) of {} queued.", added, u.unit.name));
            } else {
                commands.start_easiest_first(std::move(*vas));
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%zu function(s) not matched, refused, skipped or library%s.", unfinished,
                              live ? ", added to the live run's queue" : ", easiest first");
        if (u.unit.kind != UnitKind::code || u.unit.source.empty()) return;

        same_line_or_wrap(button_width("Verify the source"));
        ImGui::BeginDisabled(report_job_.pending());
        if (ImGui::Button("Verify the source")) start_report(ctx, access, u.unit.name, false);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Compile %s and diff every function it holds against the target.", u.unit.source.c_str());
        same_line_or_wrap(button_width("Emit own files"));
        ImGui::BeginDisabled(report_job_.pending() || live);
        if (ImGui::Button("Emit own files")) start_report(ctx, access, u.unit.name, true);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Move the unit's matched functions' own files (src/functions/) into %s, keeping those that do not stay "
                              "byte-exact there. Not during a run.",
                              u.unit.source.c_str());
        if (report_ && report_->unit == u.unit.name) {
            colored_text(report_->ok ? ctx.colors().ok : ctx.colors().warn, report_->summary);
            for (const auto& line : report_->lines) ImGui::BulletText("%s", line.c_str());
        }
    }

    void start_report(ViewContext& ctx, const ProjectAccess& access, const std::string& name, bool emit) {
        report_.reset();
        report_job_ = ctx.jobs.submit([program = access.program, project = *access.project, name, emit](const CancelToken& token) {
            UnitReport out;
            out.unit = name;
            auto setup = project::make_match_setup(&project, "");
            if (!setup) {
                out.summary = std::format("Cannot compile {}: {}", name, setup.error().message);
                return out;
            }
            setup->cancelled = [token] { return token.cancelled(); };
            auto label = [&](u64 va) {
                const Symbol* s = program->symbols().at(va);
                return s && !s->display.empty() ? s->display : std::format("{:#x}", va);
            };
            const std::string names[] = {name};
            if (emit) {
                auto report = project::emit_unit_sources(project, *program, *setup,
                                                         project::ChangeOrigin{SymbolSource::user, "", "emitted into the unit source"}, names);
                if (!report) {
                    out.summary = std::format("Cannot emit {}: {}", name, report.error().message);
                    return out;
                }
                usize emitted = 0;
                for (const auto& r : report->units) {
                    emitted += r.emitted.size();
                    for (const auto& [va, why] : r.kept) out.lines.push_back(std::format("{} keeps its own file: {}", label(va), why));
                }
                out.ok = out.lines.empty();
                out.summary = report->units.empty() ? std::format("{}: no matched function has its own file to move.", name)
                                                    : std::format("{}: {} function(s) moved into its source{}.", name, emitted,
                                                                  out.lines.empty() ? "" : std::format(", {} kept", out.lines.size()));
                return out;
            }
            auto reports = project::verify_unit_sources(project, *program, *setup, names);
            if (!reports) {
                out.summary = std::format("Cannot verify {}: {}", name, reports.error().message);
                return out;
            }
            if (reports->empty()) {
                out.summary = std::format("{} has no source yet.", name);
                return out;
            }
            const auto& v = reports->front().verification;
            if (!v.error.empty()) {
                out.summary = std::format("{} does not compile: {}", reports->front().unit.source, v.error);
                for (const auto& diag : v.compile.diagnostics)
                    if (diag.severity.find("error") != std::string::npos) out.lines.push_back(std::format("line {}: {}", diag.line, diag.message));
                return out;
            }
            usize exact = 0;
            for (const auto& f : v.functions) {
                if (f.byte_exact()) ++exact;
                else out.lines.push_back(std::format("{}: {}", label(f.va), f.diff ? matching::summary_line(*f.diff) : f.error));
            }
            out.ok = exact == v.functions.size();
            out.summary = std::format("{}: {}/{} functions byte-exact.", reports->front().unit.source, exact, v.functions.size());
            return out;
        });
    }

    KeyedJob<UnitsKey, UnitsData> data_;
    u64 refresh_ = 0;
    std::optional<std::string> selected_;
    JobHandle<UnitReport> report_job_;
    std::optional<UnitReport> report_;
};

} // namespace

std::unique_ptr<View> make_units_view() { return std::make_unique<UnitsView>(); }

} // namespace decomp::gui
