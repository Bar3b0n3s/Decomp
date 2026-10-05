#include "gui/views/treemap_widget.hpp"

#include "core/strings.hpp"
#include "gui/views/view_support.hpp"

#include <algorithm>
#include <format>
#include <vector>

namespace decomp::gui {

std::string describe_cell(const vm::TreemapCell& cell, std::span<const vm::FunctionRow> rows) {
    const vm::FunctionRow* row = cell.row < rows.size() ? &rows[cell.row] : nullptr;
    const std::string name = row ? row->display : hex(cell.va, 8);
    std::string text = std::format("{} at {}, {} bytes, {}", name, hex(cell.va, 8), cell.bytes, status_text(cell.status));
    if (cell.best_match > 0) text += std::format(", best {:.1f}%", cell.best_match);
    return text;
}

TreemapEvents TreemapWidget::draw(ViewContext& ctx, const vm::Treemap& map, std::span<const vm::FunctionRow> rows, ImVec2 size,
                                  TreemapColoring coloring, std::optional<u64> highlight) {
    TreemapEvents events;
    size.x = std::max(size.x, 1.0f);
    size.y = std::max(size.y, 1.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##treemap", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg));

    // A layout made for another size (while a resize is going on) is stretched to the box.
    const double sx = map.bounds.w > 0 ? size.x / map.bounds.w : 1.0;
    const double sy = map.bounds.h > 0 ? size.y / map.bounds.h : 1.0;
    auto to_screen = [&](const vm::Rect& r) {
        return std::pair{ImVec2(origin.x + static_cast<float>((r.x - map.bounds.x) * sx), origin.y + static_cast<float>((r.y - map.bounds.y) * sy)),
                         ImVec2(origin.x + static_cast<float>((r.x - map.bounds.x + r.w) * sx),
                                origin.y + static_cast<float>((r.y - map.bounds.y + r.h) * sy))};
    };

    // Without large-mesh support a draw list holds at most 64K vertices: draw the largest cells only.
    const bool vtx_offset = (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset) != 0;
    if (!vtx_offset && (limited_for_ != &map || limited_cells_ != map.cells.size())) {
        limited_for_ = &map;
        limited_cells_ = map.cells.size();
        min_area_ = 0;
        if (map.cells.size() > kMaxCellsWithoutVtxOffset) {
            std::vector<double> areas;
            areas.reserve(map.cells.size());
            for (const auto& c : map.cells) areas.push_back(c.rect.area());
            std::nth_element(areas.begin(), areas.begin() + static_cast<std::ptrdiff_t>(kMaxCellsWithoutVtxOffset), areas.end(), std::greater<>());
            min_area_ = areas[kMaxCellsWithoutVtxOffset];
        }
    }

    const ImU32 group_bg = ImGui::GetColorU32(ImGuiCol_TableHeaderBg);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const float font = ImGui::GetFontSize();
    for (const auto& g : map.groups) {
        const auto [a, b] = to_screen(g.rect);
        dl->AddRectFilled(a, b, group_bg);
        dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border));
        const auto [ia, ib] = to_screen(g.inner);
        if (ia.y - a.y >= font) {
            const std::string label = std::format("{}: {} in {} functions", g.name, bytes_text(g.bytes), g.functions);
            dl->PushClipRect(a, ImVec2(b.x, ia.y), true);
            dl->AddText(ImVec2(a.x + 3, a.y + 1), text, label.c_str());
            dl->PopClipRect();
        }
        (void)ib;
    }

    ImU32 status_colors[8];
    for (usize i = 0; i < 8; ++i) status_colors[i] = ImGui::GetColorU32(status_color(ctx, static_cast<project::FunctionStatus>(i)));
    const ImU32 unworked = ImGui::GetColorU32(status_color(ctx, project::FunctionStatus::unstarted));
    for (const auto& c : map.cells) {
        if (!vtx_offset && c.rect.area() < min_area_) continue;
        auto [a, b] = to_screen(c.rect);
        const float w = b.x - a.x, h = b.y - a.y;
        if (w < 0.35f && h < 0.35f) continue;  // below a pixel: the group's background shows
        ImU32 color = 0;
        if (coloring == TreemapColoring::status) {
            color = status_colors[static_cast<usize>(c.status) & 7];
        } else {
            const bool worked = c.best_match > 0 || c.status == project::FunctionStatus::matched;
            color = worked ? ImGui::GetColorU32(best_match_color(c.status == project::FunctionStatus::matched ? 100.0 : c.best_match)) : unworked;
        }
        // A one-pixel gap separates cells that are large enough to show it.
        if (w >= 3.0f && h >= 3.0f) {
            a.x += 0.5f;
            a.y += 0.5f;
            b.x -= 0.5f;
            b.y -= 0.5f;
        }
        dl->AddRectFilled(a, b, color);
    }

    if (highlight)
        if (auto i = map.find(*highlight)) {
            const auto [a, b] = to_screen(map.cells[*i].rect);
            dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), ImGui::GetColorU32(ImGuiCol_NavCursor), 0.0f, 0, 2.0f);
        }

    if (hovered) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const double x = map.bounds.x + (mouse.x - origin.x) / sx, y = map.bounds.y + (mouse.y - origin.y) / sy;
        if (auto i = map.hit(x, y)) {
            events.hovered = i;
            const auto [a, b] = to_screen(map.cells[*i].rect);
            dl->AddRect(a, b, text, 0.0f, 0, 1.5f);
            ImGui::SetTooltip("%s\nClick to open it in the Inspector.", describe_cell(map.cells[*i], rows).c_str());
            if (clicked) events.clicked = i;
        }
    }
    dl->PopClipRect();
    return events;
}

} // namespace decomp::gui
