#pragma once

// A tab bar whose selected tab is remembered per project (the view state's "tab") and can be chosen by
// setting that key, for example by a test or a navigation anchor. Keep one per view (a member), and call
// begin() each frame right after ImGui::BeginTabBar().

#include "core/json.hpp"
#include "gui/view.hpp"

#include <string>
#include <string_view>

namespace decomp::gui {

class RememberedTabs {
public:
    void begin(ViewContext& ctx, std::string_view view_id, std::string_view fallback) {
        ctx_ = &ctx;
        state_ = &ctx.view_state(view_id);
        wanted_ = json_string_or(*state_, "tab", std::string(fallback));
    }
    // ImGui::BeginTabItem() for the tab `key`; call ImGui::EndTabItem() when it returns true.
    bool item(const char* label, const char* key) {
        const ImGuiTabItemFlags flags = wanted_ != shown_ && wanted_ == key ? ImGuiTabItemFlags_SetSelected : 0;
        if (!ImGui::BeginTabItem(label, nullptr, flags)) return false;
        if (shown_ != key) {
            shown_ = key;
            if (wanted_ != key && state_) {
                (*state_)["tab"] = key;
                ctx_->mark_settings_dirty();
            }
        }
        return true;
    }
    const std::string& shown() const { return shown_; }

private:
    ViewContext* ctx_ = nullptr;
    Json* state_ = nullptr;
    std::string wanted_, shown_;
};

} // namespace decomp::gui
