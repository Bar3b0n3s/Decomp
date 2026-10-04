#include "events/bus.hpp"

#include "core/fs.hpp"
#include "core/strings.hpp"

#include <algorithm>

namespace decomp::events {

int EventBus::subscribe(Handler handler) {
    std::lock_guard lock(mutex_);
    int id = next_id_++;
    handlers_.emplace(id, std::move(handler));
    return id;
}

void EventBus::unsubscribe(int id) {
    std::lock_guard lock(mutex_);
    handlers_.erase(id);
}

Event EventBus::publish(Payload payload, int worker) {
    Event e;
    e.seq = next_seq_.fetch_add(1);
    e.time = std::chrono::system_clock::now();
    e.run = run_;
    e.worker = worker;
    e.payload = std::move(payload);
    std::vector<Handler> handlers;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, h] : handlers_) handlers.push_back(h);
    }
    for (const auto& h : handlers) h(e);
    return e;
}

Result<std::unique_ptr<JsonlEventLog>> JsonlEventLog::open(const std::filesystem::path& path) {
    TRY(fs::create_directories(path.parent_path()));
    auto log = std::make_unique<JsonlEventLog>();
    log->path_ = path;
    log->out_.open(path, std::ios::app | std::ios::binary);
    if (!log->out_) return make_error(ErrorCode::io, "cannot open event log '{}'", fs::to_utf8(path));
    return log;
}

void JsonlEventLog::write(const Event& e) {
    // Stream deltas are high-volume and fully captured by the session transcript; keep the log lean.
    if (std::holds_alternative<StreamDelta>(e.payload)) return;
    auto line = dump_compact(to_json(e));
    std::lock_guard lock(mutex_);
    out_ << line << '\n';
    out_.flush();
}

Result<std::vector<Event>> read_event_log(const std::filesystem::path& path) {
    TRY_ASSIGN(auto text, fs::read_text(path));
    std::vector<Event> out;
    for (const auto& line : split_lines(text)) {
        auto j = parse_json(line);
        if (!j) continue;
        auto e = event_from_json(*j);
        if (e) out.push_back(std::move(*e));
    }
    std::ranges::stable_sort(out, {}, &Event::seq);
    return out;
}

} // namespace decomp::events
