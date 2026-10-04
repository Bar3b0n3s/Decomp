-- Decomp: AI-assisted matching decompilation for x86/x86-64 binaries.
--   premake5 vs2022            (Windows, Visual Studio 2022)
--   premake5 gmake             (Linux/macOS; then: make -C build config=release_x64 CC=gcc-14 CXX=g++-14)

workspace "Decomp"
    configurations { "Debug", "Release" }
    platforms { "x64" }
    architecture "x86_64"
    location "build"
    startproject "decomp"

    targetdir "bin/%{cfg.buildcfg}"
    objdir "build/obj/%{cfg.buildcfg}/%{prj.name}"

    cppdialect "C++23"
    cdialect "C11"
    multiprocessorcompile "On"
    symbols "On"

    filter "configurations:Debug"
        optimize "Off"
        defines { "DECOMP_DEBUG" }

    filter "configurations:Release"
        optimize "Speed"
        defines { "NDEBUG" }

    filter "system:windows"
        systemversion "latest"
        staticruntime "On"
        defines { "UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN", "_CRT_SECURE_NO_WARNINGS" }

    -- Older VS 2022 updates reject stdcpp23; /std:c++latest is a superset of it.
    filter "action:vs*"
        cppdialect "C++latest"
        buildoptions { "/utf-8", "/permissive-", "/Zc:__cplusplus", "/Zc:preprocessor" }

    filter {}

include "premake/deps.lua"

local function decomp_settings()
    language "C++"
    warnings "Extra"
    includedirs { "src" }
    use_thirdparty()
end

-- Executables link decomp_lib, then the third-party static libs, then system libraries (in that order,
-- so GNU ld's --as-needed keeps libcurl).
local function link_decomp()
    links { "decomp_lib" }
    link_thirdparty()
    filter "system:windows"
        links { "winhttp" }
    filter "system:linux"
        links { "curl", "pthread" }
    filter "system:macosx"
        links { "curl" }
    filter {}
end

project "decomp_lib"
    kind "StaticLib"
    decomp_settings()
    files { "src/**.hpp", "src/**.cpp" }
    removefiles { "src/cli/**", "src/gui/**" }

project "decomp"
    kind "ConsoleApp"
    decomp_settings()
    files { "src/cli/**.hpp", "src/cli/**.cpp" }
    link_decomp()

project "decomp_tests"
    kind "ConsoleApp"
    decomp_settings()
    files { "tests/**.hpp", "tests/**.cpp" }
    removefiles { "tests/fixtures/**" }
    includedirs { "tests" }
    defines { "DECOMP_SOURCE_DIR=\"" .. path.getabsolute(".") .. "\"" }
    link_decomp()

-- The supervision GUI (docs/ui.md). decomp_gui_lib holds the shell and the views and needs no window
-- system; decomp-gui adds GLFW and the OpenGL 3 backend; decomp_gui_tests renders the shell headless
-- with ImGui's null backend.
local function gui_settings()
    decomp_settings()
    use_imgui()
end

-- GUI executables link the GUI library, decomp_lib, the third-party libraries, ImGui and friends, then
-- system libraries. "linkgroups" lets GNU ld resolve references between these static libraries in any
-- order (ImGui calls item hooks that decomp_gui_lib defines).
local function link_gui(extra)
    links { "decomp_gui_lib", "decomp_lib" }
    link_thirdparty()
    link_imgui()
    links(extra or {})
    linkgroups "On"
    filter "system:windows"
        links { "winhttp", "user32", "gdi32", "shell32", "imm32" }
    filter "system:linux"
        links { "curl", "pthread", "dl", "m", "rt" }
    filter "system:macosx"
        links { "curl" }
    filter {}
end

project "decomp_gui_lib"
    kind "StaticLib"
    gui_settings()
    files { "src/gui/**.hpp", "src/gui/**.cpp", "src/gui/**.h", "src/gui/**.inc" }
    removefiles { "src/gui/platform/**" }

project "decomp-gui"
    kind "WindowedApp"
    gui_settings()
    files { "src/gui/platform/**.hpp", "src/gui/platform/**.cpp" }
    use_imgui_backends()
    -- No console window, yet a plain main(); AttachConsole() reaches the parent's console.
    filter "system:windows"
        entrypoint "mainCRTStartup"
    filter {}
    link_gui({ "glfw" })
