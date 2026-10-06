#pragma once

// Running the original linker for a relink (docs/architecture.md#relinking): the flags that make it
// reproduce the target's headers, and the run itself through a response file.

#include "core/result.hpp"
#include "formats/pe.hpp"
#include "matching/toolchain.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace decomp::relink {

enum class LinkerKind : u8 { msvc, lld };
std::string_view to_string(LinkerKind kind);

struct Linker {
    std::string path;  // link.exe, lld-link: a path or a name found through PATH
    LinkerKind kind = LinkerKind::msvc;
    std::vector<std::string> wrapper;
    std::vector<std::pair<std::string, std::string>> env, env_prepend;  // as the toolchain's
};

// The linker a relink runs: `configured` (a path or a name) when given, else the toolchain's: lld-link
// for clang-cl, link.exe beside cl.exe (or on PATH) for MSVC. The kind follows the file name. The
// toolchain's wrapper and environment apply.
Linker linker_for(const matching::Toolchain& toolchain, const std::string& configured = {});

// Flags that make the linker write the image's headers: machine, subsystem and its version, OS and image
// versions, base address, section and file alignment, stack and heap sizes, DLL and file
// characteristics, debug information (with the CodeView record's PDB path) and /Brepro when it has a
// repro entry, /release when it has a checksum. `entry` is the entry point's symbol as the linker's
// /ENTRY takes it (empty: none, /NOENTRY for a DLL).
std::vector<std::string> image_link_flags(const pe::Image& image, std::string_view entry);

struct LinkRequest {
    std::vector<std::string> inputs;  // objects and libraries, in link order
    std::vector<std::string> flags;
    std::filesystem::path output;
    std::filesystem::path pdb;        // when the flags ask for debug information
    std::filesystem::path work_dir;   // the response file goes there
    std::chrono::seconds timeout{600};
    std::function<bool()> cancelled;
};

struct LinkResult {
    bool ok = false;
    int exit_code = -1;
    bool timed_out = false;
    bool cancelled = false;
    std::string output;
    std::vector<std::string> command;  // the full command, as in the response file
    std::chrono::milliseconds duration{0};
};

Result<LinkResult> run_linker(const Linker& linker, const LinkRequest& request);

} // namespace decomp::relink
