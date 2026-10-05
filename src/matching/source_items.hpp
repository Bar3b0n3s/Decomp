#pragma once

// The top-level items of a C or C++ source: what unit sources are composed of
// (docs/project-format.md#unit-sources). A lexer that knows comments, string and character literals and
// preprocessor lines and counts braces, parentheses and brackets; it does not parse C++.

#include "core/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace decomp::matching {

enum class ItemKind : u8 {
    preprocessor,  // a directive line, with its continuation lines
    declaration,   // anything that ends in `;` at the top level: declarations, data and type definitions
    function,      // a function definition: a head with a parameter list, then a body in braces
    block,         // a braced block that is not a function body: extern "C" { ... }, namespace n { ... }
    comment,       // comments after the last item
};

struct SourceItem {
    ItemKind kind = ItemKind::declaration;
    std::string text;  // as written, with the comments and blank lines that lead it
    // Functions and function declarations: the declared name ("Player::Hit", "add", "operator+");
    // preprocessor lines: the directive ("include", "pragma", "define", "line").
    std::string name;
    bool function_declaration = false;  // a declaration whose declarator has a parameter list
    bool is_static = false;             // `static` before the declared name
    usize line = 1;                     // where its own text starts (after the leading comments)
};

std::vector<SourceItem> parse_source_items(std::string_view source);

// The text without comments, with whitespace kept only between two words: two items that read the same
// normalize to the same text.
std::string normalized(std::string_view text);

// The text of an item without its leading comments and blank lines.
std::string_view item_body(const SourceItem& item);

// The types a declaration item declares or defines: the tag of `struct`, `class`, `union` or `enum`
// (`struct Player { ... };`, `enum class Color : int;`), the names a typedef introduces
// (`typedef struct { ... } Point, *PPoint;`, `typedef void (*Callback)(int);`), the alias of
// `using Name = ...;`. Empty for other items, and for templates.
std::vector<std::string> declared_types(const SourceItem& item);

} // namespace decomp::matching
