#include "core/version.hpp"

#include <CLI/CLI.hpp>

#include <print>

int main(int argc, char** argv) {
    CLI::App app{"decomp - AI-assisted matching decompilation for x86/x86-64 binaries"};
    app.set_version_flag("--version", decomp::kVersion);
    CLI11_PARSE(app, argc, argv);
    std::println("{}", app.help());
    return 0;
}
