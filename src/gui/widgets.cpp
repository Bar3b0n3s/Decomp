#include "gui/widgets.hpp"

#include <imgui_internal.h>

#include <ctime>
#include <format>

namespace decomp::gui {

void status_dot(const ImVec4& color) {
    const float size = ImGui::GetTextLineHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float frame_offset = ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;
    ImGui::Dummy(ImVec2(size * 0.7f, size));
    const ImVec2 center(pos.x + size * 0.35f, pos.y + frame_offset + size * 0.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(center, size * 0.3f, ImGui::GetColorU32(color));
}

void status_label(std::string_view text, const ImVec4& color) {
    status_dot(color);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void colored_text(const ImVec4& color, std::string_view text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
}

std::string local_clock(std::chrono::system_clock::time_point time) {
    std::time_t t = std::chrono::system_clock::to_time_t(time);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return std::format("{:02}:{:02}:{:02}", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

std::string local_month_day_time(std::chrono::system_clock::time_point time) {
    std::time_t t = std::chrono::system_clock::to_time_t(time);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return std::format("{:02}-{:02} {:02}:{:02}", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

std::chrono::minutes local_utc_offset(std::chrono::system_clock::time_point time) {
    const std::time_t t = std::chrono::system_clock::to_time_t(time);
    std::tm local{}, utc{};
#ifdef _WIN32
    localtime_s(&local, &t);
    gmtime_s(&utc, &t);
#else
    localtime_r(&t, &local);
    gmtime_r(&t, &utc);
#endif
    // The two wall clocks of one instant are at most a day apart.
    int days = local.tm_yday - utc.tm_yday;
    if (local.tm_year != utc.tm_year) days = local.tm_year > utc.tm_year ? 1 : -1;
    return std::chrono::minutes(days * 1440 + (local.tm_hour - utc.tm_hour) * 60 + (local.tm_min - utc.tm_min));
}

void align_right(float item_width) {
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float x = right - item_width;
    if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
}

} // namespace decomp::gui
