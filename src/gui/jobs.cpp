#include "gui/jobs.hpp"

#include <algorithm>

namespace decomp::gui {

JobQueue::JobQueue(unsigned threads, std::function<void()> on_done) : on_done_(std::move(on_done)) {
    if (threads == 0) threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    workers_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) workers_.emplace_back([this] { work(); });
}

JobQueue::~JobQueue() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (auto& task : queue_) task.token.cancel();
        queue_.clear();
        for (auto& [id, token] : running_) token.cancel();
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
}

usize JobQueue::pending() const {
    std::lock_guard lock(mutex_);
    return queue_.size() + running_.size();
}

void JobQueue::push(std::function<void()> run, CancelToken token) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            token.cancel();
            return;
        }
        queue_.push_back({std::move(run), std::move(token)});
    }
    cv_.notify_one();
}

void JobQueue::work() {
    for (;;) {
        Task task;
        u64 id = 0;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            task = std::move(queue_.front());
            queue_.pop_front();
            id = next_running_++;
            running_.emplace(id, task.token);
        }
        task.run();  // never throws: submit() wraps the job
        {
            std::lock_guard lock(mutex_);
            running_.erase(id);
        }
        if (on_done_) on_done_();
    }
}

} // namespace decomp::gui
