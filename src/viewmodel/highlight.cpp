#include "viewmodel/highlight.hpp"

#include <algorithm>
#include <span>

namespace decomp::vm {

namespace {

constexpr std::string_view kKeywords[] = {
    "alignas", "alignof", "asm", "auto", "break", "case", "catch", "class", "const", "consteval", "constexpr", "constinit",
    "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "dynamic_cast", "else",
    "enum", "explicit", "export", "extern", "false", "for", "friend", "goto", "if", "inline", "mutable", "namespace", "new",
    "noexcept", "nullptr", "operator", "private", "protected", "public", "register", "reinterpret_cast", "requires", "return",
    "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true",
    "try", "typedef", "typeid", "typename", "union", "using", "virtual", "volatile", "while", "__cdecl", "__stdcall", "__fastcall",
    "__thiscall", "__vectorcall", "__declspec", "__forceinline", "__inline", "__asm", "__try", "__except", "__finally", "__leave",
    "__restrict",
};

constexpr std::string_view kTypes[] = {
    "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float", "int", "long", "short", "signed", "unsigned", "void",
    "wchar_t", "size_t", "ptrdiff_t", "intptr_t", "uintptr_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
    "uint32_t", "uint64_t", "__int8", "__int16", "__int32", "__int64",
};

bool ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || static_cast<unsigned char>(c) >= 0x80; }
bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }
bool digit(char c) { return c >= '0' && c <= '9'; }

bool listed(std::span<const std::string_view> words, std::string_view w) { return std::ranges::find(words, w) != words.end(); }

// The end of a quoted literal starting at `open` (the quote), or `end` when the line ends first.
usize literal_end(std::string_view src, usize open, usize end) {
    const char quote = src[open];
    usize k = open + 1;
    while (k < end && src[k] != quote) k += src[k] == '\\' ? 2 : 1;
    return std::min(end, k + 1);
}

} // namespace

std::vector<CodeLine> highlight_cpp(std::string_view src) {
    std::vector<CodeLine> lines;
    bool in_comment = false;       // inside /* ... */ at the start of the line
    bool in_preprocessor = false;  // a directive continued with a backslash
    usize start = 0;
    while (start < src.size()) {
        usize nl = src.find('\n', start);
        if (nl == std::string_view::npos) nl = src.size();
        usize end = nl;
        if (end > start && src[end - 1] == '\r') --end;
        const std::string_view text = src.substr(0, end);
        CodeLine line;
        line.begin = static_cast<u32>(start);
        line.end = static_cast<u32>(end);
        auto span = [&](usize b, usize e, CodeToken kind) {
            if (e > b) line.spans.push_back(CodeSpan{static_cast<u32>(b), static_cast<u32>(e), kind});
        };
        usize i = start;
        usize first = start;
        while (first < end && (src[first] == ' ' || src[first] == '\t')) ++first;
        if (!in_comment && (in_preprocessor || (first < end && src[first] == '#'))) {
            // A directive is one span, comments inside it included; a trailing backslash continues it.
            span(first, end, CodeToken::preprocessor);
            in_preprocessor = end > start && src[end - 1] == '\\';
            i = end;
        }
        while (i < end) {
            if (in_comment) {
                const usize close = text.find("*/", i);
                const usize stop = close == std::string_view::npos ? end : close + 2;
                span(i, stop, CodeToken::comment);
                in_comment = close == std::string_view::npos;
                i = stop;
                continue;
            }
            const char c = src[i];
            if (c == '/' && i + 1 < end && src[i + 1] == '/') {
                span(i, end, CodeToken::comment);
                break;
            }
            if (c == '/' && i + 1 < end && src[i + 1] == '*') {
                const usize close = text.find("*/", i + 2);
                const usize stop = close == std::string_view::npos ? end : close + 2;
                span(i, stop, CodeToken::comment);
                in_comment = close == std::string_view::npos;
                i = stop;
                continue;
            }
            if (c == '"' || c == '\'') {
                const usize stop = literal_end(src, i, end);
                span(i, stop, CodeToken::string);
                i = stop;
                continue;
            }
            if (digit(c) || (c == '.' && i + 1 < end && digit(src[i + 1]))) {
                const bool hex = c == '0' && i + 1 < end && (src[i + 1] == 'x' || src[i + 1] == 'X');
                usize k = i + 1;
                while (k < end) {
                    const char d = src[k], prev = src[k - 1];
                    const bool sign = (d == '+' || d == '-') && (hex ? (prev == 'p' || prev == 'P') : (prev == 'e' || prev == 'E'));
                    if (!(ident_char(d) || d == '.' || d == '\'' || sign)) break;
                    ++k;
                }
                span(i, k, CodeToken::number);
                i = k;
                continue;
            }
            if (ident_start(c)) {
                usize k = i + 1;
                while (k < end && ident_char(src[k])) ++k;
                const std::string_view word = src.substr(i, k - i);
                // String literal prefixes (L"", u8"", u"", U"") belong to the literal.
                if (k < end && (src[k] == '"' || src[k] == '\'') && (word == "L" || word == "u8" || word == "u" || word == "U")) {
                    const usize stop = literal_end(src, k, end);
                    span(i, stop, CodeToken::string);
                    i = stop;
                    continue;
                }
                if (listed(kKeywords, word)) span(i, k, CodeToken::keyword);
                else if (listed(kTypes, word)) span(i, k, CodeToken::type);
                i = k;
                continue;
            }
            ++i;
        }
        lines.push_back(std::move(line));
        start = nl + 1;
    }
    return lines;
}

} // namespace decomp::vm
