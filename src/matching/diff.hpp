#pragma once

#include "analysis/program.hpp"
#include "arch/x86/decoder.hpp"
#include "core/json.hpp"
#include "formats/coff.hpp"

#include <optional>
#include <string>
#include <vector>

namespace decomp::matching {

// What an address-bearing instruction field refers to, in a form comparable across the linked target
// image and an unlinked candidate object.
enum class RefKind : u8 { symbol, label, string, float32, float64, vector, table, unknown };
std::string_view to_string(RefKind kind);

struct Ref {
    RefKind kind = RefKind::unknown;
    std::string key;      // comparison key: symbol name, label index, string content, constant bits, table labels
    std::string alt;      // alternative name for equivalence (PDB name of a target symbol)
    i64 offset = 0;       // symbol + offset
    u64 target_va = 0;    // target side: the referenced address
    std::string display;  // readable text used when rendering
};

struct SideInstruction {
    x86::Instruction ins;                  // decoded; address is the offset from the function start
    std::vector<std::optional<Ref>> refs;  // parallel to ins.fields
    std::string text;                      // canonical text: refs rendered as display names
};

struct Side {
    std::string name;
    u64 address = 0;  // target: function VA; candidate: offset in its section
    usize size = 0;
    std::vector<SideInstruction> instructions;
};

// Builds the comparable view of the target function at `va`.
Result<Side> build_target_side(const Program& program, u64 va);

// Finds the candidate symbol for a target function: exact decorated name, else an equivalent name.
const coff::Symbol* find_candidate_symbol(const coff::Object& obj, const Symbol& target);

// Builds the comparable view of a function defined in a COFF object.
Result<Side> build_candidate_side(const coff::Object& obj, const coff::Symbol& function);

enum class RowKind : u8 { equal, encoding, operand, opcode, insert, del };
std::string_view to_string(RowKind kind);

enum class OperandDiff : u8 { reg, imm, mem, stack, symbol };
std::string_view to_string(OperandDiff kind);

struct Row {
    RowKind kind = RowKind::equal;
    std::optional<usize> target;     // instruction index
    std::optional<usize> candidate;  // instruction index
    std::vector<std::pair<usize, OperandDiff>> operands;  // for operand rows: operand index + category
};

struct Binding {
    u64 target_va = 0;
    std::string candidate_symbol;
};

struct FunctionDiff {
    Side target;
    Side candidate;
    std::vector<Row> rows;
    usize equal = 0, encoding = 0, operand = 0, opcode = 0, inserted = 0, deleted = 0;
    double match_percent = 0;
    bool exact = false;       // every instruction and every reference equivalent
    bool byte_exact = false;  // exact and every non-reference byte identical (the real matching criterion)
    std::vector<std::string> hints;
    std::vector<Binding> bindings;
};

// Compares two sides. `program` (the target) is used to verify string/float/table contents by address.
FunctionDiff diff_sides(Side target, Side candidate, const Program& program);

// Convenience: target function at `va` vs its counterpart in `obj` (or `candidate_symbol` if given).
Result<FunctionDiff> diff_function(const Program& program, u64 va, const coff::Object& obj,
                                   const std::string& candidate_symbol = {});

struct ReportOptions {
    usize context = 2;      // equal rows shown around differences (compact mode)
    usize max_rows = 400;   // cap on printed rows
    bool compact = false;   // only differences plus context
    bool color = false;     // ANSI colors
    bool bytes = false;     // show instruction bytes
};

std::string to_text(const FunctionDiff& diff, const ReportOptions& options = {});
Json to_json(const FunctionDiff& diff, const ReportOptions& options = {});
// One-line summary: "match 87.5% (25/31 equal; 3 operand, 1 opcode, 1 insert) - not matching"
std::string summary_line(const FunctionDiff& diff);

} // namespace decomp::matching
