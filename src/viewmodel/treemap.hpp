#pragma once

// The Dashboard's treemap of the code: one cell per function, sized by bytes and grouped by section,
// laid out with the squarified algorithm (Bruls, Huizing and van Wijk, "Squarified Treemaps", 2000).
// The view colors each cell by its status or best match and hit-tests the mouse with hit().

#include "formats/image.hpp"
#include "project/project.hpp"
#include "viewmodel/function_table.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace decomp::vm {

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;

    double area() const { return w * h; }
    // Half-open on the right and bottom edges, so neighbouring cells never both contain a point.
    bool contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// Lays out `weights` in `bounds`: one rectangle per weight (parallel to `weights`) whose area is
// proportional to it, the rectangles tiling `bounds` exactly. Rows are filled largest weight first
// (ties by index), each row along the shorter side of the space left, as long as that keeps the
// worst aspect ratio from growing. Zero or negative weights get an empty rectangle. Deterministic,
// O(n log n).
std::vector<Rect> squarify(std::span<const double> weights, Rect bounds);

struct TreemapCell {
    u64 va = 0;
    u32 bytes = 0;
    project::FunctionStatus status = project::FunctionStatus::unstarted;  // the row's shown status
    double best_match = 0;  // percent
    u32 group = 0;          // index into Treemap::groups
    u32 row = 0;            // index into the rows the treemap was built from
    Rect rect;
};

struct TreemapGroup {
    std::string name;  // section name, "(none)" for functions outside every section
    u64 bytes = 0;
    usize functions = 0;
    Rect rect;   // the group's area
    Rect inner;  // where its cells are: `rect` minus the padding and the header
    u32 first_cell = 0, cell_count = 0;  // its cells in Treemap::cells
};

struct TreemapOptions {
    double padding = 1;  // space inside each group's border
    double header = 0;   // space above each group's cells (for its label); dropped when it does not fit
};

struct Treemap {
    Rect bounds;
    std::vector<TreemapGroup> groups;  // largest first
    std::vector<TreemapCell> cells;    // grouped (see TreemapGroup::first_cell), largest first in a group

    // The cell under a point, through a grid index: a few rectangle tests per query (0.2 to 0.5
    // microseconds with 100,000 cells in a Release build).
    std::optional<usize> hit(double x, double y) const;
    // The cell of a function (binary search over an address index).
    std::optional<usize> find(u64 va) const;

    // Builds the indexes behind hit() and find(); build_text_treemap() calls it.
    void index();

private:
    usize grid_cols_ = 0, grid_rows_ = 0;
    std::vector<u32> grid_start_;  // per grid cell, offsets into grid_cells_ (CSR)
    std::vector<u32> grid_cells_;
    std::vector<std::pair<u64, u32>> by_va_;
};

// One cell per row with a non-zero size, grouped by the image section holding the function, sized by
// bytes, in `bounds` (any unit, typically pixels). Deterministic for the same rows and bounds. Lays
// out 100,000 functions in 45 to 60 ms in a Release build (measured in
// tests/unit/viewmodel_treemap_tests.cpp): fine for a resize, too slow for every frame of a drag, so
// rebuild it when the drag ends or in a background job. Recompute it when the rows change (statuses
// are copied into the cells).
Treemap build_text_treemap(std::span<const FunctionRow> rows, const BinaryImage& image, Rect bounds, const TreemapOptions& options = {});

} // namespace decomp::vm
