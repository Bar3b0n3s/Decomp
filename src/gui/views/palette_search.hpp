#pragma once

// The command palette's searches over the open project (docs/ui.md#search-and-command-palette):
// functions and symbols by decorated or demangled name (fuzzy), and strings (prefix "). An index of the
// names and of the image's strings is built per program generation, and every search runs as a
// background job over it (latest query wins), so typing never waits for 100,000 names to be scored.

#include "gui/palette.hpp"

namespace decomp::gui {

// Adds the "symbols" and "strings" providers to the palette.
void add_palette_providers(CommandPalette& palette);

} // namespace decomp::gui
