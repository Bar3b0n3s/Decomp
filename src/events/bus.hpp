#pragma once

#include "events/events.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace decomp::events {

// Serialized publish/subscribe: publish() assigns the sequence number and runs every handler under
// one lock, so subscribers see events one at a time in seq order whatever thread publishes. A publish
// from inside a handler (same thread) is queued and delivered right after the current event.
// Handlers must be quick, must not block on other publishers, and must not call into components that
// publish while holding their own locks. After unsubscribe() returns, the handler is not running and
// will not run again (unless called from that handler itself).
class EventBus {
public:
    using Handler = std::function<void(const Event&)>;

    explicit EventBus(std::string run_id = {}) : run_(std::move(run_id)) {}

    int subscribe(Handler handler);
    void unsubscribe(int id);
    Event publish(Payload payload, int worker = -1);
    const std::string& run_id() const { return run_; }
    // Continues numbering after `last` (a resumed run appends to its existing log).
    void set_next_seq(u64 next);
    u64 last_seq() const { return next_seq_.load() - 1; }

private:
    Event make_event(Payload payload, int worker);
    void deliver(const Event& e);

    std::string run_;
    std::atomic<u64> next_seq_{1};
    std::recursive_mutex mutex_;
    std::atomic<std::thread::id> dispatching_{};
    std::vector<Event> pending_;  // published from inside a handler
    std::map<int, std::shared_ptr<Handler>> handlers_;
    int next_id_ = 1;
};

// Appends every event as one JSON line (the run's durable record; replayable into RunState).
class JsonlEventLog {
public:
    static Result<std::unique_ptr<JsonlEventLog>> open(const std::filesystem::path& path);
    void write(const Event& e);
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    std::ofstream out_;
    std::mutex mutex_;
};

// Reads an events.jsonl file (unknown or malformed lines are skipped, events sorted by seq).
Result<std::vector<Event>> read_event_log(const std::filesystem::path& path);

} // namespace decomp::events
