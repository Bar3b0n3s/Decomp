#include "gui/settings.hpp"

#include "core/fs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace decomp::gui {

namespace stdfs = std::filesystem;

std::string_view to_string(Theme theme) {
    switch (theme) {
    case Theme::dark: return "dark";
    case Theme::light: return "light";
    case Theme::high_contrast: return "high_contrast";
    }
    return "dark";
}

std::optional<Theme> theme_from_string(std::string_view name) {
    for (Theme t : {Theme::dark, Theme::light, Theme::high_contrast})
        if (to_string(t) == name) return t;
    if (name == "high-contrast") return Theme::high_contrast;
    return std::nullopt;
}

std::string_view theme_label(Theme theme) {
    switch (theme) {
    case Theme::dark: return "Dark";
    case Theme::light: return "Light";
    case Theme::high_contrast: return "High contrast";
    }
    return "Dark";
}

std::string_view to_string(DiffPaletteKind kind) {
    switch (kind) {
    case DiffPaletteKind::standard: return "standard";
    case DiffPaletteKind::okabe_ito: return "okabe_ito";
    }
    return "standard";
}

std::optional<DiffPaletteKind> diff_palette_from_string(std::string_view name) {
    for (DiffPaletteKind k : {DiffPaletteKind::standard, DiffPaletteKind::okabe_ito})
        if (to_string(k) == name) return k;
    return std::nullopt;
}

std::string_view diff_palette_label(DiffPaletteKind kind) {
    switch (kind) {
    case DiffPaletteKind::standard: return "Standard";
    case DiffPaletteKind::okabe_ito: return "Colorblind-safe (Okabe-Ito)";
    }
    return "Standard";
}

stdfs::path Settings::file() const { return dir.empty() ? stdfs::path() : dir / "gui.json"; }
stdfs::path Settings::ini_file() const { return dir.empty() ? stdfs::path() : dir / "imgui.ini"; }

void Settings::add_recent_project(const stdfs::path& root) {
    std::string key = project_key(root);
    if (key.empty()) return;
    std::erase(recent_projects, key);
    recent_projects.insert(recent_projects.begin(), std::move(key));
    if (recent_projects.size() > kMaxRecentProjects) recent_projects.resize(kMaxRecentProjects);
}

ProjectViewState& Settings::project_state(const stdfs::path& root) { return projects[project_key(root)]; }

const SavedLayout* Settings::find_layout(std::string_view name) const {
    auto it = std::ranges::find(layouts, name, &SavedLayout::name);
    return it == layouts.end() ? nullptr : &*it;
}

void Settings::put_layout(SavedLayout layout) {
    auto it = std::ranges::find(layouts, layout.name, &SavedLayout::name);
    if (it != layouts.end()) *it = std::move(layout);
    else layouts.push_back(std::move(layout));
}

bool Settings::remove_layout(std::string_view name) {
    return std::erase_if(layouts, [&](const SavedLayout& l) { return l.name == name; }) > 0;
}

std::string project_key(const stdfs::path& root) {
    if (root.empty()) return {};
    std::error_code ec;
    stdfs::path abs = stdfs::absolute(root, ec);
    if (ec) abs = root;
    abs = abs.lexically_normal();
    std::string key = fs::to_utf8(abs);
    std::ranges::replace(key, '\\', '/');
    while (key.size() > 1 && key.back() == '/' && !(key.size() == 3 && key[1] == ':')) key.pop_back();
    return key;
}

stdfs::path default_config_dir() {
#ifdef _WIN32
    const DWORD size = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);  // including the terminator
    if (size == 0) return {};
    std::wstring value(size, L'\0');
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", value.data(), size);
    if (n == 0 || n >= size) return {};
    value.resize(n);
    return stdfs::path(value) / "decomp";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg == '/') return stdfs::path(xdg) / "decomp";
    if (const char* home = std::getenv("HOME"); home && *home) return stdfs::path(home) / ".config" / "decomp";
    return {};
#endif
}

namespace {

std::vector<std::string> string_array(const Json& j) {
    std::vector<std::string> out;
    if (!j.is_array()) return out;
    for (const auto& v : j)
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

const Json* member(const Json& obj, std::string_view key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

} // namespace

Json to_json(const Settings& s) {
    Json j = Json::object();
    j["version"] = 1;
    j["theme"] = std::string(to_string(s.theme));
    j["font_size"] = s.font_size;
    j["diff_palette"] = std::string(to_string(s.diff_palette));
    Json layouts = Json::array();
    for (const auto& l : s.layouts) layouts.push_back({{"name", l.name}, {"ini", l.ini}, {"open_views", l.open_views}});
    j["layouts"] = std::move(layouts);
    j["recent_projects"] = s.recent_projects;
    Json projects = Json::object();
    for (const auto& [key, state] : s.projects) {
        const bool has_views = state.views.is_object() && !state.views.empty();
        if (!state.open_views && !has_views) continue;  // nothing remembered yet
        Json p = Json::object();
        if (state.open_views) p["open_views"] = *state.open_views;
        if (has_views) p["views"] = state.views;
        projects[key] = std::move(p);
    }
    j["projects"] = std::move(projects);
    j["developer"] = {{"replay_dir", s.developer.replay_dir}};
    j["budget_alert"] = s.budget_alert;
    return j;
}

Settings settings_from_json(const Json& j) {
    Settings s;
    if (!j.is_object()) return s;
    if (auto t = theme_from_string(json_string_or(j, "theme", ""))) s.theme = *t;
    double size = json_number_or(j, "font_size", Settings::kDefaultFontSize);
    if (std::isfinite(size)) s.font_size = std::clamp(static_cast<float>(size), Settings::kMinFontSize, Settings::kMaxFontSize);
    if (auto p = diff_palette_from_string(json_string_or(j, "diff_palette", ""))) s.diff_palette = *p;
    if (const Json* layouts = member(j, "layouts"); layouts && layouts->is_array()) {
        for (const auto& l : *layouts) {
            SavedLayout layout{json_string_or(l, "name", ""), json_string_or(l, "ini", ""), {}};
            if (const Json* views = member(l, "open_views")) layout.open_views = string_array(*views);
            if (!layout.name.empty()) s.put_layout(std::move(layout));
        }
    }
    if (const Json* recent = member(j, "recent_projects")) {
        s.recent_projects = string_array(*recent);
        if (s.recent_projects.size() > Settings::kMaxRecentProjects) s.recent_projects.resize(Settings::kMaxRecentProjects);
    }
    if (const Json* projects = member(j, "projects"); projects && projects->is_object()) {
        for (const auto& [key, p] : projects->items()) {
            ProjectViewState state;
            if (const Json* views = member(p, "open_views"); views && views->is_array()) state.open_views = string_array(*views);
            if (const Json* views = member(p, "views"); views && views->is_object()) state.views = *views;
            s.projects[key] = std::move(state);
        }
    }
    if (const Json* dev = member(j, "developer")) s.developer.replay_dir = json_string_or(*dev, "replay_dir", "");
    if (const double alert = json_number_or(j, "budget_alert", s.budget_alert); std::isfinite(alert)) s.budget_alert = std::clamp(alert, 0.05, 1.0);
    return s;
}

Result<Settings> load_settings(const stdfs::path& dir) {
    Settings s;
    s.dir = dir;
    if (dir.empty()) return s;
    std::error_code ec;
    if (!stdfs::exists(s.file(), ec)) return s;
    TRY_ASSIGN(std::string text, fs::read_text(s.file()));
    auto parsed = parse_json(text);
    if (!parsed) return std::unexpected(std::move(parsed.error()).with_context(fs::to_utf8(s.file())));
    Settings loaded = settings_from_json(*parsed);
    loaded.dir = dir;
    return loaded;
}

Result<void> save_settings(const Settings& settings) {
    if (settings.dir.empty()) return {};
    TRY(fs::create_directories(settings.dir));
    return fs::write_text(settings.file(), dump_pretty(to_json(settings)) + "\n");
}

} // namespace decomp::gui
