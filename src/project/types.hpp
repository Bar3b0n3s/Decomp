#pragma once

// Shared types in the project's headers under include/ (docs/project-format.md#include): what the
// define_type tool adds or replaces. A change is composed and checked before anything is written: the
// header must still compile and define the type, and every verified source that includes it must keep
// its byte-exact functions byte-exact.

#include "analysis/program.hpp"
#include "core/result.hpp"
#include "matching/match.hpp"
#include "project/project.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace decomp::project {

// The header a type goes into when none is named, under include/.
inline constexpr std::string_view kDefaultTypesHeader = "types.h";

// Whether `header` can name a project header: relative to include/, written with forward slashes,
// without `.` or `..`, of letters, digits and "_-.", and ending in .h, .hh, .hpp or .hxx.
bool valid_header_name(std::string_view header);

// Puts `declaration`, the definition of the type `name`, into `header_text`: in place of the header's
// items that declare `name` (a definition or a forward declaration), or at the end. A new header starts
// with `#pragma once`. The declaration may hold several type declarations and #pragma lines (for
// `#pragma pack`), but no functions, data or other directives, and one of its items must declare `name`.
struct ComposedType {
    std::string text;
    bool replaced = false;  // the header declared the type already
};
Result<ComposedType> compose_type(std::string_view header_text, std::string_view name, std::string_view declaration);

// A type change, composed and checked but not written yet.
struct TypeChange {
    std::string name;
    std::string header;         // the header's path in the project: "include/types.h"
    std::string base, content;  // the header before (empty when new) and after
    bool replaced = false;
    std::vector<std::string> sources;  // the verified sources that include the header, checked with the new one
};
// Composes the change and checks it: the new header compiles (as C++, with the project's toolchain,
// flags and include directories) and names the type, and every verified source that includes it (unit
// sources, and matched functions' own files) still compiles with each function byte-exact that was
// before. Fails without writing anything, saying what does not compile or which functions break.
Result<TypeChange> prepare_type_change(const Project& project, const Program& program, const matching::MatchSetup& setup, std::string_view name,
                                       std::string_view declaration, std::string_view header);
// Writes a prepared change (recorded in changes.jsonl). Fails with ErrorCode::conflict when the header
// changed since it was prepared.
Result<WriteReceipt> commit_type_change(const Project& project, const TypeChange& change, const ChangeOrigin& origin, const ChangeSubject& subject);

} // namespace decomp::project
