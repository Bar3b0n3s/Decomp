#include "viewmodel/treemap.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace decomp::vm {

namespace {

// The worst aspect ratio in a row of total area `sum` whose smallest and largest items are `lo` and
// `hi`, laid along a side of length `side` (Bruls et al.: max(w^2 r+ / s^2, s^2 / (w^2 r-))).
double worst_ratio(double sum, double lo, double hi, double side) {
    const double s2 = sum * sum, w2 = side * side;
    return std::max(w2 * hi / s2, s2 / (w2 * lo));
}

} // namespace

std::vector<Rect> squarify(std::span<const double> weights, Rect bounds) {
    std::vector<Rect> out(weights.size(), Rect{bounds.x, bounds.y, 0, 0});
    std::vector<u32> order;
    double total = 0;
    for (usize i = 0; i < weights.size(); ++i)
        if (weights[i] > 0) {
            order.push_back(static_cast<u32>(i));
            total += weights[i];
        }
    if (order.empty() || bounds.w <= 0 || bounds.h <= 0) return out;
    std::ranges::stable_sort(order, [&](u32 a, u32 b) { return weights[a] > weights[b]; });
    const double scale = bounds.area() / total;  // area per unit of weight

    Rect free = bounds;
    usize i = 0;
    while (i < order.size()) {
        const bool columns = free.w >= free.h;  // the row runs along the shorter side
        const double side = std::max(columns ? free.h : free.w, std::numeric_limits<double>::min());
        usize j = i;
        double sum = 0, lo = std::numeric_limits<double>::max(), hi = 0, worst = std::numeric_limits<double>::max();
        while (j < order.size()) {
            const double a = weights[order[j]] * scale;
            const double next = worst_ratio(sum + a, std::min(lo, a), std::max(hi, a), side);
            if (j > i && next > worst) break;
            sum += a;
            lo = std::min(lo, a);
            hi = std::max(hi, a);
            worst = next;
            ++j;
        }
        // The last row takes all the space left, which absorbs rounding.
        const double room = columns ? free.w : free.h;
        const double thickness = j == order.size() ? room : std::min(room, sum / side);
        double pos = columns ? free.y : free.x;
        const double end = columns ? free.y + free.h : free.x + free.w;
        for (usize k = i; k < j; ++k) {
            const double length = k + 1 == j ? end - pos : weights[order[k]] * scale / thickness;
            out[order[k]] = columns ? Rect{free.x, pos, thickness, length} : Rect{pos, free.y, length, thickness};
            pos += length;
        }
        if (columns) {
            free.x += thickness;
            free.w = std::max(0.0, free.w - thickness);
        } else {
            free.y += thickness;
            free.h = std::max(0.0, free.h - thickness);
        }
        i = j;
    }
    return out;
}

void Treemap::index() {
    by_va_.clear();
    by_va_.reserve(cells.size());
    for (usize i = 0; i < cells.size(); ++i) by_va_.emplace_back(cells[i].va, static_cast<u32>(i));
    std::ranges::sort(by_va_);

    grid_cols_ = grid_rows_ = std::clamp<usize>(static_cast<usize>(std::ceil(std::sqrt(static_cast<double>(cells.size()) / 2.0))), 1, 512);
    grid_start_.assign(grid_cols_ * grid_rows_ + 1, 0);
    grid_cells_.clear();
    if (bounds.w <= 0 || bounds.h <= 0) return;
    const double cw = bounds.w / static_cast<double>(grid_cols_), ch = bounds.h / static_cast<double>(grid_rows_);
    auto span_of = [&](double from, double to, double unit, double origin, usize count) {
        const auto first = static_cast<long long>(std::floor((from - origin) / unit));
        const auto last = static_cast<long long>(std::ceil((to - origin) / unit)) - 1;
        const auto clamp = [&](long long v) { return static_cast<usize>(std::clamp<long long>(v, 0, static_cast<long long>(count) - 1)); };
        return std::pair{clamp(first), clamp(std::max(first, last))};
    };
    // Two passes (count, then fill) give a compact list of the cells overlapping each grid cell.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<u32> fill;
        if (pass == 1) {
            std::partial_sum(grid_start_.begin(), grid_start_.end(), grid_start_.begin());
            grid_cells_.resize(grid_start_.back());
            fill.assign(grid_start_.begin(), grid_start_.end() - 1);
        }
        for (usize c = 0; c < cells.size(); ++c) {
            const Rect& r = cells[c].rect;
            if (r.w <= 0 || r.h <= 0) continue;
            const auto [x0, x1] = span_of(r.x, r.x + r.w, cw, bounds.x, grid_cols_);
            const auto [y0, y1] = span_of(r.y, r.y + r.h, ch, bounds.y, grid_rows_);
            for (usize gy = y0; gy <= y1; ++gy)
                for (usize gx = x0; gx <= x1; ++gx) {
                    const usize g = gy * grid_cols_ + gx;
                    if (pass == 0) ++grid_start_[g + 1];
                    else grid_cells_[fill[g]++] = static_cast<u32>(c);
                }
        }
    }
}

std::optional<usize> Treemap::hit(double x, double y) const {
    if (grid_cols_ == 0 || !bounds.contains(x, y)) return std::nullopt;
    const auto gx = std::min(grid_cols_ - 1, static_cast<usize>((x - bounds.x) / bounds.w * static_cast<double>(grid_cols_)));
    const auto gy = std::min(grid_rows_ - 1, static_cast<usize>((y - bounds.y) / bounds.h * static_cast<double>(grid_rows_)));
    const usize g = gy * grid_cols_ + gx;
    for (u32 k = grid_start_[g]; k < grid_start_[g + 1]; ++k)
        if (cells[grid_cells_[k]].rect.contains(x, y)) return grid_cells_[k];
    return std::nullopt;
}

std::optional<usize> Treemap::find(u64 va) const {
    auto it = std::ranges::lower_bound(by_va_, std::pair<u64, u32>{va, 0});
    if (it == by_va_.end() || it->first != va) return std::nullopt;
    return it->second;
}

Treemap build_text_treemap(std::span<const FunctionRow> rows, const BinaryImage& image, Rect bounds, const TreemapOptions& options) {
    Treemap t;
    t.bounds = bounds;
    const auto& sections = image.image_sections();
    // Members of each section (the last slot collects functions outside every section).
    std::vector<std::vector<u32>> members(sections.size() + 1);
    for (usize i = 0; i < rows.size(); ++i) {
        if (rows[i].size == 0) continue;
        const ImageSection* s = image.section_at(rows[i].va);
        members[s ? static_cast<usize>(s - sections.data()) : sections.size()].push_back(static_cast<u32>(i));
    }
    std::vector<u32> group_slots;
    std::vector<double> group_weights;
    for (usize g = 0; g < members.size(); ++g) {
        if (members[g].empty()) continue;
        double bytes = 0;
        for (u32 r : members[g]) bytes += rows[r].size;
        group_slots.push_back(static_cast<u32>(g));
        group_weights.push_back(bytes);
    }
    const std::vector<Rect> group_rects = squarify(group_weights, bounds);
    std::vector<usize> group_order(group_slots.size());
    std::iota(group_order.begin(), group_order.end(), usize{0});
    std::ranges::stable_sort(group_order, [&](usize a, usize b) { return group_weights[a] > group_weights[b]; });

    for (usize gi : group_order) {
        const usize slot = group_slots[gi];
        TreemapGroup group;
        group.name = slot < sections.size() ? sections[slot].name : std::string("(none)");
        group.bytes = static_cast<u64>(group_weights[gi]);
        group.functions = members[slot].size();
        group.rect = group_rects[gi];
        const double p = options.padding;
        group.inner = Rect{group.rect.x + p, group.rect.y + p, std::max(0.0, group.rect.w - 2 * p), std::max(0.0, group.rect.h - 2 * p)};
        if (options.header > 0 && group.inner.h > 2 * options.header) {
            group.inner.y += options.header;
            group.inner.h -= options.header;
        }
        std::vector<double> weights;
        weights.reserve(members[slot].size());
        for (u32 r : members[slot]) weights.push_back(rows[r].size);
        const std::vector<Rect> rects = squarify(weights, group.inner);
        // Cells in layout order: largest first, then by address (rows are in address order).
        std::vector<usize> order(weights.size());
        std::iota(order.begin(), order.end(), usize{0});
        std::ranges::stable_sort(order, [&](usize a, usize b) { return weights[a] > weights[b]; });
        group.first_cell = static_cast<u32>(t.cells.size());
        group.cell_count = static_cast<u32>(order.size());
        for (usize k : order) {
            const FunctionRow& row = rows[members[slot][k]];
            t.cells.push_back(TreemapCell{row.va, row.size, row.status, row.best_match, static_cast<u32>(t.groups.size()), members[slot][k], rects[k]});
        }
        t.groups.push_back(std::move(group));
    }
    t.index();
    return t;
}

} // namespace decomp::vm
