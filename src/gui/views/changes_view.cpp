// Changes and approvals (docs/ui.md#changes-and-approvals): gated actions waiting for a decision, the
// approval policy, and every file written on the agent's (or the user's) behalf, with revert.

#include "gui/views/changes_view.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/views/text_diff.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <format>

namespace decomp::gui {

namespace {

class ChangesView final : public View {
public:
    std::string_view id() const override { return "changes"; }
    std::string_view title() const override { return "Changes and approvals"; }

    void draw(ViewContext& ctx) override {
        RunCommands& commands = *ctx.services.commands;
        draw_approvals(ctx, commands);
        draw_policy(ctx, commands);
        draw_changes(ctx);
    }

private:
    void draw_approvals(ViewContext& ctx, RunCommands& commands) {
        const auto pending = commands.pending_approvals();
        ImGui::SeparatorText(std::format("Waiting for a decision ({})", pending.size()).c_str());
        if (pending.empty()) {
            ImGui::TextDisabled(commands.live() ? "Nothing waits. With the policy \"ask\", verified matches wait here before they are saved."
                                                : "No live run.");
            return;
        }
        for (const auto& p : pending) {
            ImGui::PushID(static_cast<int>(p.id));
            const bool open = ImGui::TreeNodeEx("##approval", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s: %s",
                                                p.request.function.c_str(), p.request.summary.c_str());
            if (open) {
                ImGui::TextDisabled("%s, session %s, waiting since %s", p.request.path.c_str(), p.request.session.c_str(),
                                    local_clock(p.requested).c_str());
                auto& reason = reasons_[p.id];
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
                ImGui::InputTextWithHint("##reason", "Reason (sent to the agent when denied)", &reason);
                ImGui::SameLine();
                if (ImGui::Button("Approve")) {
                    commands.decide(p.id, true, reason);
                    reasons_.erase(p.id);
                }
                ImGui::SameLine();
                if (ImGui::Button("Deny")) {
                    commands.decide(p.id, false, reason);
                    reasons_.erase(p.id);
                }
                if (p.request.previous.empty()) {
                    ImGui::TextDisabled("A new file:");
                    draw_text_block(ctx, "##new", p.request.content, ImGui::GetFontSize() * 12);
                } else {
                    draw_text_diff(ctx, "##diff", p.request.previous, p.request.content, ImGui::GetFontSize() * 14);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    void draw_policy(ViewContext& ctx, RunCommands& commands) {
        ImGui::SeparatorText("Policy");
        const auto* s = ctx.snapshot.get();
        std::string current = "auto";
        if (s && s->config.is_object())
            if (auto p = s->config.find("policies"); p != s->config.end() && p->is_object()) current = json_string_or(*p, "write_source", "auto");
        // Live changes are recorded as control events; the newest one wins.
        if (s)
            for (const auto& c : s->controls)
                if (c.control.command == "set_policy" && c.control.target == "write_source") current = c.control.detail;
        ImGui::BeginDisabled(!commands.live());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::BeginCombo("Saving a verified match", current.c_str())) {
            for (const char* name : {"auto", "ask", "deny"})
                if (ImGui::Selectable(name, current == name))
                    if (auto policy = agent::parse_approval_policy(name)) commands.set_policy("write_source", *policy);
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("For this run; Settings saves the project's default in decomp.json.");
    }

    void draw_changes(ViewContext& ctx) {
        ImGui::SeparatorText("Files written");
        Workspace* ws = ctx.services.workspace;
        project::Project* project = ws ? ws->project() : nullptr;
        if (!project) {
            ImGui::TextDisabled("Open a project to see the files written to it.");
            return;
        }
        // Re-read the change log when the run writes a file (or on request).
        const usize written = ctx.snapshot ? ctx.snapshot->files_written.size() : 0;
        if (ImGui::SmallButton("Refresh") || changes_root_ != project->root() || written != seen_written_) {
            changes_ = project->changes();
            changes_root_ = project->root();
            seen_written_ = written;
        }
        if (changes_.empty()) {
            ImGui::TextDisabled("Nothing has been written yet.");
            return;
        }
        // The latest change of each file is the one that can be reverted.
        std::map<std::string, usize> latest;
        for (usize i = 0; i < changes_.size(); ++i) latest[json_string_or(changes_[i], "path", "")] = i;

        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
        const float table_h = std::max(ImGui::GetContentRegionAvail().y * 0.45f, ImGui::GetFrameHeight() * 4);
        if (ImGui::BeginTable("##changes", 5, flags, ImVec2(0, table_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            for (const char* h : {"Time", "File", "By", "Session", "Reason"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (usize n = 0; n < changes_.size(); ++n) {
                const usize i = changes_.size() - 1 - n;  // newest first
                const Json& c = changes_[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(json_string_or(c, "time", "").c_str(), selected_ == i, ImGuiSelectableFlags_SpanAllColumns)) selected_ = i;
                ImGui::TableNextColumn();
                const std::string path = json_string_or(c, "path", "");
                ImGui::TextUnformatted(c["sha1"].is_null() ? (path + " (removed)").c_str() : path.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(json_string_or(c, "source", "").c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(json_string_or(c, "session", "").c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(json_string_or(c, "reason", "").c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!selected_ || *selected_ >= changes_.size()) {
            ImGui::TextDisabled("Select a change to see what it did.");
            return;
        }
        const Json& c = changes_[*selected_];
        const std::string path = json_string_or(c, "path", "");
        const bool revertible = latest[path] == *selected_ && !c["sha1"].is_null();
        if (shown_ != *selected_) {
            // Before and after, from the blob store and the file (or the blob of a later version).
            shown_ = *selected_;
            before_.clear();
            after_.clear();
            if (c.contains("previous_sha1") && c["previous_sha1"].is_string())
                before_ = project->read_blob(c["previous_sha1"].get<std::string>()).value_or("(the previous content is not available)");
            if (c["sha1"].is_string()) {
                const std::string sha1 = c["sha1"].get<std::string>();
                auto blob = project->read_blob(sha1);
                after_ = blob ? *blob : fs::read_text(project->root() / fs::from_utf8(path)).value_or("(not available)");
            }
        }
        ImGui::BeginDisabled(!revertible || (ws && ws->run_live()));
        if (ImGui::Button("Revert this change")) {
            if (auto r = project->revert_change(c, project::ChangeOrigin{SymbolSource::user, "", "revert"}); !r) {
                ctx.notify(Severity::error, std::format("Cannot revert: {}", r.error().message));
            } else {
                ctx.notify(Severity::info, std::format("Reverted {}.", path));
                if (ws) ws->reload_symbols();
                changes_ = project->changes();
                shown_.reset();
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(revertible ? "Restore what this write replaced (or remove the new file). Not during a run."
                                         : "Only the latest change of a file can be reverted.");
        if (before_.empty() && after_.empty()) ImGui::TextDisabled("The file was removed.");
        else if (before_.empty()) draw_text_block(ctx, "##after", after_, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFontSize() * 6));
        else draw_text_diff(ctx, "##change_diff", before_, after_, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFontSize() * 6));
    }

    std::map<u64, std::string> reasons_;
    std::vector<Json> changes_;
    std::filesystem::path changes_root_;
    usize seen_written_ = 0;
    std::optional<usize> selected_, shown_;
    std::string before_, after_;
};

} // namespace

std::unique_ptr<View> make_changes_view() { return std::make_unique<ChangesView>(); }

} // namespace decomp::gui
