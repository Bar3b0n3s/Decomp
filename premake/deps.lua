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
