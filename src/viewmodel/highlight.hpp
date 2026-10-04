#pragma once

// Syntax highlighting of C++ sources for read-only code blocks (candidate sources in the Agent session's
// timeline; the Diff viewer's editor highlights on its own): comments (also across lines), string and
// character literals, numbers, keywords, built-in types and preprocessor lines. A lexer of the common
// cases, not a parser: raw string literals and macros are not understood.

#include "core/types.hpp"

#include <string_view>
#include <vector>

namespace decomp::vm {

enum class CodeToken : u8 { keyword, type, number, string, comment, preprocessor };

struct CodeSpan {
    u32 begin = 0, end = 0;  // byte offsets into the source
    CodeToken kind = CodeToken::keyword;
};

struct CodeLine {
    u32 begin = 0, end = 0;       // the line's bytes in the source, without its line break
    std::vector<CodeSpan> spans;  // highlighted ranges in order; the bytes between them are plain
};

// One entry per line ('\n' separated; a '\r' before it is not part of the line; a final line break
// does not start an empty line).
std::vector<CodeLine> highlight_cpp(std::string_view source);

} // namespace decomp::vm
