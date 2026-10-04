#pragma once

#include "gui/view.hpp"

#include <memory>

namespace decomp::gui {

std::unique_ptr<View> make_settings_view();

} // namespace decomp::gui
