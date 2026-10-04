// Toolchains and compiles (docs/ui.md#toolchains-and-compiles): the registry with health checks and an
// editor for the user's toolchains, and the run's recent compiles.

#include "gui/views/toolchains_view.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"
#include "gui/widgets.hpp"
#include "matching/health.hpp"
#include "matching/toolchain.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <format>

namespace decomp::gui {

namespace {

std::string join_words(const std::vector<std::string>& words) { return join(words, " "); }

std::vector<std::string> split_words(std::string_view text) {
    std::vector<std::string> out;
    for (auto word : split(text, ' '))
        if (!trim(word).empty()) out.emplace_back(trim(word));
    return out;
}

struct Health {
    std::string toolchain;
    bool ok = false;
    std::string summary;  // or the error
    std::string command, output;
};

class ToolchainsView final : public View {
public:
    std::string_view id() const override { return "toolchains"; }
    std::string_view title() const override { return "Toolchains and compiles"; }

    void draw(ViewContext& ctx) override {
        if (!loaded_ && !load_.valid())
            load_ = ctx.jobs.submit([] { return matching::ToolchainRegistry::load(); });
        if (auto r = load_.take()) {
            loaded_ = true;
            if (*r) registry_ = std::move(**r);
            else load_error_ = r->error().message;
        }
        if (auto r = health_.take()) health_result_ = std::move(*r);

        if (ImGui::BeginTabBar("##toolchain_tabs")) {
            if (ImGui::BeginTabItem("Recent compiles")) {
                draw_compiles(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Registry")) {
                draw_registry(ctx);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

private:
    void draw_registry(ViewContext& ctx) {
        if (!loaded_) {
            ImGui::TextDisabled("Loading the registry...");
            return;
        }
        if (!registry_) {
            colored_text(ctx.colors().error, "Cannot read the toolchain registry: " + load_error_);
            return;
        }
        ImGui::TextDisabled("User registry: %s", fs::to_utf8(registry_->path()).c_str());
        if (ImGui::SmallButton("Reload")) {
            loaded_ = false;
            registry_.reset();
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##registry", 5, flags)) {
            for (const char* h : {"Name", "Kind", "Compiler", "Wrapper", "Origin"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (const auto& t : registry_->toolchains()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(t.name.c_str(), selected_ == t.name, ImGuiSelectableFlags_SpanAllColumns)) {
                    selected_ = t.name;
                    edit_ = t;
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::string(matching::to_string(t.kind)).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(t.compiler.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(join_words(t.wrapper).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(t.builtin ? "detected" : "registry");
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("New toolchain")) {
            edit_ = matching::Toolchain{};
            edit_->name = "my-compiler";
            selected_.clear();
        }
        if (edit_) draw_editor(ctx);
    }

    void draw_editor(ViewContext& ctx) {
        matching::Toolchain& t = *edit_;
        ImGui::SeparatorText(t.builtin ? "Detected toolchain (read-only; save a copy to change it)" : "Toolchain");
        const float field = ImGui::GetFontSize() * 24;
        ImGui::SetNextItemWidth(field);
        ImGui::InputText("Name", &t.name);
        ImGui::SetNextItemWidth(field);
        if (ImGui::BeginCombo("Kind", std::string(matching::to_string(t.kind)).c_str())) {
            for (auto k : {matching::ToolchainKind::msvc, matching::ToolchainKind::clang_cl, matching::ToolchainKind::gcc, matching::ToolchainKind::clang})
                if (ImGui::Selectable(std::string(matching::to_string(k)).c_str(), t.kind == k)) t.kind = k;
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(field);
        ImGui::InputText("Compiler", &t.compiler);
        std::string wrapper = join_words(t.wrapper), flags = join_words(t.flags);
        ImGui::SetNextItemWidth(field);
        if (ImGui::InputTextWithHint("Wrapper", "e.g. wine", &wrapper)) t.wrapper = split_words(wrapper);
        ImGui::SetNextItemWidth(field);
        if (ImGui::InputTextWithHint("Flags", "always passed", &flags)) t.flags = split_words(flags);
        ImGui::SetNextItemWidth(field);
        ImGui::InputInt("Timeout, seconds", &t.timeout_seconds);
        t.timeout_seconds = std::max(1, t.timeout_seconds);
        if (!t.env.empty() && ImGui::TreeNode("Environment")) {
            for (const auto& [k, v] : t.env) ImGui::BulletText("%s=%s", k.c_str(), v.c_str());
            ImGui::TreePop();
        }

        const bool busy = health_.valid() && !health_.finished();
        ImGui::BeginDisabled(busy || trim(t.compiler).empty());
        if (ImGui::Button("Health check")) {
            health_ = ctx.jobs.submit([t]() -> Health {
                Health h;
                h.toolchain = t.name;
                auto report = matching::check_toolchain(t);
                if (!report) {
                    h.summary = report.error().message;
                    return h;
                }
                h.ok = report->ok;
                h.summary = std::format("{} in {} ms: {}", report->ok ? "works" : "failed", report->duration.count(), report->object);
                h.command = join_words(report->command);
                h.output = report->output;
                return h;
            });
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Compiles a one-line function with this toolchain (no compile cache).");
        ImGui::SameLine();
        const bool can_save = !trim(t.name).empty() && !trim(t.compiler).empty();
        ImGui::BeginDisabled(!can_save);
        if (ImGui::Button(t.builtin ? "Save a copy to the registry" : "Save to the registry")) {
            matching::Toolchain saved = t;
            if (saved.builtin) {
                saved.builtin = false;
                if (registry_->find(saved.name)) saved.name += "-copy";
            }
            registry_->upsert(saved);
            if (auto r = registry_->save(); !r) ctx.notify(Severity::error, std::format("Cannot save the toolchain registry: {}", r.error().message));
            else ctx.notify(Severity::info, std::format("Toolchain {} saved.", saved.name));
            selected_ = saved.name;
            edit_ = saved;
        }
        ImGui::EndDisabled();
        if (!t.builtin && registry_->find(t.name) && !registry_->find(t.name)->builtin) {
            ImGui::SameLine();
            if (ImGui::Button("Remove")) {
                registry_->remove(t.name);
                if (auto r = registry_->save(); !r) ctx.notify(Severity::error, std::format("Cannot save the toolchain registry: {}", r.error().message));
                edit_.reset();
                selected_.clear();
                return;
            }
        }
        if (busy) ImGui::TextDisabled("Checking...");
        if (health_result_ && health_result_->toolchain == t.name) {
            colored_text(health_result_->ok ? ctx.colors().ok : ctx.colors().error, health_result_->summary);
            if (!health_result_->command.empty()) {
                ImGui::PushFont(ctx.fonts.mono, 0.0f);
                ImGui::TextWrapped("%s", health_result_->command.c_str());
                if (!health_result_->output.empty()) ImGui::TextWrapped("%s", health_result_->output.c_str());
                ImGui::PopFont();
            }
        }
    }

    void draw_compiles(ViewContext& ctx) {
        const auto* s = ctx.snapshot.get();
        if (!s) {
            ImGui::TextDisabled("No run loaded. The run's compiles appear here.");
            return;
        }
        if (s->recent_compiles.empty()) {
            ImGui::TextDisabled("No compile yet in this run.");
            return;
        }
        ImGui::Text("%d compile(s) in this run; the latest %zu are listed.", s->compiles, s->recent_compiles.size());
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
        const float table_h = std::max(ImGui::GetContentRegionAvail().y * 0.55f, ImGui::GetFrameHeight() * 4);
        if (ImGui::BeginTable("##compiles", 7, flags, ImVec2(0, table_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            for (const char* h : {"Time", "Function", "Toolchain", "Result", "Exit", "Duration", "Cache"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(s->recent_compiles.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    // Newest first.
                    const usize index = s->recent_compiles.size() - 1 - static_cast<usize>(i);
                    const auto& c = *s->recent_compiles[index];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(local_clock(c.time).c_str(), selected_compile_ == &c, ImGuiSelectableFlags_SpanAllColumns))
                        selected_compile_ = &c;
                    ImGui::TableNextColumn();
                    const auto* session = s->session(c.session);
                    ImGui::TextUnformatted(session ? (session->display.empty() ? session->function : session->display).c_str() : "-");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(c.toolchain.c_str());
                    ImGui::TableNextColumn();
                    if (c.ok) colored_text(ctx.colors().ok, "ok");
                    else colored_text(ctx.colors().error, c.errors > 0 ? std::format("{} error(s)", c.errors) : std::string("failed"));
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", c.exit_code);
                    ImGui::TableNextColumn();
                    ImGui::Text("%lld ms", c.duration_ms);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(c.cached ? "hit" : "-");
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        // The selected record stays valid while this snapshot is held; look it up again in the new one.
        const events::CompileRecord* shown = nullptr;
        for (const auto& c : s->recent_compiles)
            if (c.get() == selected_compile_) shown = c.get();
        if (!shown) {
            ImGui::TextDisabled("Select a compile to see its command line and output.");
            return;
        }
        ImGui::SeparatorText("Command line");
        ImGui::PushFont(ctx.fonts.mono, 0.0f);
        ImGui::TextWrapped("%s", shown->command.empty() ? "(not recorded)" : shown->command.c_str());
        ImGui::PopFont();
        if (ImGui::SmallButton("Copy command")) ImGui::SetClipboardText(shown->command.c_str());
        ImGui::SeparatorText("Output");
        ImGui::PushFont(ctx.fonts.mono, 0.0f);
        ImGui::TextWrapped("%s", shown->output.empty() ? "(no output)" : shown->output.c_str());
        ImGui::PopFont();
    }

    JobHandle<Result<matching::ToolchainRegistry>> load_;
    bool loaded_ = false;
    std::optional<matching::ToolchainRegistry> registry_;
    std::string load_error_;
    std::string selected_;
    std::optional<matching::Toolchain> edit_;
    JobHandle<Health> health_;
    std::optional<Health> health_result_;
    const events::CompileRecord* selected_compile_ = nullptr;
};

} // namespace

std::unique_ptr<View> make_toolchains_view() { return std::make_unique<ToolchainsView>(); }

} // namespace decomp::gui
