#include "agent/sse.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace decomp::agent;

namespace {

struct SseEvent {
    std::string event;
    std::string data;
    bool operator==(const SseEvent&) const = default;
};

std::vector<SseEvent> parse_chunked(std::string_view text, std::size_t chunk) {
    std::vector<SseEvent> out;
    SseParser parser([&](std::string_view event, std::string_view data) { out.push_back({std::string(event), std::string(data)}); });
    for (std::size_t i = 0; i < text.size(); i += chunk) parser.feed(text.substr(i, chunk));
    parser.finish();
    return out;
}

std::vector<SseEvent> parse_split_at(std::string_view text, std::size_t cut) {
    std::vector<SseEvent> out;
    SseParser parser([&](std::string_view event, std::string_view data) { out.push_back({std::string(event), std::string(data)}); });
    parser.feed(text.substr(0, cut));
    parser.feed(text.substr(cut));
    parser.finish();
    return out;
}

const std::string kStream =
    ": comment line\n"
    "event: message_start\n"
    "data: {\"type\":\"message_start\"}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"a\":1,\n"
    "data: \"b\":2}\n"
    "\n"
    ":keepalive\n"
    "event: ping\n"
    "data: {\"type\": \"ping\"}\n"
    "\n"
    "data: no event name\n"
    "\n";

const std::vector<SseEvent> kExpected = {
    {"message_start", "{\"type\":\"message_start\"}"},
    {"content_block_delta", "{\"a\":1,\n\"b\":2}"},
    {"ping", "{\"type\": \"ping\"}"},
    {"", "no event name"},
};

} // namespace

TEST_CASE("SseParser dispatches events, joins multi-line data and skips comments") {
    CHECK(parse_chunked(kStream, kStream.size()) == kExpected);
}

TEST_CASE("SseParser handles chunk boundaries anywhere") {
    // Byte-by-byte and every other fixed chunk size.
    for (std::size_t chunk = 1; chunk <= 17; ++chunk) {
        CAPTURE(chunk);
        CHECK(parse_chunked(kStream, chunk) == kExpected);
    }
    // A single split at every position.
    for (std::size_t cut = 0; cut <= kStream.size(); ++cut) {
        CAPTURE(cut);
        CHECK(parse_split_at(kStream, cut) == kExpected);
    }
}

TEST_CASE("SseParser accepts CRLF and CR line endings, also when CRLF is split across chunks") {
    std::string crlf;
    for (char c : kStream) {
        if (c == '\n') crlf += "\r\n";
        else crlf += c;
    }
    for (std::size_t chunk = 1; chunk <= 7; ++chunk) {
        CAPTURE(chunk);
        CHECK(parse_chunked(crlf, chunk) == kExpected);
    }
    for (std::size_t cut = 0; cut <= crlf.size(); ++cut) {
        CAPTURE(cut);
        CHECK(parse_split_at(crlf, cut) == kExpected);
    }

    std::string cr;
    for (char c : kStream) cr += c == '\n' ? '\r' : c;
    CHECK(parse_chunked(cr, 1) == kExpected);
    CHECK(parse_chunked(cr, cr.size()) == kExpected);
}

TEST_CASE("SseParser flushes a trailing event at finish()") {
    // No blank line after the last event, and the last line has no terminator.
    const std::string text = "event: a\ndata: 1\n\nevent: message_stop\ndata: {\"type\":\"message_stop\"}";
    for (std::size_t chunk = 1; chunk <= text.size(); ++chunk) {
        CAPTURE(chunk);
        CHECK(parse_chunked(text, chunk) ==
              std::vector<SseEvent>{{"a", "1"}, {"message_stop", "{\"type\":\"message_stop\"}"}});
    }

    std::vector<SseEvent> out;
    SseParser parser([&](std::string_view e, std::string_view d) { out.push_back({std::string(e), std::string(d)}); });
    parser.feed("event: x\ndata: y\n");
    CHECK(out.empty());  // not dispatched before the blank line or finish()
    parser.finish();
    CHECK(out == std::vector<SseEvent>{{"x", "y"}});
    CHECK(parser.events_dispatched() == 1);
}

TEST_CASE("SseParser field edge cases") {
    // "data" without a colon is an empty data line; only one leading space is stripped; an event with
    // no data is not dispatched; unknown fields are ignored; id is remembered; a BOM is skipped.
    const std::string text =
        "\xEF\xBB\xBF"
        "event: empty\n"
        "data\n"
        "\n"
        "event: spaces\n"
        "data:  two\n"
        "data:none\n"
        "id: 42\n"
        "retry: 1000\n"
        "custom: x\n"
        "\n"
        "event: no-data\n"
        "\n"
        "data: last\n"
        "\n";
    for (std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{5}, text.size()}) {
        CAPTURE(chunk);
        std::vector<SseEvent> out;
        SseParser parser([&](std::string_view e, std::string_view d) { out.push_back({std::string(e), std::string(d)}); });
        for (std::size_t i = 0; i < text.size(); i += chunk) parser.feed(std::string_view(text).substr(i, chunk));
        parser.finish();
        CHECK(out == std::vector<SseEvent>{{"empty", ""}, {"spaces", " two\nnone"}, {"", "last"}});
        CHECK(parser.last_event_id() == "42");
    }
}

TEST_CASE("SseParser reset starts a fresh stream") {
    std::vector<SseEvent> out;
    SseParser parser([&](std::string_view e, std::string_view d) { out.push_back({std::string(e), std::string(d)}); });
    parser.feed("event: half\ndata: partial");
    parser.reset();
    parser.feed("data: fresh\n\n");
    CHECK(out == std::vector<SseEvent>{{"", "fresh"}});
}
