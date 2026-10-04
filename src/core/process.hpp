#pragma once

#include "core/result.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace decomp {

struct ProcessSpec {
    // argv[0] is the program: an absolute/relative path or a name resolved through PATH.
    std::vector<std::string> argv;
    std::filesystem::path cwd;  // empty = inherit
    // Environment overrides applied on top of the parent environment (nullopt value = unset).
    std::vector<std::pair<std::string, std::optional<std::string>>> env;
    std::chrono::milliseconds timeout{0};  // 0 = no timeout
    std::string stdin_data;
};

struct ProcessResult {
    int exit_code = -1;
    std::string out;
    std::string err;
    bool timed_out = false;
    std::chrono::milliseconds duration{0};

    bool ok() const { return !timed_out && exit_code == 0; }
};

// Runs a process to completion, capturing stdout and stderr. Fails only when the process cannot be
// started; a non-zero exit code or timeout is reported in the result.
Result<ProcessResult> run_process(const ProcessSpec& spec);

// Reads an environment variable of the current process.
std::optional<std::string> get_env(std::string_view name);

// Quotes one argument following the MSVCRT/CommandLineToArgvW rules.
std::string quote_windows_arg(std::string_view arg);
std::string build_windows_command_line(std::span<const std::string> argv);

// Character separating entries in PATH-like variables on the host (';' on Windows, ':' elsewhere).
char path_list_separator();

} // namespace decomp
