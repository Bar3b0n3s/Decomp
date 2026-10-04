#include "agent/sse.hpp"

namespace decomp::agent {

void SseParser::feed(std::string_view chunk) {
    std::size_t pos = 0;
    if (skip_lf_ && !chunk.empty()) {
        if (chunk.front() == '\n') pos = 1;
        skip_lf_ = false;
    }
    while (pos < chunk.size()) {
        std::size_t end = chunk.find_first_of("\r\n", pos);
        if (end == std::string_view::npos) {
            line_.append(chunk.substr(pos));
            return;
        }
        if (line_.empty()) {
            process_line(chunk.substr(pos, end - pos));
        } else {
            line_.append(chunk.substr(pos, end - pos));
            process_line(line_);
            line_.clear();
        }
        if (chunk[end] == '\r') {
            if (end + 1 < chunk.size()) {
                if (chunk[end + 1] == '\n') ++end;
            } else {
                skip_lf_ = true;
            }
        }
        pos = end + 1;
    }
}

void SseParser::finish() {
    skip_lf_ = false;
    if (!line_.empty()) {
        std::string last = std::move(line_);
        line_.clear();
        process_line(last);
    }
    dispatch();
}

void SseParser::reset() {
    line_.clear();
    event_.clear();
    data_.clear();
    last_id_.clear();
    has_data_ = false;
    skip_lf_ = false;
    first_line_ = true;
    dispatched_ = 0;
}

void SseParser::process_line(std::string_view line) {
    if (first_line_) {
        first_line_ = false;
        if (line.starts_with("\xEF\xBB\xBF")) line.remove_prefix(3);
    }
    if (line.empty()) {
        dispatch();
        return;
    }
    if (line.front() == ':') return;  // comment

    std::string_view field = line;
    std::string_view value;
    if (const auto colon = line.find(':'); colon != std::string_view::npos) {
        field = line.substr(0, colon);
        value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    }

    if (field == "event") {
        event_.assign(value);
    } else if (field == "data") {
        data_.append(value);
        data_.push_back('\n');
        has_data_ = true;
    } else if (field == "id") {
        if (value.find('\0') == std::string_view::npos) last_id_.assign(value);
    }
    // "retry" and unknown fields are ignored.
}

void SseParser::dispatch() {
    if (!has_data_) {
        event_.clear();
        return;
    }
    if (!data_.empty() && data_.back() == '\n') data_.pop_back();
    ++dispatched_;
    if (callback_) callback_(event_, data_);
    event_.clear();
    data_.clear();
    has_data_ = false;
}

} // namespace decomp::agent
