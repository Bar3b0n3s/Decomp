#pragma once

// The Agent session view's timeline (docs/ui.md "Agent session"): a transcript flattened into the items
// the view draws one at a time, so that only the visible ones are drawn however long the session is,
// with the variable-height bookkeeping that clipping needs; the candidate sources the session sent (for
// the diff against the previous attempt); whether guidance has been sent; and what a tool result says.

#include "viewmodel/transcript.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::vm {

struct TimelineItem {
    enum class Kind : u8 {
        brief,        // the first user message
        turn,         // a turn's header: number, serving model, stop reason, usage, cost, latency, retries
        user,         // what the request carried: guidance, status line, nudge, text, pause markers, and tool
                      // results not already shown with their calls
        thinking,     // a thinking summary (empty: the model gave none)
        redacted,     // redacted thinking
        text,         // assistant text
        tool_call,    // a tool call's input
        tool_result,  // a tool call's result (right after its call)
        fallback,     // a fallback switch point
        other,        // a content block of another type
        retry,        // a failed attempt of the turn's request
        no_response,  // the turn has no response (the request is in flight, or failed)
        live,         // the turn being streamed (the snapshot's live text and thinking)
        pending,      // recorded after the last request: guidance on its way in, pause markers
        auto_submit,  // the byte-exact attempt saved at the end
        outcome,
    };
    Kind kind = Kind::turn;
    i32 turn = -1;    // index into TranscriptDoc::turns (-1: none)
    i32 index = -1;   // the user item, block, tool exchange, retry or pending item it shows
    i32 source = -1;  // tool_call with a candidate source: its index in transcript_sources()
};

// `live`: append the live item (show_live() says when).
std::vector<TimelineItem> build_timeline(const TranscriptDoc& doc, bool live);

// Whether the snapshot's live buffer shows something the transcript does not have yet: the session runs,
// has streamed text or thinking, and its current turn has no response record.
bool show_live(const TranscriptDoc& doc, int current_turn, bool finished, bool has_live_text);

// The session the Agent session and the Diff viewer show when nothing is selected: the most recently
// started session still running, else the most recently started one (ties: the larger id). Null for a
// run without sessions. O(sessions).
const events::SessionState* newest_session(const events::RunStateData& run);

// The candidate sources the session sent, in order: the "source" of compile_and_diff and submit_result
// calls.
struct SourceRef {
    i32 turn = -1, block = -1;  // TranscriptDoc::turns[turn].blocks[block]
    std::string tool;
};
std::vector<SourceRef> transcript_sources(const TranscriptDoc& doc);
// The source text of a tool call (empty when it has none).
std::string_view block_source(const TranscriptDoc& doc, const SourceRef& ref);

// Guidance with this id is in the transcript (it was sent).
bool guidance_sent(const TranscriptDoc& doc, u64 id);

// What a tool result says, for the timeline: a compile's outcome and diff summary, or its errors.
struct ToolResultView {
    bool compile = false;     // the result of a compile (compile_and_diff, or submit_result's verification)
    bool compiled = false;    // the compile succeeded
    bool cached = false;
    std::string headline;     // the diff's summary line, why there is no diff, or the first line
    std::vector<std::string> errors;  // compiler diagnostics
    std::string attempt;      // "attempt 2: best so far 87.5%"
};
ToolResultView summarize_tool_result(std::string_view result);

// Offsets of a list of variable-height items, for drawing only the visible ones: heights are measured
// as items are drawn (or estimated before), and offset() and find() answer in O(log n).
class ItemHeights {
public:
    // Keeps the heights of the first `count` items; new items get `estimate`.
    void resize(usize count, float estimate);
    void set(usize i, float height);
    float height(usize i) const { return heights_[i]; }
    float offset(usize i) const;  // the top of item i (the sum of the heights before it)
    float total() const { return offset(heights_.size()); }
    usize find(float y) const;    // the item covering y; size() when y is past the end
    usize size() const { return heights_.size(); }

private:
    void rebuild();
    std::vector<float> heights_;
    std::vector<float> tree_;  // Fenwick tree over heights_
};

} // namespace decomp::vm
