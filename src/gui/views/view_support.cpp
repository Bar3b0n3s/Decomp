#include "gui/views/view_support.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/widgets.hpp"
#include "gui/workspace.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <format>

namespace decomp::gui {

using project::FunctionStatus;

namespace {

constexpr ImVec4 rgb(unsigned hex) {
    return ImVec4(static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(hex & 0xFF) / 255.0f, 1.0f);
}

// Indexed by FunctionStatus: unstarted, in_progress, nonmatching, matched, refused, gave_up, skipped, library.
constexpr std::array<unsigned, 8> kDarkStatus = {0x4A4E57, 0x6CB6FF, 0xF2B33D, 0x4CC38A, 0xF2555A, 0xC678DD, 0x8A8F98, 0x5F87AD};
constexpr std::array<unsigned, 8> kLightStatus = {0xC9CED6, 0x1F6FD6, 0xB7791F, 0x1A7F45, 0xC62828, 0x8E44AD, 0x8C959F, 0x4F6D8A};
constexpr std::array<unsigned, 8> kContrastStatus = {0x5A5A5A, 0x6CD4FF, 0xFFD400, 0x3DFF8B, 0xFF6E6E, 0xFF8CFF, 0xBDBDBD, 0x7FB2FF};

} // namespace

std::string_view status_text(FunctionStatus status) {
    switch (status) {
    case FunctionStatus::unstarted: return "unstarted";
    case FunctionStatus::in_progress: return "in progress";
    case FunctionStatus::nonmatching: return "non-matching";
    case FunctionStatus::matched: return "matched";
    case FunctionStatus::refused: return "refused";
    case FunctionStatus::gave_up: return "gave up";
    case FunctionStatus::skipped: return "skipped";
    case FunctionStatus::library: return "library";
    }
    return "unstarted";
}

ImVec4 status_color(const ViewContext& ctx, FunctionStatus status) {
    const auto i = static_cast<usize>(status);
    switch (ctx.settings.theme) {
    case Theme::light: return rgb(kLightStatus[i]);
    case Theme::high_contrast: return rgb(kContrastStatus[i]);
    case Theme::dark: break;
    }
    return rgb(kDarkStatus[i]);
}

ImVec4 best_match_color(double percent) {
    // Viridis at 0, 25, 50, 75 and 100 percent, interpolated.
    static constexpr std::array<unsigned, 5> stops = {0x440154, 0x3B528B, 0x21918C, 0x5EC962, 0xFDE725};
    const double t = std::clamp(percent, 0.0, 100.0) / 25.0;
    const usize i = std::min<usize>(static_cast<usize>(t), 3);
    const float f = static_cast<float>(t - static_cast<double>(i));
    const ImVec4 a = rgb(stops[i]), b = rgb(stops[i + 1]);
    return ImVec4(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f, 1.0f);
}

void status_cell(const ViewContext& ctx, FunctionStatus status) { status_label(status_text(status), status_color(ctx, status)); }

std::string bytes_text(u64 bytes) {
    if (bytes < 1024) return std::format("{} B", bytes);
    if (bytes < 1024 * 1024) return std::format("{:.1f} KiB", static_cast<double>(bytes) / 1024.0);
    return std::format("{:.1f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

std::string local_date_time(std::chrono::system_clock::time_point time) {
    if (time == std::chrono::system_clock::time_point{}) return "-";
    std::time_t t = std::chrono::system_clock::to_time_t(time);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

void function_open_items(ViewContext& ctx, u64 va, std::string_view session) {
    if (ImGui::MenuItem("Open in Inspector")) ctx.open("inspector", {.va = va});
    if (ImGui::MenuItem("Show in Function browser")) ctx.open("function_browser", {.va = va});
    if (ImGui::MenuItem("Open in Diff viewer")) ctx.open("diff_viewer", {.va = va});
    if (ImGui::MenuItem("Open in Agent session")) ctx.open("agent_session", {.va = va, .session = std::string(session)});
    if (ImGui::MenuItem("Show in Binary explorer")) ctx.open("binary_explorer", {.va = va, .anchor = hex(va)});
    if (ImGui::MenuItem("Show in Symbols")) ctx.open("symbols", {.va = va});
    ImGui::Separator();
    if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(hex(va, 8).c_str());
}

bool function_link(ViewContext& ctx, std::string_view label, u64 va, std::string_view session) {
    const bool clicked = ImGui::TextLink(std::string(label).c_str());
    if (clicked) ctx.open("inspector", {.va = va});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s\nOpen in the Inspector (right-click for more)", hex(va, 8).c_str());
    if (ImGui::BeginPopupContextItem()) {
        function_open_items(ctx, va, session);
        ImGui::EndPopup();
    }
    return clicked;
}

bool address_link(ViewContext& ctx, std::string_view label, u64 va, bool is_function) {
    if (is_function) return function_link(ctx, label, va);
    const bool clicked = ImGui::TextLink(std::string(label).c_str());
    if (clicked) ctx.open("binary_explorer", {.anchor = hex(va)});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s\nShow in the Binary explorer", hex(va, 8).c_str());
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Show in Binary explorer")) ctx.open("binary_explorer", {.anchor = hex(va)});
        if (ImGui::MenuItem("Show in Symbols")) ctx.open("symbols", {.anchor = hex(va)});
        if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(hex(va, 8).c_str());
        ImGui::EndPopup();
    }
    return clicked;
}

ProjectAccess project_access(ViewContext& ctx) {
    ProjectAccess a;
    a.workspace = ctx.services.workspace;
    if (a.workspace && a.workspace->project_state().phase == ProjectPhase::open) {
        a.project = a.workspace->project();
        a.program = a.workspace->program();
    }
    return a;
}

bool require_project(ViewContext& ctx, const ProjectAccess& access, std::string_view what) {
    if (access.ready()) return true;
    const ProjectPhase phase = access.workspace ? access.workspace->project_state().phase : ProjectPhase::none;
    switch (phase) {
    case ProjectPhase::loading: ImGui::TextDisabled("Loading the project..."); break;
    case ProjectPhase::failed:
        colored_text(ctx.colors().error, "The project could not be opened: " + access.workspace->project_state().error);
        break;
    case ProjectPhase::none:
    case ProjectPhase::open:
        ImGui::TextDisabled("No project is open. Open one with File > Open project...");
        break;
    }
    ImGui::TextDisabled("%.*s", static_cast<int>(what.size()), what.data());
    return false;
}

const events::RunStateData* live_run(ViewContext& ctx) { return ctx.services.commands->live() ? ctx.snapshot.get() : nullptr; }

ProjectInputs project_inputs(const ProjectAccess& access) {
    ProjectInputs k;
    if (access.project) {
        k.root = fs::to_utf8(access.project->root());
        k.version = access.project->version();
    }
    k.program = access.program;
    return k;
}

void busy_marker(const ViewContext& ctx, bool busy, std::string_view text) {
    if (!busy) return;
    ImGui::SameLine();
    colored_text(ctx.colors().muted, text);
}

void same_line_or_wrap(float item_width) {
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < item_width) ImGui::NewLine();
}

float button_width(std::string_view label) {
    const std::string text(label);
    return ImGui::CalcTextSize(text.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
}

float checkbox_width(std::string_view label) {
    const std::string text(label);
    return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(text.c_str(), nullptr, true).x;
}

bool multiline_text(const char* id, std::string* text, ImVec2 size) {
    ImGui::PushItemFlag(ImGuiItemFlags_AllowDuplicateId, true);
    const bool changed = ImGui::InputTextMultiline(id, text, size);
    ImGui::PopItemFlag();
    return changed;
}

std::chrono::milliseconds refresh_interval(ViewContext& ctx, std::chrono::milliseconds live) {
    return ctx.services.commands->live() ? live : std::chrono::milliseconds(0);
}

} // namespace decomp::gui
