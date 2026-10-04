#include "gui/views/code_view.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace decomp::gui {

namespace {

ImU32 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImGui::GetColorU32(ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f));
}

// The visible lines of a block of `count` lines starting at `top` (screen y).
std::pair<usize, usize> visible_lines(float top, float line_h, usize count) {
    const ImVec2 clip_min = ImGui::GetWindowDrawList()->GetClipRectMin();
    const ImVec2 clip_max = ImGui::GetWindowDrawList()->GetClipRectMax();
    const float first = std::floor((clip_min.y - top) / line_h);
    const float last = std::ceil((clip_max.y - top) / line_h);
    const usize lo = first <= 0 ? 0 : std::min(count, static_cast<usize>(first));
    const usize hi = last <= 0 ? 0 : std::min(count, static_cast<usize>(last) + 1);
    return {lo, std::max(lo, hi)};
}

} // namespace

CodeColors code_colors(const ViewContext& ctx) {
    const ThemeColors& c = ctx.colors();
    const DiffPalette p = ctx.diff_palette();
    CodeColors out;
    out.text = ImGui::GetColorU32(c.text);
    out.muted = ImGui::GetColorU32(c.muted);
    out.keyword = ImGui::GetColorU32(c.accent);
    out.type = p.encoding;
    out.number = p.operand;
    out.string = p.insert;
    out.comment = mix(c.muted, c.text, 0.1f);
    out.preprocessor = p.symbol;
    out.added = p.insert;
    out.removed = p.del;
    return out;
}

void draw_code(ViewContext& ctx, std::string_view source, std::span<const vm::CodeLine> lines, bool line_numbers) {
    ImGui::PushFont(ctx.fonts.mono, 0.0f);
    const CodeColors colors = code_colors(ctx);
    const float line_h = ImGui::GetTextLineHeight();
    const float gutter = line_numbers ? ImGui::CalcTextSize(std::format("{} ", std::max<usize>(lines.size(), 10)).c_str()).x : 0.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(line_h, line_h * static_cast<float>(lines.size()))));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto [lo, hi] = visible_lines(origin.y, line_h, lines.size());
    const char* base = source.data();
    for (usize i = lo; i < hi; ++i) {
        const vm::CodeLine& line = lines[i];
        const float y = origin.y + line_h * static_cast<float>(i);
        if (line_numbers) {
            const std::string number = std::format("{}", i + 1);
            dl->AddText(ImVec2(origin.x + gutter - ImGui::CalcTextSize(number.c_str()).x - ImGui::GetStyle().ItemInnerSpacing.x, y), colors.muted,
                        number.c_str());
        }
        float x = origin.x + gutter;
        auto put = [&](u32 begin, u32 end, ImU32 color) {
            if (end <= begin) return;
            dl->AddText(ImVec2(x, y), color, base + begin, base + end);
            x += ImGui::CalcTextSize(base + begin, base + end).x;
        };
        u32 at = line.begin;
        for (const vm::CodeSpan& span : line.spans) {
            put(at, span.begin, colors.text);
            ImU32 color = colors.text;
            switch (span.kind) {
            case vm::CodeToken::keyword: color = colors.keyword; break;
            case vm::CodeToken::type: color = colors.type; break;
            case vm::CodeToken::number: color = colors.number; break;
            case vm::CodeToken::string: color = colors.string; break;
            case vm::CodeToken::comment: color = colors.comment; break;
            case vm::CodeToken::preprocessor: color = colors.preprocessor; break;
            }
            put(span.begin, span.end, color);
            at = span.end;
        }
        put(at, line.end, colors.text);
    }
    ImGui::PopFont();
}

void draw_line_diff(ViewContext& ctx, const vm::LineDiff& diff, usize context) {
    if (diff.identical()) {
        ImGui::TextDisabled("(identical)");
        return;
    }
    const CodeColors colors = code_colors(ctx);
    // Unified lines with their markers, built once per call (the diff of two attempts is small).
    struct Line {
        char marker;
        const std::string* text;
    };
    std::vector<Line> lines;
    const auto hunks = vm::unified_hunks(diff, context);
    for (const auto& h : hunks) {
        for (usize k = h.first_edit; k < h.first_edit + h.edit_count; ++k) {
            const vm::LineEdit& e = diff.edits[k];
            if (e.op == vm::DiffOp::insert) lines.push_back({'+', &diff.new_lines[e.new_index]});
            else if (e.op == vm::DiffOp::del) lines.push_back({'-', &diff.old_lines[e.old_index]});
            else lines.push_back({' ', &diff.old_lines[e.old_index]});
        }
        lines.push_back({'@', nullptr});
    }
    if (!lines.empty()) lines.pop_back();
    ImGui::PushFont(ctx.fonts.mono, 0.0f);
    const float line_h = ImGui::GetTextLineHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(std::max(ImGui::GetContentRegionAvail().x, 1.0f), line_h * static_cast<float>(lines.size())));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto [lo, hi] = visible_lines(origin.y, line_h, lines.size());
    for (usize i = lo; i < hi; ++i) {
        const float y = origin.y + line_h * static_cast<float>(i);
        const Line& l = lines[i];
        if (!l.text) {
            dl->AddText(ImVec2(origin.x, y), colors.muted, "...");
            continue;
        }
        const ImU32 color = l.marker == '+' ? colors.added : l.marker == '-' ? colors.removed : colors.muted;
        const char marker[2] = {l.marker, 0};
        dl->AddText(ImVec2(origin.x, y), color, marker);
        dl->AddText(ImVec2(origin.x + ImGui::CalcTextSize("  ").x, y), l.marker == ' ' ? colors.text : color, l.text->c_str());
    }
    ImGui::PopFont();
}

} // namespace decomp::gui
