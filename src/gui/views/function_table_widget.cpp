#include "gui/views/function_table_widget.hpp"

#include "analysis/difficulty.hpp"
#include "core/strings.hpp"
#include "gui/views/view_support.hpp"
#include "viewmodel/common.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <format>

namespace decomp::gui {

namespace {

// Initial width of a column, in font sizes.
float initial_width(vm::Column column) {
    switch (column) {
    case vm::Column::address: return 6.5f;
    case vm::Column::display:
    case vm::Column::name: return 20.0f;
    case vm::Column::status: return 7.5f;
    case vm::Column::last_attempt: return 8.0f;
    case vm::Column::source: return 5.5f;
    case vm::Column::difficulty: return 6.5f;
    case vm::Column::cost: return 4.5f;
    default: return 4.0f;
    }
}

void right_aligned(const char* text) {
    const float w = ImGui::CalcTextSize(text).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    ImGui::TextUnformatted(text);
}

template <class T>
void optional_number(const std::optional<T>& v, bool pending) {
    if (v) right_aligned(std::to_string(*v).c_str());
    else right_aligned(pending ? "..." : "-");
}

} // namespace

void FunctionTable::set_sort_keys(std::vector<vm::SortKey> keys) {
    if (keys.empty()) keys.push_back({vm::Column::address, false});
    sort_ = std::move(keys);
    apply_sort_ = true;
}

void FunctionTable::set_layout(std::vector<vm::ColumnState> layout) {
    layout_ = std::move(layout);
    apply_layout_ = true;
}

void FunctionTable::select_only(u64 va) {
    selected_.clear();
    selected_.insert(va);
    ++selection_version_;
}

std::vector<u64> FunctionTable::selected_in_order(const std::vector<vm::FunctionRow>& rows, std::span<const u32> order) const {
    std::vector<u64> out;
    if (selected_.empty()) return out;
    for (u32 i : order)
        if (i < rows.size() && selected_.contains(rows[i].va)) out.push_back(rows[i].va);
    return out;
}

void FunctionTable::apply_requests(ImGuiMultiSelectIO* io, const std::vector<vm::FunctionRow>& rows, std::span<const u32> order,
                                   Events& events) {
    for (const ImGuiSelectionRequest& req : io->Requests) {
        if (req.Type == ImGuiSelectionRequestType_SetAll) {
            selected_.clear();
            if (req.Selected) {
                selected_.reserve(order.size());
                for (u32 i : order) selected_.insert(rows[i].va);
            }
            events.selection_changed = true;
            ++selection_version_;
        } else if (req.Type == ImGuiSelectionRequestType_SetRange) {
            const auto first = std::max<ImGuiSelectionUserData>(0, req.RangeFirstItem);
            const auto last = std::min<ImGuiSelectionUserData>(static_cast<ImGuiSelectionUserData>(order.size()) - 1, req.RangeLastItem);
            for (auto n = first; n <= last; ++n) {
                const u64 va = rows[order[static_cast<usize>(n)]].va;
                if (req.Selected) selected_.insert(va);
                else selected_.erase(va);
            }
            events.selection_changed = true;
            ++selection_version_;
        }
    }
}

void FunctionTable::draw_cell(ViewContext& ctx, const vm::FunctionRow& r, vm::Column column) {
    char buf[64];
    switch (column) {
    case vm::Column::address: break;  // the row's selectable
    case vm::Column::display: ImGui::TextUnformatted(r.display.c_str()); break;
    case vm::Column::name: ImGui::TextUnformatted(r.name.c_str()); break;
    case vm::Column::size: right_aligned(std::to_string(r.size).c_str()); break;
    case vm::Column::status:
        status_cell(ctx, r.status);
        if (r.status != r.stored_status && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Recorded as %s; a session is working on it.", std::string(status_text(r.stored_status)).c_str());
        break;
    case vm::Column::best_match:
        if (r.best_match > 0 || r.attempts > 0) {
            *std::format_to_n(buf, sizeof buf - 1, "{:.1f}%", r.best_match).out = '\0';
            right_aligned(buf);
        } else {
            right_aligned("-");
        }
        break;
    case vm::Column::attempts: right_aligned(std::to_string(r.attempts).c_str()); break;
    case vm::Column::cost: right_aligned(r.cost_usd > 0 ? vm::format_usd(r.cost_usd).c_str() : "-"); break;
    case vm::Column::last_attempt: ImGui::TextUnformatted(r.last_attempt ? local_date_time(*r.last_attempt).c_str() : "-"); break;
    case vm::Column::source: ImGui::TextUnformatted(std::string(to_string(r.source)).c_str()); break;
    case vm::Column::callers: optional_number(r.callers, analysis_pending_); break;
    case vm::Column::callees: optional_number(r.callees, analysis_pending_); break;
    case vm::Column::unknown_callees: optional_number(r.unknown_callees, analysis_pending_); break;
    case vm::Column::blocks: optional_number(r.blocks, analysis_pending_); break;
    case vm::Column::loops: optional_number(r.loops, analysis_pending_); break;
    case vm::Column::difficulty:
        if (r.difficulty) {
            *std::format_to_n(buf, sizeof buf - 1, "{:.1f} {}", *r.difficulty, difficulty_label(*r.difficulty)).out = '\0';
            ImGui::TextUnformatted(buf);
        } else {
            ImGui::TextUnformatted(analysis_pending_ ? "..." : "-");
        }
        break;
    }
}

FunctionTable::Events FunctionTable::draw(ViewContext& ctx, const std::vector<vm::FunctionRow>& rows, std::span<const u32> order, ImVec2 size,
                                          const RowMenu& menu) {
    Events events;
    const auto columns = vm::browser_columns();
    const int count = static_cast<int>(columns.size());
    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                                  ImGuiTableFlags_SortMulti | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_NoSavedSettings;
    if (!ImGui::BeginTable("##functions", count, flags, size)) return events;
    const float font = ImGui::GetFontSize();
    ImGui::TableSetupScrollFreeze(1, 1);
    for (const auto& c : columns) {
        ImGuiTableColumnFlags cf = ImGuiTableColumnFlags_WidthFixed;
        if (c.column == vm::Column::address) cf |= ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_DefaultSort;
        if (!c.visible_by_default) cf |= ImGuiTableColumnFlags_DefaultHide;
        if (c.numeric) cf |= ImGuiTableColumnFlags_PreferSortDescending;
        const std::string label(c.label);
        ImGui::TableSetupColumn(label.c_str(), cf, initial_width(c.column) * font, static_cast<ImGuiID>(c.column));
    }
    ImGuiTable* table = ImGui::GetCurrentTable();
    auto index_of = [&](vm::Column column) {
        for (int i = 0; i < count; ++i)
            if (columns[static_cast<usize>(i)].column == column) return i;
        return 0;
    };
    // A table's first frame initializes its columns' order, visibility and sort, so the saved ones are
    // applied from the next frame on.
    if (apply_layout_ && !table->IsInitializing) {
        int position = 0;
        for (const auto& c : layout_) {
            const int i = index_of(c.column);
            ImGui::TableSetColumnDisplayOrder(table, i, position++);
            ImGui::TableSetColumnEnabled(i, c.visible || c.column == vm::Column::address);
        }
        apply_layout_ = false;
        settle_frames_ = 2;  // enabling takes effect on the next frame
    }
    if (table->IsInitializing) settle_frames_ = std::max(settle_frames_, 1);
    if (apply_sort_ && !table->IsInitializing) {
        for (usize k = 0; k < sort_.size(); ++k)
            ImGui::TableSetColumnSortDirection(index_of(sort_[k].column), sort_[k].descending ? ImGuiSortDirection_Descending : ImGuiSortDirection_Ascending,
                                               k > 0);
        apply_sort_ = false;
    }
    ImGui::TableHeadersRow();
    // Until the saved sort is applied, the table's own default is not the user's choice.
    if (ImGuiTableSortSpecs* specs = apply_sort_ ? nullptr : ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty) {
        std::vector<vm::SortKey> keys;
        for (int i = 0; i < specs->SpecsCount; ++i) {
            const ImGuiTableColumnSortSpecs& s = specs->Specs[i];
            keys.push_back({static_cast<vm::Column>(s.ColumnUserID), s.SortDirection == ImGuiSortDirection_Descending});
        }
        specs->SpecsDirty = false;
        if (keys.empty()) keys.push_back({vm::Column::address, false});
        if (keys.size() != sort_.size() ||
            !std::equal(keys.begin(), keys.end(), sort_.begin(), [](const vm::SortKey& a, const vm::SortKey& b) { return a.column == b.column && a.descending == b.descending; })) {
            sort_ = std::move(keys);
            events.sort_changed = true;
        }
    }

    const ImGuiMultiSelectFlags ms_flags = ImGuiMultiSelectFlags_ClearOnEscape | ImGuiMultiSelectFlags_BoxSelect1d | ImGuiMultiSelectFlags_ScopeRect;
    ImGuiMultiSelectIO* io = ImGui::BeginMultiSelect(ms_flags, static_cast<int>(selected_.size()), static_cast<int>(order.size()));
    apply_requests(io, rows, order, events);

    std::optional<int> scroll_index;
    if (scroll_to_) {
        for (usize n = 0; n < order.size(); ++n)
            if (rows[order[n]].va == *scroll_to_) {
                scroll_index = static_cast<int>(n);
                break;
            }
        scroll_to_.reset();
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(order.size()));
    if (io->RangeSrcItem >= 0 && io->RangeSrcItem < static_cast<ImGuiSelectionUserData>(order.size()))
        clipper.IncludeItemByIndex(static_cast<int>(io->RangeSrcItem));
    if (scroll_index) clipper.IncludeItemByIndex(*scroll_index);
    char address[32];
    while (clipper.Step()) {
        for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
            const vm::FunctionRow& r = rows[order[static_cast<usize>(n)]];
            ImGui::PushID(n);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool selected = selected_.contains(r.va);
            *std::format_to_n(address, sizeof address - 1, "{:08x}", r.va).out = '\0';
            ImGui::SetNextItemSelectionUserData(n);
            ImGui::PushFont(ctx.fonts.mono, 0.0f);
            const bool pressed = ImGui::Selectable(address, selected,
                                                   ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick);
            ImGui::PopFont();
            if (pressed) {
                events.clicked = r.va;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) events.activated = r.va;
            }
            if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) events.activated = r.va;
            if (scroll_index && *scroll_index == n) ImGui::SetScrollHereY(0.4f);
            if (menu && ImGui::BeginPopupContextItem("##row")) {
                menu(r);
                ImGui::EndPopup();
            }
            for (int c = 1; c < count; ++c)
                if (ImGui::TableSetColumnIndex(c)) draw_cell(ctx, r, columns[static_cast<usize>(c)].column);
            ImGui::PopID();
        }
    }
    io = ImGui::EndMultiSelect();
    apply_requests(io, rows, order, events);

    // The layout the user sees (dragged or hidden columns), saved per project by the view.
    if (settle_frames_ > 0) {
        --settle_frames_;
    } else {
        std::vector<vm::ColumnState> now(static_cast<usize>(count));
        for (int i = 0; i < count; ++i) {
            const int position = table->Columns[i].DisplayOrder;
            if (position < 0 || position >= count) continue;
            now[static_cast<usize>(position)] = {columns[static_cast<usize>(i)].column,
                                                 (ImGui::TableGetColumnFlags(i) & ImGuiTableColumnFlags_IsEnabled) != 0};
        }
        const bool same = now.size() == layout_.size() && std::equal(now.begin(), now.end(), layout_.begin(), [](const auto& a, const auto& b) {
                              return a.column == b.column && a.visible == b.visible;
                          });
        if (!same) {
            layout_ = std::move(now);
            events.layout_changed = true;
        }
    }
    ImGui::EndTable();
    return events;
}

} // namespace decomp::gui
