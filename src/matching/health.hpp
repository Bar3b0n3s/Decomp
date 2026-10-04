#pragma once

#include "core/json.hpp"
#include "core/result.hpp"
#include "formats/image.hpp"
#include "matching/toolchain.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace decomp::matching {

// Result of compiling a one-line probe function with a toolchain (no compile cache).
struct HealthReport {
    bool ok = false;
    std::vector<std::string> command;
    std::string output;
    std::chrono::milliseconds duration{0};
    std::optional<Arch> arch;  // of the produced object
    usize functions = 0;       // function symbols in the produced object
    std::string object;        // "COFF x86 object, 1 functions", or why the output is not usable
    // The compiler's own version line ("Microsoft (R) 32-bit C/C++ Optimizing Compiler Version
    // 12.00.8804 for 80x86", "clang version 18.1.3 ..."); empty when it could not be read.
    std::string version;
};

// Asks the compiler for its version: cl.exe prints its banner when run without arguments, the others
// answer --version. Empty when the compiler cannot be run or prints nothing recognizable.
std::string detect_version(const Toolchain& toolchain);

Result<HealthReport> check_toolchain(const Toolchain& toolchain);
Json to_json(const HealthReport& report);

} // namespace decomp::matching
