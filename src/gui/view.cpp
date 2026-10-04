#include "gui/view.hpp"

namespace decomp::gui {

Json& ViewContext::view_state(std::string_view view_id) {
    Json& views = settings.project_state(project.root).views;
    if (!views.is_object()) views = Json::object();
    Json& state = views[std::string(view_id)];
    if (!state.is_object()) state = Json::object();
    return state;
}

} // namespace decomp::gui
