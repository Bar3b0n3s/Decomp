// Relink (docs/ui.md#relink): the whole target linked again with its original linker and compared with it.
// The last relink's units in link order (each from its source or from a split object of its original bytes,
// and why), the selected unit's source check section by section, the comparison (SHA-1s, the identity
// fields taken over, the differing bytes per section, the first difference with the unit holding it and the
// bytes around it in both images), and the linker's command and output. Relink and Check units run in the
// background; the result is .decomp/relink/result.json, so a relink made by `decomp relink` shows too.

#include "gui/views/relink_view.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/views/view_support.hpp"
#include "gui/widgets.hpp"
#include "project/relink.hpp"
#include "project/setup.hpp"
#include "project/units.hpp"
#include "viewmodel/common.hpp"
#include "viewmodel/relink.hpp"

#include <format>
#include <map>
#include <mutex>

namespace decomp::gui {

namespace {

struct RelinkKey {
    ProjectInputs project;
    u64 refresh = 0;  // a relink finished, or the Refresh button
    bool operator==(const RelinkKey&) const = default;
};

struct RelinkData {
    std::optional<vm::RelinkReport> report;  // the last relink (result.json); nullopt: none yet
    std::vector<Unit> units;                 // units.txt, listed before the first relink
    std::vector<vm::HexCompareRow> hex;      // the bytes around the first difference
    std::string hex_error;
    std::string error;
};

// The running relink's latest step, written by the job and read by the UI thread.
struct ProgressLine {
    std::mutex mutex;
    std::string text;

    void set(std::string t) {
        std::lock_guard lock(mutex);
        text = std::move(t);
    }
    std::string get() {
        std::lock_guard lock(mutex);
        return text;
    }
};

struct RelinkOutcome {
    bool compared = false;
    bool identical = false;
    std::string text;
};

struct CheckOutcome {
    std::map<std::string, vm::RelinkUnitCheck> checks;
    std::string error;
};

// What the next relink does with a unit, besides what the relink decides.
enum class Override : u8 { none, source, split };

constexpr int kComparisonTab = 0, kUnitTab = 1, kLinkTab = 2;

class RelinkView final : public View {
public:
    std::string_view id() const override { return "relink"; }
    std::string_view title() const override { return "Relink"; }

    // The anchor is a unit (its details), or "tab:comparison" / "tab:link".
    void navigate(ViewContext&, const NavTarget& target) override {
        if (target.anchor.empty()) return;
        if (target.anchor == "tab:comparison") {
            tab_request_ = kComparisonTab;
        } else if (target.anchor == "tab:link") {
            tab_request_ = kLinkTab;
        } else {
            selected_ = target.anchor;
            tab_request_ = kUnitTab;
        }
    }

    void draw(ViewContext& ctx) override {
        const ProjectAccess access = project_access(ctx);
        if (!require_project(ctx, access,
                             "Relink links the whole target again with its original linker, each unit from its source once that "
                             "is complete and from its original bytes otherwise, and compares the result with the target byte for "
                             "byte.")) {
            return;
        }
        update(ctx, access);
        draw_toolbar(ctx, access);
        if (!data_.value()) {
            ImGui::TextDisabled("Reading the last relink...");
            return;
        }
        const RelinkData& d = *data_.value();
        if (!d.error.empty()) colored_text(ctx.colors().error, d.error);
        if (d.report) draw_summary(ctx, access, *d.report);
        else
            ImGui::TextWrapped("No relink yet. Relink compiles each unit whose source is complete, carries every other unit's "
                               "original code and data in a split object, links them with the target's linker and compares the "
                               "result with the target.");
        draw_units(ctx, d);
        draw_details(ctx, access, d);
    }

private:
    // ---- jobs -----------------------------------------------------------------------------------------

    void update(ViewContext& ctx, const ProjectAccess& access) {
        data_.poll();
        try {
            if (auto r = relink_job_.take()) {
                ctx.notify(r->compared && r->identical ? Severity::info : Severity::warning, r->text, NavEntry{std::string(id()), {}});
                ++refresh_;
                tab_request_ = kComparisonTab;
            }
        } catch (const std::exception& e) {
            ctx.notify(Severity::error, std::format("The relink failed: {}", e.what()));
        }
        try {
            if (auto c = check_job_.take()) {
                if (!c->error.empty()) {
                    ctx.notify(Severity::warning, c->error);
                } else {
                    checks_ = std::move(c->checks);
                    usize complete = 0;
                    for (const auto& [unit, check] : checks_) complete += check.complete ? 1 : 0;
                    ctx.notify(Severity::info, std::format("{} of {} unit source(s) complete.", complete, checks_.size()),
                               NavEntry{std::string(id()), {}});
                }
            }
        } catch (const std::exception& e) {
            ctx.notify(Severity::error, std::format("The unit check failed: {}", e.what()));
        }
        const auto program = access.program;
        const project::Project& project = *access.project;  // copied into the job
        data_.update(
            ctx.jobs, RelinkKey{project_inputs(access), refresh_},
            [&] {
                return [program, project]() {
                    RelinkData out;
                    auto json = project::last_relink(project);
                    if (!json) {
                        auto units = project::load_units(project);
                        if (units) out.units = std::move(*units);
                        else out.error = "The units cannot be read: " + units.error().message;
                        return out;
                    }
                    out.report = vm::read_relink_report(*json);
                    const auto& r = *out.report;
                    // Where to look: the first difference in the sections, else the first in the headers
                    // (whose RVAs are their file offsets).
                    std::optional<u32> at;
                    if (r.first && r.first->rva) at = r.first->rva;
                    else if (!r.differences.empty()) at = r.differences.front().offset;
                    if (r.compared && !r.identical && at && !r.image.empty()) {
                        auto relinked = vm::load_stamped_relink(project.root() / fs::from_utf8(r.image), r.stamped);
                        if (relinked) out.hex = vm::hex_compare(program->image(), *relinked, *at, 2, 6);
                        else out.hex_error = relinked.error().message;
                    }
                    return out;
                };
            },
            refresh_interval(ctx, std::chrono::milliseconds(2000)));
    }

    void start_relink(ViewContext& ctx, const ProjectAccess& access) {
        auto progress = std::make_shared<ProgressLine>();
        progress->set("starting...");
        progress_ = progress;
        project::RelinkOptions options;
        options.all_split = all_split_;
        for (const auto& [unit, o] : overrides_) {
            if (o == Override::source) options.source.push_back(unit);
            else if (o == Override::split) options.split.push_back(unit);
        }
        relink_job_ = ctx.jobs.submit([program = access.program, project = *access.project, options, progress](const CancelToken& token) mutable {
            RelinkOutcome out;
            auto setup = project::make_match_setup(&project, "");
            if (!setup) {
                out.text = "Cannot relink: " + setup.error().message;
                return out;
            }
            setup->cancelled = [token] { return token.cancelled(); };
            options.cancelled = [token] { return token.cancelled(); };
            options.progress = [progress](const std::string& line) { progress->set(line); };
            auto r = project::relink_project(project, *program, *setup, options);
            if (!r) {
                out.text = "The relink failed: " + r.error().message;
                return out;
            }
            out.compared = r->comparison.has_value();
            out.identical = r->identical();
            out.text = "Relink: " + vm::read_relink_report(project::to_json(*r)).headline() + ".";
            return out;
        });
    }

    void start_check(ViewContext& ctx, const ProjectAccess& access) {
        check_job_ = ctx.jobs.submit([program = access.program, project = *access.project](const CancelToken& token) {
            CheckOutcome out;
            auto setup = project::make_match_setup(&project, "");
            if (!setup) {
                out.error = "Cannot check the units: " + setup.error().message;
                return out;
            }
            setup->cancelled = [token] { return token.cancelled(); };
            auto checks = project::check_unit_sources(project, *program, *setup);
            if (!checks) {
                out.error = "Cannot check the units: " + checks.error().message;
                return out;
            }
            if (checks->empty()) out.error = "No unit has a source yet: matches go into them, and Units' Emit moves older ones there.";
            for (const auto& c : *checks) out.checks[c.unit.name] = vm::read_unit_check(project::to_json(c));
            return out;
        });
    }

    // ---- toolbar and summary --------------------------------------------------------------------------

    void draw_toolbar(ViewContext& ctx, const ProjectAccess& access) {
        const bool relinking = relink_job_.pending();
        ImGui::BeginDisabled(relinking);
        if (ImGui::Button("Relink")) start_relink(ctx, access);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Link the target again with its linker (decomp relink) and compare the result with it.");
        same_line_or_wrap(checkbox_width("Every unit split"));
        ImGui::Checkbox("Every unit split", &all_split_);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Carry every unit's original bytes, sources or not: tests the relink itself (--all-split).");
        same_line_or_wrap(button_width("Check units"));
        ImGui::BeginDisabled(check_job_.pending());
        if (ImGui::Button("Check units")) start_check(ctx, access);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Compile each unit source and place its object in the image, without linking (decomp units check).");
        same_line_or_wrap(button_width("Refresh"));
        if (ImGui::SmallButton("Refresh")) ++refresh_;
        if (relinking) {
            same_line_or_wrap(button_width("Cancel"));
            if (ImGui::SmallButton("Cancel")) relink_job_.cancel();
        }
        const std::string step = relinking && progress_ ? progress_->get() : std::string();
        busy_marker(ctx, relinking || check_job_.pending() || data_.busy(),
                    relinking ? "relinking: " + step : check_job_.pending() ? "checking the unit sources..." : "updating...");
    }

    void draw_summary(ViewContext& ctx, const ProjectAccess& access, const vm::RelinkReport& r) {
        const ImVec4 color = !r.link_ok || !r.compared ? ctx.colors().error : r.identical ? ctx.colors().ok : ctx.colors().warn;
        colored_text(color, r.headline());
        const auto when = vm::parse_iso8601(r.time);
        ImGui::TextDisabled("Relinked %s: %zu unit(s) from source, %zu split, %zu made by the linker",
                            when ? local_date_time(*when).c_str() : r.time.c_str(), r.count("source"), r.count("split"), r.count("linker"));
        if (!r.linker_text.empty()) {
            if (r.same_linker == false) colored_text(ctx.colors().warn, "Linker: " + r.linker_text);
            else ImGui::TextDisabled("Linker: %s", r.linker_text.c_str());
        }
        if (r.first) {
            ImGui::TextUnformatted("First difference:");
            ImGui::SameLine();
            ImGui::TextUnformatted(r.first->where.c_str());
            if (r.first->rva) {
                ImGui::SameLine();
                const u64 va = access.program->image().image_base() + *r.first->rva;
                address_link(ctx, std::format("{:#010x}", va), va);
            }
            if (!r.first->unit.empty()) {
                ImGui::SameLine();
                ImGui::TextUnformatted("in");
                ImGui::SameLine();
                if (ImGui::TextLink(r.first->unit.c_str())) {
                    selected_ = r.first->unit;
                    tab_request_ = kUnitTab;
                }
            }
            if (!r.first->symbol.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", r.first->symbol.c_str());
            }
        }
        for (const auto& n : r.notes) ImGui::BulletText("%s", n.c_str());
    }

    // ---- units ----------------------------------------------------------------------------------------

    // The check to show for a unit: the latest Check units' when it has one, else the relink's.
    const vm::RelinkUnitCheck* check_of(const std::string& unit, const vm::RelinkUnit* link) const {
        if (auto it = checks_.find(unit); it != checks_.end()) return &it->second;
        return link && link->check ? &*link->check : nullptr;
    }

    void check_cell(ViewContext& ctx, const vm::RelinkUnitCheck* c) {
        if (!c) return;
        if (c->complete) status_label("complete", ctx.colors().ok);
        else status_label(c->error.empty() ? c->summary : c->error, ctx.colors().warn);
    }

    void draw_units(ViewContext& ctx, const RelinkData& d) {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
        const float height = std::max(ImGui::GetContentRegionAvail().y * 0.4f, ImGui::GetFrameHeight() * 5);
        if (!ImGui::BeginTable("##relink_units", 5, flags, ImVec2(0, height))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Unit");
        ImGui::TableSetupColumn("Linked");
        ImGui::TableSetupColumn("Bytes");
        ImGui::TableSetupColumn("Source check");
        ImGui::TableSetupColumn("Why", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        auto row = [&](const std::string& unit, int index, auto&& cells) {
            ImGui::PushID(index);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string label = unit.empty() ? std::string("(no unit)") : unit;
            if (ImGui::Selectable(label.c_str(), selected_ == unit, ImGuiSelectableFlags_SpanAllColumns)) {
                selected_ = unit;
                tab_request_ = kUnitTab;
            }
            cells();
            ImGui::PopID();
        };
        if (d.report) {
            int index = 0;
            for (const auto& u : d.report->units) {
                row(u.unit, index++, [&] {
                    ImGui::TableNextColumn();
                    if (u.mode == "source") status_label("source", ctx.colors().ok);
                    else if (u.mode == "linker") ImGui::TextDisabled("linker");
                    else ImGui::TextUnformatted(u.mode.c_str());
                    if (auto o = overrides_.find(u.unit); o != overrides_.end() && o->second != Override::none) {
                        ImGui::SameLine();
                        ImGui::TextDisabled(o->second == Override::source ? "(next: source)" : "(next: split)");
                    }
                    ImGui::TableNextColumn();
                    if (u.mode != "linker") ImGui::TextUnformatted(bytes_text(u.bytes).c_str());
                    ImGui::TableNextColumn();
                    check_cell(ctx, check_of(u.unit, &u));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(u.reason.c_str());
                });
            }
        } else {
            int index = 0;
            for (const auto& u : d.units) {
                row(u.name, index++, [&] {
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    check_cell(ctx, check_of(u.name, nullptr));
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s unit", std::string(to_string(u.kind)).c_str());
                });
            }
        }
        ImGui::EndTable();
    }

    // ---- details --------------------------------------------------------------------------------------

    void draw_details(ViewContext& ctx, const ProjectAccess& access, const RelinkData& d) {
        if (!ImGui::BeginTabBar("##relink_tabs")) return;
        const char* names[] = {"Comparison", "Unit", "Link"};
        for (int t = 0; t < 3; ++t) {
            const ImGuiTabItemFlags flags = tab_request_ == t ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (!ImGui::BeginTabItem(names[t], nullptr, flags)) continue;
            if (ImGui::BeginChild("##relink_tab", ImVec2(0, 0))) {
                switch (t) {
                case kComparisonTab: draw_comparison(ctx, access, d); break;
                case kUnitTab: draw_unit(ctx, access, d); break;
                case kLinkTab: draw_link(ctx, d); break;
                default: break;
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        tab_request_ = -1;
        ImGui::EndTabBar();
    }

    void hex_bytes(ViewContext& ctx, const std::array<std::optional<u8>, 16>& bytes, const std::array<bool, 16>& differs) {
        for (usize i = 0; i < bytes.size(); ++i) {
            ImGui::SameLine(0, i == 0 ? ImGui::GetFontSize() : (i == 8 ? ImGui::CalcTextSize("  ").x : ImGui::CalcTextSize(" ").x));
            const std::string text = bytes[i] ? std::format("{:02x}", *bytes[i]) : std::string("..");
            if (differs[i]) colored_text(ctx.colors().error, text);
            else if (!bytes[i]) ImGui::TextDisabled("%s", text.c_str());
            else ImGui::TextUnformatted(text.c_str());
        }
    }

    void draw_comparison(ViewContext& ctx, const ProjectAccess& access, const RelinkData& d) {
        if (!d.report) {
            ImGui::TextDisabled("Relink to compare the relinked image with the target.");
            return;
        }
        const auto& r = *d.report;
        if (!r.compared) {
            colored_text(ctx.colors().error, r.error.empty() ? std::string("The relinked image was not compared.") : r.error);
            ImGui::TextDisabled("The Link tab has the linker's output.");
            return;
        }
        const u64 base = access.program->image().image_base();
        ImGui::PushFont(ctx.fonts.mono, 0.0f);
        ImGui::Text("target    %s  %llu bytes", r.original_sha1.c_str(), static_cast<unsigned long long>(r.original_size));
        ImGui::Text("relinked  %s  %llu bytes", r.relinked_sha1.c_str(), static_cast<unsigned long long>(r.relinked_size));
        ImGui::PopFont();
        if (r.relinked_unstamped_sha1 != r.relinked_sha1)
            ImGui::TextDisabled("As the linker wrote it (before the fields below were taken over): %s", r.relinked_unstamped_sha1.c_str());

        if (!r.identical) {
            ImGui::SeparatorText("Differences");
            if (!d.hex.empty()) {
                ImGui::TextDisabled("Target and relinked bytes around the first difference (differing bytes highlighted):");
                ImGui::PushFont(ctx.fonts.mono, 0.0f);
                for (const auto& row : d.hex) {
                    ImGui::Text("%08llx", static_cast<unsigned long long>(base + row.rva));
                    hex_bytes(ctx, row.original, row.differs);
                    ImGui::SameLine(0, ImGui::GetFontSize());
                    ImGui::TextDisabled("|");
                    hex_bytes(ctx, row.relinked, row.differs);
                }
                ImGui::PopFont();
            } else if (!d.hex_error.empty()) {
                ImGui::TextDisabled("The relinked image cannot be read: %s", d.hex_error.c_str());
            }
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
            if (!r.sections.empty() && ImGui::BeginTable("##relink_sections", 5, flags)) {
                for (const char* h : {"Section", "Differing bytes", "First at", "In unit", ""}) ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                for (usize i = 0; i < r.sections.size(); ++i) {
                    const auto& s = r.sections[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(s.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(s.differing_bytes));
                    ImGui::TableNextColumn();
                    if (s.first_rva) address_link(ctx, std::format("{:#010x}", base + *s.first_rva), base + *s.first_rva);
                    ImGui::TableNextColumn();
                    if (!s.first_unit.empty() && ImGui::TextLink(s.first_unit.c_str())) {
                        selected_ = s.first_unit;
                        tab_request_ = kUnitTab;
                    }
                    ImGui::TableNextColumn();
                    if (s.size_differs) colored_text(ctx.colors().warn, "its size or place differs");
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            usize headers = 0;
            for (const auto& diff : r.differences) {
                if (diff.rva) continue;
                if (headers++ == 0) ImGui::TextUnformatted("Header fields that differ:");
                ImGui::BulletText("%s: %s -> %s", diff.where.c_str(), diff.original.c_str(), diff.relinked.c_str());
            }
        }

        if (!r.stamped.empty()) {
            ImGui::SeparatorText("Taken over from the target");
            ImGui::TextDisabled("Fields that only record when and how the image was built.");
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
            if (ImGui::BeginTable("##relink_stamped", 3, flags)) {
                for (const char* h : {"Field", "Target", "Relinked"}) ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                for (const auto& s : r.stamped) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(s.name.c_str());
                    ImGui::PushFont(ctx.fonts.mono, 0.0f);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(s.original.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(s.relinked.c_str());
                    ImGui::PopFont();
                }
                ImGui::EndTable();
            }
        }
    }

    void draw_override(const vm::RelinkUnit* link, const std::string& unit, bool has_source) {
        if (unit.empty() || (link && link->mode == "linker")) return;
        Override& o = overrides_[unit];
        int value = static_cast<int>(o);
        ImGui::TextUnformatted("Next relink:");
        ImGui::SameLine();
        ImGui::RadioButton("as checked", &value, static_cast<int>(Override::none));
        ImGui::SameLine();
        ImGui::BeginDisabled(!has_source);
        ImGui::RadioButton("from its source", &value, static_cast<int>(Override::source));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(has_source ? "Link the unit from its source even when its check fails: to see what it breaks (--source)."
                                         : "The unit has no source.");
        ImGui::SameLine();
        ImGui::RadioButton("from its original bytes", &value, static_cast<int>(Override::split));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Carry the unit's original code and data even when its source is complete (--split).");
        o = static_cast<Override>(value);
    }

    void draw_unit(ViewContext& ctx, const ProjectAccess& access, const RelinkData& d) {
        if (!selected_) {
            ImGui::TextDisabled("Select a unit to see how it was linked and what its source's check found.");
            return;
        }
        const std::string& unit = *selected_;
        const vm::RelinkUnit* link = nullptr;
        if (d.report)
            for (const auto& u : d.report->units)
                if (u.unit == unit) link = &u;
        const Unit* listed = nullptr;
        for (const auto& u : d.units)
            if (u.name == unit) listed = &u;
        if (!link && !listed && !checks_.contains(unit)) {
            ImGui::TextDisabled("%s is not in the last relink.", unit.c_str());
            return;
        }
        ImGui::TextUnformatted(unit.empty() ? "(no unit)" : unit.c_str());
        if (!unit.empty()) {
            ImGui::SameLine();
            if (ImGui::TextLink("open in Units")) ctx.open("units", {.anchor = unit});
        }
        if (link) ImGui::TextDisabled("%s unit, linked %s: %s", link->kind.c_str(), link->mode == "source" ? "from its source" : link->mode == "split" ? "from its original bytes" : "by the linker", link->reason.c_str());
        const vm::RelinkUnitCheck* c = check_of(unit, link);
        const bool has_source = c != nullptr || (link && link->kind == "code" && link->check) ||
                                (listed && listed->kind == UnitKind::code && !listed->source.empty());
        draw_override(link, unit, has_source);
        if (!c) {
            ImGui::TextDisabled(has_source ? "Check units compiles its source and places it in the image." : "The unit has no source to check.");
            return;
        }
        ImGui::SeparatorText("Source check");
        if (c->complete) colored_text(ctx.colors().ok, "complete: the compiled source fills the unit's place in the image exactly");
        else colored_text(ctx.colors().warn, c->error.empty() ? c->summary : c->error);
        if (!c->compile_output.empty() && ImGui::CollapsingHeader("Compiler output")) {
            ImGui::PushFont(ctx.fonts.mono, 0.0f);
            ImGui::TextUnformatted(c->compile_output.c_str());
            ImGui::PopFont();
        }
        if (!c->missing_functions.empty()) {
            ImGui::TextUnformatted("Functions not in the source:");
            for (usize i = 0; i < c->missing_functions.size(); ++i) {
                const u64 va = c->missing_functions[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::SameLine();
                function_link(ctx, access.program->describe_address(va), va);
                ImGui::PopID();
            }
        }
        const u64 base = access.program->image().image_base();
        if (!c->sections.empty()) {
            ImGui::TextDisabled("%zu of %zu sections equal; %zu differ, %zu unplaced, %zu pooled or folded into others", c->count("equal"),
                                c->sections.size(), c->count("differs"), c->count("unplaced"), c->count("discarded"));
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("##relink_placed", 6, flags)) {
                ImGui::TableSetupColumn("Section");
                ImGui::TableSetupColumn("Symbol");
                ImGui::TableSetupColumn("Address");
                ImGui::TableSetupColumn("Size");
                ImGui::TableSetupColumn("State");
                ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (usize i = 0; i < c->sections.size(); ++i) {
                    const auto& s = c->sections[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(s.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(s.symbol.c_str());
                    ImGui::TableNextColumn();
                    if (s.rva) address_link(ctx, std::format("{:#010x}", base + *s.rva), base + *s.rva);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", s.size);
                    ImGui::TableNextColumn();
                    if (s.state == "equal") status_label("equal", ctx.colors().ok);
                    else if (s.state == "differs") status_label("differs", ctx.colors().error);
                    else if (s.state == "unplaced") status_label("unplaced", ctx.colors().warn);
                    else ImGui::TextDisabled("%s", s.state.c_str());
                    ImGui::TableNextColumn();
                    // The check's own note says how a section differs or why it is not placed.
                    std::string detail = s.note;
                    if (!s.folded_into.empty()) detail = "folded into " + s.folded_into;
                    else if (s.state == "discarded" && !s.unit.empty() && detail.empty()) detail = "the image has " + s.unit + "'s copy";
                    else if (detail.empty() && s.state == "differs" && s.first_difference)
                        detail = std::format("{} byte(s) differ, the first at +{:#x}", s.differing_bytes, *s.first_difference);
                    ImGui::TextUnformatted(detail.c_str());
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        if (!c->missing.empty()) {
            ImGui::TextUnformatted("The unit's contributions nothing in the source fills:");
            for (usize i = 0; i < c->missing.size(); ++i) {
                const auto& m = c->missing[i];
                ImGui::PushID(static_cast<int>(i + 100000));
                ImGui::Bullet();
                ImGui::SameLine();
                ImGui::Text("%s%s%s (%u bytes) at", m.section.c_str(), m.name.empty() ? "" : " ", m.name.c_str(), m.size);
                ImGui::SameLine();
                address_link(ctx, std::format("{:#010x}", base + m.rva), base + m.rva);
                ImGui::PopID();
            }
        }
        for (const auto& p : c->problems) ImGui::BulletText("%s", p.c_str());
    }

    void draw_link(ViewContext& ctx, const RelinkData& d) {
        if (!d.report) {
            ImGui::TextDisabled("Relink to see the linker's command and output.");
            return;
        }
        const auto& r = *d.report;
        if (r.link_ok) colored_text(ctx.colors().ok, std::format("Linked in {:.1f} s: {}", r.link_ms / 1000.0, r.image));
        else colored_text(ctx.colors().error, std::format("The link failed (exit code {}).", r.exit_code));
        if (!r.linker_version.empty()) ImGui::TextDisabled("%s", r.linker_version.c_str());
        if (!r.libraries.empty()) ImGui::TextDisabled("Import libraries written: %s", join(r.libraries, ", ").c_str());
        ImGui::SeparatorText("Command");
        ImGui::PushFont(ctx.fonts.mono, 0.0f);
        ImGui::TextWrapped("%s", join(r.command, " ").c_str());
        ImGui::PopFont();
        ImGui::SeparatorText("Output");
        if (r.link_output.empty()) {
            ImGui::TextDisabled("(none)");
        } else {
            ImGui::PushFont(ctx.fonts.mono, 0.0f);
            ImGui::TextUnformatted(r.link_output.c_str());
            ImGui::PopFont();
        }
    }

    KeyedJob<RelinkKey, RelinkData> data_;
    u64 refresh_ = 0;
    JobHandle<RelinkOutcome> relink_job_;
    std::shared_ptr<ProgressLine> progress_;
    JobHandle<CheckOutcome> check_job_;
    std::map<std::string, vm::RelinkUnitCheck> checks_;  // the latest Check units
    std::map<std::string, Override> overrides_;
    bool all_split_ = false;
    std::optional<std::string> selected_;
    int tab_request_ = -1;
};

} // namespace

std::unique_ptr<View> make_relink_view() { return std::make_unique<RelinkView>(); }

} // namespace decomp::gui
