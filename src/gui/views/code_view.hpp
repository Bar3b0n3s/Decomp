#pragma once

// Read-only code for the Agent session and the Diff viewer: syntax-highlighted C++ and line diffs in the
// monospace font, drawn line by line and only where visible, so that long sources stay cheap.

#include "gui/view.hpp"
#include "viewmodel/highlight.hpp"
#include "viewmodel/line_diff.hpp"

#include <span>
#include <string_view>

namespace decomp::gui {

// Text colors for code tokens, per theme.
struct CodeColors {
    ImU32 text = 0, keyword = 0, type = 0, number = 0, string = 0, comment = 0, preprocessor = 0, muted = 0;
    ImU32 added = 0, removed = 0;  // line diffs: inserted and deleted lines
};
CodeColors code_colors(const ViewContext& ctx);

// `lines` is vm::highlight_cpp(source). One item of the full height; lines outside the clip rectangle are
// not drawn. Line numbers in a gutter when asked.
void draw_code(ViewContext& ctx, std::string_view source, std::span<const vm::CodeLine> lines, bool line_numbers);

// A line diff as unified lines ("+", "-" and " " markers, colored), at most `context` equal lines around
// each change; "(identical)" when nothing differs.
void draw_line_diff(ViewContext& ctx, const vm::LineDiff& diff, usize context);

} // namespace decomp::gui
