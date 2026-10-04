// Logs and errors (docs/ui.md#logs-and-errors): errors grouped by kind, the run's log, and this
// application's own log.

#include "gui/views/logs_view.hpp"

#include "core/log.hpp"
#include "core/strings.hpp"
#include "gui/widgets.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <array>
#include <format>

namespace decomp::gui {

namespace {

struct Kind {
    const char* id;
    const char* label;
};

constexpr std::array<Kind, 5> kKinds = {{
    {"api", "API errors and retries"},
    {"compiler", "Compiler failures"},
    {"tool", "Tool errors"},
    {"session", "Failed sessions"},
    {"log", "Warnings and errors logged"},
}};

enum class LevelFilter { all, warnings, errors };

bool level_passes(std::string_view level, LevelFilter filter) {
    switch (filter) {
    case LevelFilter::all: return true;
    case LevelFilter::warnings: return level == "warn" || level == "error";
    case LevelFilter::errors: return level == "error";
    }
    return true;
}

std::string session_name(const events::RunStateData& s, const std::string& id) {
    if (id.empty()) return {};
    if (const auto* session = s.session(id)) return session->display.empty() ? session->function : session->display;
    return id;
}

class LogsView final : public View {
public:
    std::string_view id() const override { return "logs"; }
    std::string_view title() const override { return "Logs and errors"; }

    void draw(ViewContext& ctx) override {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
        ImGui::InputTextWithHint("##search", "Search", &search_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
        const char* levels[] = {"All levels", "Warnings and errors", "Errors only"};
        int level = static_cast<int>(filter_);
        if (ImGui::Combo("##level", &level, levels, IM_ARRAYSIZE(levels))) filter_ = static_cast<LevelFilter>(level);
        ImGui::SameLine();
        const bool copy = ImGui::Button("Copy visible");
        copied_.clear();

        if (ImGui::BeginTabBar("##logs_tabs")) {
            if (ImGui::BeginTabItem("Errors")) {
                draw_errors(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Run log")) {
                draw_run_log(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Application log")) {
                draw_app_log();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (copy) ImGui::SetClipboardText(copied_.c_str());
    }

private:
    bool matches(std::string_view text) const {
        const std::string needle(trim(search_));
        return needle.empty() || to_lower(text).find(to_lower(needle)) != std::string::npos;
    }

    void keep(std::string line) {
        copied_ += line;
        copied_ += '\n';
    }

    // Function and session as a link to the Agent session view.
    static void session_link(ViewContext& ctx, const events::RunStateData& s, const std::string& session) {
        if (session.empty()) {
            ImGui::TextDisabled("-");
            return;
        }
        const std::string name = session_name(s, session);
        if (ImGui::TextLink(std::format("{}##{}", name, session).c_str())) ctx.open("agent_session", NavTarget{.session = session});
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Session %s\nOpen the Agent session.", session.c_str());
    }

    void draw_errors(ViewContext& ctx) {
        const auto* s = ctx.snapshot.get();
        if (!s) {
            ImGui::TextDisabled("No run loaded. Errors of a run appear here, grouped by kind.");
            return;
        }
        if (s->error_log.empty()) {
            ImGui::TextDisabled("No errors in this run.");
            return;
        }
        for (const auto& kind : kKinds) {
            std::vector<const events::ErrorRecord*> rows;
            for (const auto& r : s->error_log)
                if (r.kind == kind.id && matches(r.message + " " + session_name(*s, r.session))) rows.push_back(&r);
            if (rows.empty()) continue;
            if (!ImGui::CollapsingHeader(std::format("{} ({})###{}", kind.label, rows.size(), kind.id).c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                continue;
            const bool api = std::string_view(kind.id) == "api";
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
            if (!ImGui::BeginTable(kind.id, api ? 5 : 4, flags)) continue;
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Worker", ImGuiTableColumnFlags_WidthFixed);
            if (api) ImGui::TableSetupColumn("Status, delay", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            // Newest first.
            for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                const auto& r = **it;
                ImGui::PushID(static_cast<int>(r.seq));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(local_clock(r.time).c_str());
                ImGui::TableNextColumn();
                session_link(ctx, *s, r.session);
                ImGui::TableNextColumn();
                if (r.worker >= 0) ImGui::Text("%d", r.worker);
                else ImGui::TextDisabled("-");
                if (api) {
                    ImGui::TableNextColumn();
                    ImGui::Text("%s, %.1f s", r.status ? std::to_string(r.status).c_str() : "network", static_cast<double>(r.delay_ms) / 1000.0);
                }
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", r.message.c_str());
                if (ImGui::BeginPopupContextItem("##row")) {
                    if (ImGui::MenuItem("Copy message")) ImGui::SetClipboardText(r.message.c_str());
                    if (!r.session.empty() && ImGui::MenuItem("Open the session")) ctx.open("agent_session", NavTarget{.session = r.session});
                    ImGui::EndPopup();
                }
                keep(std::format("{} [{}] {}: {}", local_clock(r.time), r.kind, session_name(*s, r.session), r.message));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void draw_run_log(ViewContext& ctx) {
        const auto* s = ctx.snapshot.get();
        if (!s) {
            ImGui::TextDisabled("No run loaded. Warnings and errors logged during a run appear here.");
            return;
        }
        std::vector<const events::LogRecord*> rows;
        for (const auto& r : s->log_tail)
            if (level_passes(r->level, filter_) && matches(r->message + " " + session_name(*s, r->session))) rows.push_back(r.get());
        if (rows.empty()) {
            ImGui::TextDisabled(s->log_tail.empty() ? "Nothing was logged during this run." : "No line matches the filters.");
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable("##run_log", 5, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Worker", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& r = *rows[static_cast<usize>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(local_clock(r.time).c_str());
                ImGui::TableNextColumn();
                level_text(ctx, r.level);
                ImGui::TableNextColumn();
                if (r.worker >= 0) ImGui::Text("%d", r.worker);
                else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                session_link(ctx, *s, r.session);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.message.c_str());
                if (ImGui::BeginPopupContextItem("##row")) {
                    if (ImGui::MenuItem("Copy message")) ImGui::SetClipboardText(r.message.c_str());
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        for (const auto* r : rows) keep(std::format("{} {} {}", local_clock(r->time), r->level, r->message));
        ImGui::EndTable();
    }

    void draw_app_log() {
        // This process's own messages (project loading, settings, jobs, and the run's workers).
        const auto entries = log::recent(1000);
        std::vector<const log::Entry*> rows;
        for (const auto& e : entries)
            if (level_passes(log::to_string(e.level), filter_) && matches(e.message + " " + e.module)) rows.push_back(&e);
        if (rows.empty()) {
            ImGui::TextDisabled("No message matches the filters.");
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable("##app_log", 4, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& e = *rows[rows.size() - 1 - static_cast<usize>(i)];  // newest first
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(local_clock(e.time).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::string(log::to_string(e.level)).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.module.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.message.c_str());
            }
        }
        for (const auto* e : rows) keep(std::format("{} {} {}", local_clock(e->time), log::to_string(e->level), e->message));
        ImGui::EndTable();
    }

    static void level_text(ViewContext& ctx, const std::string& level) {
        const ThemeColors& c = ctx.colors();
        const ImVec4& color = level == "error" ? c.error : level == "warn" ? c.warn : c.muted;
        colored_text(color, level);
    }

    std::string search_;
    LevelFilter filter_ = LevelFilter::all;
    std::string copied_;
};

} // namespace

std::unique_ptr<View> make_logs_view() { return std::make_unique<LogsView>(); }

} // namespace decomp::gui
