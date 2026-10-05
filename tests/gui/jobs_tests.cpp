#include "gui/jobs.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>

using namespace decomp;
using namespace decomp::gui;
using namespace std::chrono_literals;

namespace {

// Polls `ready` like the UI does once per frame, for up to five seconds.
template <class F>
bool eventually(F&& ready) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return ready();
}

} // namespace

TEST_CASE("jobs run in the background and their results are taken once") {
    std::atomic<int> wakes = 0;
    JobQueue jobs(2, [&] { ++wakes; });
    CHECK(jobs.threads() == 2);

    auto sum = jobs.submit([] { return 2 + 3; });
    auto text = jobs.submit([](const CancelToken& token) { return token.cancelled() ? std::string("cancelled") : std::string("done"); });
    auto nothing = jobs.submit([] {});
    REQUIRE(eventually([&] { return sum.ready() && text.ready() && nothing.ready(); }));
    CHECK(sum.take() == 5);
    CHECK_FALSE(sum.take());  // taken
    CHECK(text.take() == std::string("done"));
    CHECK(nothing.take().has_value());
    CHECK(sum.finished());
    CHECK(eventually([&] { return wakes.load() == 3; }));
    CHECK(eventually([&] { return jobs.pending() == 0; }));

    JobHandle<int> none;
    CHECK_FALSE(none.valid());
    CHECK_FALSE(none.ready());
    CHECK_FALSE(none.take());
}

TEST_CASE("a job's exception is rethrown on the UI thread") {
    JobQueue jobs(1);
    auto failing = jobs.submit([]() -> int { throw std::runtime_error("boom"); });
    REQUIRE(eventually([&] { return failing.ready(); }));
    CHECK_THROWS_WITH_AS(failing.take(), "boom", std::runtime_error);
    CHECK_FALSE(failing.take());
}

TEST_CASE("cancelled jobs drop their results") {
    JobQueue jobs(1);
    std::latch started(1), release(1);
    std::atomic<bool> saw_cancel = false;

    // Occupies the only worker until released.
    auto blocker = jobs.submit([&](const CancelToken& token) {
        started.count_down();
        release.wait();
        saw_cancel = token.cancelled();
        return 1;
    });
    started.wait();

    std::atomic<bool> queued_ran = false;
    auto queued = jobs.submit([&] {
        queued_ran = true;
        return 2;
    });
    queued.cancel();   // cancelled before it started: it never runs
    blocker.cancel();  // cancelled while running: the job sees the token
    release.count_down();

    REQUIRE(eventually([&] { return blocker.finished() && queued.finished(); }));
    CHECK(saw_cancel);
    CHECK_FALSE(queued_ran);
    CHECK(blocker.cancelled());
    CHECK_FALSE(blocker.ready());
    CHECK_FALSE(blocker.take());  // dropped
    CHECK_FALSE(queued.take());
}

TEST_CASE("throw_if_cancelled ends a job early") {
    JobQueue jobs(1);
    std::latch started(1);
    std::atomic<int> iterations = 0;
    auto job = jobs.submit([&](const CancelToken& token) {
        started.count_down();
        for (;;) {
            token.throw_if_cancelled();
            ++iterations;
            std::this_thread::sleep_for(1ms);
        }
        return 0;
    });
    started.wait();
    job.cancel();
    REQUIRE(eventually([&] { return job.finished(); }));
    CHECK_FALSE(job.take());
}

TEST_CASE("latest wins: only the newest request's result is delivered") {
    JobQueue jobs(2);
    LatestWins<int> latest;
    std::atomic<int> runs = 0;
    for (int i = 1; i <= 5; ++i)
        latest.submit(jobs, [i, &runs](const CancelToken&) {
            ++runs;
            std::this_thread::sleep_for(2ms);
            return i;
        });
    CHECK(latest.generation() == 5);
    std::optional<int> result;
    REQUIRE(eventually([&] {
        result = latest.poll();
        return result.has_value();
    }));
    CHECK(*result == 5);
    CHECK_FALSE(latest.poll());
    CHECK_FALSE(latest.busy());
}

TEST_CASE("a finished job stays pending until its result is taken") {
    JobQueue jobs(1);
    auto handle = jobs.submit([] { return 7; });
    REQUIRE(eventually([&] { return handle.finished(); }));
    CHECK(handle.pending());  // done, but the caller has not seen the result yet
    CHECK(handle.take() == 7);
    CHECK_FALSE(handle.pending());
    // An exception is the result too: pending until it is rethrown.
    auto failing = jobs.submit([]() -> int { throw std::runtime_error("no"); });
    REQUIRE(eventually([&] { return failing.finished(); }));
    CHECK(failing.pending());
    CHECK_THROWS(failing.take());
    CHECK_FALSE(failing.pending());
    // LatestWins is busy until poll() has delivered the newest result.
    LatestWins<int> latest;
    latest.submit(jobs, [] { return 1; });
    std::optional<int> got;
    REQUIRE(eventually([&] {
        if (!latest.busy()) return false;  // never idle before its result is polled
        got = latest.poll();
        return got.has_value();
    }));
    CHECK_FALSE(latest.busy());
}

TEST_CASE("latest wins with a delay debounces bursts") {
    JobQueue jobs(2);
    LatestWins<std::string> latest;
    std::atomic<int> runs = 0;
    for (const char* text : {"i", "in", "int", "int a"})
        latest.submit(
            jobs,
            [text, &runs] {
                ++runs;
                return std::string(text);
            },
            50ms);
    CHECK(latest.busy());
    std::optional<std::string> result;
    REQUIRE(eventually([&] {
        result = latest.poll();
        return result.has_value();
    }));
    CHECK(*result == "int a");
    CHECK(runs == 1);  // the superseded requests never ran

    latest.submit(jobs, [] { return std::string("late"); }, 20ms);
    latest.cancel();
    CHECK_FALSE(latest.busy());
    std::this_thread::sleep_for(40ms);
    CHECK_FALSE(latest.poll());
}

TEST_CASE("destroying the queue cancels pending jobs and joins running ones") {
    std::atomic<bool> stopped_early = false;
    std::atomic<bool> second_ran = false;
    JobHandle<int> running, queued;
    {
        JobQueue jobs(1);
        std::latch started(1);
        running = jobs.submit([&](const CancelToken& token) {
            started.count_down();
            while (!token.cancelled()) std::this_thread::sleep_for(1ms);
            stopped_early = true;
            return 1;
        });
        queued = jobs.submit([&] {
            second_ran = true;
            return 2;
        });
        started.wait();
    }
    CHECK(stopped_early);
    CHECK_FALSE(second_ran);
    CHECK(running.cancelled());
    CHECK_FALSE(running.take());
    CHECK_FALSE(queued.take());
}
