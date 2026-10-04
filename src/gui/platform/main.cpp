// decomp-gui: a GLFW window with an OpenGL 3 context, Dear ImGui's GLFW and OpenGL 3 backends, and the
// shell from decomp_gui_lib (gui/app.hpp). No GL headers are needed: the ImGui backend has its own
// loader, and the few GL 1.x calls made here are loaded through GLFW.

#ifndef GLFW_INCLUDE_NONE  // premake defines it for the whole executable
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/version.hpp"
#include "gui/app.hpp"
#include "gui/settings.hpp"
#include "gui/theme.hpp"

#include <CLI/CLI.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <implot.h>
#include <stb_image_write.h>

#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

namespace {

using namespace decomp;
namespace stdfs = std::filesystem;

#ifdef _WIN32
#define DECOMP_GLAPI __stdcall
#else
#define DECOMP_GLAPI
#endif

// GL 1.x entry points, loaded through glfwGetProcAddress() once a context is current.
struct Gl {
    using Viewport = void(DECOMP_GLAPI*)(int, int, int, int);
    using ClearColor = void(DECOMP_GLAPI*)(float, float, float, float);
    using Clear = void(DECOMP_GLAPI*)(unsigned int);
    using PixelStorei = void(DECOMP_GLAPI*)(unsigned int, int);
    using ReadBuffer = void(DECOMP_GLAPI*)(unsigned int);
    using ReadPixels = void(DECOMP_GLAPI*)(int, int, int, int, unsigned int, unsigned int, void*);

    static constexpr unsigned int kColorBufferBit = 0x00004000;
    static constexpr unsigned int kRgba = 0x1908;
    static constexpr unsigned int kUnsignedByte = 0x1401;
    static constexpr unsigned int kPackAlignment = 0x0D05;
    static constexpr unsigned int kBack = 0x0405;

    Viewport viewport = nullptr;
    ClearColor clear_color = nullptr;
    Clear clear = nullptr;
    PixelStorei pixel_storei = nullptr;
    ReadBuffer read_buffer = nullptr;
    ReadPixels read_pixels = nullptr;

    template <class F>
    static F load(const char* name) {
        return reinterpret_cast<F>(glfwGetProcAddress(name));
    }

    bool load_all() {
        viewport = load<Viewport>("glViewport");
        clear_color = load<ClearColor>("glClearColor");
        clear = load<Clear>("glClear");
        pixel_storei = load<PixelStorei>("glPixelStorei");
        read_buffer = load<ReadBuffer>("glReadBuffer");
        read_pixels = load<ReadPixels>("glReadPixels");
        return viewport && clear_color && clear && pixel_storei && read_buffer && read_pixels;
    }
};

struct Options {
    std::string project;
    std::string view;
    std::string screenshot;
    std::string config_dir;
    std::string theme;
    int frames = 0;
    int width = 1600;
    int height = 1000;
    int verbose = 0;
};

// Reads the back buffer of the frame just rendered (before the swap) and writes it as a PNG.
bool write_screenshot(const Gl& gl, int width, int height, const stdfs::path& path) {
    if (width <= 0 || height <= 0) return false;
    const usize stride = static_cast<usize>(width) * 4;
    std::vector<unsigned char> pixels(stride * static_cast<usize>(height));
    gl.pixel_storei(Gl::kPackAlignment, 1);
    gl.read_buffer(Gl::kBack);
    gl.read_pixels(0, 0, width, height, Gl::kRgba, Gl::kUnsignedByte, pixels.data());
    // GL's rows start at the bottom, PNG's at the top; and the window is opaque whatever alpha says.
    std::vector<unsigned char> flipped(pixels.size());
    for (int y = 0; y < height; ++y) {
        const unsigned char* src = pixels.data() + static_cast<usize>(height - 1 - y) * stride;
        std::copy(src, src + stride, flipped.data() + static_cast<usize>(y) * stride);
    }
    for (usize i = 3; i < flipped.size(); i += 4) flipped[i] = 0xFF;

    std::vector<std::byte> png;
    auto append = [](void* context, void* data, int size) {
        auto* out = static_cast<std::vector<std::byte>*>(context);
        const auto* bytes = static_cast<const std::byte*>(data);
        out->insert(out->end(), bytes, bytes + size);
    };
    if (!stbi_write_png_to_func(append, &png, width, height, 4, flipped.data(), static_cast<int>(stride)) || png.empty()) return false;
    if (path.has_parent_path()) (void)fs::create_directories(path.parent_path());
    if (auto r = fs::write_file(path, png); !r) {
        log::error("{}", r.error().describe());
        return false;
    }
    return true;
}

// Keeps the log in the config directory from growing without bound.
void open_log(const stdfs::path& dir) {
    const stdfs::path file = dir / "decomp-gui.log";
    std::error_code ec;
    if (stdfs::exists(file, ec) && stdfs::file_size(file, ec) > 1024 * 1024) stdfs::rename(file, dir / "decomp-gui.log.old", ec);
    log::set_file(file);
}

#ifdef _WIN32
// A windowed app has no console of its own. When started from one, print there (--help, errors), unless
// the output is already redirected to a file or pipe.
void attach_parent_console() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    auto reopen = [](FILE* stream) {
        const int fd = _fileno(stream);
        if (fd >= 0) {
            const intptr_t handle = _get_osfhandle(fd);
            if (handle != -1 && handle != -2) return;
        }
        FILE* reopened = nullptr;
        (void)freopen_s(&reopened, "CONOUT$", "w", stream);
    };
    reopen(stdout);
    reopen(stderr);
}
#endif

gui::App* g_app = nullptr;  // for GLFW callbacks (ImGui's backend owns the window user pointer)

int run(const Options& opt) {
    if (opt.verbose >= 2) log::set_level(log::Level::trace);
    else if (opt.verbose == 1) log::set_level(log::Level::debug);

    std::optional<gui::Theme> theme;
    if (!opt.theme.empty()) {
        theme = gui::theme_from_string(opt.theme);
        if (!theme) {
            log::error("unknown theme '{}' (dark, light or high-contrast)", opt.theme);
            return 2;
        }
    }

    // Settings, imgui.ini and the log live in the user config directory.
    stdfs::path dir = opt.config_dir.empty() ? gui::default_config_dir() : fs::from_utf8(opt.config_dir);
    if (!dir.empty()) {
        if (auto r = fs::create_directories(dir); !r) {
            log::warn("settings will not be saved: {}", r.error().describe());
            dir.clear();
        }
    }
    if (!dir.empty()) open_log(dir);
    log::info("decomp-gui {} starting", kVersion);

    gui::Settings settings;
    std::string settings_problem;
    if (auto loaded = gui::load_settings(dir)) {
        settings = std::move(*loaded);
    } else {
        // Keep the unreadable file for the user and start from the defaults.
        settings.dir = dir;
        std::error_code ec;
        stdfs::rename(settings.file(), dir / "gui.json.bad", ec);
        settings_problem = std::format("The GUI settings could not be read and were reset ({}); the old file is gui.json.bad.",
                                       loaded.error().message);
        log::warn("{}", settings_problem);
    }
    stdfs::path project_root;
    if (!opt.project.empty()) {
        std::error_code ec;
        project_root = stdfs::absolute(fs::from_utf8(opt.project), ec).lexically_normal();
        settings.add_recent_project(project_root);
    }
    if (theme) settings.theme = *theme;

    glfwSetErrorCallback([](int code, const char* description) { log::error("GLFW error {:#x}: {}", code, description ? description : "?"); });
    if (!glfwInit()) {
        log::error("could not initialize GLFW (is a display available?)");
        return 1;
    }
#ifdef __APPLE__
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(opt.width, opt.height, "Decomp", nullptr, nullptr);
    if (!window) {
        log::error("could not create a window with an OpenGL 3 context");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    Gl gl;
    if (!gl.load_all()) {
        log::error("could not load the OpenGL functions");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    const std::string ini_path = settings.dir.empty() ? std::string() : fs::to_utf8(settings.ini_file());
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = ini_path.empty() ? nullptr : ini_path.c_str();
    io.LogFilename = nullptr;
#ifndef DECOMP_DEBUG
    io.ConfigDebugHighlightIdConflicts = false;  // a programmer aid; the headless tests check every item
#endif
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    gui::AppServices services;
    services.post_empty_event = [] { glfwPostEmptyEvent(); };
    if (!project_root.empty()) services.project = [project_root] { return gui::ProjectInfo{project_root, {}, false}; };
    auto app = std::make_unique<gui::App>(std::move(services), settings);
    g_app = app.get();
    if (!project_root.empty() || theme) app->context().mark_settings_dirty();  // the recent project, the theme
    if (!settings_problem.empty()) app->notifications().notify(gui::Severity::warning, settings_problem);
    app->set_dpi_scale(ImGui_ImplGlfw_GetContentScaleForWindow(window));
    glfwSetWindowContentScaleCallback(window, [](GLFWwindow*, float x, float) {
        if (g_app) g_app->set_dpi_scale(x);
    });

    int exit_code = 0;
    if (!opt.view.empty() && !app->focus_view(opt.view)) {
        std::string ids;
        for (const auto& v : app->views()) ids += std::format("{}{}", ids.empty() ? "" : ", ", v->id());
        log::error("unknown view '{}' (one of: {})", opt.view, ids);
        exit_code = 2;
    }

    // Event-driven: wait for input or a wake-up (glfwPostEmptyEvent from workers and jobs), then render a
    // few frames so that ImGui can settle (popups appear one frame after they open). --frames renders
    // continuously and stops after that many frames.
    int frame = 0;
    int settle = 3;
    while (exit_code == 0 && !app->wants_quit()) {
        if (opt.frames > 0) {
            if (frame >= opt.frames) break;
            glfwPollEvents();
        } else if (settle > 0) {
            glfwPollEvents();
            --settle;
        } else {
            glfwWaitEventsTimeout(app->idle_timeout());
        }
        if (ImGui::GetCurrentContext()->InputEventsQueue.Size > 0) settle = 3;
        if (glfwWindowShouldClose(window)) {
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            app->request_quit();
            if (app->wants_quit()) break;
        }
        if (opt.frames == 0 && glfwGetWindowAttrib(window, GLFW_ICONIFIED)) continue;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app->frame();
        ImGui::Render();

        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        const ImVec4 bg = gui::theme_colors(settings.theme).background;
        gl.viewport(0, 0, width, height);
        gl.clear_color(bg.x, bg.y, bg.z, 1.0f);
        gl.clear(Gl::kColorBufferBit);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        ++frame;
        if (opt.frames > 0 && frame == opt.frames && !opt.screenshot.empty()) {
            const stdfs::path path = fs::from_utf8(opt.screenshot);
            if (write_screenshot(gl, width, height, path)) log::info("wrote {} ({}x{})", opt.screenshot, width, height);
            else exit_code = 1;
        }
        glfwSwapBuffers(window);
    }

    g_app = nullptr;
    app.reset();  // saves the settings
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();  // writes imgui.ini
    glfwDestroyWindow(window);
    glfwTerminate();
    return exit_code;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    attach_parent_console();
#endif
    CLI::App cli{"decomp-gui - the supervision GUI for Decomp"};
    argv = cli.ensure_utf8(argv);
    cli.set_version_flag("--version", decomp::kVersion);
    Options opt;
    cli.add_option("--project", opt.project, "Project directory (recorded in the recent projects)");
    cli.add_option("--view", opt.view, "Open and focus a view (id or title, e.g. dashboard, run-monitor)");
    cli.add_option("--frames", opt.frames, "Render this many frames, then exit")->check(CLI::PositiveNumber);
    cli.add_option("--screenshot", opt.screenshot, "With --frames: write the last frame to this PNG file");
    cli.add_option("--width", opt.width, "Window width")->check(CLI::Range(320, 16384));
    cli.add_option("--height", opt.height, "Window height")->check(CLI::Range(240, 16384));
    cli.add_option("--theme", opt.theme, "Theme: dark, light or high-contrast (saved like a menu choice)");
    cli.add_option("--config-dir", opt.config_dir, "Directory for gui.json, imgui.ini and the log (default: the user config directory)");
    cli.add_flag("-v,--verbose", opt.verbose, "More logging (repeat for trace)");
    try {
        cli.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return cli.exit(e);
    }
    if (!opt.screenshot.empty() && opt.frames == 0) {
        std::fputs("error: --screenshot needs --frames\n", stderr);
        return 2;
    }
    return run(opt);
}
