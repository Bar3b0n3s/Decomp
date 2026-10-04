#include "viewmodel/transcript.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <format>
#include <fstream>

namespace decomp::vm {

namespace {

constexpr std::string_view kGuidancePrefix = "[Supervisor guidance] ";
constexpr std::string_view kStatusPrefix = "[status]";

const Json* member(const Json& obj, std::string_view key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

std::string string_member(const Json& obj, std::string_view key) {
    const Json* v = member(obj, key);
    return v && v->is_string() ? v->get<std::string>() : std::string();
}

u64 u64_member(const Json& obj, std::string_view key) {
    const Json* v = member(obj, key);
    if (!v || !v->is_number()) return 0;
    return v->is_number_unsigned() ? v->get<u64>() : static_cast<u64>(std::max<i64>(0, v->get<i64>()));
}

events::TokenUsage usage_of(const Json& j) {
    return {json_int_or(j, "input_tokens", 0), json_int_or(j, "output_tokens", 0), json_int_or(j, "cache_creation_input_tokens", 0),
            json_int_or(j, "cache_read_input_tokens", 0)};
}

// Billed tokens of a response: the sum of the attempts when a fallback ran (as agent::response_cost
// counts them), else the top-level counts.
events::TokenUsage billed_usage(const Json& usage) {
    if (const Json* it = member(usage, "iterations"); it && it->is_array() && !it->empty()) {
        events::TokenUsage sum;
        for (const auto& entry : *it) sum += usage_of(entry);
        return sum;
    }
    return usage.is_object() ? usage_of(usage) : events::TokenUsage{};
}

// Tool result content: a string, or content blocks (text blocks joined; others as "[<type>]").
std::string content_text(const Json& content) {
    if (content.is_string()) return content.get<std::string>();
    if (!content.is_array()) return content.is_null() ? std::string() : dump_compact(content);
    std::string out;
    for (const auto& block : content) {
        if (!out.empty()) out += '\n';
        const std::string type = string_member(block, "type");
        out += type == "text" ? string_member(block, "text") : "[" + type + "]";
    }
    return out;
}

// The loop joins guidance and the status line into one text block, separated by blank lines.
std::vector<std::string_view> split_segments(std::string_view text) {
    std::vector<std::string_view> out;
    usize start = 0;
    for (usize pos = text.find("\n\n"); pos != std::string_view::npos; pos = text.find("\n\n", pos + 1)) {
        const std::string_view rest = text.substr(pos + 2);
        if (rest.starts_with(kGuidancePrefix) || rest.starts_with(kStatusPrefix)) {
            out.push_back(text.substr(start, pos - start));
            start = pos + 2;
        }
    }
    out.push_back(text.substr(start));
    return out;
}

// Items of one user message's content. When `brief` is given and still empty, the first text block goes
// there instead.
std::vector<UserItem> user_items(const Json& content, bool later_turn, std::string* brief) {
    std::vector<UserItem> items;
    bool has_results = false;
    if (content.is_array())
        for (const auto& b : content) has_results = has_results || string_member(b, "type") == "tool_result";
    auto add_text = [&](const std::string& text) {
        if (brief && brief->empty()) {
            *brief = text;
            brief = nullptr;
            return;
        }
        for (std::string_view segment : split_segments(text)) {
            UserItem& item = items.emplace_back();
            if (segment.starts_with(kGuidancePrefix)) {
                item.kind = UserItem::Kind::guidance;
                segment.remove_prefix(kGuidancePrefix.size());
            } else if (segment.starts_with(kStatusPrefix)) {
                item.kind = UserItem::Kind::status;
            } else {
                item.kind = later_turn && !has_results ? UserItem::Kind::nudge : UserItem::Kind::text;
            }
            item.text = std::string(segment);
        }
    };
    if (content.is_string()) {
        add_text(content.get<std::string>());
        return items;
    }
    if (!content.is_array()) return items;
    for (const auto& b : content) {
        const std::string type = string_member(b, "type");
        if (type == "text") {
            add_text(string_member(b, "text"));
            continue;
        }
        UserItem& item = items.emplace_back();
        if (type == "tool_result") {
            item.kind = UserItem::Kind::tool_result;
            item.tool_use_id = string_member(b, "tool_use_id");
            item.is_error = json_bool_or(b, "is_error", false);
            if (const Json* c = member(b, "content")) item.text = content_text(*c);
        } else {
            item.kind = UserItem::Kind::text;
            item.text = "[" + type + "]";
        }
    }
    return items;
}

ResponseBlock response_block(const Json& b) {
    ResponseBlock out;
    const std::string type = string_member(b, "type");
    if (type == "thinking") {
        out.kind = ResponseBlock::Kind::thinking;
        out.text = string_member(b, "thinking");
    } else if (type == "redacted_thinking") {
        out.kind = ResponseBlock::Kind::redacted_thinking;
    } else if (type == "text") {
        out.kind = ResponseBlock::Kind::text;
        out.text = string_member(b, "text");
    } else if (type == "tool_use") {
        out.kind = ResponseBlock::Kind::tool_use;
        out.tool_id = string_member(b, "id");
        out.tool_name = string_member(b, "name");
        if (const Json* input = member(b, "input")) out.input = *input;
    } else if (type == "fallback") {
        out.kind = ResponseBlock::Kind::fallback;
        if (const Json* from = member(b, "from")) out.from_model = string_member(*from, "model");
        if (const Json* to = member(b, "to")) out.to_model = string_member(*to, "model");
    } else {
        out.text = type;
    }
    return out;
}

} // namespace

TurnRecord& TranscriptReader::turn(int number) {
    auto it = std::ranges::lower_bound(doc_.turns, number, {}, &TurnRecord::turn);
    if (it != doc_.turns.end() && it->turn == number) return *it;
    it = doc_.turns.insert(it, TurnRecord{});
    it->turn = number;
    return *it;
}

void TranscriptReader::request(int number, const Json& messages, RequestKind kind, TimePoint time) {
    TurnRecord& rec = turn(number);
    rec.has_request = true;
    rec.request_time = time;
    // Pause markers come first; guidance records are matched with the guidance the message carries.
    std::vector<UserItem> recorded_guidance;
    for (auto& p : doc_.pending) {
        if (p.kind == UserItem::Kind::guidance) recorded_guidance.push_back(std::move(p));
        else rec.before.push_back(std::move(p));
    }
    doc_.pending.clear();

    std::vector<const Json*> users;
    if (messages.is_array()) {
        for (const auto& m : messages)
            if (string_member(m, "role") == "user") users.push_back(&m);
    }
    // A whole request after the first turn repeats the history: only its last user message is new.
    if (kind == RequestKind::history && users.size() > 1) users.erase(users.begin(), users.end() - 1);
    std::string* brief = kind == RequestKind::first && doc_.brief.empty() ? &doc_.brief : nullptr;
    for (const Json* m : users) {
        const Json* content = member(*m, "content");
        if (!content) continue;
        for (auto& item : user_items(*content, number > 1, brief)) {
            if (item.kind == UserItem::Kind::guidance) {
                auto match = std::ranges::find(recorded_guidance, item.text, &UserItem::text);
                if (match != recorded_guidance.end()) {
                    item.guidance_id = match->guidance_id;
                    item.time = match->time;
                    recorded_guidance.erase(match);
                }
            }
            rec.before.push_back(std::move(item));
        }
        brief = nullptr;
    }
    for (auto& g : recorded_guidance) rec.before.push_back(std::move(g));
}

void TranscriptReader::record(const Json& j) {
    const std::string type = string_member(j, "type");
    const TimePoint time = from_unix_ms(json_int_or(j, "time", 0));
    const int number = static_cast<int>(json_int_or(j, "turn", 0));
    if (type == "session") {
        TranscriptHeader h;
        h.session = string_member(j, "session");
        h.function = string_member(j, "function");
        h.display = string_member(j, "display");
        h.model = string_member(j, "model");
        h.effort = string_member(j, "effort");
        h.va = u64_member(j, "va");
        h.worker = static_cast<int>(json_int_or(j, "worker", -1));
        h.time = time;
        doc_.header = std::move(h);
    } else if (type == "request") {
        const Json* body = member(j, "body");
        const Json* messages = body ? member(*body, "messages") : nullptr;
        request(number, messages ? *messages : Json::array(), number <= 1 ? RequestKind::first : RequestKind::history, time);
    } else if (type == "request_delta") {
        const Json* messages = member(j, "messages");
        request(number, messages ? *messages : Json::array(), RequestKind::delta, time);
    } else if (type == "response") {
        TurnRecord& rec = turn(number);
        rec.has_response = true;
        rec.response_time = time;
        rec.response_id = string_member(j, "id");
        rec.model = string_member(j, "model");
        rec.stop_reason = string_member(j, "stop_reason");
        rec.request_id = string_member(j, "request_id");
        if (const Json* d = member(j, "stop_details")) rec.stop_details = *d;
        if (const Json* u = member(j, "usage")) rec.usage_raw = *u;
        rec.usage = billed_usage(rec.usage_raw);
        rec.cost_usd = json_number_or(j, "cost_usd", 0);
        rec.latency_ms = json_int_or(j, "latency_ms", 0);
        rec.ttft_ms = json_int_or(j, "ttft_ms", 0);
        rec.had_fallback = json_bool_or(j, "had_fallback", false);
        rec.blocks.clear();
        if (const Json* content = member(j, "content"); content && content->is_array())
            for (const auto& b : *content) rec.blocks.push_back(response_block(b));
    } else if (type == "tool") {
        ToolExchange t;
        t.id = string_member(j, "id");
        t.name = string_member(j, "name");
        if (const Json* input = member(j, "input")) t.input = *input;
        if (const Json* result = member(j, "result")) t.result = content_text(*result);
        t.is_error = json_bool_or(j, "is_error", false);
        t.elapsed_ms = json_int_or(j, "elapsed_ms", 0);
        t.time = time;
        turn(number).tools.push_back(std::move(t));
    } else if (type == "retry") {
        RetryRecord r;
        r.attempt = static_cast<int>(json_int_or(j, "attempt", 0));
        r.error = string_member(j, "error");
        r.delay_ms = json_int_or(j, "delay_ms", 0);
        r.status = static_cast<int>(json_int_or(j, "status", 0));
        r.retry_after_ms = json_int_or(j, "retry_after_ms", 0);
        r.time = time;
        turn(number).retries.push_back(std::move(r));
    } else if (type == "guidance") {
        UserItem g;
        g.kind = UserItem::Kind::guidance;
        g.text = string_member(j, "text");
        g.guidance_id = u64_member(j, "id");
        g.time = time;
        doc_.pending.push_back(std::move(g));
    } else if (type == "paused" || type == "resumed") {
        UserItem marker;
        marker.kind = type == "paused" ? UserItem::Kind::paused : UserItem::Kind::resumed;
        marker.time = time;
        doc_.pending.push_back(std::move(marker));
    } else if (type == "auto_submit") {
        AutoSubmit a;
        a.accepted = json_bool_or(j, "accepted", false);
        if (const Json* result = member(j, "result")) a.result = content_text(*result);
        a.time = time;
        doc_.auto_submit = std::move(a);
    } else if (type == "outcome") {
        TranscriptOutcome o;
        o.outcome = string_member(j, "outcome");
        o.detail = string_member(j, "detail");
        o.best_match = json_number_or(j, "best_match", 0);
        o.turns = static_cast<int>(json_int_or(j, "turns", 0));
        o.cost_usd = json_number_or(j, "cost_usd", 0);
        if (const Json* u = member(j, "usage")) o.usage = usage_of(*u);
        o.time = time;
        doc_.outcome = std::move(o);
    } else {
        ++doc_.unknown;
    }
}

void TranscriptReader::line(std::string_view text) {
    if (trim(text).empty()) return;
    auto json = parse_json(text);
    if (!json || !json->is_object()) {
        ++doc_.malformed;
        ++version_;
        return;
    }
    ++doc_.records;
    try {
        record(*json);
    } catch (const Json::exception&) {
        ++doc_.malformed;
    }
    ++version_;
}

void TranscriptReader::feed(std::string_view appended) {
    while (!appended.empty()) {
        const usize nl = appended.find('\n');
        if (nl == std::string_view::npos) {
            partial_ += appended;
            return;
        }
        if (partial_.empty()) {
            line(appended.substr(0, nl));
        } else {
            partial_ += appended.substr(0, nl);
            line(partial_);
            partial_.clear();
        }
        appended.remove_prefix(nl + 1);
    }
}

void TranscriptReader::finish() {
    if (partial_.empty()) return;
    line(partial_);
    partial_.clear();
}

Result<bool> TranscriptReader::feed_file(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return make_error(ErrorCode::io, "cannot read {}: {}", path.string(), ec.message());
    if (size < offset_) reset();
    if (size == offset_) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return make_error(ErrorCode::io, "cannot open {}", path.string());
    in.seekg(static_cast<std::streamoff>(offset_));
    std::string chunk(static_cast<usize>(size - offset_), '\0');
    in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    chunk.resize(static_cast<usize>(in.gcount()));
    offset_ += chunk.size();
    feed(chunk);
    return !chunk.empty();
}

void TranscriptReader::reset() {
    doc_ = {};
    partial_.clear();
    offset_ = 0;
    ++version_;
}

TranscriptDoc parse_transcript(std::string_view text) {
    TranscriptReader reader;
    reader.feed(text);
    reader.finish();
    return reader.doc();
}

namespace {

// A code fence longer than any run of backticks in `text`.
std::string fenced(std::string_view text, std::string_view lang) {
    usize longest = 0, run = 0;
    for (char c : text) {
        run = c == '`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    const std::string fence(std::max<usize>(3, longest + 1), '`');
    std::string out = fence + std::string(lang) + "\n" + std::string(text);
    if (!text.empty() && text.back() != '\n') out += '\n';
    return out + fence + "\n";
}

std::string blockquote(std::string_view text) {
    std::string out;
    for (const auto& l : split_lines(text)) out += l.empty() ? ">\n" : "> " + l + "\n";
    return out.empty() ? ">\n" : out;
}

std::string clock(TimePoint t) {
    if (t == TimePoint{}) return "?";
    return std::format("{:%Y-%m-%d %H:%M:%S} UTC", std::chrono::floor<std::chrono::seconds>(t));
}

// A tool input: candidate sources (any "source" string) in cpp fences, the other fields as JSON.
std::string tool_input(const Json& input) {
    std::string out;
    if (input.is_object()) {
        Json rest = Json::object();
        std::string source;
        for (auto it = input.begin(); it != input.end(); ++it) {
            if (it.key() == "source" && it->is_string()) source = it->get<std::string>();
            else rest[it.key()] = *it;
        }
        if (!rest.empty()) out += fenced(dump_pretty(rest), "json");
        if (!source.empty()) out += fenced(source, "cpp");
        return out.empty() ? fenced("{}", "json") : out;
    }
    if (input.is_string()) return fenced(input.get<std::string>(), "text");  // input that was not valid JSON
    return fenced(input.is_null() ? std::string("{}") : dump_pretty(input), "json");
}

std::string usage_line(const events::TokenUsage& u) {
    return std::format("input {}, output {}, cache write {}, cache read {}", u.input, u.output, u.cache_write, u.cache_read);
}

void render_user_items(std::string& out, const std::vector<UserItem>& items, const std::vector<std::string>& shown_results) {
    for (const auto& item : items) {
        switch (item.kind) {
        case UserItem::Kind::tool_result:
            if (std::ranges::find(shown_results, item.tool_use_id) != shown_results.end()) break;
            out += std::format("**Tool result** `{}`{}:\n\n{}\n", item.tool_use_id, item.is_error ? " (error)" : "", fenced(item.text, "text"));
            break;
        case UserItem::Kind::guidance:
            out += std::format("**Supervisor guidance**{}:\n\n{}\n", item.time ? " (" + clock(*item.time) + ")" : "", blockquote(item.text));
            break;
        case UserItem::Kind::status: out += std::format("*{}*\n\n", item.text); break;
        case UserItem::Kind::nudge: out += std::format("**Reminder:** {}\n\n", item.text); break;
        case UserItem::Kind::text: out += std::format("{}\n\n", item.text); break;
        case UserItem::Kind::paused: out += std::format("*Paused{}*\n\n", item.time ? " at " + clock(*item.time) : ""); break;
        case UserItem::Kind::resumed: out += std::format("*Resumed{}*\n\n", item.time ? " at " + clock(*item.time) : ""); break;
        }
    }
}

} // namespace

std::string to_markdown(const TranscriptDoc& doc) {
    std::string out;
    const auto& h = doc.header;
    const std::string title = h ? (h->display.empty() ? h->function : h->display) : std::string("session");
    out += std::format("# Agent session: {}\n\n", title);
    if (h) {
        out += std::format("- Function: `{}` at {:#x}\n", h->function, h->va);
        out += std::format("- Session: `{}` (worker {})\n", h->session, h->worker);
        out += std::format("- Model: {}, effort {}\n", h->model, h->effort);
        out += std::format("- Started: {}\n", clock(h->time));
    }
    if (doc.outcome) {
        const auto& o = *doc.outcome;
        out += std::format("- Outcome: **{}**, best match {:.1f}%, {} turn{}, {}\n", o.outcome, o.best_match, o.turns, o.turns == 1 ? "" : "s",
                           format_usd(o.cost_usd));
        if (!o.detail.empty()) out += std::format("- Detail: {}\n", o.detail);
    } else {
        out += "- Outcome: (the session has not ended)\n";
    }
    out += "\n";
    if (!doc.brief.empty()) out += "## Brief\n\n" + fenced(doc.brief, "text") + "\n";

    std::vector<std::string> shown_results;  // tool results already shown with their calls
    for (const auto& t : doc.turns) {
        out += std::format("## Turn {}\n\n", t.turn);
        render_user_items(out, t.before, shown_results);
        if (!t.has_response) {
            out += "*No response recorded.*\n\n";
        } else {
            for (const auto& b : t.blocks) {
                switch (b.kind) {
                case ResponseBlock::Kind::thinking:
                    out += b.text.empty() ? std::string("*Thinking (no summary)*\n\n") : "*Thinking (summary):*\n\n" + blockquote(b.text) + "\n";
                    break;
                case ResponseBlock::Kind::redacted_thinking: out += "*Thinking (redacted)*\n\n"; break;
                case ResponseBlock::Kind::text: out += b.text + "\n\n"; break;
                case ResponseBlock::Kind::tool_use:
                    out += std::format("**Tool call** `{}` (`{}`):\n\n{}\n", b.tool_name, b.tool_id, tool_input(b.input));
                    break;
                case ResponseBlock::Kind::fallback:
                    out += std::format("*Fallback: {} -> {}*\n\n", b.from_model.empty() ? "?" : b.from_model, b.to_model.empty() ? "?" : b.to_model);
                    break;
                case ResponseBlock::Kind::other: out += std::format("*[{} block]*\n\n", b.text); break;
                }
            }
        }
        for (const auto& x : t.tools) {
            out += std::format("**Result of `{}`** (`{}`, {} ms{}):\n\n{}\n", x.name, x.id, x.elapsed_ms, x.is_error ? ", error" : "",
                               fenced(x.result, "text"));
            shown_results.push_back(x.id);
        }
        for (const auto& r : t.retries)
            out += std::format("*Retry {} after {} ms{}: {}*\n\n", r.attempt, r.delay_ms, r.status ? std::format(" (HTTP {})", r.status) : "", r.error);
        if (t.has_response) {
            out += std::format("*Stop: {}; model {}{}; tokens: {}; {}; first token {} ms, total {} ms*\n\n", t.stop_reason.empty() ? "?" : t.stop_reason,
                               t.model, t.had_fallback ? " (fallback)" : "", usage_line(t.usage), format_usd(t.cost_usd), t.ttft_ms, t.latency_ms);
        }
    }
    if (!doc.pending.empty()) {
        out += "## After the last request\n\n";
        render_user_items(out, doc.pending, shown_results);
    }
    if (doc.auto_submit)
        out += std::format("## Saved at the end\n\nThe byte-exact attempt the session did not submit was {}.\n\n{}\n",
                           doc.auto_submit->accepted ? "verified and saved" : "not accepted", fenced(doc.auto_submit->result, "text"));
    if (doc.outcome) {
        const auto& o = *doc.outcome;
        out += std::format("## Outcome\n\n**{}** after {} turn{}, best match {:.1f}%, {} ({})\n", o.outcome, o.turns, o.turns == 1 ? "" : "s",
                           o.best_match, format_usd(o.cost_usd), usage_line(o.usage));
        if (!o.detail.empty()) out += "\n" + o.detail + "\n";
    }
    return out;
}

} // namespace decomp::vm
