#ifndef _WIN32

#include "core/process.hpp"
#include "core/strings.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>

extern char** environ;

namespace decomp {
namespace {

std::map<std::string, std::string> build_env(const ProcessSpec& spec) {
    std::map<std::string, std::string> env;
    for (char** e = environ; e && *e; ++e) {
        std::string_view entry(*e);
        auto eq = entry.find('=');
        if (eq == std::string_view::npos) continue;
        env[std::string(entry.substr(0, eq))] = std::string(entry.substr(eq + 1));
    }
    for (const auto& [key, value] : spec.env) {
        if (value) env[key] = *value;
        else env.erase(key);
    }
    return env;
}

bool is_executable(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

std::optional<std::string> resolve_program(const std::string& program, const std::map<std::string, std::string>& env) {
    if (program.find('/') != std::string::npos) return program;
    auto it = env.find("PATH");
    std::string path = it != env.end() ? it->second : "/usr/local/bin:/usr/bin:/bin";
    for (auto dir : split(path, ':')) {
        std::string candidate = (dir.empty() ? std::string(".") : std::string(dir)) + "/" + program;
        if (is_executable(candidate)) return candidate;
    }
    return std::nullopt;
}

void set_nonblocking(int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK); }

} // namespace

Result<ProcessResult> run_process(const ProcessSpec& spec) {
    if (spec.argv.empty()) return make_error(ErrorCode::invalid_argument, "run_process: empty argv");
    auto env_map = build_env(spec);
    auto program = resolve_program(spec.argv[0], env_map);
    if (!program) return make_error(ErrorCode::not_found, "program '{}' not found in PATH", spec.argv[0]);

    // Everything the child needs is prepared before fork(): only async-signal-safe calls happen after.
    std::vector<std::string> env_strings;
    env_strings.reserve(env_map.size());
    for (const auto& [k, v] : env_map) env_strings.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<std::string> args = spec.argv;
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::string cwd = spec.cwd.empty() ? std::string() : spec.cwd.string();

    int out_pipe[2], err_pipe[2], in_pipe[2];
    if (::pipe(out_pipe) != 0) return make_error(ErrorCode::process, "pipe: {}", std::strerror(errno));
    if (::pipe(err_pipe) != 0) {
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        return make_error(ErrorCode::process, "pipe: {}", std::strerror(errno));
    }
    if (::pipe(in_pipe) != 0) {
        ::close(out_pipe[0]); ::close(out_pipe[1]); ::close(err_pipe[0]); ::close(err_pipe[1]);
        return make_error(ErrorCode::process, "pipe: {}", std::strerror(errno));
    }

    auto start = std::chrono::steady_clock::now();
    pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1], in_pipe[0], in_pipe[1]}) ::close(fd);
        return make_error(ErrorCode::process, "fork: {}", std::strerror(errno));
    }
    if (pid == 0) {
        ::setpgid(0, 0);  // own process group so a timeout can kill compiler subprocesses too
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1], in_pipe[0], in_pipe[1]}) ::close(fd);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(126);
        ::execve(program->c_str(), argv.data(), envp.data());
        ::_exit(127);
    }
    ::setpgid(pid, pid);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    ::close(in_pipe[0]);

    // Feed stdin (small payloads only; compilers don't read stdin) and close it.
    if (!spec.stdin_data.empty()) {
        usize written = 0;
        while (written < spec.stdin_data.size()) {
            auto n = ::write(in_pipe[1], spec.stdin_data.data() + written, spec.stdin_data.size() - written);
            if (n <= 0) break;
            written += static_cast<usize>(n);
        }
    }
    ::close(in_pipe[1]);

    set_nonblocking(out_pipe[0]);
    set_nonblocking(err_pipe[0]);

    ProcessResult result;
    bool out_open = true, err_open = true;
    char buf[65536];
    while (out_open || err_open) {
        int wait_ms = -1;
        if (spec.timeout.count() > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            if (elapsed >= spec.timeout) {
                result.timed_out = true;
                ::kill(-pid, SIGKILL);
                break;
            }
            wait_ms = static_cast<int>((spec.timeout - elapsed).count());
        }
        pollfd fds[2];
        int nfds = 0;
        if (out_open) fds[nfds++] = {out_pipe[0], POLLIN, 0};
        if (err_open) fds[nfds++] = {err_pipe[0], POLLIN, 0};
        int rc = ::poll(fds, static_cast<nfds_t>(nfds), wait_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < nfds; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            bool is_out = fds[i].fd == out_pipe[0];
            while (true) {
                auto n = ::read(fds[i].fd, buf, sizeof(buf));
                if (n > 0) {
                    (is_out ? result.out : result.err).append(buf, static_cast<usize>(n));
                    continue;
                }
                if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                    (is_out ? out_open : err_open) = false;
                }
                break;
            }
        }
    }
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
    if (!result.timed_out && result.exit_code == 127 && result.err.empty())
        return make_error(ErrorCode::process, "failed to execute '{}'", *program);
    return result;
}

} // namespace decomp

#endif // !_WIN32
