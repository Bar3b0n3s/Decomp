#include "viewmodel/timeline.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <set>

namespace decomp::vm {

std::vector<TimelineItem> build_timeline(const TranscriptDoc& doc, bool live) {
    using Kind = TimelineItem::Kind;
    std::vector<TimelineItem> items;
    if (!doc.brief.empty()) items.push_back({Kind::brief});
    std::set<std::string, std::less<>> shown_results;  // tool results already shown with their calls
    i32 source = 0;
    for (usize t = 0; t < doc.turns.size(); ++t) {
        const TurnRecord& turn = doc.turns[t];
        const i32 ti = static_cast<i32>(t);
        items.push_back({Kind::turn, ti});
        for (usize u = 0; u < turn.before.size(); ++u) {
            const UserItem& item = turn.before[u];
            if (item.kind == UserItem::Kind::tool_result && shown_results.contains(item.tool_use_id)) continue;
            items.push_back({Kind::user, ti, static_cast<i32>(u)});
        }
        for (usize r = 0; r < turn.retries.size(); ++r) items.push_back({Kind::retry, ti, static_cast<i32>(r)});
        std::vector<bool> placed(turn.tools.size(), false);
        auto place_result = [&](const std::string& id) {
            for (usize x = 0; x < turn.tools.size(); ++x)
                if (!placed[x] && turn.tools[x].id == id) {
                    placed[x] = true;
                    items.push_back({Kind::tool_result, ti, static_cast<i32>(x)});
                    shown_results.insert(turn.tools[x].id);
                    return;
                }
        };
        if (!turn.has_response) {
            items.push_back({Kind::no_response, ti});
        } else {
            for (usize b = 0; b < turn.blocks.size(); ++b) {
                const ResponseBlock& block = turn.blocks[b];
                const i32 bi = static_cast<i32>(b);
                switch (block.kind) {
                case ResponseBlock::Kind::thinking: items.push_back({Kind::thinking, ti, bi}); break;
                case ResponseBlock::Kind::redacted_thinking: items.push_back({Kind::redacted, ti, bi}); break;
                case ResponseBlock::Kind::text: items.push_back({Kind::text, ti, bi}); break;
                case ResponseBlock::Kind::fallback: items.push_back({Kind::fallback, ti, bi}); break;
                case ResponseBlock::Kind::other: items.push_back({Kind::other, ti, bi}); break;
                case ResponseBlock::Kind::tool_use: {
                    const bool has_source = block.input.is_object() && block.input.contains("source") && block.input["source"].is_string();
                    items.push_back({Kind::tool_call, ti, bi, has_source ? source++ : -1});
                    place_result(block.tool_id);
                    break;
                }
                }
            }
        }
        // Results without a matching call (a call that was cut off, say) after the response.
        for (usize x = 0; x < turn.tools.size(); ++x)
            if (!placed[x]) {
                items.push_back({Kind::tool_result, ti, static_cast<i32>(x)});
                shown_results.insert(turn.tools[x].id);
            }
    }
    if (live) items.push_back({Kind::live});
    for (usize p = 0; p < doc.pending.size(); ++p) items.push_back({Kind::pending, -1, static_cast<i32>(p)});
    if (doc.auto_submit) items.push_back({Kind::auto_submit});
    if (doc.outcome) items.push_back({Kind::outcome});
    return items;
}

bool show_live(const TranscriptDoc& doc, int current_turn, bool finished, bool has_live_text) {
    if (finished || !has_live_text) return false;
    auto it = std::ranges::find(doc.turns, current_turn, &TurnRecord::turn);
    return it == doc.turns.end() || !it->has_response;
}

const events::SessionState* newest_session(const events::RunStateData& run) {
    const events::SessionState* best = nullptr;
    for (const auto& [id, s] : run.sessions) {
        if (!best) {
            best = s.get();
            continue;
        }
        if (s->finished != best->finished) {
            if (!s->finished) best = s.get();
            continue;
        }
        if (s->started > best->started || (s->started == best->started && s->id > best->id)) best = s.get();
    }
    return best;
}

std::vector<SourceRef> transcript_sources(const TranscriptDoc& doc) {
    std::vector<SourceRef> out;
    for (usize t = 0; t < doc.turns.size(); ++t) {
        const auto& turn = doc.turns[t];
        for (usize b = 0; b < turn.blocks.size(); ++b) {
            const auto& block = turn.blocks[b];
            if (block.kind != ResponseBlock::Kind::tool_use || !block.input.is_object()) continue;
            auto it = block.input.find("source");
            if (it == block.input.end() || !it->is_string()) continue;
            out.push_back(SourceRef{static_cast<i32>(t), static_cast<i32>(b), block.tool_name});
        }
    }
    return out;
}

std::string_view block_source(const TranscriptDoc& doc, const SourceRef& ref) {
    if (ref.turn < 0 || static_cast<usize>(ref.turn) >= doc.turns.size()) return {};
    const auto& turn = doc.turns[static_cast<usize>(ref.turn)];
    if (ref.block < 0 || static_cast<usize>(ref.block) >= turn.blocks.size()) return {};
    const Json& input = turn.blocks[static_cast<usize>(ref.block)].input;
    if (!input.is_object()) return {};
    auto it = input.find("source");
    if (it == input.end() || !it->is_string()) return {};
    return it->get_ref<const std::string&>();
}

bool guidance_sent(const TranscriptDoc& doc, u64 id) {
    if (id == 0) return false;
    auto has = [id](const std::vector<UserItem>& items) {
        return std::ranges::any_of(items, [id](const UserItem& u) { return u.kind == UserItem::Kind::guidance && u.guidance_id == id; });
    };
    if (has(doc.pending)) return true;
    return std::ranges::any_of(doc.turns, [&](const TurnRecord& t) { return has(t.before); });
}

ToolResultView summarize_tool_result(std::string_view result) {
    ToolResultView v;
    const auto lines = split_lines(result);
    usize i = 0;
    for (; i < lines.size(); ++i)
        if (lines[i].starts_with("compile: ")) break;
    if (i == lines.size()) {
        v.headline = lines.empty() ? std::string() : lines.front();
        return v;
    }
    v.compile = true;
    v.compiled = lines[i].starts_with("compile: ok");
    v.cached = lines[i].find("(cached)") != std::string::npos;
    for (const auto& l : lines)
        if (l.starts_with("attempt ")) v.attempt = l;
    if (!v.compiled) {
        v.headline = lines[i].find("timed out") != std::string::npos ? "compile failed (timed out)" : "compile failed";
        for (usize k = i + 1; k < lines.size(); ++k) {
            if (lines[k].starts_with("attempt ")) break;
            if (!trim(lines[k]).empty()) v.errors.push_back(lines[k]);
        }
        return v;
    }
    for (usize k = i + 1; k < lines.size(); ++k) {
        if (lines[k].starts_with("diff: ")) {
            v.headline = lines[k].substr(6);
            break;
        }
        if (lines[k].starts_with("match ")) {
            v.headline = lines[k];
            break;
        }
    }
    return v;
}

// ---- ItemHeights ----------------------------------------------------------------------------------

void ItemHeights::resize(usize count, float estimate) {
    if (count == heights_.size()) return;
    heights_.resize(count, estimate);
    rebuild();
}

void ItemHeights::rebuild() {
    const usize n = heights_.size();
    tree_.assign(n + 1, 0.0f);
    for (usize i = 0; i < n; ++i) {
        const usize k = i + 1;
        tree_[k] += heights_[i];
        const usize parent = k + (k & (~k + 1));
        if (parent <= n) tree_[parent] += tree_[k];
    }
}

void ItemHeights::set(usize i, float height) {
    if (i >= heights_.size()) return;
    const float delta = height - heights_[i];
    if (delta == 0.0f) return;
    heights_[i] = height;
    for (usize k = i + 1; k < tree_.size(); k += k & (~k + 1)) tree_[k] += delta;
}

float ItemHeights::offset(usize i) const {
    float sum = 0;
    for (usize k = std::min(i, heights_.size()); k > 0; k -= k & (~k + 1)) sum += tree_[k];
    return sum;
}

usize ItemHeights::find(float y) const {
    // Binary lifting over the Fenwick tree: the largest prefix whose sum is <= y.
    const usize n = heights_.size();
    if (n == 0 || y < 0) return 0;
    usize pos = 0;
    float rest = y;
    usize step = 1;
    while (step * 2 <= n) step *= 2;
    for (; step > 0; step /= 2) {
        if (pos + step <= n && tree_[pos + step] <= rest) {
            pos += step;
            rest -= tree_[pos];
        }
    }
    return std::min(pos, n);
}

} // namespace decomp::vm
