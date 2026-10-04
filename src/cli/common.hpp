#pragma once

#include "analysis/program.hpp"
#include "core/json.hpp"
#include "core/result.hpp"

#include <CLI/CLI.hpp>

#include <functional>
#include <optional>
#include <string>

namespace decomp::cli {

struct GlobalOptions {
    bool json = false;
    int verbose = 0;
    bool quiet = false;
    std::string project;  // project directory (default: search upwards from cwd)
};

// Runs a command body, printing errors uniformly. Returns the process exit code.
int run(const GlobalOptions& g, const std::function<Result<int>()>& body);

// Opens the target program: `binary` if given, otherwise the current project's target.
Result<Program> open_program(const GlobalOptions& g, const std::string& binary, const std::string& pdb = {});

// Resolves a function argument (name or address) to an address, with a helpful error.
Result<u64> resolve_function(const Program& program, const std::string& text);

void print_json(const Json& value);

using Registrar = void (*)(CLI::App& app, GlobalOptions& g);
void register_analysis_commands(CLI::App& app, GlobalOptions& g);
void register_matching_commands(CLI::App& app, GlobalOptions& g);
void register_project_commands(CLI::App& app, GlobalOptions& g);
void register_agent_commands(CLI::App& app, GlobalOptions& g);

} // namespace decomp::cli
