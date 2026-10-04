#pragma once

#include "events/events.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace decomp::events {

// Thread-safe publish/subscribe. Handlers run on the publishing thread and must be quick.
class EventBus {
public:
    using Handler = std::function<void(const Event&)>;

    explicit EventBus(std::string run_id = {}) : run_(std::move(run_id)) {}

    int subscribe(Handler handler);
    void unsubscribe(int id);
    Event publish(Payload payload, int worker = -1);
    const std::string& run_id() const { return run_; }

private:
    std::string run_;
    std::atomic<u64> next_seq_{1};
    std::mutex mutex_;
    std::map<int, Handler> handlers_;
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
