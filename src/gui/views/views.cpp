#include "gui/views/views.hpp"

#include "gui/views/agent_session_view.hpp"
#include "gui/views/changes_view.hpp"
#include "gui/views/cost_view.hpp"
#include "gui/views/diff_viewer_view.hpp"
#include "gui/views/logs_view.hpp"
#include "gui/views/placeholder.hpp"
#include "gui/views/run_monitor_view.hpp"
#include "gui/views/settings_view.hpp"
#include "gui/views/toolchains_view.hpp"

namespace decomp::gui {

std::vector<std::unique_ptr<View>> make_all_views() {
    std::vector<std::unique_ptr<View>> views;
    views.push_back(make_placeholder_view("dashboard"));         // V1
    views.push_back(make_run_monitor_view());
    views.push_back(make_agent_session_view());
    views.push_back(make_diff_viewer_view());
    views.push_back(make_placeholder_view("function_browser"));  // V1
    views.push_back(make_placeholder_view("inspector"));         // V1
    views.push_back(make_placeholder_view("binary_explorer"));   // V5
    views.push_back(make_placeholder_view("symbols"));           // V5
    views.push_back(make_changes_view());
    views.push_back(make_cost_view());
    views.push_back(make_toolchains_view());
    views.push_back(make_logs_view());
    views.push_back(make_settings_view());
    return views;
}

} // namespace decomp::gui
