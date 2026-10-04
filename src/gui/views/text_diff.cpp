#include "gui/views/text_diff.hpp"

namespace decomp::gui {

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
    if (ImGui::BeginTable("##sides", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Before");
        ImGui::TableSetupColumn("After");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        draw_text_block(ctx, "##before", before, height);
        ImGui::TableNextColumn();
        draw_text_block(ctx, "##after", after, height);
        ImGui::EndTable();
    }
    ImGui::PopID();
}

} // namespace decomp::gui
