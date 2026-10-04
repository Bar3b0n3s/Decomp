#include "cli/common.hpp"
#include "core/log.hpp"
#include "core/version.hpp"

#include <CLI/CLI.hpp>

#include <functional>
#include <print>

int main(int argc, char** argv) {
    using namespace decomp;
    CLI::App app{"decomp - AI-assisted matching decompilation for x86/x86-64 binaries"};
    app.set_version_flag("--version", kVersion);
    app.require_subcommand(1);
    cli::GlobalOptions g;
    app.add_flag("--json", g.json, "Machine-readable JSON output");
    app.add_flag("-v,--verbose", g.verbose, "More logging (repeat for trace)");
    app.add_flag("-q,--quiet", g.quiet, "Only errors");
    app.add_option("-C,--project", g.project, "Project directory (default: search upwards from the current directory)");
    app.parse_complete_callback([&g] {
        if (g.quiet) log::set_level(log::Level::error);
        else if (g.verbose >= 2) log::set_level(log::Level::trace);
        else if (g.verbose == 1) log::set_level(log::Level::debug);
    });

    cli::register_analysis_commands(app, g);
    cli::register_matching_commands(app, g);
    cli::register_project_commands(app, g);
    cli::register_agent_commands(app, g);
    cli::register_run_commands(app, g);

    // Global options may also follow the subcommand (`decomp status --json`).
    std::function<void(CLI::App*)> allow_fallthrough = [&](CLI::App* parent) {
        for (CLI::App* sub : parent->get_subcommands([](CLI::App*) { return true; })) {
            sub->fallthrough();
            allow_fallthrough(sub);
        }
    };
    allow_fallthrough(&app);

    try {
        app.parse(argc, argv);
    } catch (const CLI::RuntimeError& e) {
        return e.get_exit_code();  // command finished; carries its exit code
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }
    return 0;
}
