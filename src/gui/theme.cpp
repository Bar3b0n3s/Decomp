#include "gui/theme.hpp"

#include <implot.h>

namespace decomp::gui {
namespace {

constexpr ImVec4 rgb(unsigned hex, float alpha = 1.0f) {
    return ImVec4(static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(hex & 0xFF) / 255.0f, alpha);
}

constexpr ImU32 rgb32(unsigned hex) { return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, 0xFF); }

const ThemeColors kDark{
    rgb(0xE3E5E8), rgb(0x9AA0A6), rgb(0x5B9BFF), rgb(0x4CC38A), rgb(0xF2B33D), rgb(0xF2555A), rgb(0x6CB6FF), rgb(0x16171A),
};
const ThemeColors kLight{
    rgb(0x1F2328), rgb(0x59636E), rgb(0x1F5FD6), rgb(0x1A7F45), rgb(0x9A5B00), rgb(0xC62828), rgb(0x0B5CAD), rgb(0xE4E6EA),
};
const ThemeColors kHighContrast{
    rgb(0xFFFFFF), rgb(0xD8D8D8), rgb(0xFFD400), rgb(0x3DFF8B), rgb(0xFFD400), rgb(0xFF6E6E), rgb(0x6CD4FF), rgb(0x000000),
};

void common_sizes(ImGuiStyle& s) {
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(6, 4);
    s.CellPadding = ImVec2(6, 3);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 18;
    s.ScrollbarSize = 13;
    s.GrabMinSize = 10;
    s.WindowRounding = 4;
    s.ChildRounding = 4;
    s.FrameRounding = 3;
    s.PopupRounding = 4;
    s.ScrollbarRounding = 6;
    s.GrabRounding = 3;
    s.TabRounding = 3;
    s.WindowBorderSize = 1;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.SeparatorTextBorderSize = 2;
}

void dark_colors(ImGuiStyle& s) {
    ImGui::StyleColorsDark(&s);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = rgb(0xE3E5E8);
    c[ImGuiCol_TextDisabled] = rgb(0x80848C);
    c[ImGuiCol_WindowBg] = rgb(0x1F2024);
    c[ImGuiCol_ChildBg] = rgb(0x000000, 0.0f);
    c[ImGuiCol_PopupBg] = rgb(0x26272C, 0.98f);
    c[ImGuiCol_Border] = rgb(0x3A3C42);
    c[ImGuiCol_BorderShadow] = rgb(0x000000, 0.0f);
    c[ImGuiCol_FrameBg] = rgb(0x2B2D33);
    c[ImGuiCol_FrameBgHovered] = rgb(0x34363D);
    c[ImGuiCol_FrameBgActive] = rgb(0x3C3F47);
    c[ImGuiCol_TitleBg] = rgb(0x18191C);
    c[ImGuiCol_TitleBgActive] = rgb(0x1F2024);
    c[ImGuiCol_TitleBgCollapsed] = rgb(0x18191C);
    c[ImGuiCol_MenuBarBg] = rgb(0x18191C);
    c[ImGuiCol_ScrollbarBg] = rgb(0x1F2024, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = rgb(0x45474E);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x55575F);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(0x65676F);
    c[ImGuiCol_CheckMark] = rgb(0x5B9BFF);
    c[ImGuiCol_SliderGrab] = rgb(0x5B9BFF);
    c[ImGuiCol_SliderGrabActive] = rgb(0x7DB0FF);
    c[ImGuiCol_Button] = rgb(0x2F3238);
    c[ImGuiCol_ButtonHovered] = rgb(0x3A3E46);
    c[ImGuiCol_ButtonActive] = rgb(0x454A54);
    c[ImGuiCol_Header] = rgb(0x2F3238);
    c[ImGuiCol_HeaderHovered] = rgb(0x3A3E46);
    c[ImGuiCol_HeaderActive] = rgb(0x454A54);
    c[ImGuiCol_Separator] = rgb(0x3A3C42);
    c[ImGuiCol_SeparatorHovered] = rgb(0x5B9BFF, 0.7f);
    c[ImGuiCol_SeparatorActive] = rgb(0x5B9BFF);
    c[ImGuiCol_ResizeGrip] = rgb(0x5B9BFF, 0.2f);
    c[ImGuiCol_ResizeGripHovered] = rgb(0x5B9BFF, 0.6f);
    c[ImGuiCol_ResizeGripActive] = rgb(0x5B9BFF, 0.9f);
    c[ImGuiCol_Tab] = rgb(0x24252A);
    c[ImGuiCol_TabHovered] = rgb(0x3A3E46);
    c[ImGuiCol_TabSelected] = rgb(0x2F3238);
    c[ImGuiCol_TabSelectedOverline] = rgb(0x5B9BFF);
    c[ImGuiCol_TabDimmed] = rgb(0x1C1D21);
    c[ImGuiCol_TabDimmedSelected] = rgb(0x26282D);
    c[ImGuiCol_TabDimmedSelectedOverline] = rgb(0x5B9BFF, 0.0f);
    c[ImGuiCol_DockingPreview] = rgb(0x5B9BFF, 0.5f);
    c[ImGuiCol_DockingEmptyBg] = rgb(0x16171A);
    c[ImGuiCol_TableHeaderBg] = rgb(0x26282D);
    c[ImGuiCol_TableBorderStrong] = rgb(0x3A3C42);
    c[ImGuiCol_TableBorderLight] = rgb(0x2E3035);
    c[ImGuiCol_TableRowBgAlt] = rgb(0xFFFFFF, 0.025f);
    c[ImGuiCol_TextLink] = rgb(0x6CB6FF);
    c[ImGuiCol_TextSelectedBg] = rgb(0x5B9BFF, 0.35f);
    c[ImGuiCol_NavCursor] = rgb(0x5B9BFF);
}

void light_colors(ImGuiStyle& s) {
    ImGui::StyleColorsLight(&s);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = rgb(0x1F2328);
    c[ImGuiCol_TextDisabled] = rgb(0x6E7781);
    c[ImGuiCol_WindowBg] = rgb(0xF6F7F9);
    c[ImGuiCol_PopupBg] = rgb(0xFFFFFF, 0.98f);
    c[ImGuiCol_Border] = rgb(0xC9CED6);
    c[ImGuiCol_FrameBg] = rgb(0xFFFFFF);
    c[ImGuiCol_FrameBgHovered] = rgb(0xE8EEF9);
    c[ImGuiCol_FrameBgActive] = rgb(0xD6E2F8);
    c[ImGuiCol_TitleBg] = rgb(0xE4E6EA);
    c[ImGuiCol_TitleBgActive] = rgb(0xEDEFF2);
    c[ImGuiCol_MenuBarBg] = rgb(0xE9EBEF);
    c[ImGuiCol_CheckMark] = rgb(0x1F5FD6);
    c[ImGuiCol_SliderGrab] = rgb(0x1F5FD6);
    c[ImGuiCol_Button] = rgb(0xE4E7EC);
    c[ImGuiCol_ButtonHovered] = rgb(0xD3DCEB);
    c[ImGuiCol_ButtonActive] = rgb(0xBFCDE6);
    c[ImGuiCol_Header] = rgb(0xDCE4F2);
    c[ImGuiCol_HeaderHovered] = rgb(0xCCD8EE);
    c[ImGuiCol_HeaderActive] = rgb(0xB8CAEB);
    c[ImGuiCol_Tab] = rgb(0xE4E6EA);
    c[ImGuiCol_TabHovered] = rgb(0xD3DCEB);
    c[ImGuiCol_TabSelected] = rgb(0xF6F7F9);
    c[ImGuiCol_TabSelectedOverline] = rgb(0x1F5FD6);
    c[ImGuiCol_TabDimmed] = rgb(0xE4E6EA);
    c[ImGuiCol_TabDimmedSelected] = rgb(0xEFF1F4);
    c[ImGuiCol_TabDimmedSelectedOverline] = rgb(0x1F5FD6, 0.0f);
    c[ImGuiCol_DockingPreview] = rgb(0x1F5FD6, 0.4f);
    c[ImGuiCol_DockingEmptyBg] = rgb(0xE4E6EA);
    c[ImGuiCol_TextLink] = rgb(0x0B5CAD);
    c[ImGuiCol_TextSelectedBg] = rgb(0x1F5FD6, 0.25f);
    c[ImGuiCol_NavCursor] = rgb(0x1F5FD6);
}

void high_contrast_colors(ImGuiStyle& s) {
    ImGui::StyleColorsDark(&s);
    ImVec4* c = s.Colors;
    const ImVec4 black = rgb(0x000000), white = rgb(0xFFFFFF), yellow = rgb(0xFFD400);
    c[ImGuiCol_Text] = white;
    c[ImGuiCol_TextDisabled] = rgb(0xBDBDBD);
    c[ImGuiCol_WindowBg] = black;
    c[ImGuiCol_ChildBg] = black;
    c[ImGuiCol_PopupBg] = black;
    c[ImGuiCol_Border] = white;
    c[ImGuiCol_BorderShadow] = rgb(0x000000, 0.0f);
    c[ImGuiCol_FrameBg] = black;
    c[ImGuiCol_FrameBgHovered] = rgb(0x262626);
    c[ImGuiCol_FrameBgActive] = rgb(0x404040);
    c[ImGuiCol_TitleBg] = black;
    c[ImGuiCol_TitleBgActive] = rgb(0x1A1A1A);
    c[ImGuiCol_TitleBgCollapsed] = black;
    c[ImGuiCol_MenuBarBg] = black;
    c[ImGuiCol_ScrollbarBg] = black;
    c[ImGuiCol_ScrollbarGrab] = rgb(0xBDBDBD);
    c[ImGuiCol_ScrollbarGrabHovered] = white;
    c[ImGuiCol_ScrollbarGrabActive] = yellow;
    c[ImGuiCol_CheckMark] = yellow;
    c[ImGuiCol_SliderGrab] = yellow;
    c[ImGuiCol_SliderGrabActive] = white;
    c[ImGuiCol_Button] = black;
    c[ImGuiCol_ButtonHovered] = rgb(0x333333);
    c[ImGuiCol_ButtonActive] = rgb(0x4D4D4D);
    c[ImGuiCol_Header] = rgb(0x1A1A1A);
    c[ImGuiCol_HeaderHovered] = rgb(0x333333);
    c[ImGuiCol_HeaderActive] = rgb(0x4D4D4D);
    c[ImGuiCol_Separator] = white;
    c[ImGuiCol_SeparatorHovered] = yellow;
    c[ImGuiCol_SeparatorActive] = yellow;
    c[ImGuiCol_ResizeGrip] = rgb(0xFFFFFF, 0.5f);
    c[ImGuiCol_ResizeGripHovered] = yellow;
    c[ImGuiCol_ResizeGripActive] = yellow;
    c[ImGuiCol_Tab] = black;
    c[ImGuiCol_TabHovered] = rgb(0x333333);
    c[ImGuiCol_TabSelected] = rgb(0x1A1A1A);
    c[ImGuiCol_TabSelectedOverline] = yellow;
    c[ImGuiCol_TabDimmed] = black;
    c[ImGuiCol_TabDimmedSelected] = rgb(0x1A1A1A);
    c[ImGuiCol_TabDimmedSelectedOverline] = rgb(0xBDBDBD);
    c[ImGuiCol_DockingPreview] = rgb(0xFFD400, 0.5f);
    c[ImGuiCol_DockingEmptyBg] = black;
    c[ImGuiCol_PlotLines] = white;
    c[ImGuiCol_PlotHistogram] = yellow;
    c[ImGuiCol_TableHeaderBg] = rgb(0x1A1A1A);
    c[ImGuiCol_TableBorderStrong] = white;
    c[ImGuiCol_TableBorderLight] = rgb(0xBDBDBD);
    c[ImGuiCol_TableRowBgAlt] = rgb(0xFFFFFF, 0.06f);
    c[ImGuiCol_TextLink] = rgb(0x6CD4FF);
    c[ImGuiCol_TextSelectedBg] = rgb(0x0050FF, 0.85f);
    c[ImGuiCol_NavCursor] = yellow;
    c[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, 0.7f);
}

} // namespace

const ThemeColors& theme_colors(Theme theme) {
    switch (theme) {
    case Theme::dark: return kDark;
    case Theme::light: return kLight;
    case Theme::high_contrast: return kHighContrast;
    }
    return kDark;
}

DiffPalette diff_palette(DiffPaletteKind kind, Theme theme) {
    const bool light = theme == Theme::light;
    const bool hc = theme == Theme::high_contrast;
    switch (kind) {
    case DiffPaletteKind::standard:
        // objdiff-like hues; pairs that matter (insert/delete, operand/opcode) also differ in lightness.
        if (light) return {rgb32(0x2B2F36), rgb32(0x0451A5), rgb32(0x7A5A00), rgb32(0xA31515), rgb32(0x0B7A55), rgb32(0xC4361C), rgb32(0x8A2BC2)};
        return {rgb32(hc ? 0xFFFFFF : 0xCDD1D6), rgb32(0x8CC4FF), rgb32(0xF0D875), rgb32(0xFF8A80), rgb32(0x5FD3A6), rgb32(0xFF5C47), rgb32(0xD7A0FF)};
    case DiffPaletteKind::okabe_ito:
        // Okabe & Ito, "Color Universal Design" (2008): sky blue, yellow, vermillion, bluish green, orange,
        // reddish purple; darker shades of the same hues on light backgrounds.
        if (light) return {rgb32(0x2B2F36), rgb32(0x0072B2), rgb32(0x8A7A00), rgb32(0xD55E00), rgb32(0x00805C), rgb32(0xB07400), rgb32(0xA8507F)};
        return {rgb32(hc ? 0xFFFFFF : 0xCDD1D6), rgb32(0x56B4E9), rgb32(0xF0E442), rgb32(0xE8743B), rgb32(0x1FB38A), rgb32(0xE69F00), rgb32(0xCC79A7)};
    }
    return {};
}

void build_style(Theme theme, float dpi_scale, ImGuiStyle& style) {
    const float font_size_base = style.FontSizeBase;
    style = ImGuiStyle();
    common_sizes(style);
    switch (theme) {
    case Theme::dark: dark_colors(style); break;
    case Theme::light: light_colors(style); break;
    case Theme::high_contrast:
        high_contrast_colors(style);
        style.FrameBorderSize = 1;
        style.TabBorderSize = 1;
        style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding = 0;
        style.GrabRounding = style.TabRounding = style.ScrollbarRounding = 0;
        break;
    }
    if (dpi_scale > 0 && dpi_scale != 1.0f) style.ScaleAllSizes(dpi_scale);
    style.FontScaleDpi = dpi_scale > 0 ? dpi_scale : 1.0f;
    style.FontSizeBase = font_size_base;
}

void apply_plot_theme(Theme theme) {
    if (!ImPlot::GetCurrentContext()) return;
    // Plot colors follow the ImGui style. (ImPlot v1.0 sets line weights and markers per item, through
    // ImPlotSpec, not in ImPlotStyle.)
    ImPlot::StyleColorsAuto();
    ImPlotStyle& style = ImPlot::GetStyle();
    style.PlotBorderSize = theme == Theme::high_contrast ? 2.0f : 1.0f;
    style.UseISO8601 = true;
    style.Use24HourClock = true;
}

} // namespace decomp::gui
