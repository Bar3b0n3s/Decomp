#pragma once

#include "core/json.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace decomp::matching {

enum class ToolchainKind : u8 { msvc, clang_cl, gcc, clang };
std::string_view to_string(ToolchainKind kind);
std::optional<ToolchainKind> toolchain_kind_from_string(std::string_view s);

// How to run one compiler. MSVC-style kinds take /c /Fo; GCC-style kinds take -c -o.
struct Toolchain {
    std::string name;
    ToolchainKind kind = ToolchainKind::msvc;
    std::string compiler;                  // absolute path or a name found through PATH
    std::vector<std::string> wrapper;      // e.g. {"wine"} to run a Windows compiler on Linux
    std::vector<std::string> flags;        // always passed (e.g. --target=i686-pc-windows-msvc)
    std::vector<std::string> include_dirs;
    std::vector<std::pair<std::string, std::string>> env;          // variables to set
    std::vector<std::pair<std::string, std::string>> env_prepend;  // prepended to PATH-like variables
    std::string description;
    int timeout_seconds = 120;
    bool builtin = false;  // auto-detected, not stored in the registry file

    bool msvc_style() const { return kind == ToolchainKind::msvc || kind == ToolchainKind::clang_cl; }
    Json to_json() const;
    static Result<Toolchain> from_json(std::string name, const Json& j);
};

// User-level registry (%APPDATA%\decomp\toolchains.json, ~/.config/decomp/toolchains.json) plus
// auto-detected clang-cl toolchains. DECOMP_TOOLCHAINS overrides the file location.
class ToolchainRegistry {
public:
    static std::filesystem::path default_path();
    static Result<ToolchainRegistry> load(const std::optional<std::filesystem::path>& path = {});

    const Toolchain* find(std::string_view name) const;
    const std::vector<Toolchain>& toolchains() const { return toolchains_; }
    void upsert(Toolchain toolchain);
    bool remove(std::string_view name);
    Result<void> save() const;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    std::vector<Toolchain> toolchains_;
};

// Locates clang-cl (PATH, then common LLVM install locations).
std::optional<std::string> find_clang_cl();
// Locates another LLVM tool (e.g. "lld-link"): next to clang-cl first, then PATH.
std::optional<std::string> find_llvm_tool(std::string_view name);

struct Diagnostic {
    std::string file;
    int line = 0;
    int column = 0;
    std::string severity;  // error, warning, note, fatal error
    std::string code;      // C2065 (MSVC) when present
    std::string message;
};

// Parses MSVC-style "file(line[,col]): error C1234: msg" and GCC-style "file:line:col: error: msg".
std::vector<Diagnostic> parse_diagnostics(std::string_view output);
std::string format_diagnostics(const std::vector<Diagnostic>& diagnostics, usize max = 20);

struct CompileRequest {
    std::string source;
    std::vector<std::string> flags;                      // project flags, after the toolchain's
    std::vector<std::filesystem::path> include_dirs;     // project include directories
    std::string file_name = "candidate.cpp";
};

struct CompileResult {
    bool ok = false;
    std::vector<std::byte> object_data;  // the compiled object when ok
    std::filesystem::path object;        // cache location of the object (empty when not cached)
    std::vector<Diagnostic> diagnostics;
    std::string output;            // combined compiler output
    std::vector<std::string> command;
    std::chrono::milliseconds duration{0};
    bool cached = false;
    bool timed_out = false;
};

class Compiler {
public:
    // `work_dir` receives per-compile directories; `cache_dir` (optional) keeps objects by content hash.
    Compiler(Toolchain toolchain, std::filesystem::path work_dir, std::optional<std::filesystem::path> cache_dir = {});
    Result<CompileResult> compile(const CompileRequest& request) const;
    const Toolchain& toolchain() const { return toolchain_; }

    // Command line for compiling `source` into `object` (exposed for tests and the UI).
    std::vector<std::string> command_line(const CompileRequest& request, const std::filesystem::path& source,
                                          const std::filesystem::path& object) const;

private:
    std::string cache_key(const CompileRequest& request) const;
    Toolchain toolchain_;
    std::filesystem::path work_dir_;
    std::optional<std::filesystem::path> cache_dir_;
};

} // namespace decomp::matching
