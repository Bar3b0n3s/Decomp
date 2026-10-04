#pragma once

#include "gui/view.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::gui {

// What a placeholder shows: the view's title, the step that implements it, and what the view will show
// (from docs/ui.md#views).
struct PlaceholderSpec {
    std::string id;
    std::string title;
    std::string step;  // "V1"
    std::vector<std::string> shows;
};

// The specs of every view that is still a placeholder, in View-menu order.
const std::vector<PlaceholderSpec>& placeholder_specs();

// Stands in for the view with this id (one of placeholder_specs()); unknown ids get a generic
// placeholder.
std::unique_ptr<View> make_placeholder_view(std::string_view id);

// The Settings placeholder also edits what the shell already supports (theme, font size, diff palette,
// developer options) until the Settings view (V6) replaces it.
std::unique_ptr<View> make_settings_placeholder_view();

} // namespace decomp::gui
