#pragma once

// The objdiff-style table of a FunctionDiff (docs/ui.md "Diff viewer"): target and candidate side by
// side, each row colored by kind and marked with its glyph, the differing operand boxed with its
// category, branch arrows in both gutters, offsets, optional raw bytes and relocation markers, symbol
// tooltips (mirrored in a details pane), row selection and copying. Only the visible rows are drawn.

#include "gui/view.hpp"
#include "matching/diff.hpp"
#include "viewmodel/diff_view.hpp"

#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace decomp::gui {

struct DiffDrawOptions {
    vm::TextMode mode = vm::TextMode::normalized;
    bool bytes = false;
    bool relocations = false;
};

class DiffTable {
public:
    // Shows `diff` laid out with `layout`; the selection is kept for rows that stay shown.
    void set(std::shared_ptr<const matching::FunctionDiff> diff, const vm::DiffViewOptions& layout);
    void clear();
    const matching::FunctionDiff* diff() const { return diff_.get(); }
    const std::shared_ptr<const matching::FunctionDiff>& shared() const { return diff_; }
    const std::vector<vm::DisplayRow>& rows() const { return rows_; }

    // The table in a child window of `size`; `program` (the target) feeds the symbol tooltips (may be null).
    void draw(ViewContext& ctx, const char* id, ImVec2 size, const DiffDrawOptions& options, const Program* program);
    // The current row in full: both sides, the differing operands and what every operand refers to.
    void draw_details(ViewContext& ctx, const DiffDrawOptions& options, const Program* program);

    // Selects these rows (indices into FunctionDiff::rows) and scrolls the first shown one into view.
    void select(std::span<const usize> rows);
    // Moves to the next (previous) differing row and selects it: F7 and Shift+F7.
    void step(bool forward);
    bool has_selection() const { return !selected_.empty(); }
    // The selected rows (all shown rows when none is selected) as `decomp diff` prints them.
    std::string copy_text(const DiffDrawOptions& options) const;
    // The FunctionDiff row the cursor is on.
    std::optional<usize> current_row() const;

private:
    void click(usize display, bool ctrl, bool shift);
    void draw_side(ViewContext& ctx, const matching::Row& row, vm::DiffSide side, ImVec2 pos, float text_w, float bytes_w,
                   const DiffDrawOptions& options, const Program* program, ImU32 color);
    void draw_arrows(ViewContext& ctx, std::span<const vm::BranchArrow> arrows, float gutter_right, ImVec2 origin, float row_h, float lane_w);

    std::shared_ptr<const matching::FunctionDiff> diff_;
    vm::DiffViewOptions layout_;
    std::vector<vm::DisplayRow> rows_;
    std::vector<vm::BranchArrow> target_arrows_, candidate_arrows_;
    usize bytes_chars_ = 0;     // the longest instruction encoding, as "8b 44 24 08"
    std::set<usize> selected_;  // FunctionDiff rows
    std::optional<usize> cursor_;  // display row
    std::optional<usize> anchor_;  // display row a shift-click extends from
    std::optional<usize> scroll_to_;  // display row to bring into view
};

} // namespace decomp::gui
