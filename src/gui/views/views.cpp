#include "gui/views/views.hpp"

#include "gui/views/agent_session_view.hpp"
#include "gui/views/binary_explorer_view.hpp"
#include "gui/views/changes_view.hpp"
#include "gui/views/cost_view.hpp"
#include "gui/views/dashboard_view.hpp"
#include "gui/views/diff_viewer_view.hpp"
#include "gui/views/function_browser_view.hpp"
#include "gui/views/inspector_view.hpp"
#include "gui/views/logs_view.hpp"
#include "gui/views/placeholder.hpp"
#include "gui/views/run_monitor_view.hpp"
#include "gui/views/settings_view.hpp"
#include "gui/views/symbols_view.hpp"
#include "gui/views/toolchains_view.hpp"
#include "gui/views/types_view.hpp"
#include "gui/views/units_view.hpp"

namespace decomp::gui {

std::vector<std::unique_ptr<View>> make_all_views() {
    std::vector<std::unique_ptr<View>> views;
    views.push_back(make_dashboard_view());
    views.push_back(make_run_monitor_view());
    views.push_back(make_agent_session_view());
    views.push_back(make_diff_viewer_view());
    views.push_back(make_function_browser_view());
    views.push_back(make_inspector_view());
    views.push_back(make_binary_explorer_view());
    views.push_back(make_symbols_view());
    views.push_back(make_changes_view());
    views.push_back(make_units_view());  // after the views with a Ctrl+digit shortcut, which keep theirs
    views.push_back(make_types_view());
    views.push_back(make_cost_view());
    views.push_back(make_toolchains_view());
    views.push_back(make_logs_view());
    views.push_back(make_settings_view());
    return views;
}

} // namespace decomp::gui
