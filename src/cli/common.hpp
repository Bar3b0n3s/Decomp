#pragma once

#include "analysis/program.hpp"
#include "core/json.hpp"
#include "core/result.hpp"
#include "matching/match.hpp"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

// Toolchain + flags + include dirs from the project (when there is one) and the command line.
Result<matching::MatchSetup> make_match_setup(const GlobalOptions& g, const std::string& toolchain, const std::vector<std::string>& extra_flags);

void print_json(const Json& value);
bool is_tty(std::FILE* stream);

// Ctrl+C for long-running commands: `on_interrupt(n)` runs on a watcher thread for the first and second
// press (n = 1, 2); a third press exits the process at once. The previous handler is restored on
// destruction.
class InterruptWatcher {
public:
    explicit InterruptWatcher(std::function<void(int)> on_interrupt);
    ~InterruptWatcher();
    InterruptWatcher(const InterruptWatcher&) = delete;
    InterruptWatcher& operator=(const InterruptWatcher&) = delete;

private:
    std::function<void(int)> on_interrupt_;
    void (*previous_)(int) = nullptr;
    std::jthread thread_;
};

using Registrar = void (*)(CLI::App& app, GlobalOptions& g);
void register_analysis_commands(CLI::App& app, GlobalOptions& g);
void register_matching_commands(CLI::App& app, GlobalOptions& g);
void register_project_commands(CLI::App& app, GlobalOptions& g);
void register_agent_commands(CLI::App& app, GlobalOptions& g);
void register_run_commands(CLI::App& app, GlobalOptions& g);

} // namespace decomp::cli
