#pragma once

// Monospace text and before/after comparisons for views that show sources and file changes.

#include "gui/view.hpp"

#include <string_view>

namespace decomp::gui {

// Read-only monospace text in a scrolling child of the given height.
void draw_text_block(ViewContext& ctx, const char* id, std::string_view text, float height);
// A line diff of two versions of a text (vm::diff_lines), side by side with line numbers, in a scrolling
// child of the given height. Removed, added and changed lines are tinted and marked with -, + and ~,
// and a toggle hides the unchanged lines away from the changes. The diff is computed once per pair of
// texts and kept while the same id keeps drawing it.
void draw_text_diff(ViewContext& ctx, const char* id, std::string_view before, std::string_view after, float height);

} // namespace decomp::gui
