#include "gui/fonts.hpp"

#include "gui/settings.hpp"

#include <cstdio>

namespace decomp::gui {
namespace {

// Generated with Dear ImGui's misc/fonts/binary_to_compressed_c.cpp -u32 (stb_compress, 32-bit words:
// no string literals, so MSVC's literal limits do not apply).
#include "gui/fonts/jetbrains_mono_regular.inc"
#include "gui/fonts/roboto_medium.inc"

ImFont* add(ImFontAtlas& atlas, const unsigned int* data, unsigned int size, const char* name, bool pixel_snap) {
    ImFontConfig cfg;
    std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", name);
    cfg.PixelSnapH = pixel_snap;  // whole-pixel advances keep monospace columns aligned
    return atlas.AddFontFromMemoryCompressedTTF(data, static_cast<int>(size), Settings::kDefaultFontSize, &cfg);
}

} // namespace

Fonts load_fonts(ImFontAtlas& atlas) {
    Fonts fonts;
    fonts.ui = add(atlas, roboto_medium_compressed_data, roboto_medium_compressed_size, "Roboto Medium", false);
    fonts.mono = add(atlas, jetbrains_mono_regular_compressed_data, jetbrains_mono_regular_compressed_size, "JetBrains Mono", true);
    return fonts;
}

std::string_view ui_font_name() { return "Roboto Medium"; }
std::string_view mono_font_name() { return "JetBrains Mono 2.304"; }

} // namespace decomp::gui
