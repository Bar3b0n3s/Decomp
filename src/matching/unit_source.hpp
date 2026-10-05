#pragma once

// Unit sources (docs/project-format.md#unit-sources): one source file per translation unit, holding its
// matched functions as the original source file held them. A unit source is a prelude (includes,
// macros, declarations) followed by the matched functions in address order, each after a marker line
// `// FUNCTION: 0x00401060`. A function is added by composing the translation unit a session verified
// it with into the unit source; verify_unit() compiles the result once and diffs every function in it.

#include "analysis/program.hpp"
#include "core/result.hpp"
#include "matching/match.hpp"
#include "matching/source_items.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace decomp::matching {

inline constexpr std::string_view kFunctionMarker = "// FUNCTION: ";

struct UnitSource {
    std::vector<SourceItem> prelude;
    struct Function {
        u64 va = 0;
        std::string text;  // the definition, with the #pragma and #line directives around it
        std::string name;  // the defined function's name ("Player::Hit"); empty when none was found
        bool is_static = false;
    };
    std::vector<Function> functions;  // address order

    // Parses a unit source: the text before the first marker line is the prelude, and each marker line
    // starts a function that runs to the next one.
    static UnitSource parse(std::string_view text);
    // The source: the prelude's items, then each function after its marker, one blank line apart.
    std::string render() const;
    const Function* find(u64 va) const;
    bool remove(u64 va);
};

// Composes the translation unit `source` of the function at `va` into `unit`. The definition of a
// function with one of `names` (its qualified name, its C name) becomes the function's entry, with the
// #pragma and #line directives right before and after it; an earlier entry for `va` is replaced. The
// source's other items join the prelude unless the unit has them already: the same item (ignoring
// comments and spacing), a definition of the same function, or a static declaration where the item
// declares the function without `static`. Conditional directives (#if ... #endif) always join. Fails
// when the source has no such definition at its top level (not inside a class or namespace).
Result<void> compose_function(UnitSource& unit, u64 va, std::span<const std::string> names, std::string_view source);

// The names a function's definition may carry in a source: its qualified name ("Player::Hit"), its PDB
// name and its C name without decoration ("add" for _add@8).
std::vector<std::string> definition_names(const Symbol& function);

// A unit source compiled once, with each of its functions diffed against the target.
struct UnitCheck {
    u64 va = 0;
    std::optional<FunctionDiff> diff;
    std::string error;  // why there is no diff
    bool byte_exact() const { return diff && diff->byte_exact; }
};
struct UnitVerification {
    CompileResult compile;
    std::string error;  // why nothing could be diffed (the compile failed)
    std::vector<UnitCheck> functions;
    bool all_byte_exact() const;
};
// Compiles `source` as `file_name` ("player.cpp", "util.c": the extension picks the language) and diffs
// the target function at each of `vas` against the object.
Result<UnitVerification> verify_unit(const Program& program, const MatchSetup& setup, const std::string& source, std::string_view file_name,
                                     std::span<const u64> vas);

} // namespace decomp::matching
