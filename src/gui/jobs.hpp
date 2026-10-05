#pragma once

// Background work for the GUI (docs/ui.md: expensive derivations run off the render thread). Jobs run on
// a small thread pool; the UI thread polls their handles once per frame, so results are always consumed
// on the UI thread. Every job gets a CancelToken; a cancelled job's result is dropped.

#include "core/types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace decomp::gui {

// Thrown by CancelToken::throw_if_cancelled(); a job may also simply return early.
struct JobCancelled : std::exception {
    const char* what() const noexcept override { return "job cancelled"; }
};

// A cancellation flag shared between the UI (cancel) and the job (polls). Copies share the flag.
class CancelToken {
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    bool cancelled() const { return flag_->load(std::memory_order_acquire); }
    void cancel() const { flag_->store(true, std::memory_order_release); }
    void throw_if_cancelled() const {
        if (cancelled()) throw JobCancelled{};
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

template <class T>
using JobValue = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

namespace detail {

template <class V>
struct JobState {
    CancelToken token;
    std::mutex mutex;
    bool done = false;  // ran to the end (returned, threw or skipped because cancelled)
    std::optional<V> value;
    std::exception_ptr error;
};

// Jobs take a `const CancelToken&` or nothing.
template <class F>
decltype(auto) invoke_job(F& fn, const CancelToken& token) {
    if constexpr (std::is_invocable_v<F&, const CancelToken&>) return fn(token);
    else return fn();
}

template <class F>
using job_result_t = std::remove_cvref_t<decltype(invoke_job(std::declval<F&>(), std::declval<const CancelToken&>()))>;

} // namespace detail

// The result of a submitted job. Handles are cheap to copy and share one result; a default-constructed
// handle refers to no job.
template <class T>
class JobHandle {
public:
    using Value = JobValue<T>;

    JobHandle() = default;

    bool valid() const { return state_ != nullptr; }
    // The job has ended (whether or not its result was taken or dropped).
    bool finished() const {
        if (!state_) return false;
        std::lock_guard lock(state_->mutex);
        return state_->done;
    }
    // A result (or an exception) is waiting to be taken.
    bool ready() const {
        if (!state_ || state_->token.cancelled()) return false;
        std::lock_guard lock(state_->mutex);
        return state_->done && (state_->value || state_->error);
    }
    // Moves the result out, once; rethrows the job's exception. nullopt while the job runs, after
    // cancel() (the result is dropped) and once taken.
    std::optional<Value> take() {
        if (!state_ || state_->token.cancelled()) return std::nullopt;
        std::lock_guard lock(state_->mutex);
        if (!state_->done) return std::nullopt;
        if (auto error = std::exchange(state_->error, nullptr)) std::rethrow_exception(error);
        return std::exchange(state_->value, std::nullopt);
    }
    // Asks the job to stop (through its CancelToken) and drops its result.
    void cancel() const {
        if (state_) state_->token.cancel();
    }
    bool cancelled() const { return state_ && state_->token.cancelled(); }
    // The job runs, or its result (or exception) waits to be taken: its effect is still to come.
    bool pending() const { return valid() && !cancelled() && (!finished() || ready()); }
    const CancelToken* token() const { return state_ ? &state_->token : nullptr; }
    void reset() { state_.reset(); }

private:
    friend class JobQueue;
    explicit JobHandle(std::shared_ptr<detail::JobState<Value>> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::JobState<Value>> state_;
};

class JobQueue {
public:
    // threads == 0 picks min(4, max(1, hardware threads / 2)). `on_done` runs on the worker thread after
    // every job (the App wakes the UI with it).
    explicit JobQueue(unsigned threads = 0, std::function<void()> on_done = {});
    // Cancels every queued and running job and joins the workers.
    ~JobQueue();
    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    // Queues `fn` (called as fn(const CancelToken&) or fn()); jobs start in submission order.
    template <class F>
    auto submit(F&& fn) -> JobHandle<detail::job_result_t<std::decay_t<F>>> {
        using T = detail::job_result_t<std::decay_t<F>>;
        using V = JobValue<T>;
        auto state = std::make_shared<detail::JobState<V>>();
        auto task = [state, fn = std::forward<F>(fn)]() mutable {
            std::optional<V> value;
            std::exception_ptr error;
            if (!state->token.cancelled()) {
                try {
                    if constexpr (std::is_void_v<T>) {
                        detail::invoke_job(fn, state->token);
                        value.emplace();
                    } else {
                        value.emplace(detail::invoke_job(fn, state->token));
                    }
                } catch (...) {
                    error = std::current_exception();
                }
            }
            std::lock_guard lock(state->mutex);
            state->value = std::move(value);
            state->error = error;
            state->done = true;
        };
        push(std::move(task), state->token);
        return JobHandle<T>(std::move(state));
    }

    usize pending() const;  // queued or running
    unsigned threads() const { return static_cast<unsigned>(workers_.size()); }

private:
    struct Task {
        std::function<void()> run;
        CancelToken token;
    };
    void push(std::function<void()> run, CancelToken token);
    void work();

    std::function<void()> on_done_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    std::map<u64, CancelToken> running_;
    u64 next_running_ = 0;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

// Debounced work where only the newest request matters (recompile after an edit, filter a large table):
// each submit() cancels the previous job, and poll() delivers only the newest job's result. With a delay,
// the job waits that long first and never runs if a newer request arrives meanwhile.
template <class T>
class LatestWins {
public:
    using Value = JobValue<T>;

    template <class F>
    void submit(JobQueue& queue, F&& fn, std::chrono::milliseconds delay = std::chrono::milliseconds(0)) {
        static_assert(std::is_same_v<detail::job_result_t<std::decay_t<F>>, T>, "the job must return T");
        current_.cancel();
        ++generation_;
        if (delay.count() <= 0) {
            current_ = queue.submit(std::forward<F>(fn));
            return;
        }
        current_ = queue.submit([fn = std::forward<F>(fn), delay](const CancelToken& token) mutable -> T {
            auto until = std::chrono::steady_clock::now() + delay;
            while (std::chrono::steady_clock::now() < until) {
                token.throw_if_cancelled();
                std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(until - std::chrono::steady_clock::now(),
                                                                                           std::chrono::milliseconds(5)));
            }
            token.throw_if_cancelled();
            return detail::invoke_job(fn, token);
        });
    }

    // The newest job's result, once.
    std::optional<Value> poll() {
        if (!current_.valid()) return std::nullopt;
        auto value = current_.take();
        if (value) current_.reset();
        return value;
    }

    // A job runs or its result waits for poll() (so a caller that polls once per frame never sees a
    // finished job as idle before its result has been applied).
    bool busy() const { return current_.pending(); }
    void cancel() {
        current_.cancel();
        current_.reset();
    }
    // Increments with every submit(): lets a view tell which request a result answers.
    u64 generation() const { return generation_; }

private:
    JobHandle<T> current_;
    u64 generation_ = 0;
};

} // namespace decomp::gui
