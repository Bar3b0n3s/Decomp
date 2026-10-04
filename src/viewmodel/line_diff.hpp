#pragma once

// Line diffs for source versions (Agent session: compare two attempts) and file changes (Changes and
// approvals): Myers' O(ND) algorithm in its linear-space form, unified hunks with context, and rows
// for a side-by-side view.
//
// Lines: the text is split at '\n'; a final line without one is still a line, and a final '\n' does not
// start an empty line. With `strip_cr` (the default) one '\r' before each '\n' is dropped, so a file
// and its CRLF copy compare equal line by line. A missing newline at the end is not a difference.

#include "core/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace decomp::vm {

enum class DiffOp : u8 { equal, insert, del };

struct LineEdit {
    DiffOp op = DiffOp::equal;
    // 0-based indices into old_lines and new_lines. An insert has no old line: old_index is the old
    // line it goes before (the number of old lines before it); likewise new_index for a deletion.
    u32 old_index = 0, new_index = 0;
};

struct LineDiffOptions {
    bool strip_cr = true;
};

struct LineDiff {
    std::vector<std::string> old_lines, new_lines;  // without line terminators
    // Every line of both sides in order; within a change, deletions come before insertions.
    std::vector<LineEdit> edits;
    usize inserted = 0, deleted = 0;

    bool identical() const { return inserted == 0 && deleted == 0; }
};

// A minimal edit script (fewest inserted plus deleted lines) after trimming the common start and end.
// O((N + M) * D) time for N and M lines and D differing lines, O(N + M) memory; texts without a common
// line skip the search. A 2,000-line file with 100 changed lines takes about half a millisecond in a
// Release build (tests/unit/viewmodel_line_diff_tests.cpp).
LineDiff diff_lines(std::string_view old_text, std::string_view new_text, const LineDiffOptions& options = {});

struct Hunk {
    // Unified-diff ranges: 1-based first line and line count of each side. An empty side's range
    // starts at the line before it (0 at the top of the file), as in `diff -u`.
    usize old_start = 0, old_count = 0, new_start = 0, new_count = 0;
    usize first_edit = 0, edit_count = 0;  // the hunk's lines in LineDiff::edits
};
// Groups the changes into hunks with `context` equal lines around them; hunks whose context would
// touch or overlap are merged.
std::vector<Hunk> unified_hunks(const LineDiff& diff, usize context = 3);
// "--- <old>\n+++ <new>\n@@ -a,b +c,d @@\n" and the hunk lines (' ', '-', '+'); empty when identical.
std::string to_unified(const LineDiff& diff, std::string_view old_label, std::string_view new_label, usize context = 3);

// Rows of a side-by-side view. Within a change, the i-th deleted line is paired with the i-th inserted
// line ("changed"); the extra lines of the longer side are "removed" or "added" with nothing opposite.
struct SideBySideRow {
    enum class Kind : u8 { equal, changed, removed, added };
    Kind kind = Kind::equal;
    i32 left = -1, right = -1;  // indices into old_lines and new_lines; -1: nothing on that side
};
std::vector<SideBySideRow> side_by_side(const LineDiff& diff);

} // namespace decomp::vm
