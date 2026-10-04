#pragma once

// Monospace text and before/after comparisons for views that show sources and file changes.

#include "gui/view.hpp"

#include <string_view>

namespace decomp::gui {

// Read-only monospace text in a scrolling child of the given height.
void draw_text_block(ViewContext& ctx, const char* id, std::string_view text, float height);
// Two versions of a text side by side (before | after) in a scrolling child of the given height.
void draw_text_diff(ViewContext& ctx, const char* id, std::string_view before, std::string_view after, float height);

} // namespace decomp::gui
