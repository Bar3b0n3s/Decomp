#pragma once

// User-level GUI settings (docs/ui.md#persistence): gui.json in the user config directory, next to
// ImGui's imgui.ini. Nothing about the API key is ever stored here.

#include "core/json.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::gui {

enum class Theme { dark, light, high_contrast };
std::string_view to_string(Theme theme);
std::optional<Theme> theme_from_string(std::string_view name);
std::string_view theme_label(Theme theme);  // "Dark", "Light", "High contrast"

// Diff row colors (gui/theme.hpp). Both variants keep the row kinds apart under the common color-vision
// deficiencies by lightness as well as hue; Okabe-Ito uses only that palette's hues. Rows also carry
// glyphs, so color is never the only cue.
enum class DiffPaletteKind { standard, okabe_ito };
std::string_view to_string(DiffPaletteKind kind);
std::optional<DiffPaletteKind> diff_palette_from_string(std::string_view name);
std::string_view diff_palette_label(DiffPaletteKind kind);

// A dock layout saved by name: ImGui's ini text (SaveIniSettingsToMemory) plus the views that were open.
struct SavedLayout {
    std::string name;
    std::string ini;
    std::vector<std::string> open_views;
};

// What the GUI remembers per project (keyed by project_key()): which views are open, and whatever each
// view keeps (filters, columns, sort order) under its view id.
struct ProjectViewState {
    std::optional<std::vector<std::string>> open_views;  // nullopt: the default set
    Json views = Json::object();
};

struct DeveloperSettings {
    std::string replay_dir;  // drive sessions from recorded transcripts instead of the API (no key needed)
};

struct Settings {
    static constexpr float kDefaultFontSize = 15.0f;
    static constexpr float kMinFontSize = 10.0f;
    static constexpr float kMaxFontSize = 32.0f;
    static constexpr usize kMaxRecentProjects = 10;

    Theme theme = Theme::dark;
    float font_size = kDefaultFontSize;  // UI font size in pixels before DPI scaling
    DiffPaletteKind diff_palette = DiffPaletteKind::standard;
    std::vector<SavedLayout> layouts;
    std::vector<std::string> recent_projects;           // newest first, UTF-8 paths
    std::map<std::string, ProjectViewState> projects;   // "" holds the state used without a project
    DeveloperSettings developer;

    // Where gui.json and imgui.ini live. Not serialized; empty keeps everything in memory (tests).
    std::filesystem::path dir;

    std::filesystem::path file() const;      // dir/gui.json (empty when dir is)
    std::filesystem::path ini_file() const;  // dir/imgui.ini (empty when dir is)

    // Moves (or adds) `root` to the front of the recent projects.
    void add_recent_project(const std::filesystem::path& root);
    // The state for a project root; an empty root gives the no-project state. Created on first use.
    ProjectViewState& project_state(const std::filesystem::path& root);

    const SavedLayout* find_layout(std::string_view name) const;
    void put_layout(SavedLayout layout);  // replaces the layout with the same name, or appends
    bool remove_layout(std::string_view name);
};

// The key for per-project state: the absolute, normalized path in UTF-8 with '/' separators.
std::string project_key(const std::filesystem::path& root);

// %APPDATA%\decomp on Windows; $XDG_CONFIG_HOME/decomp or ~/.config/decomp elsewhere. Empty when none
// of these variables is set.
std::filesystem::path default_config_dir();

Json to_json(const Settings& settings);
// Unknown keys are ignored and invalid values keep their defaults, so an old or hand-edited file loads.
Settings settings_from_json(const Json& json);

// Reads dir/gui.json; a missing file gives the defaults. The result's `dir` is `dir`.
Result<Settings> load_settings(const std::filesystem::path& dir);
// Writes gui.json atomically (creating the directory). Does nothing when settings.dir is empty.
Result<void> save_settings(const Settings& settings);

} // namespace decomp::gui
