#pragma once

// The Dashboard's treemap of the code (docs/ui.md "Dashboard"): one cell per function, laid out by
// vm::build_text_treemap(), drawn with ImDrawList and colored by status or best match. Hovering shows
// the function; a click opens it in the Inspector. It draws from a layout and its rows only, so tests
// can drive it with synthetic data.

#include "gui/view.hpp"
#include "viewmodel/function_table.hpp"
#include "viewmodel/treemap.hpp"

#include <imgui.h>

#include <optional>
#include <span>

namespace decomp::gui {

enum class TreemapColoring : u8 { status, best_match };

struct TreemapEvents {
    std::optional<usize> hovered;  // index into Treemap::cells
    std::optional<usize> clicked;  // left click on a cell
};

class TreemapWidget {
public:
    // Draws `map` at the cursor in a box of `size` pixels (a layout made for another size is scaled to
    // it). `rows` are the rows the layout was built from (for names). `highlight` outlines a function.
    TreemapEvents draw(ViewContext& ctx, const vm::Treemap& map, std::span<const vm::FunctionRow> rows, ImVec2 size, TreemapColoring coloring,
                       std::optional<u64> highlight);

    // Without the renderer's large-mesh support (ImGuiBackendFlags_RendererHasVtxOffset) at most this
    // many cells are drawn, largest first; the others show their group's background.
    static constexpr usize kMaxCellsWithoutVtxOffset = 12'000;

private:
    const vm::Treemap* limited_for_ = nullptr;
    usize limited_cells_ = 0;
    double min_area_ = 0;  // cells below this area are skipped (large-mesh fallback)
};

// One line describing a cell (the details pane mirrors the tooltip): "name, 120 bytes, matched, best 100%".
std::string describe_cell(const vm::TreemapCell& cell, std::span<const vm::FunctionRow> rows);

} // namespace decomp::gui
