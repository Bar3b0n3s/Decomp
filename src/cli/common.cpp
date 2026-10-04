#include "cli/common.hpp"

#include "core/log.hpp"
#include "project/project.hpp"
#include "project/setup.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <print>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace decomp::cli {

int run(const GlobalOptions& g, const std::function<Result<int>()>& body) {
    (void)g;
    auto result = body();
    if (!result) {
        log::error("{}", result.error().message);
        return 1;
    }
    return *result;
}

Result<Program> open_program(const GlobalOptions& g, const std::string& binary, const std::string& pdb) {
    std::optional<std::filesystem::path> pdb_path;
    if (!pdb.empty()) pdb_path = std::filesystem::path(pdb);
    if (!binary.empty()) return Program::open(binary, pdb_path);
    TRY_ASSIGN(auto project, project::Project::find(g.project));
    return project.open_program();
}

Result<u64> resolve_function(const Program& program, const std::string& text) {
    auto va = program.resolve(text);
    if (!va) return make_error(ErrorCode::not_found, "unknown function '{}' (use a name from `decomp funcs` or an address like 0x401000)", text);
    return *va;
}

Result<matching::MatchSetup> make_match_setup(const GlobalOptions& g, const std::string& toolchain, const std::vector<std::string>& extra_flags) {
    auto project = project::Project::find(g.project);
    return project::make_match_setup(project ? &*project : nullptr, toolchain, extra_flags);
}

void print_json(const Json& value) { std::println("{}", dump_pretty(value)); }

bool is_tty(std::FILE* stream) {
#ifdef _WIN32
    return _isatty(_fileno(stream)) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

namespace {

// The handler only bumps a counter; the watcher thread turns presses into commands.
std::atomic<int> g_interrupts{0};

void on_sigint(int) {
    if (g_interrupts.fetch_add(1) + 1 >= 3) std::_Exit(130);
    std::signal(SIGINT, on_sigint);
}

} // namespace

InterruptWatcher::InterruptWatcher(std::function<void(int)> on_interrupt) : on_interrupt_(std::move(on_interrupt)) {
    g_interrupts = 0;
    previous_ = std::signal(SIGINT, on_sigint);
    thread_ = std::jthread([this](std::stop_token stop) {
        int seen = 0;
        while (!stop.stop_requested()) {
            const int n = g_interrupts.load();
            if (n > seen) {
                seen = n;
                on_interrupt_(n);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

InterruptWatcher::~InterruptWatcher() {
    thread_.request_stop();
    thread_.join();
    if (previous_ != SIG_ERR) std::signal(SIGINT, previous_);
}

} // namespace decomp::cli
