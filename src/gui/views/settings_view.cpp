// Settings (docs/ui.md#settings): the API key's status, the project's agent settings (decomp.json),
// appearance, layouts, recent projects and developer options.

#include "gui/views/settings_view.hpp"

#include "agent/approvals.hpp"
#include "agent/client.hpp"
#include "agent/cost.hpp"
#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <cmath>
#include <format>

namespace decomp::gui {

namespace {

struct KeyCheck {
    bool ok = false;
    std::string message;
    std::vector<std::string> models;
};

// Lists the models the key can use: a request that consumes no tokens.
KeyCheck check_key() {
    KeyCheck r;
    agent::ClientConfig config;  // the key and base URL come from the environment
    if (trim(config.api_key).empty()) {
        r.message = "ANTHROPIC_API_KEY is not set.";
        return r;
    }
    config.max_retries = 1;
    agent::Client client(config, std::shared_ptr<agent::HttpTransport>(agent::make_default_transport()));
    auto models = client.list_models();
    if (!models) {
        r.message = models.error().message;
        return r;
    }
    r.ok = true;
    for (const auto& m : *models) r.models.push_back(m.display_name.empty() ? m.id : std::format("{} ({})", m.id, m.display_name));
    r.message = std::format("The key works: {} model(s) available.", r.models.size());
    return r;
}

class SettingsView final : public View {
public:
    std::string_view id() const override { return "settings"; }
    std::string_view title() const override { return "Settings"; }

    void draw(ViewContext& ctx) override {
        if (auto result = check_.take()) checked_ = std::move(*result);
        draw_api_key(ctx);
        draw_agent(ctx);
        draw_appearance(ctx);
        draw_projects_and_layouts(ctx);
        draw_developer(ctx);
    }

private:
    void draw_api_key(ViewContext& ctx) {
        ImGui::SeparatorText("API key");
        const ThemeColors& c = ctx.colors();
        // The key itself is never shown or entered here: it comes from the environment.
        const bool present = !trim(agent::default_api_key()).empty();
        if (present) status_label("ANTHROPIC_API_KEY is set", c.ok);
        else status_label("ANTHROPIC_API_KEY is not set: export it before starting decomp-gui", c.warn);
        ImGui::SameLine();
        ImGui::BeginDisabled(!present || (check_.valid() && !check_.finished()));
        if (ImGui::Button("Check")) check_ = ctx.jobs.submit(check_key);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Lists the models the key can use (no tokens are spent).");
        if (check_.valid() && !check_.finished()) {
            ImGui::SameLine();
            ImGui::TextDisabled("checking...");
        }
        if (checked_) {
            colored_text(checked_->ok ? c.ok : c.error, checked_->message);
            if (checked_->ok && ImGui::TreeNode("Models")) {
                for (const auto& m : checked_->models) ImGui::BulletText("%s", m.c_str());
                ImGui::TreePop();
            }
        }
        ImGui::TextDisabled("Base URL: %s", agent::default_base_url().c_str());
    }

    void draw_agent(ViewContext& ctx) {
        ImGui::SeparatorText("Agent (this project's decomp.json)");
        Workspace* ws = ctx.services.workspace;
        project::Project* project = ws ? ws->project() : nullptr;
        if (!project) {
            ImGui::TextDisabled("Open a project to edit its agent settings.");
            draft_.reset();
            return;
        }
        if (!draft_ || draft_root_ != project->root()) {
            draft_ = project->config().agent;
            draft_root_ = project->root();
        }
        project::AgentSettings& a = *draft_;
        const float field = ImGui::GetFontSize() * 12;
        ImGui::SetNextItemWidth(field);
        if (ImGui::BeginCombo("Model", a.model.c_str())) {
            for (const char* m : {"claude-opus-5-5", "claude-sonnet-5-5"})
                if (ImGui::Selectable(m, a.model == m)) a.model = m;
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(field);
        if (ImGui::BeginCombo("Effort", a.effort.c_str())) {
            for (const char* e : {"low", "medium", "high", "xhigh", "max"})
                if (ImGui::Selectable(e, a.effort == e)) a.effort = e;
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Let the API retry declined requests on a fallback model", &a.fallbacks);
        ImGui::SetNextItemWidth(field);
        ImGui::InputInt("Workers", &a.workers);
        a.workers = std::clamp(a.workers, 1, 64);
        ImGui::SetNextItemWidth(field);
        ImGui::InputDouble("Run budget, $ (0 = unlimited)", &a.max_usd_per_run, 0, 0, "%.2f");
        ImGui::SetNextItemWidth(field);
        ImGui::InputInt("Turns per function", &a.max_turns);
        a.max_turns = std::max(1, a.max_turns);
        ImGui::SetNextItemWidth(field);
        ImGui::InputDouble("Dollars per function (0 = unlimited)", &a.max_usd_per_function, 0, 0, "%.2f");
        ImGui::SetNextItemWidth(field);
        ImGui::InputScalar("Tokens per function (0 = unlimited)", ImGuiDataType_S64, &a.max_tokens_per_function);
        ImGui::SetNextItemWidth(field);
        ImGui::InputInt("Minutes per function (0 = unlimited)", &a.max_minutes_per_function);
        a.max_usd_per_run = std::max(0.0, a.max_usd_per_run);
        a.max_usd_per_function = std::max(0.0, a.max_usd_per_function);
        a.max_tokens_per_function = std::max(0LL, a.max_tokens_per_function);
        a.max_minutes_per_function = std::max(0, a.max_minutes_per_function);

        // An action decomp.json leaves out has its default: saving a verified match is automatic; symbol and
        // type edits ask in the GUI (and are denied in command-line runs).
        for (std::string_view action : agent::approval_actions()) {
            const std::string name(action);
            const std::string policy = a.approvals.contains(name) ? a.approvals[name] : std::string(agent::to_string(agent::default_policy(action, true)));
            const char* label = action == agent::kWriteSourceAction ? "Saving a verified match"
                                : action == agent::kSetSymbolAction ? "Symbol changes (set_symbol)"
                                                                    : "Type definitions (define_type)";
            ImGui::SetNextItemWidth(field);
            if (ImGui::BeginCombo(label, policy.c_str())) {
                for (const char* p : {"auto", "ask", "deny"})
                    if (ImGui::Selectable(p, policy == p)) a.approvals[name] = p;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("auto: at once; ask: wait for a decision in Changes and approvals (command-line runs deny instead); "
                                  "deny: never");
        }

        const bool changed = !(a.model == project->config().agent.model && a.effort == project->config().agent.effort &&
                               a.fallbacks == project->config().agent.fallbacks && a.workers == project->config().agent.workers &&
                               a.max_usd_per_run == project->config().agent.max_usd_per_run &&
                               a.max_turns == project->config().agent.max_turns &&
                               a.max_usd_per_function == project->config().agent.max_usd_per_function &&
                               a.max_tokens_per_function == project->config().agent.max_tokens_per_function &&
                               a.max_minutes_per_function == project->config().agent.max_minutes_per_function &&
                               a.approvals == project->config().agent.approvals);
        ImGui::BeginDisabled(!changed);
        if (ImGui::Button("Save to decomp.json")) {
            project->config().agent = a;
            if (auto r = project->save_config(); !r) ctx.notify(Severity::error, std::format("Cannot save decomp.json: {}", r.error().message));
            else ctx.notify(Severity::info, "Agent settings saved to decomp.json (new runs use them).");
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) draft_ = project->config().agent;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Shared through git with the project.");

        if (ImGui::TreeNode("Prices (USD per million tokens)")) {
            if (ImGui::BeginTable("##prices", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                for (const char* h : {"Model", "Input", "Output", "Cache write", "Cache read"}) ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                for (const auto& [model, p] : agent::default_price_table()) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(model.c_str());
                    for (double v : {p.input, p.output, p.cache_write_5m, p.cache_read}) {
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", v);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
    }

    void draw_appearance(ViewContext& ctx) {
        ImGui::SeparatorText("Appearance");
        Settings& s = ctx.settings;
        const float field = ImGui::GetFontSize() * 12;
        ImGui::SetNextItemWidth(field);
        if (ImGui::BeginCombo("Theme", std::string(theme_label(s.theme)).c_str())) {
            for (Theme t : {Theme::dark, Theme::light, Theme::high_contrast}) {
                if (ImGui::Selectable(std::string(theme_label(t)).c_str(), s.theme == t)) {
                    s.theme = t;
                    ctx.mark_settings_dirty();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(field);
        if (ImGui::SliderFloat("Font size", &s.font_size, Settings::kMinFontSize, Settings::kMaxFontSize, "%.0f px")) {
            s.font_size = std::round(s.font_size);
            ctx.mark_settings_dirty();
        }
        ImGui::SetNextItemWidth(field);
        if (ImGui::BeginCombo("Diff palette", std::string(diff_palette_label(s.diff_palette)).c_str())) {
            for (DiffPaletteKind k : {DiffPaletteKind::standard, DiffPaletteKind::okabe_ito}) {
                if (ImGui::Selectable(std::string(diff_palette_label(k)).c_str(), s.diff_palette == k)) {
                    s.diff_palette = k;
                    ctx.mark_settings_dirty();
                }
            }
            ImGui::EndCombo();
        }
        draw_palette_preview(ctx);
    }

    void draw_projects_and_layouts(ViewContext& ctx) {
        ImGui::SeparatorText("Recent projects and layouts");
        Settings& s = ctx.settings;
        std::optional<usize> remove;
        if (s.recent_projects.empty()) ImGui::TextDisabled("No recent projects.");
        for (usize i = 0; i < s.recent_projects.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Forget")) remove = i;
            ImGui::SameLine();
            ImGui::TextUnformatted(s.recent_projects[i].c_str());
            ImGui::PopID();
        }
        if (remove) {
            s.recent_projects.erase(s.recent_projects.begin() + static_cast<std::ptrdiff_t>(*remove));
            ctx.mark_settings_dirty();
        }
        if (s.layouts.empty()) ImGui::TextDisabled("No saved layouts (View > Layout > Save layout as...).");
        for (const auto& layout : s.layouts) {
            ImGui::PushID(layout.name.c_str());
            if (ImGui::SmallButton("Load")) ctx.actions.run("layout.load." + layout.name);
            ImGui::SameLine();
            ImGui::TextUnformatted(layout.name.c_str());
            ImGui::PopID();
        }
    }

    void draw_developer(ViewContext& ctx) {
        ImGui::SeparatorText("Developer");
        Settings& s = ctx.settings;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 28);
        if (ImGui::InputTextWithHint("Replay directory", "Scripted API responses per function (e.g. tests/replay/run)", &s.developer.replay_dir))
            ctx.mark_settings_dirty();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("When set, new runs answer every request from <dir>/<function>.jsonl or default.jsonl: no key needed.");
        if (s.dir.empty()) ImGui::TextDisabled("Settings are not saved (no configuration directory).");
        else ImGui::TextDisabled("Saved in %s", fs::to_utf8(s.file()).c_str());
    }

    static void draw_palette_preview(ViewContext& ctx) {
        const DiffPalette p = ctx.diff_palette();
        struct Row {
            const char* glyph;
            ImU32 color;
            const char* kind;
            const char* text;
        };
        const Row rows[] = {
            {"=", p.equal, "equal", "push    ebp"},
            {"e", p.encoding, "encoding", "mov     ebp, esp"},
            {"~", p.operand, "operand", "mov     eax, [ebp+8]"},
            {"@", p.symbol, "symbol", "call    ?helper@@YAHH@Z"},
            {"!", p.opcode, "opcode", "add     eax, ecx"},
            {"+", p.insert, "insert", "nop"},
            {"-", p.del, "delete", "pop     ebp"},
        };
        ImGui::PushFont(ctx.fonts.mono, 0.0f);
        for (const Row& r : rows) {
            ImGui::PushStyleColor(ImGuiCol_Text, r.color);
            ImGui::Text("%s  %-24s ; %s", r.glyph, r.text, r.kind);
            ImGui::PopStyleColor();
        }
        ImGui::PopFont();
    }

    JobHandle<KeyCheck> check_;
    std::optional<KeyCheck> checked_;
    std::optional<project::AgentSettings> draft_;
    std::filesystem::path draft_root_;
};

} // namespace

std::unique_ptr<View> make_settings_view() { return std::make_unique<SettingsView>(); }

} // namespace decomp::gui
