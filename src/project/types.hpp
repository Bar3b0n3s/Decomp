#pragma once

// Shared types in the project's headers under include/ (docs/project-format.md#include), the source of
// truth for the program's types: what the define_type tool adds or replaces, and what the compiler makes
// of them, read back from its debug information. A change is composed and checked before anything is
// written: the header must still compile and define the type, and every verified source that includes
// it must keep its byte-exact functions byte-exact.

#include "analysis/program.hpp"
#include "analysis/types.hpp"
#include "core/result.hpp"
#include "core/types.hpp"
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
// items that declare `name` (a definition or a forward declaration, or a namespace block that declares
// only it), or at the end; with `replace` false, always at the end. A new header starts with
// `#pragma once`. The declaration may hold several type declarations, #pragma lines (for `#pragma
// pack`) and namespace blocks of them, but no functions, data or other directives, and it must declare
// `name` ("game::Shape" in `namespace game { ... }`).
struct ComposedType {
    std::string text;
    bool replaced = false;  // the header declared the type already
};
Result<ComposedType> compose_type(std::string_view header_text, std::string_view name, std::string_view declaration, bool replace = true);

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

// The project's headers: the files under include/ that valid_header_name() accepts, as "include/..."
// paths, in path order.
Result<std::vector<std::string>> project_headers(const Project& project);

// A type a header declares: at its top level, in named namespaces ("game::Shape") and in extern "C"
// blocks; struct, class, union and enum definitions and forward declarations, typedefs and using
// aliases (not templates).
struct DeclaredType {
    std::string name;
    std::string keyword;  // "struct", "class", "union" or "enum"; "" for typedefs and aliases
};
std::vector<DeclaredType> header_declared_types(std::string_view header_text);

struct HeaderType {
    std::string name;
    std::string header;  // "include/types.h"
};

// The project's types as its compiler lays them out: every project header included by one translation
// unit, compiled with the project's toolchain, flags and include directories and with debug information
// (/Z7; clang-cl also -fstandalone-debug, so that it writes every type's definition), each declared type
// referenced (a `T*` variable) so that the compiler writes it, and the layouts read back from the
// object's type records (.debug$T). Fails when the headers do not compile, or the toolchain writes no
// type records this reads (GCC-style toolchains, CodeView before Visual C++ 7.0). `staged` gives headers
// ("include/types.h") a text other than the file's, or adds them.
struct HeaderTypes {
    std::vector<HeaderType> declared;  // in header order
    TypeCatalog catalog;               // every type the compile wrote: also those the headers use from elsewhere
    std::string source;                // the translation unit compiled

    const HeaderType* header_of(std::string_view name) const;
    // The declared types the compile defined (not only declared forward, not typedefs of other types).
    bool defines(std::string_view name) const { return header_of(name) && catalog.find(name); }
};
Result<HeaderTypes> compile_header_types(const Project& project, const matching::MatchSetup& setup, Arch arch,
                                         const std::vector<std::pair<std::string, std::string>>& staged = {});

// The target PDB's types `names`, and what they need, declared in a project header
// (analysis/declarations.hpp): written at the end of `header` (types.h unless named), checked first. The
// header must compile, every type it defines must have the PDB's layout, and the verified sources that
// include it must keep their byte-exact functions. Types the project's headers define already are left
// as they are; types they declare in other headers are included from there.
struct TypeImport {
    TypeChange change;                  // the header before and after
    std::vector<std::string> defined;   // in the order the header defines them
    std::vector<std::string> declared;  // declared forward
    std::vector<std::string> skipped;   // what was not declared, and why
    std::vector<std::string> includes;  // headers it now includes
};
Result<TypeImport> prepare_type_import(const Project& project, const Program& program, const matching::MatchSetup& setup,
                                       const std::vector<std::string>& names, std::string_view header);

} // namespace decomp::project
