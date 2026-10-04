#include "gui/views/views.hpp"

#include "gui/views/placeholder.hpp"

namespace decomp::gui {

std::vector<std::unique_ptr<View>> make_all_views() {
    std::vector<std::unique_ptr<View>> views;
    views.push_back(make_placeholder_view("dashboard"));         // V1
    views.push_back(make_placeholder_view("run_monitor"));       // V2
    views.push_back(make_placeholder_view("agent_session"));     // V3
    views.push_back(make_placeholder_view("diff_viewer"));       // V4
    views.push_back(make_placeholder_view("function_browser"));  // V1
    views.push_back(make_placeholder_view("inspector"));         // V1
    views.push_back(make_placeholder_view("binary_explorer"));   // V5
    views.push_back(make_placeholder_view("symbols"));           // V5
    views.push_back(make_placeholder_view("changes"));           // V6
    views.push_back(make_placeholder_view("cost"));              // V6
    views.push_back(make_placeholder_view("toolchains"));        // V6
    views.push_back(make_placeholder_view("logs"));              // V6
    views.push_back(make_settings_placeholder_view());           // V6
    return views;
}

} // namespace decomp::gui
