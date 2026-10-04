#include "gui/layout.hpp"

#include <imgui_internal.h>

namespace decomp::gui::layout {

Slot default_slot(std::string_view view_id) {
    if (view_id == "function_browser") return Slot::left;
    if (view_id == "inspector") return Slot::right;
    if (view_id == "run_monitor" || view_id == "logs") return Slot::bottom;
    return Slot::center;
}

const std::vector<std::string>& default_open_views() {
    static const std::vector<std::string> views = {"dashboard", "agent_session", "diff_viewer", "function_browser",
                                                   "inspector", "run_monitor",   "logs"};
    return views;
}

bool default_selected(std::string_view view_id) {
    return view_id == "dashboard" || view_id == "run_monitor" || view_id == "function_browser" || view_id == "inspector";
}

void build_default(ImGuiID dockspace_id, ImVec2 size, const std::vector<DockedWindow>& windows) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, size);
    ImGuiID center = dockspace_id;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.21f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    for (const DockedWindow& w : windows) {
        ImGuiID node = center;
        switch (w.slot) {
        case Slot::left: node = left; break;
        case Slot::center: node = center; break;
        case Slot::right: node = right; break;
        case Slot::bottom: node = bottom; break;
        }
        ImGui::DockBuilderDockWindow(w.name.c_str(), node);
        // A new tab bar opens on its node's SelectedTabId: the window's "#TAB" id (ImGuiWindow::TabId).
        if (w.selected)
            if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(node)) n->SelectedTabId = ImHashStr("#TAB", 0, ImHashStr(w.name.c_str()));
    }
    ImGui::DockBuilderFinish(dockspace_id);
}

std::string capture() {
    size_t size = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&size);
    return std::string(ini, size);
}

void apply(std::string_view ini) {
    if (ini.empty()) return;  // ImGui reads a size of 0 as "zero-terminated"
    ImGui::LoadIniSettingsFromMemory(ini.data(), ini.size());
}

} // namespace decomp::gui::layout
