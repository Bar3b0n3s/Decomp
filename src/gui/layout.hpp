#pragma once

// The dock layout (docs/ui.md#default-layout): left Function browser; center Dashboard, Agent session and
// Diff viewer as tabs; right Inspector; bottom Run monitor and Logs & errors. Named layouts are ImGui's
// ini text (SaveIniSettingsToMemory / LoadIniSettingsFromMemory).

#include <imgui.h>

#include <string>
#include <string_view>
#include <vector>

namespace decomp::gui::layout {

enum class Slot { left, center, right, bottom };

// Where a view docks in the default layout (views not named in the spec go to the center).
Slot default_slot(std::string_view view_id);
// The views open in the default layout.
const std::vector<std::string>& default_open_views();
// The tab shown first in its slot (Dashboard in the center, Run monitor at the bottom).
bool default_selected(std::string_view view_id);

struct DockedWindow {
    std::string name;  // the ImGui window name, "Title###id"
    Slot slot = Slot::center;
    bool selected = false;  // the visible tab of its slot
};

// Rebuilds the dock space as the default layout and docks each window into its slot, open or not, so
// that a view opened later appears where it belongs. Call before ImGui::DockSpace() in the frame.
void build_default(ImGuiID dockspace_id, ImVec2 size, const std::vector<DockedWindow>& windows);

// The current layout as ImGui ini text, and the reverse. apply() may be called mid-frame, before the
// dock space and the windows are submitted.
std::string capture();
void apply(std::string_view ini);

} // namespace decomp::gui::layout
