#include "gui/views/text_diff.hpp"

#include "viewmodel/line_diff.hpp"

#include <format>
#include <unordered_map>

namespace decomp::gui {

namespace {

// Unchanged lines kept around a change when only the changes are shown.
constexpr usize kContext = 3;

struct CachedDiff {
    u64 key = 0;
    vm::LineDiff diff;
    std::vector<vm::SideBySideRow> rows;
    std::vector<u32> changed_rows;  // rows within kContext of a change
    int used_frame = 0;
};

u64 text_key(std::string_view before, std::string_view after) {
    u64 h = 1469598103934665603ull;  // FNV-1a over both texts and their lengths
    auto mix = [&](std::string_view s) {
        for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
        h = (h ^ s.size()) * 1099511628211ull;
    };
    mix(before);
    mix(after);
    return h;
}

// Diffs by the ImGui id that draws them (UI thread only); entries unused for a while are dropped.
const CachedDiff& cached_diff(ImGuiID id, std::string_view before, std::string_view after) {
    static std::unordered_map<ImGuiID, CachedDiff> cache;
    const int frame = ImGui::GetFrameCount();
    if (cache.size() > 32)
        std::erase_if(cache, [&](const auto& entry) { return frame - entry.second.used_frame > 600; });
    CachedDiff& c = cache[id];
    const u64 key = text_key(before, after);
    if (c.key != key || c.rows.empty()) {
        c.key = key;
        c.diff = vm::diff_lines(before, after);
        c.rows = vm::side_by_side(c.diff);
        c.changed_rows.clear();
        std::vector<bool> keep(c.rows.size(), false);
        for (usize i = 0; i < c.rows.size(); ++i)
            if (c.rows[i].kind != vm::SideBySideRow::Kind::equal)
                for (usize j = i >= kContext ? i - kContext : 0; j < std::min(c.rows.size(), i + kContext + 1); ++j) keep[j] = true;
        for (usize i = 0; i < keep.size(); ++i)
            if (keep[i]) c.changed_rows.push_back(static_cast<u32>(i));
    }
    c.used_frame = frame;
    return c;
}

ImU32 with_alpha(ImU32 color, float alpha) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(color);
    v.w = alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

} // namespace

void draw_text_block(ViewContext& ctx, const char* id, std::string_view text, float height) {
    if (ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::PushFont(ctx.fonts.mono, 0.0f);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopFont();
    }
    ImGui::EndChild();
}

void draw_text_diff(ViewContext& ctx, const char* id, std::string_view before, std::string_view after, float height) {
    ImGui::PushID(id);
    const CachedDiff& c = cached_diff(ImGui::GetID("##diff"), before, after);
    bool& changes_only = *ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("##changes_only"), false);
    if (c.diff.identical()) ImGui::TextDisabled("No differences.");
    else ImGui::Text("%zu line(s) removed, %zu added", c.diff.deleted, c.diff.inserted);
    ImGui::SameLine();
    ImGui::Checkbox("Changes only", &changes_only);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Hide unchanged lines more than %zu lines away from a change.", kContext);

    const DiffPalette palette = ctx.diff_palette();
    const float body_h = std::max(height - ImGui::GetFrameHeightWithSpacing(), ImGui::GetTextLineHeightWithSpacing() * 3);
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    ImGui::PushFont(ctx.fonts.mono, 0.0f);
    if (ImGui::BeginTable("##lines", 4, flags, ImVec2(0, body_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##old_no");
        ImGui::TableSetupColumn("Before", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##new_no");
        ImGui::TableSetupColumn("After", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        const usize count = changes_only ? c.changed_rows.size() : c.rows.size();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(count));
        while (clipper.Step()) {
            for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
                const usize index = changes_only ? c.changed_rows[static_cast<usize>(n)] : static_cast<usize>(n);
                const vm::SideBySideRow& row = c.rows[index];
                // A gap in the shown rows (changes only) gets a line above the row (rows keep one height, so
                // the clipper stays exact).
                const bool gap = changes_only && n > 0 && c.changed_rows[static_cast<usize>(n) - 1] + 1 != index;
                auto gap_line = [&] {
                    if (!gap) return;
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), ImGui::GetColorU32(ImGuiCol_Separator));
                };
                ImGui::TableNextRow();
                using Kind = vm::SideBySideRow::Kind;
                const bool left_changed = row.kind == Kind::removed || row.kind == Kind::changed;
                const bool right_changed = row.kind == Kind::added || row.kind == Kind::changed;
                const char* glyph = row.kind == Kind::changed ? "~" : row.kind == Kind::removed ? "-" : row.kind == Kind::added ? "+" : " ";
                ImGui::TableSetColumnIndex(0);
                if (left_changed) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, with_alpha(palette.del, 0.18f));
                gap_line();
                if (row.left >= 0) ImGui::TextDisabled("%4d %s", row.left + 1, left_changed ? glyph : " ");
                ImGui::TableSetColumnIndex(1);
                if (left_changed) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, with_alpha(palette.del, 0.18f));
                gap_line();
                if (row.left >= 0) {
                    const std::string& line = c.diff.old_lines[static_cast<usize>(row.left)];
                    if (left_changed) ImGui::PushStyleColor(ImGuiCol_Text, palette.del);
                    ImGui::TextUnformatted(line.data(), line.data() + line.size());
                    if (left_changed) ImGui::PopStyleColor();
                }
                ImGui::TableSetColumnIndex(2);
                if (right_changed) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, with_alpha(palette.insert, 0.18f));
                gap_line();
                if (row.right >= 0) ImGui::TextDisabled("%4d %s", row.right + 1, right_changed ? glyph : " ");
                ImGui::TableSetColumnIndex(3);
                if (right_changed) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, with_alpha(palette.insert, 0.18f));
                gap_line();
                if (row.right >= 0) {
                    const std::string& line = c.diff.new_lines[static_cast<usize>(row.right)];
                    if (right_changed) ImGui::PushStyleColor(ImGuiCol_Text, palette.insert);
                    ImGui::TextUnformatted(line.data(), line.data() + line.size());
                    if (right_changed) ImGui::PopStyleColor();
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
    ImGui::PopID();
}

} // namespace decomp::gui
