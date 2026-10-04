#pragma once

#include <imgui.h>

#include <memory>
#include <string>
#include <vector>

struct ImGuiContext;

namespace decomp::gui::debug {

struct IdConflict {
    ImGuiID id = 0;
    int frame = 0;
    std::string label;  // the item's label, when its widget reported one
};

// Records item IDs submitted more than once within a frame of one ImGui context. ImGui's own check
// (io.ConfigDebugHighlightIdConflicts) only sees the item under the mouse; this one sees every item that
// goes through ItemAdd(), like IMGUI_DEBUG_HIGHLIGHT_ALL_ID_CONFLICTS, by way of the item hooks that
// IMGUI_ENABLE_TEST_ENGINE enables (gui/imgui_config.h). Items flagged ImGuiItemFlags_AllowDuplicateId
// are exempt. One detector per context at a time; UI thread only.
class IdConflictDetector {
public:
    explicit IdConflictDetector(ImGuiContext* ctx = nullptr);  // nullptr: the current context
    ~IdConflictDetector();
    IdConflictDetector(const IdConflictDetector&) = delete;
    IdConflictDetector& operator=(const IdConflictDetector&) = delete;

    const std::vector<IdConflict>& conflicts() const;
    // One line per conflict ("frame 3: 0x1234abcd \"Start\""), for test failure messages.
    std::string describe() const;
    void clear();

    struct State;

private:
    ImGuiContext* ctx_ = nullptr;
    std::unique_ptr<State> state_;
};

} // namespace decomp::gui::debug
