#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace decomp::agent {

// Incremental Server-Sent Events parser following the WHATWG EventSource rules: chunks may split
// anywhere (even inside "\r\n"); lines end with "\n", "\r\n" or "\r"; ':' lines are comments; multiple
// `data:` lines are joined with '\n'; an event is dispatched on a blank line. The callback receives the
// `event:` field (empty when the event had none) and the data. Events without data are not dispatched.
class SseParser {
public:
    using Callback = std::function<void(std::string_view event, std::string_view data)>;

    explicit SseParser(Callback callback) : callback_(std::move(callback)) {}

    void feed(std::string_view chunk);
    // End of stream: processes an unterminated last line and dispatches a pending event that was not
    // followed by a blank line.
    void finish();
    // Forgets all state (for reusing the parser on a new stream).
    void reset();

    std::size_t events_dispatched() const { return dispatched_; }
    const std::string& last_event_id() const { return last_id_; }

private:
    void process_line(std::string_view line);
    void dispatch();

    Callback callback_;
    std::string line_;  // incomplete line carried over from previous chunks
    std::string event_;
    std::string data_;
    std::string last_id_;
    bool has_data_ = false;
    bool skip_lf_ = false;     // the previous chunk ended with '\r': a leading '\n' belongs to it
    bool first_line_ = true;   // a UTF-8 BOM may precede the first line
    std::size_t dispatched_ = 0;
};

} // namespace decomp::agent
