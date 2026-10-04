#include "core/file_lock.hpp"
#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/process.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace decomp;
using namespace std::chrono_literals;

namespace {

ProcessSpec shell(const std::string& script) {
    ProcessSpec spec;
#ifdef _WIN32
    spec.argv = {"cmd.exe", "/c", script};
#else
    spec.argv = {"/bin/sh", "-c", script};
#endif
    return spec;
}

std::string sleep_command(int seconds) {
#ifdef _WIN32
    return std::format("ping -n {} 127.0.0.1 > nul", seconds + 1);
#else
    return std::format("sleep {}", seconds);
#endif
}

} // namespace

#ifndef _WIN32
TEST_CASE("concurrent processes do not inherit each other's pipes") {
    // Long-running processes started from other threads must not keep a quick process's pipes open
    // (they would if the pipes were inherited across fork/exec).
    std::atomic<bool> done{false};
    std::vector<std::thread> sleepers;
    for (int i = 0; i < 3; ++i)
        sleepers.emplace_back([&] {
            while (!done) (void)run_process(shell("sleep 1"));
        });
    std::atomic<int> slow{0}, failed{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t)
        workers.emplace_back([&, t] {
            for (int i = 0; i < 50; ++i) {
                auto spec = shell(std::format("echo {}-{}", t, i));
                spec.timeout = 10s;
                const auto start = std::chrono::steady_clock::now();
                auto r = run_process(spec);
                const auto took = std::chrono::steady_clock::now() - start;
                if (!r || !r->ok() || r->out != std::format("{}-{}\n", t, i)) ++failed;
                if (took > 700ms) ++slow;
            }
        });
    for (auto& w : workers) w.join();
    done = true;
    for (auto& s : sleepers) s.join();
    CHECK(failed == 0);
    CHECK(slow == 0);
}
#endif

TEST_CASE("run_process can be cancelled") {
    auto spec = shell(sleep_command(5));
    const auto start = std::chrono::steady_clock::now();
    spec.cancelled = [start] { return std::chrono::steady_clock::now() - start > 150ms; };
    auto r = run_process(spec).value();
    CHECK(r.cancelled);
    CHECK_FALSE(r.ok());
    CHECK(std::chrono::steady_clock::now() - start < 2s);
}

TEST_CASE("file locks conflict between holders, shared locks coexist") {
    auto dir = fs::TempDir::create("decomp-lock").value();
    const auto path = dir.path() / "sub" / "project.lock";
    {
        auto a = FileLock::acquire(path, FileLock::Mode::exclusive).value();
        CHECK(a.held());
        CHECK_FALSE(FileLock::try_acquire(path, FileLock::Mode::exclusive).value().has_value());
        CHECK_FALSE(FileLock::try_acquire(path, FileLock::Mode::shared).value().has_value());
        auto waited = FileLock::acquire(path, FileLock::Mode::exclusive, 50ms);
        REQUIRE_FALSE(waited);
        CHECK(waited.error().code == ErrorCode::timeout);
    }
    {
        auto s1 = FileLock::acquire(path, FileLock::Mode::shared).value();
        auto s2 = FileLock::try_acquire(path, FileLock::Mode::shared).value();
        CHECK(s2.has_value());
        CHECK_FALSE(FileLock::try_acquire(path, FileLock::Mode::exclusive).value().has_value());
    }
    // Released locks can be taken again; a waiter gets the lock once the holder lets go.
    auto holder = FileLock::acquire(path, FileLock::Mode::exclusive).value();
    std::thread releaser([&] {
        std::this_thread::sleep_for(100ms);
        holder.release();
    });
    auto next = FileLock::acquire(path, FileLock::Mode::exclusive, 5s);
    releaser.join();
    CHECK(next.has_value());
}

TEST_CASE("log sinks: removal waits for in-flight calls; context and recent entries") {
    std::atomic<bool> in_sink{false}, sink_done{false};
    std::atomic<int> calls{0};
    int id = log::add_sink([&](const log::Entry& e) {
        if (e.message != "slow sink test") return;
        ++calls;
        in_sink = true;
        std::this_thread::sleep_for(200ms);
        sink_done = true;
    });
    std::thread writer([] {
        log::ScopedContext ctx(3, "s-42");
        log::warn("slow sink test");
    });
    while (!in_sink) std::this_thread::yield();
    log::remove_sink(id);
    CHECK(sink_done);  // removal returned only after the call finished
    writer.join();
    log::warn("slow sink test");  // no longer delivered
    CHECK(calls == 1);

    auto recent = log::recent(10);
    REQUIRE_FALSE(recent.empty());
    bool found = false;
    for (const auto& e : recent)
        if (e.message == "slow sink test" && e.worker == 3 && e.session == "s-42") found = true;
    CHECK(found);

    // A sink may remove itself and may log without being called again recursively.
    int self = 0;
    std::atomic<int> self_calls{0};
    self = log::add_sink([&](const log::Entry& e) {
        if (e.message != "self removal") return;
        ++self_calls;
        log::warn("self removal");
        log::remove_sink(self);
    });
    log::warn("self removal");
    CHECK(self_calls == 1);
}
