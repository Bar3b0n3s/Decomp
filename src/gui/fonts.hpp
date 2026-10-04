#pragma once

// The embedded fonts (external/fonts, compiled in as compressed arrays under gui/fonts/). ImGui 1.92
// rasterizes glyphs on demand at the sizes in use, so changing the size (style.FontSizeBase) or the DPI
// scale (style.FontScaleDpi) never rebuilds the atlas.

#include <imgui.h>

#include <string_view>

namespace decomp::gui {

struct Fonts {
    ImFont* ui = nullptr;    // the default font
    ImFont* mono = nullptr;  // code, disassembly, hex: ImGui::PushFont(fonts.mono, 0.0f) keeps the size
};

// Adds both fonts to `atlas` (the UI font first, so it becomes the default).
Fonts load_fonts(ImFontAtlas& atlas);

std::string_view ui_font_name();    // "Roboto Medium"
std::string_view mono_font_name();  // "JetBrains Mono 2.304"

} // namespace decomp::gui
