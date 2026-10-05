#pragma once

// The Function browser's table (docs/ui.md "Function browser and inspector"): virtualized
// (ImGuiListClipper), sortable on several columns (the order itself comes from vm::filter_and_sort(),
// run by the view as a background job), with columns that can be hidden and reordered, and
// multi-selection (Ctrl and Shift clicks, Ctrl+A, box selection). It draws from rows and an order only,
// so it needs no Program and can be driven by synthetic rows (the 100,000-row performance test).

#include "gui/view.hpp"
#include "viewmodel/browser.hpp"
#include "viewmodel/function_table.hpp"

#include <imgui.h>

#include <functional>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace decomp::gui {

class FunctionTable {
public:
    struct Events {
        std::optional<u64> clicked;    // a row was clicked: the current function
        std::optional<u64> activated;  // a row was double-clicked or Enter was pressed on it
        bool sort_changed = false;     // sort_keys() changed: order again
        bool layout_changed = false;   // layout() changed: persist it
        bool selection_changed = false;
    };
    // Called inside a row's context menu (the row is selected first unless it already was).
    using RowMenu = std::function<void(const vm::FunctionRow& row)>;

    // `order` holds indices into `rows`. `size` as for ImGui::BeginTable (0: fill).
    Events draw(ViewContext& ctx, const std::vector<vm::FunctionRow>& rows, std::span<const u32> order, ImVec2 size = ImVec2(0, 0),
                const RowMenu& menu = {});

    // What the table sorts by and shows; the setters apply at the next draw (per-project state).
    const std::vector<vm::SortKey>& sort_keys() const { return sort_; }
    void set_sort_keys(std::vector<vm::SortKey> keys);
    const std::vector<vm::ColumnState>& layout() const { return layout_; }
    void set_layout(std::vector<vm::ColumnState> layout);

    // The selection, by address. It survives reordering and refiltering.
    const std::unordered_set<u64>& selection() const { return selected_; }
    bool is_selected(u64 va) const { return selected_.contains(va); }
    void select_only(u64 va);
    void clear_selection() {
        selected_.clear();
        ++selection_version_;
    }
    // Increases with every change of the selection.
    u64 selection_version() const { return selection_version_; }
    // The selected functions the order shows, in its order.
    std::vector<u64> selected_in_order(const std::vector<vm::FunctionRow>& rows, std::span<const u32> order) const;

    // Brings a function into view at the next draw (when the order shows it).
    void scroll_to(u64 va) { scroll_to_ = va; }
    // Analysis columns read "..." instead of "-" while the analysis runs.
    void set_analysis_pending(bool pending) { analysis_pending_ = pending; }

private:
    void apply_requests(ImGuiMultiSelectIO* io, const std::vector<vm::FunctionRow>& rows, std::span<const u32> order, Events& events);
    void draw_cell(ViewContext& ctx, const vm::FunctionRow& row, vm::Column column);

    std::vector<vm::SortKey> sort_ = {{vm::Column::address, false}};
    std::vector<vm::ColumnState> layout_ = vm::default_column_layout();
    bool apply_sort_ = true, apply_layout_ = true;
    int settle_frames_ = 0;  // frames to wait before reading the layout back after applying one
    std::unordered_set<u64> selected_;
    u64 selection_version_ = 0;
    std::optional<u64> scroll_to_;
    bool analysis_pending_ = false;
};

} // namespace decomp::gui
