#include "gui/notifications.hpp"

#include "core/strings.hpp"
#include "gui/theme.hpp"
#include "gui/widgets.hpp"

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>

namespace decomp::gui {

std::string_view to_string(Severity severity) {
    switch (severity) {
    case Severity::info: return "Info";
    case Severity::warning: return "Warning";
    case Severity::error: return "Error";
    }
    return "Info";
}

namespace {

const ImVec4& severity_color(Severity severity, const ThemeColors& colors) {
    switch (severity) {
    case Severity::info: return colors.info;
    case Severity::warning: return colors.warn;
    case Severity::error: return colors.error;
    }
    return colors.info;
}

} // namespace

std::optional<double> Notifications::lifetime(Severity severity) {
    switch (severity) {
    case Severity::info: return kInfoSeconds;
    case Severity::warning: return kWarningSeconds;
    case Severity::error: return std::nullopt;
    }
    return kInfoSeconds;
}

u64 Notifications::notify(Severity severity, std::string text, std::optional<NavEntry> link, bool toast) {
    u64 id = 0;
    {
        std::lock_guard lock(mutex_);
        id = next_id_++;
        Notification n;
        n.id = id;
        n.severity = severity;
        n.text = std::move(text);
        n.link = std::move(link);
        n.time = std::chrono::system_clock::now();
        n.toast = toast;
        n.dismissed = !toast;
        inbox_.push_back(std::move(n));
    }
    if (wake_) wake_();
    return id;
}

void Notifications::drain() {
    std::vector<Notification> posted;
    {
        std::lock_guard lock(mutex_);
        posted.swap(inbox_);
    }
    for (auto& n : posted) {
        history_.push_back(std::move(n));
        ++unread_;
    }
    while (history_.size() > kHistoryLimit) history_.pop_front();
    unread_ = std::min(unread_, history_.size());
}

void Notifications::dismiss(u64 id) {
    drain();
    for (auto& n : history_)
        if (n.id == id) n.dismissed = true;
}

void Notifications::dismiss_all() {
    drain();
    for (auto& n : history_) n.dismissed = true;
}

void Notifications::clear() {
    drain();
    history_.clear();
    unread_ = 0;
}

const std::deque<Notification>& Notifications::history() {
    drain();
    return history_;
}

usize Notifications::unread() {
    drain();
    return unread_;
}

void Notifications::mark_read() {
    drain();
    unread_ = 0;
}

std::vector<const Notification*> Notifications::visible_toasts(double now) {
    drain();
    std::vector<const Notification*> out;
    for (auto& n : history_) {
        if (n.dismissed) continue;
        if (n.shown_at < 0) n.shown_at = now;
        if (auto life = lifetime(n.severity); life && now - n.shown_at >= *life) {
            n.dismissed = true;
            continue;
        }
        out.push_back(&n);
    }
    // Errors stay; when too many toasts pile up, the oldest non-error ones go first.
    while (out.size() > kMaxToasts) {
        auto it = std::ranges::find_if(out, [](const Notification* n) { return n->severity != Severity::error; });
        if (it == out.end()) it = out.begin();
        out.erase(it);
    }
    return out;
}

std::optional<double> Notifications::next_expiry(double now) {
    drain();
    std::optional<double> best;
    for (const auto& n : history_) {
        if (n.dismissed) continue;
        auto life = lifetime(n.severity);
        if (!life) continue;
        double left = n.shown_at < 0 ? *life : std::max(0.0, n.shown_at + *life - now);
        if (!best || left < *best) best = left;
    }
    return best;
}

void Notifications::draw_toasts(Navigation& nav, const ThemeColors& colors) {
    auto toasts = visible_toasts(ImGui::GetTime());
    if (toasts.empty()) return;

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = style.WindowPadding.x * 1.5f;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - margin, viewport->WorkPos.y + viewport->WorkSize.y - margin),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("##toasts", nullptr, flags)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        const float width = ImGui::GetFontSize() * 24.0f;
        const float pad = style.WindowPadding.x;
        const float accent = std::max(3.0f, ImGui::GetFontSize() * 0.25f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        std::optional<u64> dismissed;
        for (const Notification* n : toasts) {
            ImGui::PushID(static_cast<int>(n->id));
            const ImVec4& color = severity_color(n->severity, colors);
            const ImVec2 start = ImGui::GetCursorScreenPos();
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
            ImGui::SetCursorScreenPos(ImVec2(start.x + accent + pad, start.y + pad));
            ImGui::BeginGroup();
            const float x0 = ImGui::GetCursorPosX();
            const float inner = width - accent - 2 * pad;
            colored_text(color, to_string(n->severity));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", local_clock(n->time).c_str());
            const float close_w = ImGui::CalcTextSize("Dismiss").x + style.FramePadding.x * 2;
            ImGui::SameLine(x0 + inner - close_w);
            if (ImGui::SmallButton("Dismiss")) dismissed = n->id;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
            ImGui::TextUnformatted(n->text.c_str());
            ImGui::PopTextWrapPos();
            if (n->link && ImGui::SmallButton("Open")) {
                nav.open(n->link->view, n->link->target);
                dismissed = n->id;
            }
            ImGui::EndGroup();
            const ImVec2 end(start.x + width, ImGui::GetItemRectMax().y + pad);
            dl->ChannelsSetCurrent(0);
            dl->AddRectFilled(start, end, ImGui::GetColorU32(ImGuiCol_PopupBg), style.PopupRounding);
            dl->AddRect(start, end, ImGui::GetColorU32(ImGuiCol_Border), style.PopupRounding);
            dl->AddRectFilled(start, ImVec2(start.x + accent, end.y), ImGui::GetColorU32(color), style.PopupRounding,
                              ImDrawFlags_RoundCornersLeft);
            dl->ChannelsMerge();
            ImGui::SetCursorScreenPos(ImVec2(start.x, end.y));
            ImGui::Dummy(ImVec2(width, style.ItemSpacing.y));
            ImGui::PopID();
        }
        if (dismissed) dismiss(*dismissed);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void Notifications::draw_history(bool* open, Navigation& nav, const ThemeColors& colors) {
    if (!*open) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 44, ImGui::GetFontSize() * 24), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Notifications###notifications", open)) {
        mark_read();
        ImGui::Checkbox("Info", &show_info_);
        ImGui::SameLine();
        ImGui::Checkbox("Warnings", &show_warnings_);
        ImGui::SameLine();
        ImGui::Checkbox("Errors", &show_errors_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        ImGui::InputTextWithHint("##filter", "Filter", &filter_);
        ImGui::SameLine();
        if (ImGui::Button("Dismiss toasts")) dismiss_all();
        ImGui::SameLine();
        if (ImGui::Button("Clear")) clear();

        std::vector<const Notification*> rows;
        const std::string needle = to_lower(filter_);
        for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
            const Notification& n = *it;
            if ((n.severity == Severity::info && !show_info_) || (n.severity == Severity::warning && !show_warnings_) ||
                (n.severity == Severity::error && !show_errors_))
                continue;
            if (!needle.empty() && to_lower(n.text).find(needle) == std::string::npos) continue;
            rows.push_back(&n);
        }
        const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##history", 4, table_flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Severity");
            ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Source");
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(rows.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const Notification& n = *rows[static_cast<usize>(i)];
                    ImGui::PushID(static_cast<int>(n.id));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(local_clock(n.time).c_str());
                    ImGui::TableNextColumn();
                    colored_text(severity_color(n.severity, colors), to_string(n.severity));
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", n.text.c_str());
                    ImGui::TableNextColumn();
                    if (n.link && ImGui::SmallButton("Open")) nav.open(n.link->view, n.link->target);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

} // namespace decomp::gui
