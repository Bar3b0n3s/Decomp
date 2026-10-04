#include "viewmodel/line_diff.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <format>

using namespace decomp;
using namespace decomp::vm;

namespace {

// Rebuilds both sides from the edit script: equal and deleted lines give the old side, equal and
// inserted lines the new side, in order.
void check_script(const LineDiff& d) {
    std::vector<std::string> old_side, new_side;
    usize ins = 0, del = 0;
    for (const auto& e : d.edits) {
        if (e.op != DiffOp::insert) {
            REQUIRE(e.old_index == old_side.size());
            old_side.push_back(d.old_lines[e.old_index]);
        }
        if (e.op != DiffOp::del) {
            REQUIRE(e.new_index == new_side.size());
            new_side.push_back(d.new_lines[e.new_index]);
        }
        if (e.op == DiffOp::equal) CHECK(d.old_lines[e.old_index] == d.new_lines[e.new_index]);
        ins += e.op == DiffOp::insert;
        del += e.op == DiffOp::del;
    }
    CHECK(old_side == d.old_lines);
    CHECK(new_side == d.new_lines);
    CHECK(ins == d.inserted);
    CHECK(del == d.deleted);
}

std::string lines(std::initializer_list<const char*> list) {
    std::string out;
    for (const char* l : list) out += std::string(l) + "\n";
    return out;
}

// Length of the longest common subsequence (dynamic programming), to check minimality.
usize lcs(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::vector<std::vector<usize>> t(a.size() + 1, std::vector<usize>(b.size() + 1, 0));
    for (usize i = 1; i <= a.size(); ++i)
        for (usize j = 1; j <= b.size(); ++j)
            t[i][j] = a[i - 1] == b[j - 1] ? t[i - 1][j - 1] + 1 : std::max(t[i - 1][j], t[i][j - 1]);
    return t[a.size()][b.size()];
}

} // namespace

TEST_CASE("line diff: empty sides, identical texts and texts with nothing in common") {
    LineDiff both = diff_lines("", "");
    CHECK(both.identical());
    CHECK(both.edits.empty());
    CHECK(to_unified(both, "a", "b").empty());

    LineDiff added = diff_lines("", lines({"x", "y"}));
    check_script(added);
    CHECK(added.inserted == 2);
    CHECK(to_unified(added, "a", "b") == "--- a\n+++ b\n@@ -0,0 +1,2 @@\n+x\n+y\n");
    LineDiff removed = diff_lines(lines({"x"}), "");
    CHECK(removed.deleted == 1);
    CHECK(to_unified(removed, "a", "b") == "--- a\n+++ b\n@@ -1 +0,0 @@\n-x\n");

    const std::string text = lines({"int add(int a, int b) {", "    return a + b;", "}"});
    LineDiff same = diff_lines(text, text);
    CHECK(same.identical());
    CHECK(same.edits.size() == 3);
    check_script(same);
    for (const auto& row : side_by_side(same)) CHECK(row.kind == SideBySideRow::Kind::equal);

    LineDiff different = diff_lines(lines({"a", "b", "c"}), lines({"x", "y"}));
    check_script(different);
    CHECK(different.deleted == 3);
    CHECK(different.inserted == 2);
    CHECK(different.edits[0].op == DiffOp::del);  // deletions first within a change
    const auto rows = side_by_side(different);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].kind == SideBySideRow::Kind::changed);
    CHECK(rows[1].kind == SideBySideRow::Kind::changed);
    CHECK(rows[2].kind == SideBySideRow::Kind::removed);
    CHECK(rows[2].right == -1);
}

TEST_CASE("line diff: CRLF, missing final newline and blank lines") {
    // A CRLF copy is equal line by line, and a missing newline at the end is no difference.
    CHECK(diff_lines("a\nb\n", "a\r\nb\r\n").identical());
    CHECK(diff_lines("a\nb\n", "a\nb").identical());
    CHECK(diff_lines("a\nb", "a\r\nb").identical());
    // Kept when asked: then every line differs.
    LineDiffOptions exact;
    exact.strip_cr = false;
    LineDiff crlf = diff_lines("a\nb\n", "a\r\nb\r\n", exact);
    CHECK(crlf.deleted == 2);
    CHECK(crlf.inserted == 2);
    // Blank lines are lines; a lone newline is one empty line.
    CHECK(diff_lines("\n", "").deleted == 1);
    LineDiff blank = diff_lines("a\n\nb\n", "a\nb\n");
    CHECK(blank.deleted == 1);
    CHECK(blank.old_lines.size() == 3);
    CHECK(blank.old_lines[1].empty());
    // A lone carriage return inside a line stays.
    CHECK(diff_lines("a\rb\n", "ab\n").deleted == 1);
}

TEST_CASE("line diff: a minimal script, hunks with context and side-by-side rows") {
    const std::string old_text = lines({"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15"});
    const std::string new_text = lines({"1", "2", "three", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16"});
    LineDiff d = diff_lines(old_text, new_text);
    check_script(d);
    CHECK(d.deleted == 1);
    CHECK(d.inserted == 2);
    const auto hunks = unified_hunks(d, 3);
    REQUIRE(hunks.size() == 2);
    CHECK(hunks[0].old_start == 1);
    CHECK(hunks[0].old_count == 6);
    CHECK(hunks[0].new_start == 1);
    CHECK(hunks[0].new_count == 6);
    CHECK(hunks[1].old_start == 13);
    CHECK(hunks[1].old_count == 3);
    CHECK(hunks[1].new_start == 13);
    CHECK(hunks[1].new_count == 4);
    CHECK(to_unified(d, "old.cpp", "new.cpp", 1) ==
          "--- old.cpp\n+++ new.cpp\n@@ -2,3 +2,3 @@\n 2\n-3\n+three\n 4\n@@ -15 +15,2 @@\n 15\n+16\n");
    // Changes closer than twice the context share a hunk.
    CHECK(unified_hunks(d, 6).size() == 1);
    CHECK(unified_hunks(d, 0).size() == 2);

    const auto rows = side_by_side(d);
    CHECK(rows.size() == 16);
    CHECK(rows[2].kind == SideBySideRow::Kind::changed);
    CHECK(rows[2].left == 2);
    CHECK(rows[2].right == 2);
    CHECK(rows[15].kind == SideBySideRow::Kind::added);
    CHECK(rows[15].left == -1);

    // Moved and repeated lines: the script is as short as the longest common subsequence allows.
    const std::string a = lines({"a", "b", "c", "a", "b", "b", "a"});
    const std::string b = lines({"c", "b", "a", "b", "a", "c"});
    LineDiff m = diff_lines(a, b);
    check_script(m);
    CHECK(m.deleted + m.inserted == m.old_lines.size() + m.new_lines.size() - 2 * lcs(m.old_lines, m.new_lines));
}

TEST_CASE("line diff: random edits are minimal, and a large file is fast") {
    u32 state = 42;
    auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (int round = 0; round < 40; ++round) {
        std::string a, b;
        const u32 n = 1 + next() % 40;
        for (u32 i = 0; i < n; ++i) {
            const std::string line = std::format("line {}", next() % 8);
            a += line + "\n";
            const u32 r = next() % 10;
            if (r < 6) b += line + "\n";
            else if (r < 8) b += std::format("new {}\n", next() % 8);
            if (r == 9) b += line + "\n" + line + "\n";
        }
        LineDiff d = diff_lines(a, b);
        check_script(d);
        CHECK(d.deleted + d.inserted == d.old_lines.size() + d.new_lines.size() - 2 * lcs(d.old_lines, d.new_lines));
    }

    std::string big_old, big_new;
    for (int i = 0; i < 2000; ++i) {
        const std::string line = std::format("    statement_{}(x, {});", i, i * 7);
        big_old += line + "\n";
        big_new += (i % 20 == 0 ? std::format("    changed_{}();", i) : line) + "\n";
    }
    const auto start = std::chrono::steady_clock::now();
    LineDiff d = diff_lines(big_old, big_new);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("diff_lines: 2,000 lines with 100 changed in " << ms << " ms");
    CHECK(d.deleted == 100);
    CHECK(d.inserted == 100);
    CHECK(side_by_side(d).size() == 2000);
}
