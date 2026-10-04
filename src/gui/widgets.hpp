#pragma once

// Small widgets shared by the chrome and the views.

#include <imgui.h>

#include <chrono>
#include <string>
#include <string_view>

namespace decomp::gui {

// A filled circle sized to the current line, as an item (for state lights; always next to text).
void status_dot(const ImVec4& color);
// status_dot() followed by the label: run states and health lights never rely on color alone.
void status_label(std::string_view text, const ImVec4& color);
// Text in the given color, without format-string interpretation.
void colored_text(const ImVec4& color, std::string_view text);
// Local wall-clock time "HH:MM:SS".
std::string local_clock(std::chrono::system_clock::time_point time);
// Local date and time "MM-DD HH:MM" (short labels for runs).
std::string local_month_day_time(std::chrono::system_clock::time_point time);
// How far local time is ahead of UTC at `time` (daylight saving included), for grouping by local day.
std::chrono::minutes local_utc_offset(std::chrono::system_clock::time_point time = std::chrono::system_clock::now());
// Right-aligns the next item of the given width within the current line/region.
void align_right(float item_width);

} // namespace decomp::gui
