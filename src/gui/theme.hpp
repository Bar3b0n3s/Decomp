#pragma once

// Themes and palettes (docs/ui.md#accessibility). Color is never the only cue: run states and health
// lights carry text, and diff rows carry glyphs (= e ~ @ ! + -).

#include "gui/settings.hpp"

#include <imgui.h>

namespace decomp::gui {

// Semantic colors shared by the shell and the views, chosen per theme for contrast.
struct ThemeColors {
    ImVec4 text;
    ImVec4 muted;   // secondary text
    ImVec4 accent;  // links, focus
    ImVec4 ok;      // matched, healthy, running
    ImVec4 warn;    // budget at 80%, backoff, paused
    ImVec4 error;   // failures, budget exhausted
    ImVec4 info;
    ImVec4 background;  // behind the dock space (the GL clear color)
};

// Text colors for diff rows by kind (docs/matching.md#5-row-kinds) and for symbol differences.
struct DiffPalette {
    ImU32 equal = 0;
    ImU32 encoding = 0;
    ImU32 operand = 0;
    ImU32 opcode = 0;
    ImU32 insert = 0;
    ImU32 del = 0;  // delete
    ImU32 symbol = 0;
};

const ThemeColors& theme_colors(Theme theme);
DiffPalette diff_palette(DiffPaletteKind kind, Theme theme);

// Builds the complete ImGui style for a theme, sizes scaled by `dpi_scale` (style.FontScaleDpi too).
// style.FontSizeBase is left alone.
void build_style(Theme theme, float dpi_scale, ImGuiStyle& style);
// ImPlot colors for a theme; no-op without a current ImPlot context.
void apply_plot_theme(Theme theme);

} // namespace decomp::gui
