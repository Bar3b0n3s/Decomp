#include "gui/views/placeholder.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/run_summary.hpp"
#include "gui/widgets.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace decomp::gui {

const std::vector<PlaceholderSpec>& placeholder_specs() {
    static const std::vector<PlaceholderSpec> specs = {
        {"dashboard", "Dashboard", "V1",
         {"Target identity: path, size, SHA-1 check, format, image base, Rich header, PDB status",
          "Progress by code bytes and by functions, and the status buckets",
          "Progress over time and a treemap of .text colored by status",
          "Spend for this run and all time, and recent activity"}},
        {"run_monitor", "Run monitor", "V2",
         {"One row per worker: function, phase, turn, elapsed time, best match, tokens and dollars",
          "A plain-language live activity feed",
          "The queue with difficulty and ETA (reorder, pin, remove, requeue, skip)",
          "Worker timeline, throughput, rate-limit and retry history"}},
        {"agent_session", "Agent session", "V3",
         {"A per-turn timeline: thinking summaries, text, tool calls and results, usage and latency",
          "Guidance composer with pending guidance and retraction",
          "Score per attempt, source history with the best attempt, notes and outcome"}},
        {"diff_viewer", "Diff viewer", "V4",
         {"Target and candidate assembly side by side, rows colored by kind and marked with glyphs",
          "Branch arrows, offsets, raw bytes, relocations and symbol tooltips",
          "An attempt slider, hints and binding suggestions",
          "Manual mode: edit the source, recompile in the background, verify and save"}},
        {"function_browser", "Function browser", "V1",
         {"Every function: address, name, size, status, best match, attempts, dollars spent",
          "Filters by status, size, name and callees; multi-select to queue a run",
          "Mark functions skip or library, reset a status, edit notes"}},
        {"inspector", "Inspector", "V1",
         {"The selected function: annotated disassembly with block and loop hints",
          "Callers, callees and data references",
          "Attempt history with a score chart, notes and status history"}},
        {"binary_explorer", "Binary explorer", "V5",
         {"Sections, imports and exports, strings with cross-references",
          "A hex view with symbol overlays",
          "Rich header entries and PDB information"}},
        {"symbols", "Symbols and provenance", "V5",
         {"Every symbol: address, kind, names, size, source and status",
          "Who set each symbol and when, with the agent's edits linked to their turns",
          "Rename or edit a symbol; revert agent edits"}},
        {"changes", "Changes and approvals", "V6",
         {"Files written on the agent's behalf, with their diffs and sessions",
          "The queue of gated actions waiting for a decision",
          "The policy for each action type: auto, ask or deny"}},
        {"cost", "Cost and usage", "V6",
         {"Spend by run, by function and by day; tokens by type; cache-hit ratio over time",
          "Turns and dollars per match, success rate by effort and model",
          "A projection for the remaining functions, budgets and alerts"}},
        {"toolchains", "Toolchains and compiles", "V6",
         {"The toolchain registry and health checks",
          "Recent compiles: command line, duration, exit code, output and cache hits"}},
        {"logs", "Logs and errors", "V6",
         {"The structured log with level and module filters and search",
          "Errors grouped by kind, each with its retry history"}},
        {"settings", "Settings", "V6",
         {"API key status (present and valid; the key itself is never shown)",
          "Model, effort, budgets, fallbacks, price table, concurrency and paths",
          "Theme, font size, diff palettes, saved layouts and recent projects"}},
    };
    return specs;
}

namespace {

void draw_header(ViewContext& ctx, const PlaceholderSpec& spec) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
    ImGui::TextUnformatted(spec.title.c_str());
    ImGui::PopFont();
    colored_text(ctx.colors().muted, std::format("Placeholder: this view is coming in step {}.", spec.step));
    ImGui::Spacing();
    ImGui::TextUnformatted("It will show:");
    for (const auto& line : spec.shows) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", line.c_str());
    }
}

void draw_live_facts(ViewContext& ctx) {
    ImGui::Spacing();
    ImGui::SeparatorText("Shell");
    const RunSummary run = summarize(ctx.snapshot.get());
    if (run.phase == RunPhase::none && run.run_id.empty()) {
        ImGui::TextDisabled("No run loaded.");
    } else {
        ImGui::Text("Run %s: %s, %d of %d function(s) finished, %d matched, $%.2f", run.run_id.empty() ? "(unnamed)" : run.run_id.c_str(),
                    label(run).c_str(), run.finished, run.planned, run.matched, run.cost_usd);
    }
    if (ctx.selection.function_va) ImGui::Text("Selected function: %s", hex(*ctx.selection.function_va, 8).c_str());
    if (!ctx.selection.session.empty()) ImGui::Text("Selected session: %s", ctx.selection.session.c_str());
}

class PlaceholderView final : public View {
public:
    explicit PlaceholderView(PlaceholderSpec spec) : spec_(std::move(spec)) {}
    std::string_view id() const override { return spec_.id; }
    std::string_view title() const override { return spec_.title; }
    void draw(ViewContext& ctx) override {
        draw_header(ctx, spec_);
        draw_live_facts(ctx);
    }

private:
    PlaceholderSpec spec_;
};

const PlaceholderSpec& spec_for(std::string_view id) {
    const auto& specs = placeholder_specs();
    auto it = std::ranges::find(specs, id, &PlaceholderSpec::id);
    if (it != specs.end()) return *it;
    static const PlaceholderSpec unknown{"unknown", "Unknown view", "?", {}};
    return unknown;
}

class SettingsPlaceholderView final : public View {
public:
    std::string_view id() const override { return "settings"; }
    std::string_view title() const override { return "Settings"; }

    void draw(ViewContext& ctx) override {
        draw_header(ctx, spec_for("settings"));
        ImGui::Spacing();
        ImGui::SeparatorText("Available now");
        Settings& s = ctx.settings;

        const float field = ImGui::GetFontSize() * 14;
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

        ImGui::Spacing();
        ImGui::SeparatorText("Developer");
        ImGui::SetNextItemWidth(field * 2);
        if (ImGui::InputTextWithHint("Replay directory", "Recorded transcripts to replay instead of calling the API",
                                     &s.developer.replay_dir))
            ctx.mark_settings_dirty();
        if (s.dir.empty()) ImGui::TextDisabled("Settings are not saved (no configuration directory).");
        else ImGui::TextDisabled("Saved in %s", fs_label(s.file()).c_str());
    }

private:
    static std::string fs_label(const std::filesystem::path& p) { return fs::to_utf8(p); }

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
};

} // namespace

std::unique_ptr<View> make_placeholder_view(std::string_view id) { return std::make_unique<PlaceholderView>(spec_for(id)); }

std::unique_ptr<View> make_settings_placeholder_view() { return std::make_unique<SettingsPlaceholderView>(); }

} // namespace decomp::gui
