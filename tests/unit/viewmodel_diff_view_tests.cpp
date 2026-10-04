// The Diff viewer's derivations over real diffs of the committed x86 fixture: its own object (exact) and
// mutated.obj (strings, constants, opcodes and extra instructions differ).

#include "formats/coff.hpp"
#include "test_util.hpp"
#include "viewmodel/diff_view.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <set>

using namespace decomp;
using namespace decomp::vm;
using matching::RowKind;

namespace {

struct Fixture {
    Program program = Program::open(test::fixture("x86/basic.exe")).value();
    coff::Object exact = coff::Object::load(test::fixture("x86/basic.obj")).value();
    coff::Object mutated = coff::Object::load(test::fixture("x86/mutated.obj")).value();

    matching::FunctionDiff diff(std::string_view name, const coff::Object& obj) const {
        return matching::diff_function(program, *program.resolve(name), obj).value();
    }
};

usize count_kind(const matching::FunctionDiff& d, RowKind kind) {
    return static_cast<usize>(std::ranges::count_if(d.rows, [kind](const matching::Row& r) { return r.kind == kind; }));
}

// Arrows sharing a lane never share a row.
void check_lanes(std::span<const BranchArrow> arrows, usize rows) {
    for (usize i = 0; i < arrows.size(); ++i) {
        CHECK(arrows[i].from < rows);
        CHECK(arrows[i].to < rows);
        for (usize j = i + 1; j < arrows.size(); ++j) {
            if (arrows[i].lane != arrows[j].lane) continue;
            const usize lo1 = std::min(arrows[i].from, arrows[i].to), hi1 = std::max(arrows[i].from, arrows[i].to);
            const usize lo2 = std::min(arrows[j].from, arrows[j].to), hi2 = std::max(arrows[j].from, arrows[j].to);
            CHECK((hi1 < lo2 || hi2 < lo1));
        }
    }
}

} // namespace

TEST_CASE("diff view: an exact diff shows every row as equal and has nothing to step to") {
    Fixture f;
    const auto d = f.diff("sum_array", f.exact);
    REQUIRE(d.byte_exact);
    const auto rows = layout_rows(d);
    REQUIRE(rows.size() == d.rows.size());
    for (const auto& r : rows) {
        CHECK(r.glyph == '=');
        CHECK_FALSE(r.differs());
        CHECK_FALSE(r.gap_before);
    }
    CHECK(layout_rows(d, {.differing_only = true}).empty());
    CHECK_FALSE(next_difference(rows, std::nullopt, true));
    CHECK_FALSE(next_difference(rows, 0, false));
    CHECK(display_index(rows, 3) == 3u);

    // The loop's backward branch and the forward branches get arrows in non-overlapping lanes.
    const auto arrows = branch_arrows(d, rows, DiffSide::target);
    REQUIRE_FALSE(arrows.empty());
    check_lanes(arrows, rows.size());
    CHECK(std::ranges::any_of(arrows, [](const BranchArrow& a) { return a.to < a.from; }));
    CHECK(std::ranges::none_of(arrows, [](const BranchArrow& a) { return a.to_hidden; }));
    CHECK(branch_arrows(d, rows, DiffSide::candidate).size() == arrows.size());
    CHECK(lane_count(arrows) >= 1);
}

TEST_CASE("diff view: a mutated string literal is an operand row with the operand boxed and its data compared") {
    Fixture f;
    const auto d = f.diff("message", f.mutated);
    REQUIRE(d.operand == 1);
    const auto rows = layout_rows(d);
    const auto first = next_difference(rows, std::nullopt, true);
    REQUIRE(first);
    const DisplayRow& shown = rows[*first];
    CHECK(shown.glyph == '@');  // a symbol (here a string) differs
    CHECK(shown.kind == RowKind::operand);
    const matching::Row& row = d.rows[shown.row];

    const SideCell target = side_cell(d, row, DiffSide::target);
    const SideCell candidate = side_cell(d, row, DiffSide::candidate);
    REQUIRE(target.present);
    REQUIRE(candidate.present);
    CHECK(target.text() == d.target.instructions[*row.target].text);
    CHECK(candidate.text() == d.candidate.instructions[*row.candidate].text);
    CHECK(target.mnemonic == "mov");
    REQUIRE(target.operands.size() == 2);
    CHECK_FALSE(target.operands[0].diff);
    REQUIRE(target.operands[1].diff);
    CHECK(*target.operands[1].diff == matching::OperandDiff::symbol);
    CHECK(target.operands[1].has_ref);
    CHECK(target.operands[1].text.find("hello world") != std::string::npos);
    CHECK(candidate.operands[1].text.find("hello there") != std::string::npos);
    CHECK(target.bytes.size() == static_cast<usize>(d.target.instructions[*row.target].ins.length) * 3 - 1);

    // Raw text shows the numbers in the encoding: the target's address, the candidate's addend.
    const SideCell raw = side_cell(d, row, DiffSide::target, TextMode::raw);
    CHECK(raw.operands[1].text.find("hello") == std::string::npos);
    CHECK(raw.operands[1].text.find("0x") != std::string::npos);

    const auto marks = field_marks(d.target.instructions[*row.target]);
    REQUIRE(marks.size() == 1);
    CHECK(marks[0].kind == matching::RefKind::string);
    CHECK(marks[0].size == 4);
    CHECK(marks[0].operand == 1);

    const auto data = data_diff(d, &f.program);
    REQUIRE(data.size() == 1);
    CHECK(data[0].kind == matching::RefKind::string);
    CHECK(data[0].target.find("hello world") != std::string::npos);
    CHECK(data[0].candidate.find("hello there") != std::string::npos);
    CHECK_FALSE(data[0].equal);
    CHECK(data[0].row == shown.row);

    const auto refs = operand_refs(d, row, DiffSide::target, 1, &f.program);
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].kind == matching::RefKind::string);
    REQUIRE(refs[0].address);
    CHECK(refs[0].symbol_kind == "string");

    // Copied rows read like `decomp diff`.
    const std::string text = rows_text(d, std::span(rows).subspan(*first, 1));
    CHECK(text.starts_with("~ "));
    CHECK(text.find("(op1 symbol)") != std::string::npos);
    CHECK(text.find('|') != std::string::npos);
    CHECK(rows_text(d, rows, TextMode::normalized, true).find("| ") != std::string::npos);

    // The hint about the string names its row.
    const auto hint = std::ranges::find_if(d.hints, [](const std::string& h) { return h.starts_with("String literal"); });
    REQUIRE(hint != d.hints.end());
    CHECK(hint_rows(d, *hint) == std::vector<usize>{shown.row});
}

TEST_CASE("diff view: constants, jump tables and extra instructions") {
    Fixture f;
    const auto scale = f.diff("scale", f.mutated);
    const auto data = data_diff(scale, &f.program);
    REQUIRE_FALSE(data.empty());
    const auto changed = std::ranges::find_if(data, [](const DataDiffEntry& e) { return !e.equal; });
    REQUIRE(changed != data.end());
    CHECK(changed->kind == matching::RefKind::float32);
    CHECK(changed->target == "1.5f");
    CHECK(changed->candidate == "2.5f");
    CHECK(std::ranges::count_if(data, [](const DataDiffEntry& e) { return e.equal; }) >= 1);  // 0.25f is the same

    // dispatch's switch: one table, compared case by case, and an arrow per case from the indirect jump.
    const auto dispatch = f.diff("dispatch", f.exact);
    const auto tables = data_diff(dispatch, &f.program);
    const auto table = std::ranges::find_if(tables, [](const DataDiffEntry& e) { return e.kind == matching::RefKind::table; });
    REQUIRE(table != tables.end());
    CHECK(table->equal);
    CHECK(table->entries.size() >= 6);
    for (const auto& [t, c] : table->entries) CHECK(t.starts_with("loc_"));
    const auto rows = layout_rows(dispatch);
    const auto arrows = branch_arrows(dispatch, rows, DiffSide::target);
    CHECK(std::ranges::count_if(arrows, [](const BranchArrow& a) { return a.table; }) >= 6);
    check_lanes(arrows, rows.size());

    // sum_array gains instructions: insert and delete rows, linked from the instruction-count hint.
    const auto sum = f.diff("sum_array", f.mutated);
    REQUIRE(sum.inserted + sum.deleted > 0);
    const auto hint = std::ranges::find_if(sum.hints, [](const std::string& h) { return h.find("than the target") != std::string::npos; });
    REQUIRE(hint != sum.hints.end());
    const auto linked = hint_rows(sum, *hint);
    CHECK(linked.size() == sum.inserted + sum.deleted);
    for (usize i : linked) CHECK((sum.rows[i].kind == RowKind::insert || sum.rows[i].kind == RowKind::del));
    const auto sum_rows = layout_rows(sum);
    for (const auto& r : sum_rows) {
        const auto& row = sum.rows[r.row];
        if (row.kind == RowKind::insert) {
            CHECK(r.glyph == '+');
            CHECK_FALSE(side_cell(sum, row, DiffSide::target).present);
        }
        if (row.kind == RowKind::del) CHECK(r.glyph == '-');
    }
}

TEST_CASE("diff view: differing rows only, with context, gaps and stepping") {
    Fixture f;
    const auto d = f.diff("sum_array", f.mutated);
    const auto all = layout_rows(d);
    const auto only = layout_rows(d, {.differing_only = true});
    CHECK(only.size() == static_cast<usize>(std::ranges::count_if(all, [](const DisplayRow& r) { return r.differs(); })));
    for (const auto& r : only) CHECK(r.differs());
    const auto context = layout_rows(d, {.differing_only = true, .context = 1});
    CHECK(context.size() >= only.size());
    CHECK(context.size() <= all.size());
    REQUIRE_FALSE(only.empty());
    for (usize i = 0; i < only.size(); ++i) {
        const bool hidden_before = i == 0 ? only[i].row > 0 : only[i].row != only[i - 1].row + 1;
        CHECK(only[i].gap_before == hidden_before);
    }

    // Stepping visits every differing row once, in order, and wraps around.
    std::vector<usize> forward;
    std::optional<usize> at;
    for (usize i = 0; i < all.size(); ++i) {
        at = next_difference(all, at, true);
        REQUIRE(at);
        if (!forward.empty() && *at == forward.front()) break;
        forward.push_back(*at);
    }
    CHECK(forward.size() == only.size());
    CHECK(std::ranges::is_sorted(forward));
    CHECK(next_difference(all, forward.front(), false) == forward.back());

    // Hidden destinations: arrows end at the nearest shown row and say so.
    const auto arrows = branch_arrows(d, only, DiffSide::target);
    check_lanes(arrows, only.size());
}

TEST_CASE("diff view: fuzzy registers and stack show soft operand rows as equal") {
    matching::FunctionDiff d;
    d.rows.push_back(matching::Row{RowKind::equal, 0, 0, {}});
    d.rows.push_back(matching::Row{RowKind::operand, 1, 1, {{0, matching::OperandDiff::reg}}});
    d.rows.push_back(matching::Row{RowKind::operand, 2, 2, {{1, matching::OperandDiff::stack}, {0, matching::OperandDiff::reg}}});
    d.rows.push_back(matching::Row{RowKind::operand, 3, 3, {{1, matching::OperandDiff::imm}}});
    d.rows.push_back(matching::Row{RowKind::opcode, 4, 4, {}});
    const auto plain = layout_rows(d);
    CHECK(plain[1].glyph == '~');
    CHECK(plain[1].differs());
    const auto fuzzy = layout_rows(d, {.fuzzy = true});
    CHECK(fuzzy[1].kind == RowKind::equal);
    CHECK(fuzzy[1].glyph == '=');
    CHECK(fuzzy[2].kind == RowKind::equal);
    CHECK(fuzzy[3].kind == RowKind::operand);  // an immediate is a real difference
    CHECK(fuzzy[4].glyph == '!');
    const auto only = layout_rows(d, {.differing_only = true, .fuzzy = true});
    REQUIRE(only.size() == 2);
    CHECK(only[0].row == 3);
    CHECK(only[0].gap_before);
    CHECK_FALSE(only[1].gap_before);
}

TEST_CASE("diff view: binding suggestions name their rows and kind") {
    Fixture f;
    REQUIRE(f.program.symbols().remove(0x403000));  // forget g_counter
    const auto d = f.diff("read_counter", f.exact);
    REQUIRE(d.bindings.size() == 1);
    const auto rows = binding_rows(d, d.bindings[0]);
    REQUIRE(rows.size() == 1);
    CHECK(binding_kind(d, d.bindings[0]) == SymbolKind::data);
    const matching::Row& row = d.rows[rows[0]];
    CHECK(row_glyph(row) == '@');
    // The target's operand is an unnamed address; the candidate's names the symbol.
    usize op = 0;
    for (const auto& [index, kind] : row.operands)
        if (kind == matching::OperandDiff::symbol) op = index;
    const auto target = operand_refs(d, row, DiffSide::target, op, &f.program);
    REQUIRE(target.size() == 1);
    CHECK(target[0].address == 0x403000u);
    CHECK(target[0].kind == matching::RefKind::unknown);
    const auto candidate = operand_refs(d, row, DiffSide::candidate, op, &f.program);
    REQUIRE(candidate.size() == 1);
    CHECK(candidate[0].name == "?g_counter@@3HA");
    CHECK(candidate[0].readable.find("g_counter") != std::string::npos);
    // The hint about the address names its row too.
    const auto hint = std::ranges::find_if(d.hints, [](const std::string& h) { return h.find("has no symbol") != std::string::npos; });
    REQUIRE(hint != d.hints.end());
    CHECK(hint_rows(d, *hint) == rows);

    // A callee binding is a function.
    Fixture g;
    REQUIRE(g.program.symbols().remove(*g.program.resolve("helper")));
    const auto dispatch = g.diff("dispatch", g.exact);
    const auto callee = std::ranges::find_if(dispatch.bindings, [](const matching::Binding& b) { return b.candidate_symbol.find("helper") != std::string::npos; });
    REQUIRE(callee != dispatch.bindings.end());
    CHECK(binding_kind(dispatch, *callee) == SymbolKind::function);
}

TEST_CASE("diff view: two attempts pair their rows by target instruction") {
    Fixture f;
    const auto exact = f.diff("sum_array", f.exact);
    const auto mutated = f.diff("sum_array", f.mutated);
    const auto pairs = pair_attempt_rows(exact, mutated);
    std::set<usize> seen_a, seen_b;
    for (const auto& p : pairs) {
        CHECK((p.a || p.b));
        if (p.a) CHECK(seen_a.insert(*p.a).second);
        if (p.b) CHECK(seen_b.insert(*p.b).second);
        if (p.a && p.b && exact.rows[*p.a].target && mutated.rows[*p.b].target)
            CHECK(*exact.rows[*p.a].target == *mutated.rows[*p.b].target);
    }
    CHECK(seen_a.size() == exact.rows.size());
    CHECK(seen_b.size() == mutated.rows.size());
    // Every target instruction has one row in each diff, so each pairs with itself.
    usize paired_targets = 0;
    for (const auto& p : pairs)
        if (p.a && p.b && exact.rows[*p.a].target && mutated.rows[*p.b].target) ++paired_targets;
    CHECK(paired_targets == exact.target.instructions.size());
    CHECK(count_kind(mutated, RowKind::insert) == mutated.inserted);

    // A diff paired with itself pairs every row with itself.
    const auto self = pair_attempt_rows(mutated, mutated);
    REQUIRE(self.size() == mutated.rows.size());
    for (usize i = 0; i < self.size(); ++i) CHECK((self[i].a == i && self[i].b == i));
}
