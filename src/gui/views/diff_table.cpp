#include "gui/views/diff_table.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace decomp::gui {

using matching::RowKind;

namespace {

ImU32 kind_color(const DiffPalette& p, RowKind kind) {
    switch (kind) {
    case RowKind::equal: return p.equal;
    case RowKind::encoding: return p.encoding;
    case RowKind::operand: return p.operand;
    case RowKind::opcode: return p.opcode;
    case RowKind::insert: return p.insert;
    case RowKind::del: return p.del;
    }
    return p.equal;
}

std::string ref_line(const vm::OperandRef& r) {
    std::string out = std::format("{} {}", matching::to_string(r.kind), r.display);
    if (r.address) out += std::format(" at {}", hex(*r.address, 8));
    if (!r.readable.empty() && r.readable != r.display) out += std::format(": {}", r.readable);
    if (!r.name.empty() && r.name != r.readable && r.name != r.display) out += std::format(" [{}]", r.name);
    if (!r.symbol_kind.empty()) out += std::format(" ({}{})", r.symbol_kind, r.size ? std::format(", {} bytes", r.size) : "");
    return out;
}

const char* side_name(vm::DiffSide side) { return side == vm::DiffSide::target ? "Target" : "Candidate"; }

} // namespace

void DiffTable::set(std::shared_ptr<const matching::FunctionDiff> diff, const vm::DiffViewOptions& layout) {
    if (diff == diff_ && layout.differing_only == layout_.differing_only && layout.context == layout_.context && layout.fuzzy == layout_.fuzzy)
        return;
    const std::optional<usize> cursor_row = current_row();
    const bool same_diff = diff == diff_;
    diff_ = std::move(diff);
    layout_ = layout;
    rows_.clear();
    target_arrows_.clear();
    candidate_arrows_.clear();
    bytes_chars_ = 0;
    if (!diff_) {
        clear();
        return;
    }
    rows_ = vm::layout_rows(*diff_, layout_);
    target_arrows_ = vm::branch_arrows(*diff_, rows_, vm::DiffSide::target);
    candidate_arrows_ = vm::branch_arrows(*diff_, rows_, vm::DiffSide::candidate);
    for (const auto* side : {&diff_->target, &diff_->candidate})
        for (const auto& si : side->instructions) bytes_chars_ = std::max<usize>(bytes_chars_, si.ins.length * 3u);
    // A new attempt of the same function keeps the rows the user looked at (row numbers line up mostly).
    std::erase_if(selected_, [&](usize r) { return !vm::display_index(rows_, r); });
    cursor_ = cursor_row ? vm::display_index(rows_, *cursor_row) : std::nullopt;
    anchor_ = cursor_;
    if (!same_diff) scroll_to_ = cursor_;
}

void DiffTable::clear() {
    diff_.reset();
    rows_.clear();
    target_arrows_.clear();
    candidate_arrows_.clear();
    selected_.clear();
    cursor_.reset();
    anchor_.reset();
    scroll_to_.reset();
}

std::optional<usize> DiffTable::current_row() const {
    if (!cursor_ || *cursor_ >= rows_.size()) return std::nullopt;
    return rows_[*cursor_].row;
}

void DiffTable::select(std::span<const usize> rows) {
    selected_.clear();
    cursor_.reset();
    for (usize r : rows) {
        auto d = vm::display_index(rows_, r);
        if (!d) continue;
        selected_.insert(r);
        if (!cursor_) cursor_ = d;
    }
    anchor_ = cursor_;
    scroll_to_ = cursor_;
}

void DiffTable::step(bool forward) {
    auto next = vm::next_difference(rows_, cursor_, forward);
    if (!next) return;
    cursor_ = anchor_ = scroll_to_ = next;
    selected_ = {rows_[*next].row};
}

void DiffTable::click(usize display, bool ctrl, bool shift) {
    const usize row = rows_[display].row;
    if (shift && anchor_) {
        if (!ctrl) selected_.clear();
        for (usize d = std::min(*anchor_, display); d <= std::max(*anchor_, display); ++d) selected_.insert(rows_[d].row);
    } else if (ctrl) {
        if (!selected_.erase(row)) selected_.insert(row);
        anchor_ = display;
    } else {
        selected_ = {row};
        anchor_ = display;
    }
    cursor_ = display;
}

std::string DiffTable::copy_text(const DiffDrawOptions& options) const {
    if (!diff_) return {};
    if (selected_.empty()) return vm::rows_text(*diff_, rows_, options.mode, options.bytes);
    std::vector<vm::DisplayRow> chosen;
    for (const auto& r : rows_)
        if (selected_.contains(r.row)) chosen.push_back(r);
    // Gaps between selected rows read as "...".
    for (usize i = 0; i < chosen.size(); ++i) chosen[i].gap_before = i > 0 && chosen[i].row != chosen[i - 1].row + 1;
    return vm::rows_text(*diff_, chosen, options.mode, options.bytes);
}

void DiffTable::draw_side(ViewContext& ctx, const matching::Row& row, vm::DiffSide side, ImVec2 pos, float text_w, float bytes_w,
                          const DiffDrawOptions& options, const Program* program, ImU32 color) {
    const vm::SideCell cell = vm::side_cell(*diff_, row, side, options.mode);
    if (!cell.present) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const DiffPalette pal = ctx.diff_palette();
    const ImU32 muted = ImGui::GetColorU32(ctx.colors().muted);
    const float ch = ImGui::CalcTextSize("0").x;
    const float line_h = ImGui::GetTextLineHeight();
    const bool hover_window = ImGui::IsWindowHovered();
    const auto& si = (side == vm::DiffSide::target ? diff_->target : diff_->candidate).instructions[cell.instruction];
    // Relocated fields: branches inside the function are not relocated (their arrows show them).
    auto marks = options.relocations || options.bytes ? vm::field_marks(si) : std::vector<vm::FieldMark>{};
    std::erase_if(marks, [](const vm::FieldMark& m) { return m.kind == matching::RefKind::label; });

    dl->AddText(pos, muted, std::format("{:4x}:", cell.offset).c_str());
    float x = pos.x + ch * 6;
    if (options.bytes) {
        // Bytes of address fields stand out when relocations are shown.
        float bx = x;
        for (u8 b = 0; b < si.ins.length; ++b) {
            const bool field = options.relocations && std::ranges::any_of(marks, [b](const vm::FieldMark& m) { return b >= m.offset && b < m.offset + m.size; });
            dl->AddText(ImVec2(bx, pos.y), field ? pal.symbol : muted, std::format("{:02x}", si.ins.bytes[b]).c_str());
            bx += ch * 3;
        }
        if (hover_window && !marks.empty() && ImGui::IsMouseHoveringRect(ImVec2(x, pos.y), ImVec2(bx, pos.y + line_h)) &&
            ImGui::BeginTooltip()) {
            for (const auto& m : marks)
                ImGui::Text("Bytes %u-%u: %s (%s)", m.offset, m.offset + m.size - 1, m.target.c_str(), std::string(matching::to_string(m.kind)).c_str());
            ImGui::EndTooltip();
        }
        x += bytes_w;
    }
    // The instruction, operand by operand, clipped to its column.
    dl->PushClipRect(ImVec2(x, pos.y), ImVec2(x + text_w - ch, pos.y + line_h + 2), true);
    dl->AddText(ImVec2(x, pos.y), color, cell.mnemonic.c_str());
    float ox = x + ImGui::CalcTextSize(cell.mnemonic.c_str()).x;
    for (usize i = 0; i < cell.operands.size(); ++i) {
        const vm::OperandCell& op = cell.operands[i];
        const char* sep = i == 0 ? " " : ", ";
        dl->AddText(ImVec2(ox, pos.y), color, sep);
        ox += ImGui::CalcTextSize(sep).x;
        const float w = ImGui::CalcTextSize(op.text.c_str()).x;
        const ImVec2 a(ox, pos.y), b(ox + w, pos.y + line_h);
        ImU32 op_color = color;
        if (op.diff) {
            // Boxed as well as colored: color is never the only cue.
            op_color = *op.diff == matching::OperandDiff::symbol ? pal.symbol : color;
            dl->AddRect(ImVec2(a.x - 2, a.y), ImVec2(b.x + 2, b.y + 1), op_color);
        }
        dl->AddText(a, op_color, op.text.c_str());
        if (hover_window && (op.has_ref || op.diff) && ImGui::IsMouseHoveringRect(a, b) && ImGui::BeginTooltip()) {
            ImGui::TextUnformatted(std::format("{} operand {}: {}", side_name(side), i, op.text).c_str());
            if (op.diff) ImGui::TextUnformatted(std::format("Differs: {}", matching::to_string(*op.diff)).c_str());
            for (const auto& r : vm::operand_refs(*diff_, row, side, i, program)) ImGui::TextUnformatted(ref_line(r).c_str());
            ImGui::EndTooltip();
        }
        ox = b.x;
    }
    if (options.relocations && !marks.empty()) {
        std::string text;
        for (const auto& m : marks) text += std::format("  R+{}:{} {}", m.offset, m.size, m.target);
        dl->AddText(ImVec2(ox, pos.y), pal.symbol, text.c_str());
    }
    dl->PopClipRect();
}

void DiffTable::draw_arrows(ViewContext& ctx, std::span<const vm::BranchArrow> arrows, float gutter_right, ImVec2 origin, float row_h, float lane_w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const DiffPalette pal = ctx.diff_palette();
    const ImU32 muted = ImGui::GetColorU32(ctx.colors().muted);
    const float top = dl->GetClipRectMin().y, bottom = dl->GetClipRectMax().y;
    const float thickness = std::max(1.0f, ImGui::GetFontSize() / 14.0f);
    for (const auto& a : arrows) {
        const float y_from = origin.y + row_h * (static_cast<float>(a.from) + 0.5f);
        const float y_to = origin.y + row_h * (static_cast<float>(a.to) + 0.5f);
        if (std::max(y_from, y_to) < top - row_h || std::min(y_from, y_to) > bottom + row_h) continue;
        const vm::DisplayRow& source = rows_[a.from];
        const ImU32 color = source.differs() ? kind_color(pal, source.kind) : muted;
        const float x_lane = gutter_right - lane_w * (static_cast<float>(a.lane) + 1.0f);
        const float x_end = gutter_right - 2.0f;
        const ImVec2 points[] = {ImVec2(x_end, y_from), ImVec2(x_lane, y_from), ImVec2(x_lane, y_to), ImVec2(x_end, y_to)};
        dl->AddPolyline(points, 4, color, ImDrawFlags_None, thickness);
        const float head = lane_w * 0.45f;
        if (a.to_hidden) {
            // The destination is hidden: the arrow ends in an open circle next to the gap.
            dl->AddCircle(ImVec2(x_end, y_to), head * 0.7f, color, 0, thickness);
        } else {
            dl->AddTriangleFilled(ImVec2(x_end + 1.0f, y_to), ImVec2(x_end - head, y_to - head), ImVec2(x_end - head, y_to + head), color);
        }
    }
}

void DiffTable::draw(ViewContext& ctx, const char* id, ImVec2 size, const DiffDrawOptions& options, const Program* program) {
    if (!ImGui::BeginChild(id, size, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::EndChild();
        return;
    }
    if (!diff_) {
        ImGui::TextDisabled("No diff to show.");
        ImGui::EndChild();
        return;
    }
    if (rows_.empty()) {
        ImGui::TextDisabled(diff_->rows.empty() ? "Neither side has instructions." : "No row differs (turn off \"Differing rows only\" to see them all).");
        ImGui::EndChild();
        return;
    }
    ImGui::PushFont(ctx.fonts.mono, 0.0f);
    const DiffPalette pal = ctx.diff_palette();
    const ImU32 muted = ImGui::GetColorU32(ctx.colors().muted);
    const float ch = ImGui::CalcTextSize("0").x;
    const float row_h = std::floor(ImGui::GetTextLineHeight() * 1.2f);
    const float lane_w = std::max(5.0f, std::floor(ch * 0.9f));
    const float gutter_t = lane_w * static_cast<float>(vm::lane_count(target_arrows_) + 1);
    const float gutter_c = lane_w * static_cast<float>(vm::lane_count(candidate_arrows_) + 1);
    const float glyph_w = ch * 2, off_w = ch * 6, sep_w = ch * 2, notes_w = ch * 22;
    const float bytes_w = options.bytes ? ch * static_cast<float>(bytes_chars_ + 1) : 0.0f;
    const float fixed = gutter_t + glyph_w + 2 * (off_w + bytes_w) + sep_w + gutter_c + notes_w;
    const float text_w = std::max(ch * (options.bytes ? 24 : 30), std::floor((ImGui::GetContentRegionAvail().x - fixed) / 2));
    const float content_w = fixed + 2 * text_w;
    const float x_glyph = gutter_t, x_target = x_glyph + glyph_w;
    const float x_sep = x_target + off_w + bytes_w + text_w;
    const float x_candidate = x_sep + sep_w + gutter_c;
    const float x_notes = x_candidate + off_w + bytes_w + text_w;

    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    // Up and Down move the cursor while the table has focus.
    if (ImGui::IsWindowFocused() && !rows_.empty()) {
        const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow), down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
        if (up || down) {
            const usize at = cursor_ ? *cursor_ : 0;
            const usize next = up ? (at > 0 ? at - 1 : 0) : std::min(rows_.size() - 1, cursor_ ? at + 1 : 0);
            click(next, false, io.KeyShift);
            scroll_to_ = next;
        }
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows_.size()), row_h);
    if (scroll_to_ && *scroll_to_ < rows_.size()) clipper.IncludeItemByIndex(static_cast<int>(*scroll_to_));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const usize d = static_cast<usize>(i);
            const vm::DisplayRow& dr = rows_[d];
            const matching::Row& row = diff_->rows[dr.row];
            const ImVec2 p(origin.x, origin.y + row_h * static_cast<float>(i));
            ImGui::SetCursorScreenPos(p);
            ImGui::PushID(i);
            if (ImGui::Selectable("##row", selected_.contains(dr.row), ImGuiSelectableFlags_AllowOverlap, ImVec2(content_w, row_h)))
                click(d, io.KeyCtrl, io.KeyShift);
            if (scroll_to_ == d) {
                ImGui::SetScrollHereY(0.5f);
                scroll_to_.reset();
            }
            if (ImGui::BeginPopupContextItem("##row_menu")) {
                if (!selected_.contains(dr.row)) click(d, false, false);
                if (ImGui::MenuItem("Copy selected rows")) ImGui::SetClipboardText(copy_text(options).c_str());
                if (ImGui::MenuItem("Copy every shown row")) ImGui::SetClipboardText(vm::rows_text(*diff_, rows_, options.mode, options.bytes).c_str());
                if (ImGui::MenuItem("Copy the target instruction", nullptr, false, row.target.has_value()))
                    ImGui::SetClipboardText(vm::side_cell(*diff_, row, vm::DiffSide::target, options.mode).text().c_str());
                if (ImGui::MenuItem("Copy the candidate instruction", nullptr, false, row.candidate.has_value()))
                    ImGui::SetClipboardText(vm::side_cell(*diff_, row, vm::DiffSide::candidate, options.mode).text().c_str());
                ImGui::EndPopup();
            }
            const float y = p.y + std::floor((row_h - ImGui::GetTextLineHeight()) / 2);
            if (dr.gap_before) {
                // Hidden rows above: a dashed line across the row's top.
                for (float x = p.x; x < p.x + content_w; x += ch * 2) dl->AddLine(ImVec2(x, p.y), ImVec2(x + ch, p.y), muted);
            }
            const ImU32 color = kind_color(pal, dr.kind);
            const char glyph[2] = {dr.glyph, 0};
            dl->AddText(ImVec2(p.x + x_glyph, y), dr.differs() ? color : muted, glyph);
            draw_side(ctx, row, vm::DiffSide::target, ImVec2(p.x + x_target, y), text_w, bytes_w, options, program, color);
            dl->AddText(ImVec2(p.x + x_sep + ch * 0.5f, y), muted, "|");
            draw_side(ctx, row, vm::DiffSide::candidate, ImVec2(p.x + x_candidate, y), text_w, bytes_w, options, program, color);
            // The categories of the differing operands, as text.
            std::string note;
            if (row.kind == RowKind::operand)
                for (const auto& [op, kind] : row.operands) note += std::format("{}op{} {}", note.empty() ? "" : ", ", op, matching::to_string(kind));
            else if (row.kind == RowKind::encoding) note = "encoding";
            else if (row.kind == RowKind::opcode) note = "opcode";
            else if (row.kind == RowKind::insert) note = "extra";
            else if (row.kind == RowKind::del) note = "missing";
            if (!note.empty()) dl->AddText(ImVec2(p.x + x_notes, y), dr.differs() ? color : muted, ("(" + note + ")").c_str());
            ImGui::PopID();
        }
    }
    clipper.End();
    draw_arrows(ctx, target_arrows_, origin.x + gutter_t, origin, row_h, lane_w);
    draw_arrows(ctx, candidate_arrows_, origin.x + x_sep + sep_w + gutter_c, origin, row_h, lane_w);
    ImGui::PopFont();
    ImGui::EndChild();
}

void DiffTable::draw_details(ViewContext& ctx, const DiffDrawOptions& options, const Program* program) {
    const auto row_index = current_row();
    if (!diff_ || !row_index) {
        ImGui::TextDisabled("Select a row to see both instructions, their bytes and what their operands refer to.");
        return;
    }
    const matching::Row& row = diff_->rows[*row_index];
    ImGui::Text("Row %zu: %s (%c)", *row_index + 1, std::string(matching::to_string(row.kind)).c_str(), vm::row_glyph(row));
    for (const auto& [op, kind] : row.operands)
        ImGui::BulletText("Operand %zu differs: %s", op, std::string(matching::to_string(kind)).c_str());
    ImGui::PushFont(ctx.fonts.mono, 0.0f);
    for (vm::DiffSide side : {vm::DiffSide::target, vm::DiffSide::candidate}) {
        const vm::SideCell cell = vm::side_cell(*diff_, row, side, options.mode);
        if (!cell.present) {
            ImGui::TextDisabled("%s: (no instruction)", side_name(side));
            continue;
        }
        ImGui::Text("%s #%zu at +%llx: %s", side_name(side), cell.instruction, static_cast<unsigned long long>(cell.offset), cell.text().c_str());
        ImGui::TextDisabled("  bytes %s", cell.bytes.c_str());
        const auto& si = (side == vm::DiffSide::target ? diff_->target : diff_->candidate).instructions[cell.instruction];
        for (const auto& m : vm::field_marks(si))
            ImGui::TextDisabled("  address field at +%u (%u bytes): %s", m.offset, m.size, m.target.c_str());
        for (usize i = 0; i < cell.operands.size(); ++i)
            for (const auto& r : vm::operand_refs(*diff_, row, side, i, program)) ImGui::Text("  op%zu: %s", i, ref_line(r).c_str());
    }
    ImGui::PopFont();
}

} // namespace decomp::gui
