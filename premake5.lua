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
