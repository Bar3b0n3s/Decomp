#pragma once

// Exports (docs/ui.md#export): reports, lists, transcripts and diffs are saved under the project's
// .decomp/exports/ (a project-managed, gitignored directory) or copied to the clipboard.

#include "core/result.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace decomp::gui {

struct ViewContext;

// <project>/.decomp/exports/<stem>-<UTC time>.<extension>, made unique when the name is taken; the stem
// is reduced to a file-name-safe form.
std::filesystem::path export_path(const std::filesystem::path& project_root, std::string_view stem, std::string_view extension);
// Writes `content` to export_path() and returns where it went.
Result<std::filesystem::path> write_export(const std::filesystem::path& project_root, std::string_view stem, std::string_view extension,
                                           std::string_view content);

struct ExportFormat {
    std::string label;                    // "Markdown", "JSON", "CSV", ...
    std::string extension;                // "md", "json", "csv", ...
    std::function<std::string()> render;  // called only when the format is chosen
};

// An "Export" button (id distinguishes several in one window) opening a menu with, for each format,
// "Save <label>" (to .decomp/exports/, announced with a notification naming the file; needs a project)
// and "Copy <label>" (to the clipboard).
void export_button(ViewContext& ctx, const char* id, std::string_view stem, std::span<const ExportFormat> formats);

} // namespace decomp::gui
