-- Third-party static libraries. Included from premake5.lua inside the "Decomp" workspace.

local ext = path.getabsolute(path.join(_SCRIPT_DIR, "..", "external"))

local function thirdparty()
    kind "StaticLib"
    warnings "Off"
    targetdir (path.join(_MAIN_SCRIPT_DIR, "build", "lib", "%{cfg.buildcfg}"))
end

project "zycore"
    thirdparty()
    language "C"
    -- src/API/*.c (threads, terminal, process helpers) is not needed by Zydis and is OS-specific.
    files {
        ext .. "/zydis/dependencies/zycore/src/*.c",
        ext .. "/zydis/dependencies/zycore/include/**.h",
    }
    includedirs {
        ext .. "/zydis/dependencies/zycore/include",
        ext .. "/zydis/dependencies/zycore/src",
    }
    defines { "ZYCORE_STATIC_BUILD" }

project "zydis"
    thirdparty()
    language "C"
    files {
        ext .. "/zydis/src/*.c",
        ext .. "/zydis/include/**.h",
    }
    includedirs {
        ext .. "/zydis/include",
        ext .. "/zydis/src",
        ext .. "/zydis/dependencies/zycore/include",
    }
    defines { "ZYDIS_STATIC_BUILD", "ZYCORE_STATIC_BUILD" }

project "raw_pdb"
    thirdparty()
    language "C++"
    files { ext .. "/raw_pdb/src/*.cpp", ext .. "/raw_pdb/src/*.h", ext .. "/raw_pdb/src/Foundation/*.h" }
    includedirs { ext .. "/raw_pdb/src" }

project "llvm_demangle"
    thirdparty()
    language "C++"
    files { ext .. "/llvm-demangle/lib/*.cpp", ext .. "/llvm-demangle/include/**.h" }
    includedirs { ext .. "/llvm-demangle/include" }

-- GUI libraries (decomp-gui, decomp_gui_tests). Every translation unit that includes imgui.h, ours
-- and theirs, must see the same IMGUI_USER_CONFIG: src/gui/imgui_config.h routes IM_ASSERT to an
-- always-on handler (Release builds too) and enables the item hooks the headless tests use.
local gui = path.getabsolute(path.join(_SCRIPT_DIR, "..", "src", "gui"))

local function imgui_config()
    includedirs { ext .. "/imgui", ext .. "/imgui/backends", gui }
    defines { "IMGUI_USER_CONFIG=\"imgui_config.h\"" }
end

project "imgui"
    thirdparty()
    language "C++"
    -- The null backend serves the headless tests; the GLFW and OpenGL 3 backends are compiled into
    -- decomp-gui (use_imgui_backends()), so this library needs no window system.
    files {
        ext .. "/imgui/imgui.cpp",
        ext .. "/imgui/imgui_demo.cpp",
        ext .. "/imgui/imgui_draw.cpp",
        ext .. "/imgui/imgui_tables.cpp",
        ext .. "/imgui/imgui_widgets.cpp",
        ext .. "/imgui/*.h",
        ext .. "/imgui/misc/cpp/imgui_stdlib.cpp",
        ext .. "/imgui/misc/cpp/imgui_stdlib.h",
        ext .. "/imgui/backends/imgui_impl_null.cpp",
        ext .. "/imgui/backends/imgui_impl_null.h",
    }
    imgui_config()

project "implot"
    thirdparty()
    language "C++"
    -- implot_demo.cpp is not built (ImPlot::ShowDemoWindow() is therefore unavailable).
    files { ext .. "/implot/implot.cpp", ext .. "/implot/implot_items.cpp", ext .. "/implot/*.h" }
    includedirs { ext .. "/implot" }
    imgui_config()

project "imgui_text_edit"
    thirdparty()
    language "C++"
    -- TextDiff.cpp (and its dtl.h dependency) is not built.
    files { ext .. "/imgui_text_edit/TextEditor.cpp", ext .. "/imgui_text_edit/TextEditor.h" }
    includedirs { ext .. "/imgui_text_edit" }
    imgui_config()

project "glfw"
    thirdparty()
    language "C"
    files {
        ext .. "/glfw/include/GLFW/*.h",
        ext .. "/glfw/src/internal.h",
        ext .. "/glfw/src/platform.h",
        ext .. "/glfw/src/mappings.h",
        ext .. "/glfw/src/null_*.h",
    }
    for _, name in ipairs({ "context", "init", "input", "monitor", "platform", "vulkan", "window", "egl_context",
                            "osmesa_context", "null_init", "null_monitor", "null_window", "null_joystick" }) do
        files { ext .. "/glfw/src/" .. name .. ".c" }
    end
    includedirs { ext .. "/glfw/include" }
    filter "system:windows"
        defines { "_GLFW_WIN32" }
        files {
            ext .. "/glfw/src/win32_module.c",
            ext .. "/glfw/src/win32_time.c",
            ext .. "/glfw/src/win32_thread.c",
            ext .. "/glfw/src/win32_init.c",
            ext .. "/glfw/src/win32_joystick.c",
            ext .. "/glfw/src/win32_monitor.c",
            ext .. "/glfw/src/win32_window.c",
            ext .. "/glfw/src/wgl_context.c",
            ext .. "/glfw/src/win32_*.h",
        }
    -- X11 only (runs through XWayland on Wayland desktops). GLFW loads libX11 and libGL at run time.
    filter "system:linux"
        defines { "_GLFW_X11", "_DEFAULT_SOURCE" }
        files {
            ext .. "/glfw/src/posix_module.c",
            ext .. "/glfw/src/posix_time.c",
            ext .. "/glfw/src/posix_thread.c",
            ext .. "/glfw/src/x11_init.c",
            ext .. "/glfw/src/x11_monitor.c",
            ext .. "/glfw/src/x11_window.c",
            ext .. "/glfw/src/xkb_unicode.c",
            ext .. "/glfw/src/glx_context.c",
            ext .. "/glfw/src/linux_joystick.c",
            ext .. "/glfw/src/posix_poll.c",
            ext .. "/glfw/src/posix_*.h",
            ext .. "/glfw/src/x11_platform.h",
            ext .. "/glfw/src/linux_joystick.h",
        }
    filter {}

-- Include directories and defines a project needs to compile against ImGui, ImPlot and the text
-- editor (src/gui is where imgui_config.h lives).
function use_imgui()
    imgui_config()
    includedirs { ext .. "/implot", ext .. "/imgui_text_edit" }
end

-- Dependents first, for GNU ld.
function link_imgui()
    links { "imgui_text_edit", "implot", "imgui" }
end

-- The platform backends, compiled into the executable that owns the window (warnings off: third party),
-- plus GLFW's headers and stb_image_write (screenshots).
function use_imgui_backends()
    files {
        ext .. "/imgui/backends/imgui_impl_glfw.cpp",
        ext .. "/imgui/backends/imgui_impl_glfw.h",
        ext .. "/imgui/backends/imgui_impl_opengl3.cpp",
        ext .. "/imgui/backends/imgui_impl_opengl3.h",
    }
    includedirs { ext .. "/glfw/include", ext .. "/stb" }
    -- No GL headers anywhere: the OpenGL 3 backend loads GL itself (imgui_impl_opengl3_loader.h).
    defines { "GLFW_INCLUDE_NONE" }
    filter { "files:**/imgui/backends/imgui_impl_*.cpp or **/stb_image_write.cpp" }
        warnings "Off"
    -- Our GLFW has no Wayland backend, so the ImGui backend must not call into it.
    filter "system:linux"
        defines { "IMGUI_IMPL_GLFW_DISABLE_WAYLAND" }
    filter {}
end

-- Include directories and defines a project needs to consume the libraries above.
function use_thirdparty()
    includedirs {
        ext .. "/zydis/include",
        ext .. "/zydis/dependencies/zycore/include",
        ext .. "/raw_pdb/src",
        ext .. "/llvm-demangle/include",
        ext .. "/nlohmann/include",
        ext .. "/CLI11/include",
        ext .. "/doctest/include",
    }
    defines { "ZYDIS_STATIC_BUILD", "ZYCORE_STATIC_BUILD" }
end

function link_thirdparty()
    links { "zydis", "zycore", "raw_pdb", "llvm_demangle" }
end
