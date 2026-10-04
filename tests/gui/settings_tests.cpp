#include "core/fs.hpp"
#include "gui/settings.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

using namespace decomp;
using namespace decomp::gui;
namespace stdfs = std::filesystem;

TEST_CASE("settings round trip through gui.json") {
    auto tmp = fs::TempDir::create("decomp-gui-settings");
    REQUIRE(tmp);
    const stdfs::path dir = tmp->path() / "config";

    Settings s;
    s.dir = dir;
    s.theme = Theme::high_contrast;
    s.font_size = 18;
    s.diff_palette = DiffPaletteKind::okabe_ito;
    s.put_layout({"review", "[Docking][Data]\nDockSpace ID=0x1\n", {"dashboard", "diff_viewer"}});
    s.put_layout({"monitor", "[Window][x]\n", {"run_monitor"}});
    s.add_recent_project(tmp->path() / "older");
    s.add_recent_project(tmp->path() / "newer");
    s.project_state(tmp->path() / "newer").open_views = std::vector<std::string>{"dashboard", "cost"};
    s.project_state(tmp->path() / "newer").views["function_browser"] = Json{{"filter", "status:nonmatching"}, {"sort", "size"}};
    s.project_state({}).open_views = std::vector<std::string>{"logs"};
    s.developer.replay_dir = "tests/replay/run";
    REQUIRE(save_settings(s));
    REQUIRE(stdfs::exists(dir / "gui.json"));

    auto loaded = load_settings(dir);
    REQUIRE(loaded);
    CHECK(loaded->dir == dir);
    CHECK(loaded->theme == Theme::high_contrast);
    CHECK(loaded->font_size == doctest::Approx(18));
    CHECK(loaded->diff_palette == DiffPaletteKind::okabe_ito);
    REQUIRE(loaded->layouts.size() == 2);
    CHECK(loaded->layouts[0].name == "review");
    CHECK(loaded->layouts[0].ini == "[Docking][Data]\nDockSpace ID=0x1\n");
    CHECK(loaded->layouts[0].open_views == std::vector<std::string>{"dashboard", "diff_viewer"});
    REQUIRE(loaded->recent_projects.size() == 2);
    CHECK(loaded->recent_projects[0] == project_key(tmp->path() / "newer"));
    const ProjectViewState& p = loaded->project_state(tmp->path() / "newer");
    REQUIRE(p.open_views);
    CHECK(*p.open_views == std::vector<std::string>{"dashboard", "cost"});
    CHECK(p.views["function_browser"]["sort"] == "size");
    REQUIRE(loaded->project_state({}).open_views);
    CHECK(*loaded->project_state({}).open_views == std::vector<std::string>{"logs"});
    CHECK(loaded->developer.replay_dir == "tests/replay/run");
    CHECK(to_json(*loaded) == to_json(s));
}

TEST_CASE("missing, damaged and odd settings files") {
    auto tmp = fs::TempDir::create("decomp-gui-settings");
    REQUIRE(tmp);

    auto missing = load_settings(tmp->path() / "nowhere");
    REQUIRE(missing);
    CHECK(missing->theme == Theme::dark);
    CHECK(missing->font_size == doctest::Approx(Settings::kDefaultFontSize));

    REQUIRE(fs::write_text(tmp->path() / "gui.json", "{ not json"));
    CHECK_FALSE(load_settings(tmp->path()));

    // Wrong types and unknown values keep the defaults; out-of-range sizes are clamped.
    Json layouts = Json::array();
    layouts.push_back(Json{{"ini", "no name"}});
    layouts.push_back(Json{{"name", "ok"}, {"ini", "x"}});
    Json recent = Json::array();
    recent.push_back("a");
    recent.push_back(1);
    recent.push_back("b");
    Json odd_json = Json::object();
    odd_json["theme"] = "sepia";
    odd_json["font_size"] = 400;
    odd_json["diff_palette"] = 3;
    odd_json["layouts"] = layouts;
    odd_json["recent_projects"] = recent;
    odd_json["projects"] = "nope";
    odd_json["unknown"] = true;
    Settings odd = settings_from_json(odd_json);
    CHECK(odd.theme == Theme::dark);
    CHECK(odd.font_size == doctest::Approx(Settings::kMaxFontSize));
    CHECK(odd.diff_palette == DiffPaletteKind::standard);
    REQUIRE(odd.layouts.size() == 1);
    CHECK(odd.layouts[0].name == "ok");
    CHECK(odd.recent_projects == std::vector<std::string>{"a", "b"});
    CHECK(odd.projects.empty());
    CHECK(settings_from_json(Json::array()).theme == Theme::dark);
}

TEST_CASE("settings without a directory stay in memory") {
    Settings s;
    s.theme = Theme::light;
    CHECK(s.file().empty());
    CHECK(s.ini_file().empty());
    CHECK(save_settings(s));  // a no-op
}

TEST_CASE("recent projects: newest first, no duplicates, bounded") {
    Settings s;
    for (int i = 0; i < 15; ++i) s.add_recent_project(stdfs::path("/projects") / std::to_string(i));
    REQUIRE(s.recent_projects.size() == Settings::kMaxRecentProjects);
    CHECK(s.recent_projects.front() == project_key("/projects/14"));
    s.add_recent_project("/projects/10/");
    CHECK(s.recent_projects.front() == project_key("/projects/10"));
    CHECK(std::ranges::count(s.recent_projects, project_key("/projects/10")) == 1);
    s.add_recent_project("");
    CHECK(s.recent_projects.size() == Settings::kMaxRecentProjects);
}

TEST_CASE("project keys are absolute and normalized") {
    CHECK(project_key("").empty());
    CHECK(project_key("/projects/demo/./sub/..") == project_key("/projects/demo"));
    CHECK(project_key("/projects/demo/") == project_key("/projects/demo"));
    CHECK(project_key("relative").find('\\') == std::string::npos);
    CHECK(stdfs::path(project_key("relative")).is_absolute());
}

TEST_CASE("named layouts replace by name") {
    Settings s;
    s.put_layout({"a", "1", {}});
    s.put_layout({"b", "2", {}});
    s.put_layout({"a", "3", {"logs"}});
    REQUIRE(s.layouts.size() == 2);
    REQUIRE(s.find_layout("a"));
    CHECK(s.find_layout("a")->ini == "3");
    CHECK(s.remove_layout("a"));
    CHECK_FALSE(s.remove_layout("a"));
    CHECK(s.find_layout("a") == nullptr);
}

TEST_CASE("the config directory follows the platform conventions") {
#ifdef _WIN32
    decomp::test::ScopedEnv appdata("APPDATA", "C:\\Users\\someone\\AppData\\Roaming");
    CHECK(default_config_dir() == stdfs::path(L"C:\\Users\\someone\\AppData\\Roaming") / "decomp");
#else
    {
        decomp::test::ScopedEnv xdg("XDG_CONFIG_HOME", "/xdg/config");
        CHECK(default_config_dir() == stdfs::path("/xdg/config/decomp"));
    }
    {
        decomp::test::ScopedEnv xdg("XDG_CONFIG_HOME", "relative/is/ignored");
        decomp::test::ScopedEnv home("HOME", "/home/someone");
        CHECK(default_config_dir() == stdfs::path("/home/someone/.config/decomp"));
    }
#endif
}

TEST_CASE("settings never hold API keys") {
    Settings s;
    s.developer.replay_dir = "x";
    const std::string text = dump_compact(to_json(s));
    CHECK(text.find("key") == std::string::npos);
    CHECK(text.find("token") == std::string::npos);
}

TEST_CASE("theme and palette names") {
    for (Theme t : {Theme::dark, Theme::light, Theme::high_contrast}) CHECK(theme_from_string(to_string(t)) == t);
    CHECK(theme_from_string("high-contrast") == Theme::high_contrast);
    CHECK_FALSE(theme_from_string("sepia"));
    for (DiffPaletteKind k : {DiffPaletteKind::standard, DiffPaletteKind::okabe_ito}) CHECK(diff_palette_from_string(to_string(k)) == k);
}
