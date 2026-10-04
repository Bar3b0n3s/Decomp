#pragma once

#include "gui/view.hpp"

#include <memory>
#include <vector>

namespace decomp::gui {

// Every view, in View-menu order (Ctrl+1 ... Ctrl+9 focus the first nine). The list in views.cpp is
// explicit, one line per view: a view step replaces its placeholder line with its own factory.
std::vector<std::unique_ptr<View>> make_all_views();

} // namespace decomp::gui
