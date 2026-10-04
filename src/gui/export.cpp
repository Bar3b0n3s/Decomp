#include "gui/export.hpp"

#include "core/fs.hpp"
#include "gui/view.hpp"

#include <imgui.h>

#include <chrono>
#include <format>

namespace decomp::gui {

namespace {

std::string safe_stem(std::string_view stem) {
    std::string out;
    for (char c : stem) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (keep) out += c;
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && (out.back() == '_' || out.back() == '.')) out.pop_back();
    if (out.size() > 80) out.resize(80);
    return out.empty() ? std::string("export") : out;
}

} // namespace

std::filesystem::path export_path(const std::filesystem::path& project_root, std::string_view stem, std::string_view extension) {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const std::string base = std::format("{}-{:%Y%m%d-%H%M%S}", safe_stem(stem), now);
    const auto dir = project_root / ".decomp" / "exports";
    auto path = dir / fs::from_utf8(std::format("{}.{}", base, extension));
    std::error_code ec;
    for (int n = 2; std::filesystem::exists(path, ec); ++n) path = dir / fs::from_utf8(std::format("{}-{}.{}", base, n, extension));
    return path;
}

Result<std::filesystem::path> write_export(const std::filesystem::path& project_root, std::string_view stem, std::string_view extension,
                                           std::string_view content) {
    const auto path = export_path(project_root, stem, extension);
    TRY(fs::create_directories(path.parent_path()));
    TRY(fs::write_text(path, content));
    return path;
}

void export_button(ViewContext& ctx, const char* id, std::string_view stem, std::span<const ExportFormat> formats) {
    ImGui::PushID(id);
    if (ImGui::Button("Export")) ImGui::OpenPopup("##export");
    if (ImGui::BeginPopup("##export")) {
        const ProjectInfo& project = ctx.project;
        for (const ExportFormat& f : formats) {
            if (ImGui::MenuItem(std::format("Save {}", f.label).c_str(), nullptr, false, project.open)) {
                if (auto written = write_export(project.root, stem, f.extension, f.render()))
                    ctx.notify(Severity::info, std::format("Exported to {}", fs::to_utf8(*written)));
                else
                    ctx.notify(Severity::error, std::format("Cannot export: {}", written.error().message));
            }
            if (ImGui::MenuItem(std::format("Copy {}", f.label).c_str())) ImGui::SetClipboardText(f.render().c_str());
        }
        if (!project.open) ImGui::TextDisabled("Saving needs an open project.");
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

} // namespace decomp::gui
