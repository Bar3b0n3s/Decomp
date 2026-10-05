#pragma once

#include "analysis/cfg.hpp"
#include "analysis/program.hpp"
#include "analysis/types.hpp"
#include "core/json.hpp"

#include <string>
#include <vector>

namespace decomp {

struct AnnotatedLine {
    u64 address = 0;
    std::string label;    // "loc_401020" when the line starts a block that is branched to
    std::string bytes;    // hex bytes
    std::string text;     // instruction with symbolized operands
    std::string comment;  // strings, floats, imports, frame slots, fields and virtual calls, switch tables, loop markers
    usize block = 0;
};

struct Reference {
    u64 va = 0;
    std::string name;     // symbol name (decorated if known)
    std::string display;  // readable
    std::string kind;     // function/data/string/float/import/unknown
    std::string detail;   // string contents, float value, import dll!name
    // An imported function: "dllimport" when the code reads its import-table slot (`call [__imp_X]`,
    // which a `__declspec(dllimport)` declaration produces), "thunk" when it calls the linker's import
    // thunk (`call X`, from a plain declaration). Empty otherwise.
    std::string import_call;
};

struct AnnotatedFunction {
    u64 start = 0;
    u64 end = 0;
    std::string name;     // decorated
    std::string display;  // readable signature
    std::string pdb_name;
    usize instruction_count = 0;
    usize block_count = 0;
    usize loop_count = 0;
    std::vector<AnnotatedLine> lines;
    std::vector<Reference> callees;
    std::vector<Reference> data_refs;
    std::vector<std::string> callers;  // readable names
    std::vector<JumpTable> jump_tables;
    std::vector<std::string> virtual_slots;  // where vftables hold the function (RTTI)
    // x86 exception handling the function registers: its try blocks' catch clauses, its __try blocks'
    // __except and __finally blocks, and where they are.
    std::vector<std::string> exception_handling;
    // What `this` and the pointer parameters point to at entry ("this = Player* (ecx)"), when their types
    // are known; the lines' comments then name the fields they reach (analysis/typeflow.hpp).
    std::vector<std::string> types;
};

// What an address in the image refers to, for operands and comments.
Reference describe_reference(const Program& program, u64 va);

// Whether the field of `ins` holds an address (base relocation, RIP-relative, or a heuristic in-image
// value when the image has no relocation info).
bool is_address_field(const Program& program, const x86::Instruction& ins, const x86::Field& field);

// `header_types`: the project's types (project/types.hpp), which name fields before the PDB's do.
Result<AnnotatedFunction> annotate_function(const Program& program, u64 start, bool include_callers = true,
                                            const TypeCatalog* header_types = nullptr);

// Plain-text listing for humans and the agent.
std::string to_text(const AnnotatedFunction& fn, bool with_bytes = true);
Json to_json(const AnnotatedFunction& fn);

} // namespace decomp
