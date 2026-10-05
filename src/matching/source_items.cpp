#include "matching/source_items.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <utility>

namespace decomp::matching {

namespace {

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$'; }
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

// The end of the comment, string or character literal that starts at `i`, or `i` when none does.
usize skip_literal_or_comment(std::string_view s, usize i) {
    const usize n = s.size();
    if (s.substr(i, 2) == "//") {
        while (i < n && s[i] != '\n') ++i;
        return i;
    }
    if (s.substr(i, 2) == "/*") {
        const auto end = s.find("*/", i + 2);
        return end == std::string_view::npos ? n : end + 2;
    }
    // Raw strings: R"delimiter( ... )delimiter", with an optional encoding prefix.
    if (s[i] == 'R' && i + 1 < n && s[i + 1] == '"' && (i == 0 || !word_char(s[i - 1]) || s.substr(i - 1, 1) == "u" ||
                                                        s.substr(i - 1, 1) == "U" || s.substr(i - 1, 1) == "L" || s.substr(i - 1, 1) == "8")) {
        const auto open = s.find('(', i + 2);
        if (open == std::string_view::npos) return n;
        const std::string close = ")" + std::string(s.substr(i + 2, open - i - 2)) + "\"";
        const auto end = s.find(close, open + 1);
        return end == std::string_view::npos ? n : end + close.size();
    }
    if (s[i] == '"' || s[i] == '\'') {
        const char quote = s[i++];
        while (i < n && s[i] != quote && s[i] != '\n') i += s[i] == '\\' ? 2 : 1;
        return std::min(i + 1, n);
    }
    return i;
}

struct Token {
    std::string_view text;
    bool word = false;
};

// The tokens of a head (comments and literals as single tokens), with their parenthesis depth.
std::vector<std::pair<Token, int>> head_tokens(std::string_view s) {
    std::vector<std::pair<Token, int>> out;
    int depth = 0;
    for (usize i = 0; i < s.size();) {
        if (space(s[i])) {
            ++i;
            continue;
        }
        if (const usize end = skip_literal_or_comment(s, i); end != i) {
            if (s[i] != '/') out.push_back({{s.substr(i, end - i), false}, depth});
            i = end;
            continue;
        }
        if (word_char(s[i])) {
            usize j = i;
            while (j < s.size() && word_char(s[j])) ++j;
            out.push_back({{s.substr(i, j - i), true}, depth});
            i = j;
            continue;
        }
        static constexpr std::array<std::string_view, 20> two = {"::", "->", "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=",
                                                                 "%=", "&=", "|=", "^=", "<<", ">>", "&&", "||", "++", "--"};
        if (std::ranges::find(two, s.substr(i, 2)) != two.end()) {
            out.push_back({{s.substr(i, 2), false}, depth});
            i += 2;
            continue;
        }
        if (s[i] == ')' || s[i] == ']') --depth;
        out.push_back({{s.substr(i, 1), false}, depth});
        if (s[i] == '(' || s[i] == '[') ++depth;
        ++i;
    }
    return out;
}

bool is_keyword(std::string_view w) {
    static constexpr std::array<std::string_view, 40> kw = {
        "int",       "char",      "short",    "long",         "void",       "float",      "double",   "signed",
        "unsigned",  "bool",      "auto",     "const",        "volatile",   "static",     "extern",   "inline",
        "typedef",   "struct",    "class",    "union",        "enum",       "return",     "if",       "while",
        "for",       "switch",    "sizeof",   "__declspec",   "_declspec",  "__attribute__", "alignas", "decltype",
        "__pragma",  "throw",     "noexcept", "__asm",        "asm",        "__forceinline", "__inline", "typename"};
    return std::ranges::find(kw, w) != kw.end();
}

struct Declarator {
    std::string name;
    bool function = false;  // a parameter list follows the name
    bool is_static = false;
};

// The declared name of a head: the identifier (with its qualification) right before the first top-level
// parameter list, or an `operator` name.
Declarator declarator(std::string_view head) {
    Declarator d;
    const auto tokens = head_tokens(head);
    for (usize k = 0; k < tokens.size(); ++k) {
        const auto& [t, depth] = tokens[k];
        if (depth != 0) continue;
        if (t.word && t.text == "static") d.is_static = true;
        if (t.word && t.text == "operator") {
            std::string name = "operator";
            usize j = k + 1;
            if (j + 1 < tokens.size() && tokens[j].first.text == "(" && tokens[j + 1].first.text == ")") {
                name += "()";
                j += 2;
            } else {
                for (; j < tokens.size() && tokens[j].first.text != "("; ++j) {
                    if (tokens[j].first.word) name += ' ';
                    name += tokens[j].first.text;
                }
            }
            if (j < tokens.size() && tokens[j].first.text == "(") {
                usize q = k;  // qualification before `operator`: A::operator+
                while (q >= 2 && tokens[q - 1].first.text == "::" && tokens[q - 2].first.word) q -= 2;
                for (usize p = q; p < k; ++p) d.name += tokens[p].first.text;
                d.name += name;
                d.function = true;
            }
            return d;
        }
        if (t.text != "(" || k == 0) continue;
        const auto& prev = tokens[k - 1].first;
        if (!prev.word || is_keyword(prev.text)) continue;
        // The qualified name ends here: walk back over `::`, `~` and template arguments.
        usize first = k - 1;
        while (first >= 1) {
            const auto& before = tokens[first - 1].first;
            if (before.text == "~") {
                --first;
                continue;
            }
            if (before.text == "::" && first >= 2) {
                usize p = first - 2;
                if (tokens[p].first.text == ">") {  // A<int>::f
                    int angle = 0;
                    while (true) {
                        if (tokens[p].first.text == ">") ++angle;
                        if (tokens[p].first.text == "<" && --angle == 0) break;
                        if (p == 0) break;
                        --p;
                    }
                    if (p == 0) break;
                    --p;
                }
                if (!tokens[p].first.word) break;
                first = p;
                continue;
            }
            break;
        }
        for (usize p = first; p < k; ++p) d.name += tokens[p].first.text;
        d.function = true;
        return d;
    }
    return d;
}

// What a braced item is, from its head (the text before its first top-level `{`).
ItemKind classify_head(std::string_view head) {
    const auto tokens = head_tokens(head);
    if (tokens.empty()) return ItemKind::block;
    // namespace n {, namespace {, namespace a::b {, inline namespace v1 {
    const usize ns = tokens[0].first.text == "inline" ? 1 : 0;
    if (ns < tokens.size() && tokens[ns].first.text == "namespace" &&
        std::all_of(tokens.begin() + static_cast<std::ptrdiff_t>(ns) + 1, tokens.end(), [](const auto& t) { return t.first.word || t.first.text == "::"; }))
        return ItemKind::block;
    if (tokens[0].first.text == "extern" && tokens.size() == 2 && !tokens[1].first.word) return ItemKind::block;  // extern "C" {
    // An initializer: `=` at the top level, outside a template's parameter list and an operator's name.
    usize k = 0;
    if (tokens[0].first.text == "template") {
        int angle = 0;
        for (k = 1; k < tokens.size(); ++k) {
            if (tokens[k].first.text == "<") ++angle;
            else if (tokens[k].first.text == ">" && --angle <= 0) break;
            else if (tokens[k].first.text == ">>" && (angle -= 2) <= 0) break;
        }
    }
    for (; k < tokens.size(); ++k) {
        const auto& [t, depth] = tokens[k];
        if (t.text == "operator") {
            while (k + 1 < tokens.size() && tokens[k + 1].first.text != "(") ++k;
            continue;
        }
        if (depth == 0 && t.text == "=") return ItemKind::declaration;
    }
    // A function's head ends with its parameter list, maybe followed by qualifiers or a constructor's
    // initializer list.
    if (!declarator(head).function) return ItemKind::declaration;
    const auto& last = tokens.back().first;
    if (last.text == ")") return ItemKind::function;
    static constexpr std::array<std::string_view, 7> qualifiers = {"const", "volatile", "noexcept", "override", "final", "&", "&&"};
    if (std::ranges::find(qualifiers, last.text) != qualifiers.end()) return ItemKind::function;
    for (const auto& [t, depth] : tokens)
        if (depth == 0 && t.text == "->") return ItemKind::function;  // a trailing return type
    return ItemKind::declaration;
}

} // namespace

std::vector<SourceItem> parse_source_items(std::string_view s) {
    std::vector<SourceItem> items;
    const usize n = s.size();
    usize pending = 0;  // where the next item's leading text starts
    usize line = 1;
    auto count_lines = [&](usize from, usize to) {
        for (usize k = from; k < to && k < n; ++k)
            if (s[k] == '\n') ++line;
    };
    usize i = 0;
    bool line_start = true;
    while (i < n) {
        if (space(s[i])) {
            if (s[i] == '\n') {
                line_start = true;
                ++line;
            }
            ++i;
            continue;
        }
        if (s.substr(i, 2) == "//" || s.substr(i, 2) == "/*") {
            const usize end = skip_literal_or_comment(s, i);
            count_lines(i, end);
            i = end;
            continue;
        }
        SourceItem item;
        item.line = line;
        const usize body = i;
        if (line_start && s[i] == '#') {
            item.kind = ItemKind::preprocessor;
            usize j = i + 1;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) ++j;
            usize w = j;
            while (w < n && word_char(s[w])) ++w;
            item.name = std::string(s.substr(j, w - j));
            // To the end of the line, continued by a backslash before it.
            usize end = i;
            while (end < n) {
                if (s[end] == '\n') {
                    usize back = end;
                    while (back > i && (s[back - 1] == '\r' || s[back - 1] == ' ' || s[back - 1] == '\t')) --back;
                    if (back > i && s[back - 1] == '\\') {
                        ++end;
                        continue;
                    }
                    break;
                }
                if (s.substr(end, 2) == "/*") {  // a comment can span lines inside a directive
                    end = skip_literal_or_comment(s, end);
                    continue;
                }
                ++end;
            }
            count_lines(i, end);
            item.text = std::string(s.substr(pending, end - pending));
            items.push_back(std::move(item));
            pending = end;
            i = end;
            line_start = false;
            continue;
        }
        // A declaration, a function definition or a block.
        int braces = 0, parens = 0;
        bool head_done = false;
        usize head_end = n;
        item.kind = ItemKind::declaration;
        usize end = n;
        usize j = i;
        while (j < n) {
            if (const usize skip = skip_literal_or_comment(s, j); skip != j) {
                j = skip;
                continue;
            }
            const char c = s[j];
            if (c == '{') {
                if (!head_done && braces == 0 && parens == 0) {
                    head_done = true;
                    head_end = j;
                    item.kind = classify_head(s.substr(body, j - body));
                }
                ++braces;
            } else if (c == '}') {
                if (--braces <= 0 && (item.kind == ItemKind::function || item.kind == ItemKind::block)) {
                    end = j + 1;
                    break;
                }
            } else if (c == '(') {
                ++parens;
            } else if (c == ')') {
                --parens;
            } else if (c == ';' && braces <= 0 && parens <= 0) {
                end = j + 1;
                break;
            }
            ++j;
        }
        const Declarator d = declarator(s.substr(body, (head_done ? head_end : end) - body));
        if (item.kind == ItemKind::function || item.kind == ItemKind::declaration) {
            item.name = d.name;
            item.is_static = d.is_static;
            item.function_declaration = item.kind == ItemKind::declaration && d.function && !head_done;
        }
        // A comment on the same line after the item is the item's.
        usize after = end;
        while (after < n && (s[after] == ' ' || s[after] == '\t')) ++after;
        if (s.substr(after, 2) == "//" || (s.substr(after, 2) == "/*" && s.substr(after, s.find("*/", after) - after).find('\n') == std::string_view::npos))
            end = skip_literal_or_comment(s, after);
        count_lines(i, end);
        item.text = std::string(s.substr(pending, end - pending));
        items.push_back(std::move(item));
        pending = end;
        i = end;
        line_start = false;
    }
    if (pending < n && s.substr(pending).find_first_not_of(" \t\r\n") != std::string_view::npos) {
        SourceItem tail;
        tail.kind = ItemKind::comment;
        tail.text = std::string(s.substr(pending));
        tail.line = line;
        items.push_back(std::move(tail));
    }
    return items;
}

std::string normalized(std::string_view s) {
    std::string out;
    bool last_word = false, gap = false;
    for (usize i = 0; i < s.size();) {
        if (space(s[i])) {
            gap = true;
            ++i;
            continue;
        }
        if (s.substr(i, 2) == "//" || s.substr(i, 2) == "/*") {
            i = skip_literal_or_comment(s, i);
            gap = true;
            continue;
        }
        if (const usize end = skip_literal_or_comment(s, i); end != i) {
            if (gap && last_word) out += ' ';
            out += s.substr(i, end - i);
            i = end;
            last_word = true;  // a literal next to a word keeps its space: L"x", u8"x" are words
            gap = false;
            continue;
        }
        const bool word = word_char(s[i]);
        if (gap && word && last_word) out += ' ';
        out += s[i];
        last_word = word;
        gap = false;
        ++i;
    }
    return out;
}

std::vector<std::string> declared_types(const SourceItem& item) {
    if (item.kind != ItemKind::declaration) return {};
    // Identifiers, `::`, and single characters, without comments and literals.
    const std::string_view body = item_body(item);
    std::vector<std::string> tokens;
    for (usize i = 0; i < body.size();) {
        if (space(body[i])) {
            ++i;
        } else if (const usize end = skip_literal_or_comment(body, i); end != i) {
            tokens.emplace_back("\"\"");  // a literal
            i = end;
        } else if (word_char(body[i])) {
            usize j = i;
            while (j < body.size() && word_char(body[j])) ++j;
            tokens.emplace_back(body.substr(i, j - i));
            i = j;
        } else if (body.substr(i, 2) == "::") {
            tokens.emplace_back("::");
            i += 2;
        } else {
            tokens.emplace_back(1, body[i++]);
        }
    }
    auto ident = [](const std::string& t) { return !t.empty() && (std::isalpha(static_cast<unsigned char>(t[0])) != 0 || t[0] == '_'); };
    // Skips a parenthesized or bracketed group starting at `i` (attributes such as __declspec(align(8))).
    auto skip_group = [&](usize i) {
        int depth = 0;
        for (; i < tokens.size(); ++i) {
            if (tokens[i] == "(" || tokens[i] == "[") ++depth;
            else if ((tokens[i] == ")" || tokens[i] == "]") && --depth == 0) return i + 1;
        }
        return i;
    };
    if (tokens.empty() || tokens[0] == "template") return {};
    if (tokens[0] == "using") {
        if (tokens.size() > 2 && ident(tokens[1]) && tokens[2] == "=") return {tokens[1]};
        return {};
    }
    // The tag of `struct|class|union|enum [attributes] Name` at `i`, and the index after it.
    auto tag = [&](usize i) -> std::optional<std::pair<std::string, usize>> {
        if (i >= tokens.size() || (tokens[i] != "struct" && tokens[i] != "class" && tokens[i] != "union" && tokens[i] != "enum")) return {};
        ++i;
        if (tokens[i - 1] == "enum" && i < tokens.size() && (tokens[i] == "class" || tokens[i] == "struct")) ++i;
        while (i < tokens.size() && (tokens[i] == "__declspec" || tokens[i] == "alignas" || tokens[i] == "[")) i = skip_group(tokens[i] == "[" ? i : i + 1);
        if (i < tokens.size() && ident(tokens[i]) && tokens[i] != "final") return std::pair{tokens[i], i + 1};
        return std::pair{std::string(), i};  // anonymous
    };
    if (tokens[0] != "typedef") {
        // A definition (`{`, a base clause, an enum's underlying type) or a forward declaration (`;`),
        // not a variable of the type.
        auto name = tag(0);
        if (!name || name->first.empty() || name->second >= tokens.size()) return {};
        const std::string& next = tokens[name->second];
        if (next == "{" || next == ";" || next == ":" || next == "final") return {name->first};
        return {};
    }
    // typedef: the declarators after the type, or after its braced body.
    static constexpr std::array<std::string_view, 12> kQualifiers = {"const",      "volatile",   "__cdecl", "__stdcall", "__fastcall", "__thiscall",
                                                                     "__vectorcall", "__ptr32", "__ptr64", "__unaligned", "__restrict", "WINAPI"};
    usize start = 1;
    for (usize i = 1, depth = 0; i < tokens.size(); ++i) {
        if (tokens[i] == "{") {
            if (depth++ == 0) continue;
        } else if (tokens[i] == "}") {
            if (--depth == 0) start = i + 1;
        }
    }
    std::vector<std::string> names;
    std::string last;  // the last identifier at the top level of the current declarator
    int depth = 0;
    for (usize i = start; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        if (t == "(" || t == "[") {
            // A function pointer's name: `(*Name)` or `(__stdcall *Name)`.
            if (t == "(" && depth == 0) {
                usize j = i + 1;
                while (j < tokens.size() && (tokens[j] == "*" || tokens[j] == "&" ||
                                             std::ranges::find(kQualifiers, tokens[j]) != kQualifiers.end()))
                    ++j;
                if (j > i + 1 && j < tokens.size() && ident(tokens[j]) && j + 1 < tokens.size() && tokens[j + 1] == ")") {
                    last = tokens[j];
                    i = j + 1;
                    continue;
                }
            }
            ++depth;
        } else if (t == ")" || t == "]") {
            --depth;
        } else if (depth == 0 && (t == "," || t == ";")) {
            if (!last.empty()) names.push_back(std::exchange(last, {}));
        } else if (depth == 0 && ident(t) && std::ranges::find(kQualifiers, t) == kQualifiers.end()) {
            last = t;
        }
    }
    if (!last.empty()) names.push_back(last);
    // `typedef struct Player { ... } Player;` also defines the tag.
    if (auto name = tag(1); name && !name->first.empty() && std::ranges::find(names, name->first) == names.end())
        names.insert(names.begin(), name->first);
    return names;
}

std::string_view item_body(const SourceItem& item) {
    std::string_view t = item.text;
    for (usize i = 0; i < t.size();) {
        if (space(t[i])) {
            ++i;
            continue;
        }
        if (t.substr(i, 2) == "//" || t.substr(i, 2) == "/*") {
            i = skip_literal_or_comment(t, i);
            continue;
        }
        return t.substr(i);
    }
    return {};
}

} // namespace decomp::matching
