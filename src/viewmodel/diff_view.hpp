#pragma once

// What the Diff viewer draws for a FunctionDiff (docs/ui.md "Diff viewer"): the rows it shows and how
// (glyphs, fuzzy register and stack differences, differing rows only), each side's text split into the
// mnemonic and operands so the differing operand can be boxed, branch arrows in lanes, relocation
// markers, symbol details for tooltips, the data-diff panel, the rows each hint and binding talks about,
// copied rows, and two attempts' rows paired by target instruction. Pure and linear in the diff.

#include "analysis/program.hpp"
#include "matching/diff.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::vm {

enum class DiffSide : u8 { target, candidate };

// The gutter glyph of a row (docs/ui.md "Accessibility"): '=' equal, 'e' encoding, '~' operand ('@' when
// a symbol differs), '!' opcode, '+' insert, '-' delete.
char row_glyph(const matching::Row& row);
// The kind a row is shown as: with `fuzzy`, an operand row whose differences are all register or stack
// differences shows as equal.
matching::RowKind display_kind(const matching::Row& row, bool fuzzy);

struct DiffViewOptions {
    bool differing_only = false;
    usize context = 0;   // with differing_only: equal rows kept around each difference
    bool fuzzy = false;  // register-only and stack-only operand rows count as equal
};

struct DisplayRow {
    u32 row = 0;  // index into FunctionDiff::rows
    matching::RowKind kind = matching::RowKind::equal;  // as shown (after fuzzy)
    char glyph = '=';
    bool gap_before = false;  // rows are hidden right before this one

    bool differs() const { return kind != matching::RowKind::equal; }
};
std::vector<DisplayRow> layout_rows(const matching::FunctionDiff& diff, const DiffViewOptions& options = {});

// The next (or previous) differing row after (before) `from`, wrapping around; from nullopt the first
// (last) one. nullopt when no row differs (F7 and Shift+F7).
std::optional<usize> next_difference(std::span<const DisplayRow> rows, std::optional<usize> from, bool forward);
// The display row that shows FunctionDiff row `row`, if it is shown.
std::optional<usize> display_index(std::span<const DisplayRow> rows, usize row);

enum class TextMode : u8 {
    normalized,  // address operands as the names, strings and constants they refer to
    raw,         // the numbers in the encoding (an unlinked candidate's fields hold addends)
};

struct OperandCell {
    std::string text;
    std::optional<matching::OperandDiff> diff;  // the operand differs from the other side, with its category
    bool has_ref = false;                       // holds an address operand (a relocation)
};

// One side of a row, split for drawing. `text()` is the instruction as the diff report prints it.
struct SideCell {
    bool present = false;
    usize instruction = 0;  // index into Side::instructions
    u64 offset = 0;         // from the function start
    std::string bytes;      // "8b 44 24 08"
    std::string mnemonic;   // with its prefix ("rep movsd")
    std::vector<OperandCell> operands;

    std::string text() const;
};
SideCell side_cell(const matching::FunctionDiff& diff, const matching::Row& row, DiffSide side, TextMode mode = TextMode::normalized);

// An address-bearing field of an instruction: what the relocation marker shows.
struct FieldMark {
    u8 offset = 0, size = 0;  // bytes within the instruction
    i8 operand = -1;
    matching::RefKind kind = matching::RefKind::unknown;
    std::string target;  // the reference as displayed
};
std::vector<FieldMark> field_marks(const matching::SideInstruction& instruction);

// Branch arrows of one side, in display-row coordinates. Shorter arrows get lower lanes (nearer the
// text); arrows in one lane never share a row. An arrow whose destination row is hidden ends at the
// nearest shown row in its direction.
struct BranchArrow {
    usize from = 0, to = 0;  // display rows
    int lane = 0;
    bool to_hidden = false;
    bool table = false;  // an entry of a jump table
};
std::vector<BranchArrow> branch_arrows(const matching::FunctionDiff& diff, std::span<const DisplayRow> rows, DiffSide side);
int lane_count(std::span<const BranchArrow> arrows);

// What an operand refers to, for its tooltip: address, names, kind and size from the target's symbols.
struct OperandRef {
    matching::RefKind kind = matching::RefKind::unknown;
    std::string display;   // as the row shows it
    std::string name;      // the symbol's (decorated) name, when it is a symbol
    std::string readable;  // demangled
    std::optional<u64> address;
    std::string symbol_kind;  // "function", "data", ... when the target's symbols know it
    u32 size = 0;
};
std::vector<OperandRef> operand_refs(const matching::FunctionDiff& diff, const matching::Row& row, DiffSide side, usize operand,
                                     const Program* program);

// The data-diff panel: every string, wide string, constant and jump table an instruction references,
// with what each side holds there.
struct DataDiffEntry {
    usize row = 0;  // into FunctionDiff::rows
    i8 operand = -1;
    matching::RefKind kind = matching::RefKind::string;
    std::string target, candidate;  // empty: that side has nothing there
    bool equal = true;
    std::vector<std::pair<std::string, std::string>> entries;  // jump tables: the cases side by side
};
std::vector<DataDiffEntry> data_diff(const matching::FunctionDiff& diff, const Program* program);

// The rows (indices into FunctionDiff::rows) a hint talks about: the instructions it names ("target #4")
// or, for the general hints, the rows of the kind it describes.
std::vector<usize> hint_rows(const matching::FunctionDiff& diff, std::string_view hint);
// The rows where a binding's unnamed target address meets the candidate's symbol.
std::vector<usize> binding_rows(const matching::FunctionDiff& diff, const matching::Binding& binding);
// The kind of symbol a binding names: a function when every row it appears in calls or jumps to it,
// data otherwise.
SymbolKind binding_kind(const matching::FunctionDiff& diff, const matching::Binding& binding);

// The rows as `decomp diff` prints them: marker, offset, (bytes,) text, '|', the other side, and the
// differing operands.
std::string rows_text(const matching::FunctionDiff& diff, std::span<const DisplayRow> rows, TextMode mode = TextMode::normalized,
                      bool bytes = false);

// Two attempts' diffs of one function, row by row: rows of the same target instruction side by side;
// candidate-only rows paired in order before the next target instruction.
struct PairedRow {
    std::optional<usize> a, b;  // indices into the diffs' rows
};
std::vector<PairedRow> pair_attempt_rows(const matching::FunctionDiff& a, const matching::FunctionDiff& b);

} // namespace decomp::vm
