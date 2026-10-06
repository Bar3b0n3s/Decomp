#include "search/permute.hpp"

#include "core/strings.hpp"
#include "matching/source_items.hpp"
#include "matching/toolchain.hpp"
#include "viewmodel/line_diff.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_set>

namespace decomp::search {

namespace {

constexpr usize npos = static_cast<usize>(-1);

bool word_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80; }
bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80; }

// Punctuators of more than one character, longest first.
constexpr std::string_view kPunctuators[] = {">>=", "<<=", "...", "->*", "<=>", "::", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
                                             "&&",  "||",  "+=",  "-=",  "*=",  "/=", "%=", "&=", "|=", "^=", "##", ".*"};

// The end of the string or character literal whose quote is at `i`.
usize skip_quoted(std::string_view s, usize i) {
    const char q = s[i];
    usize j = i + 1;
    while (j < s.size() && s[j] != q && s[j] != '\n') {
        if (s[j] == '\\' && j + 1 < s.size()) ++j;
        ++j;
    }
    return j < s.size() && s[j] == q ? j + 1 : j;
}

bool is_keyword(std::string_view w) {
    static const std::unordered_set<std::string_view> keywords = {
        "auto",     "break",    "case",     "char",   "const",    "continue", "default",  "do",      "double",   "else",
        "enum",     "extern",   "float",    "for",    "goto",     "if",       "int",      "long",    "register", "return",
        "short",    "signed",   "sizeof",   "static", "struct",   "switch",   "typedef",  "union",   "unsigned", "void",
        "volatile", "while",    "bool",     "class",  "delete",   "new",      "operator", "private", "public",   "protected",
        "template", "this",     "throw",    "try",    "catch",    "typename", "using",    "virtual", "namespace", "inline",
        "__int8",   "__int16",  "__int32",  "__int64", "wchar_t", "__try",    "__except", "__finally", "__leave", "constexpr"};
    return keywords.contains(w);
}

bool is_type_word(std::string_view w) {
    static const std::unordered_set<std::string_view> types = {"char",     "short",  "int",    "long",    "float",   "double", "signed",
                                                               "unsigned", "void",   "bool",   "const",   "volatile", "struct", "class",
                                                               "union",    "enum",   "__int8", "__int16", "__int32", "__int64", "wchar_t"};
    return types.contains(w);
}

// Binary operators and their precedence (higher binds tighter).
int precedence(std::string_view op) {
    if (op == "*" || op == "/" || op == "%") return 13;
    if (op == "+" || op == "-") return 12;
    if (op == "<<" || op == ">>") return 11;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 10;
    if (op == "==" || op == "!=") return 9;
    if (op == "&") return 8;
    if (op == "^") return 7;
    if (op == "|") return 6;
    if (op == "&&") return 5;
    if (op == "||") return 4;
    return 0;
}

bool is_assignment(std::string_view op) {
    return op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%=" || op == "&=" || op == "|=" || op == "^=" ||
           op == "<<=" || op == ">>=";
}

std::string_view flipped(std::string_view op) {
    if (op == "<") return ">";
    if (op == ">") return "<";
    if (op == "<=") return ">=";
    if (op == ">=") return "<=";
    return op;
}

// Whitespace runs as one space, at most `max` characters.
std::string snippet(std::string_view text, usize max = 40) {
    std::string out;
    bool gap = false;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            gap = !out.empty();
            continue;
        }
        if (gap) out += ' ';
        gap = false;
        out += c;
    }
    if (out.size() > max) out = out.substr(0, max - 3) + "...";
    return out;
}

u64 text_key(std::string_view text) {
    u64 h = 0xcbf29ce484222325ull;  // FNV-1a over the text without comments and spacing
    for (char c : matching::normalized(text)) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

enum class StmtKind : u8 { simple, block, if_else, loop, switch_, label, barrier };

struct Stmt {
    StmtKind kind = StmtKind::simple;
    usize first = 0, last = 0;  // token indices, inclusive
    std::vector<Stmt> children;  // block: its statements; if: then (and else); loop, switch: the body
    usize cond_open = npos, cond_close = npos;  // if, loop, switch: the parentheses of the condition or header
    usize else_token = npos;
    bool is_for = false;
};

// One edit: a range of the source and what replaces it.
struct Edit {
    MutationKind kind = MutationKind::move;
    std::string description;
    usize begin = 0, end = 0;
    std::string text;
};

// A source tokenized, with its brackets matched and the bodies of the target functions parsed into
// statements.
class Parsed {
public:
    Parsed(std::string_view source, std::span<const std::string> names) : s_(source), t_(tokenize(source)) {
        match_.assign(t_.size(), npos);
        std::vector<usize> stack;
        for (usize i = 0; i < t_.size(); ++i) {
            if (t_[i].kind != TokenKind::punct) continue;
            const std::string_view x = text(i);
            if (x == "(" || x == "[" || x == "{") {
                stack.push_back(i);
            } else if (x == ")" || x == "]" || x == "}") {
                const char open = x == ")" ? '(' : x == "]" ? '[' : '{';
                // An unbalanced closer leaves the openers that do not match it unmatched.
                while (!stack.empty() && text(stack.back())[0] != open) stack.pop_back();
                if (stack.empty()) continue;
                match_[stack.back()] = i;
                match_[i] = stack.back();
                stack.pop_back();
            }
        }
        for (auto [open, close] : function_bodies(source, t_, names)) {
            (void)close;
            bodies_.push_back(block(open));
        }
    }

    std::string_view source() const { return s_; }
    const std::vector<Token>& tokens() const { return t_; }
    const std::vector<Stmt>& bodies() const { return bodies_; }
    std::string_view text(usize i) const { return i < t_.size() ? t_[i].text(s_) : std::string_view(); }
    bool is(usize i, std::string_view x) const { return i < t_.size() && t_[i].kind != TokenKind::string && t_[i].kind != TokenKind::character && text(i) == x; }
    bool word(usize i) const { return i < t_.size() && t_[i].kind == TokenKind::word; }
    usize match(usize i) const { return i < match_.size() ? match_[i] : npos; }
    // Where the text of tokens [first, last] starts and ends; with its lead: from the end of the token before.
    usize begin(usize i) const { return t_[i].begin; }
    usize end(usize i) const { return t_[i].end; }
    usize lead(usize i) const { return t_[i].lead; }
    std::string_view range(usize first, usize last) const { return s_.substr(begin(first), end(last) - begin(first)); }
    bool newline_before(usize i) const { return s_.substr(lead(i), begin(i) - lead(i)).find('\n') != std::string_view::npos; }

private:
    Stmt block(usize open) {
        Stmt b;
        b.kind = StmtKind::block;
        b.first = open;
        b.last = match(open);
        usize i = open + 1;
        while (i < b.last) {
            Stmt st = statement(i, b.last);
            i = st.last + 1;
            b.children.push_back(std::move(st));
        }
        return b;
    }

    // The `;` that ends the statement at `i`, or the last token before `limit` when there is none.
    usize scan(usize i, usize limit) const {
        for (usize k = i; k < limit;) {
            if (is(k, ";")) return k;
            if ((is(k, "(") || is(k, "[") || is(k, "{")) && match(k) != npos && match(k) < limit) {
                k = match(k) + 1;
                continue;
            }
            ++k;
        }
        return limit - 1;
    }

    Stmt statement(usize i, usize limit) {
        auto barrier = [&](usize last) {
            Stmt st;
            st.kind = StmtKind::barrier;
            st.first = i;
            st.last = std::max(i, std::min(last, limit - 1));
            return st;
        };
        if (t_[i].kind == TokenKind::directive) return barrier(i);
        if (is(i, "{")) return match(i) != npos && match(i) < limit ? block(i) : barrier(limit - 1);
        if (is(i, ";")) {
            Stmt st;
            st.first = st.last = i;
            return st;
        }
        if (word(i)) {
            const std::string_view w = text(i);
            if (w == "if") {
                usize o = i + 1;
                if (is(o, "constexpr")) ++o;
                if (!is(o, "(") || match(o) == npos || match(o) + 1 >= limit) return barrier(scan(i, limit));
                Stmt st;
                st.kind = StmtKind::if_else;
                st.first = i;
                st.cond_open = o;
                st.cond_close = match(o);
                Stmt then = statement(st.cond_close + 1, limit);
                st.last = then.last;
                st.children.push_back(std::move(then));
                if (st.last + 2 < limit && is(st.last + 1, "else")) {
                    st.else_token = st.last + 1;
                    Stmt other = statement(st.else_token + 1, limit);
                    st.last = other.last;
                    st.children.push_back(std::move(other));
                }
                return st;
            }
            if (w == "for" || w == "while" || w == "switch") {
                const usize o = i + 1;
                if (!is(o, "(") || match(o) == npos || match(o) + 1 >= limit) return barrier(scan(i, limit));
                Stmt st;
                st.kind = w == "switch" ? StmtKind::switch_ : StmtKind::loop;
                st.first = i;
                st.cond_open = o;
                st.cond_close = match(o);
                st.is_for = w == "for";
                Stmt body = statement(st.cond_close + 1, limit);
                st.last = body.last;
                st.children.push_back(std::move(body));
                return st;
            }
            if (w == "do") {
                if (i + 1 >= limit) return barrier(i);
                Stmt body = statement(i + 1, limit);
                const usize k = body.last + 1;
                if (!is(k, "while") || !is(k + 1, "(") || match(k + 1) == npos || match(k + 1) >= limit) return barrier(scan(i, limit));
                Stmt st;
                st.kind = StmtKind::loop;
                st.first = i;
                st.cond_open = k + 1;
                st.cond_close = match(k + 1);
                st.last = is(st.cond_close + 1, ";") && st.cond_close + 1 < limit ? st.cond_close + 1 : st.cond_close;
                st.children.push_back(std::move(body));
                return st;
            }
            if (w == "case" || (w == "default" && is(i + 1, ":"))) {
                usize k = i + 1;
                while (k < limit && !is(k, ":")) ++k;
                Stmt st;
                st.kind = StmtKind::label;
                st.first = i;
                st.last = std::min(k, limit - 1);
                return st;
            }
            if (w == "return" || w == "break" || w == "continue" || w == "goto" || w == "throw" || w == "__leave") return barrier(scan(i, limit));
            if (w == "try" || w == "__try") {
                usize k = i + 1;
                if (!is(k, "{") || match(k) == npos || match(k) >= limit) return barrier(scan(i, limit));
                k = match(k) + 1;
                while (k < limit && (is(k, "catch") || is(k, "__except") || is(k, "__finally"))) {
                    ++k;
                    if (is(k, "(") && match(k) != npos) k = match(k) + 1;
                    if (!is(k, "{") || match(k) == npos || match(k) >= limit) return barrier(limit - 1);
                    k = match(k) + 1;
                }
                return barrier(k - 1);
            }
            if (w == "__asm" || w == "_asm" || w == "asm" || w == "__asm__") {
                usize k = i + 1;
                while (is(k, "volatile") || is(k, "__volatile__")) ++k;
                if (is(k, "{") && match(k) != npos) return barrier(match(k));
                if (is(k, "(")) return barrier(scan(i, limit));
                usize e = i;  // MSVC's __asm without braces runs to the end of the line
                while (e + 1 < limit && !newline_before(e + 1)) ++e;
                return barrier(e);
            }
            if (is(i + 1, ":") && !is_keyword(w)) {
                Stmt st;
                st.kind = StmtKind::label;
                st.first = i;
                st.last = i + 1;
                return st;
            }
        }
        Stmt st;
        st.first = i;
        st.last = scan(i, limit);
        if (!is(st.last, ";")) st.kind = StmtKind::barrier;
        return st;
    }

    std::string_view s_;
    std::vector<Token> t_;
    std::vector<usize> match_;
    std::vector<Stmt> bodies_;
};

bool movable(const Stmt& st) { return st.kind != StmtKind::label && st.kind != StmtKind::barrier; }

// A declaration statement: its type (the tokens before the first declarator) and its declarators.
struct Declaration {
    usize type_first = 0, first_declarator = 0;
    std::vector<std::pair<usize, usize>> declarators;  // token ranges, inclusive
    std::vector<std::string> names;
};

std::optional<Declaration> as_declaration(const Parsed& p, const Stmt& st) {
    if (st.kind != StmtKind::simple || !p.is(st.last, ";") || st.last <= st.first) return std::nullopt;
    const usize end = st.last;  // the `;`
    // The first declarator's name: the word before the first `=`, `,`, `[`, `(` or `{` at the top level.
    usize stop = st.first;
    while (stop < end && !p.is(stop, "=") && !p.is(stop, ",") && !p.is(stop, "[") && !p.is(stop, "(") && !p.is(stop, "{")) ++stop;
    if (stop == st.first || !p.word(stop - 1) || is_keyword(p.text(stop - 1))) return std::nullopt;
    usize d = stop - 1;
    while (d > st.first && (p.is(d - 1, "*") || p.is(d - 1, "&") || p.is(d - 1, "&&") ||
                            ((p.is(d - 1, "const") || p.is(d - 1, "volatile")) && d >= st.first + 2 && p.is(d - 2, "*"))))
        --d;
    if (d == st.first) return std::nullopt;
    for (usize k = st.first; k < d; ++k)
        if (!p.word(k) && !p.is(k, "::")) return std::nullopt;
    if (p.text(st.first) == "return" || p.text(st.first) == "typedef" || p.text(st.first) == "using") return std::nullopt;
    Declaration decl;
    decl.type_first = st.first;
    decl.first_declarator = d;
    usize start = d;
    for (usize k = d; k <= end; ++k) {
        if ((p.is(k, "(") || p.is(k, "[") || p.is(k, "{")) && p.match(k) != npos && p.match(k) < end) {
            k = p.match(k);
            continue;
        }
        if (p.is(k, ",") || k == end) {
            if (k == start) return std::nullopt;
            decl.declarators.emplace_back(start, k - 1);
            // Its name: the last word before its `=`, `[` or `(` (or its end).
            usize n = start;
            while (n < k && !p.is(n, "=") && !p.is(n, "[") && !p.is(n, "(") && !p.is(n, "{")) ++n;
            if (n > start && p.word(n - 1)) decl.names.emplace_back(p.text(n - 1));
            start = k + 1;
        }
    }
    return decl;
}

// The words a statement holds, and the names it declares.
struct Usage {
    std::vector<std::string_view> words;
    std::vector<std::string> declares;
};

Usage usage_of(const Parsed& p, const Stmt& st) {
    Usage u;
    for (usize k = st.first; k <= st.last; ++k)
        if (p.word(k)) u.words.push_back(p.text(k));
    if (auto d = as_declaration(p, st)) u.declares = d->names;
    return u;
}

bool uses_any(const Usage& u, const std::vector<std::string>& names) {
    for (const auto& n : names)
        if (std::ranges::find(u.words, std::string_view(n)) != u.words.end()) return true;
    return false;
}

constexpr usize kMoveWindow = 3;

void move_edits(const Parsed& p, const Stmt& block, std::vector<Edit>& out) {
    const auto& c = block.children;
    std::vector<Usage> usage;
    for (const auto& st : c) usage.push_back(usage_of(p, st));
    for (usize i = 0; i < c.size(); ++i) {
        if (!movable(c[i])) continue;
        for (usize j = i >= kMoveWindow ? i - kMoveWindow : 0; j < c.size() && j <= i + kMoveWindow; ++j) {
            if (j == i) continue;
            const usize lo = std::min(i, j), hi = std::max(i, j);
            bool ok = true;
            for (usize k = lo; k <= hi && ok; ++k) ok = movable(c[k]);
            // A declaration stays before what uses it.
            for (usize k = lo; k <= hi && ok; ++k) {
                if (k == i) continue;
                if (j > i) ok = !uses_any(usage[k], usage[i].declares);
                else ok = !uses_any(usage[i], usage[k].declares);
            }
            if (!ok) continue;
            std::vector<usize> order;
            for (usize k = lo; k <= hi; ++k)
                if (k != i) order.push_back(k);
            order.insert(order.begin() + static_cast<std::ptrdiff_t>(j - lo), i);
            Edit e;
            e.kind = MutationKind::move;
            e.begin = p.lead(c[lo].first);
            e.end = p.end(c[hi].last);
            for (usize k : order) e.text += p.source().substr(p.lead(c[k].first), p.end(c[k].last) - p.lead(c[k].first));
            e.description = std::format("move `{}` {} {}", snippet(p.range(c[i].first, c[i].last)), j > i ? "down" : "up", hi - lo);
            out.push_back(std::move(e));
        }
    }
}

void declaration_edits(const Parsed& p, const Stmt& st, std::vector<Edit>& out) {
    auto decl = as_declaration(p, st);
    if (!decl || decl->declarators.size() < 2) return;
    const std::string type(p.source().substr(p.begin(decl->type_first), p.begin(decl->first_declarator) - p.begin(decl->type_first)));
    std::vector<std::string> parts;
    for (auto [a, b] : decl->declarators) parts.emplace_back(p.range(a, b));
    const std::string whole(p.range(st.first, st.last));
    for (usize k = 0; k + 1 < parts.size(); ++k) {
        auto swapped = parts;
        std::swap(swapped[k], swapped[k + 1]);
        Edit e;
        e.kind = MutationKind::swap_declarators;
        e.begin = p.begin(st.first);
        e.end = p.end(st.last);
        e.text = type + join(swapped, ", ") + ";";
        e.description = std::format("swap `{}` and `{}` in `{}`", snippet(parts[k]), snippet(parts[k + 1]), snippet(whole));
        out.push_back(std::move(e));
    }
    // One declaration a line, indented like this one (and with its line ending).
    const std::string_view lead = p.source().substr(p.lead(st.first), p.begin(st.first) - p.lead(st.first));
    const usize nl = lead.rfind('\n');
    const std::string newline = nl != std::string_view::npos && nl > 0 && lead[nl - 1] == '\r' ? "\r\n" : "\n";
    const std::string separator = nl == std::string_view::npos ? std::string(" ") : newline + std::string(lead.substr(nl + 1));
    Edit e;
    e.kind = MutationKind::split_declaration;
    e.begin = p.begin(st.first);
    e.end = p.end(st.last);
    for (usize k = 0; k < parts.size(); ++k) e.text += (k ? separator : std::string()) + type + parts[k] + ";";
    e.description = std::format("split `{}`", snippet(whole));
    out.push_back(std::move(e));
}

bool operand_end(const Parsed& p, usize i) {
    if (i >= p.tokens().size()) return false;
    const auto kind = p.tokens()[i].kind;
    if (kind == TokenKind::number || kind == TokenKind::string || kind == TokenKind::character) return true;
    if (kind == TokenKind::word) return !is_keyword(p.text(i)) || p.text(i) == "this";
    return p.is(i, ")") || p.is(i, "]");
}

bool operand_start(const Parsed& p, usize i) {
    if (i >= p.tokens().size()) return false;
    const auto kind = p.tokens()[i].kind;
    if (kind == TokenKind::number || kind == TokenKind::string || kind == TokenKind::character) return true;
    if (kind == TokenKind::word) return !is_keyword(p.text(i)) || p.text(i) == "this" || p.text(i) == "sizeof";
    return p.is(i, "(") || p.is(i, "-") || p.is(i, "+") || p.is(i, "!") || p.is(i, "~") || p.is(i, "*") || p.is(i, "&") ||
           p.is(i, "++") || p.is(i, "--") || p.is(i, "::");
}

bool prefix_operator(const Parsed& p, usize i) {
    return p.is(i, "-") || p.is(i, "+") || p.is(i, "!") || p.is(i, "~") || p.is(i, "*") || p.is(i, "&") || p.is(i, "++") || p.is(i, "--");
}

// Whether the parentheses at `open` hold a type: a cast.
bool cast_group(const Parsed& p, usize open) {
    const usize close = p.match(open);
    if (close == npos || close == open + 1) return false;
    bool type = false;
    for (usize k = open + 1; k < close; ++k) {
        if (p.word(k)) {
            type = type || is_type_word(p.text(k));
            continue;
        }
        if (!p.is(k, "*") && !p.is(k, "&") && !p.is(k, "::")) return false;
    }
    return type || p.is(close - 1, "*");
}

// The last token of the unary expression that starts at `i` (before `limit`), or npos.
usize unary_end(const Parsed& p, usize i, usize limit) {
    while (i < limit && (prefix_operator(p, i) || p.is(i, "sizeof"))) ++i;
    if (i >= limit) return npos;
    usize j;
    if (p.is(i, "(")) {
        const usize c = p.match(i);
        if (c == npos || c >= limit) return npos;
        if (cast_group(p, i) && operand_start(p, c + 1)) return unary_end(p, c + 1, limit);
        j = c;
    } else if (p.word(i) || p.is(i, "::")) {
        j = i;
        if (p.is(j, "::")) ++j;
        if (j >= limit || !p.word(j)) return npos;
        if (p.text(j).ends_with("_cast") && p.is(j + 1, "<")) {
            usize k = j + 2;
            int depth = 1;
            for (; k < limit && depth > 0; ++k) depth += p.is(k, "<") ? 1 : p.is(k, ">") ? -1 : 0;
            if (depth != 0 || !p.is(k, "(") || p.match(k) == npos || p.match(k) >= limit) return npos;
            j = p.match(k);
        }
        while (j + 2 < limit && p.is(j + 1, "::") && p.word(j + 2)) j += 2;
    } else if (operand_end(p, i)) {
        j = i;
        while (j + 1 < limit && p.tokens()[j + 1].kind == TokenKind::string && p.tokens()[j].kind == TokenKind::string) ++j;
    } else {
        return npos;
    }
    for (;;) {
        const usize n = j + 1;
        if (n >= limit) break;
        if ((p.is(n, "[") || p.is(n, "(")) && p.match(n) != npos && p.match(n) < limit) {
            j = p.match(n);
            continue;
        }
        if ((p.is(n, ".") || p.is(n, "->")) && p.word(n + 1) && n + 1 < limit) {
            j = n + 1;
            continue;
        }
        if ((p.is(n, "++") || p.is(n, "--")) && !operand_start(p, n + 1)) {
            j = n;
            continue;
        }
        break;
    }
    return j;
}

// The first token of the unary expression that ends at `j` (after `floor`), or npos.
usize unary_begin(const Parsed& p, usize j, usize floor) {
    usize i = j;
    for (;;) {
        if (i <= floor) return npos;
        if (p.is(i, ")") || p.is(i, "]")) {
            const usize o = p.match(i);
            if (o == npos || o <= floor) return npos;
            if (p.is(o - 1, ">")) {  // static_cast<T>(x)
                usize k = o - 1;
                int depth = 1;
                while (k > floor + 1 && depth > 0) {
                    --k;
                    depth += p.is(k, ">") ? 1 : p.is(k, "<") ? -1 : 0;
                }
                if (depth != 0 || !p.word(k - 1) || !p.text(k - 1).ends_with("_cast")) return npos;
                i = k - 1;
                break;
            }
            if ((p.word(o - 1) && !is_keyword(p.text(o - 1))) || p.is(o - 1, ")") || p.is(o - 1, "]")) {
                i = o - 1;  // a call or a subscript
                continue;
            }
            i = o;  // parentheses around an expression
            break;
        }
        if (p.is(i, "++") || p.is(i, "--")) {
            i -= 1;
            continue;
        }
        if (operand_end(p, i)) {
            if (p.is(i - 1, ".") || p.is(i - 1, "->") || p.is(i - 1, "::")) {
                if (p.is(i - 1, "::") && !operand_end(p, i - 2)) {
                    i -= 1;
                    break;
                }
                i -= 2;
                continue;
            }
            break;
        }
        return npos;
    }
    // Prefix operators and casts before it.
    for (;;) {
        if (i <= floor + 1) break;
        const usize b = i - 1;
        if ((prefix_operator(p, b) && !operand_end(p, b - 1)) || p.is(b, "sizeof")) {
            i = b;
            continue;
        }
        if (p.is(b, ")") && p.match(b) != npos && p.match(b) > floor && cast_group(p, p.match(b))) {
            i = p.match(b);
            continue;
        }
        break;
    }
    return i;
}

// The operand of an operator of precedence `prec` that starts at `i`: a unary expression and what the
// operators that bind tighter than `prec` join to it. Its last token, or npos.
usize operand_after(const Parsed& p, usize i, usize limit, int prec) {
    usize j = unary_end(p, i, limit);
    while (j != npos && j + 2 < limit && p.tokens()[j + 1].kind == TokenKind::punct && precedence(p.text(j + 1)) > prec && operand_start(p, j + 2))
        j = unary_end(p, j + 2, limit);
    return j;
}

// The operand of an operator of precedence `prec` that ends at `j`. Its first token, or npos.
usize operand_before(const Parsed& p, usize j, usize floor, int prec) {
    usize i = unary_begin(p, j, floor);
    while (i != npos && i >= floor + 3 && p.tokens()[i - 1].kind == TokenKind::punct && precedence(p.text(i - 1)) > prec && operand_end(p, i - 2))
        i = unary_begin(p, i - 2, floor);
    return i;
}

// What can be before a whole operand of an operator of precedence `prec`, and after one.
bool bounds_left(const Parsed& p, usize i, int prec) {
    const std::string_view x = p.text(i);
    if (x == "(" || x == "[" || x == "{" || x == "}" || x == ";" || x == "," || x == "?" || x == ":" || x == "return" || x == "case" ||
        is_assignment(x))
        return true;
    const int q = precedence(x);
    return q > 0 && q < prec;
}

bool bounds_right(const Parsed& p, usize i, int prec) {
    const std::string_view x = p.text(i);
    if (x == ")" || x == "]" || x == "}" || x == ";" || x == "," || x == "?" || x == ":" || is_assignment(x)) return true;
    const int q = precedence(x);
    return q > 0 && q <= prec;
}

bool side_effects(const Parsed& p, usize first, usize last) {
    for (usize k = first; k <= last; ++k) {
        if (is_assignment(p.text(k)) || p.is(k, "++") || p.is(k, "--")) return true;
        if (p.is(k, "(") && k > first && p.word(k - 1) && !is_keyword(p.text(k - 1))) return true;  // a call
    }
    return false;
}

void operator_edits(const Parsed& p, const Stmt& body, std::vector<Edit>& out) {
    for (usize k = body.first + 2; k + 2 < body.last; ++k) {
        if (p.tokens()[k].kind != TokenKind::punct) continue;
        const std::string_view op = p.text(k);
        const bool commutes = op == "+" || op == "*" || op == "&" || op == "|" || op == "^" || op == "==" || op == "!=" || op == "&&" || op == "||";
        const bool flips = op == "<" || op == ">" || op == "<=" || op == ">=";
        if (!commutes && !flips) continue;
        if (!operand_end(p, k - 1) || !operand_start(p, k + 1)) continue;
        const int prec = precedence(op);
        const usize l = operand_before(p, k - 1, body.first, prec);
        const usize r = operand_after(p, k + 1, body.last, prec);
        if (l == npos || r == npos || l >= k || r <= k) continue;
        if (!bounds_left(p, l - 1, prec) || !bounds_right(p, r + 1, prec)) continue;
        if ((op == "&&" || op == "||") && (side_effects(p, l, k - 1) || side_effects(p, k + 1, r))) continue;
        const std::string_view left = p.range(l, k - 1), right = p.range(k + 1, r);
        std::string middle(p.source().substr(p.end(k - 1), p.begin(k + 1) - p.end(k - 1)));
        if (flips) middle.replace(p.begin(k) - p.end(k - 1), op.size(), flipped(op));
        Edit e;
        e.kind = flips ? MutationKind::flip_comparison : MutationKind::commute;
        e.begin = p.begin(l);
        e.end = p.end(r);
        e.text = std::string(right) + middle + std::string(left);
        e.description = flips ? std::format("flip `{}`", snippet(p.range(l, r)))
                              : std::format("swap the operands of `{}` in `{}`", op, snippet(p.range(l, r)));
        out.push_back(std::move(e));
    }
}

void negate_edit(const Parsed& p, const Stmt& st, std::vector<Edit>& out) {
    if (st.kind != StmtKind::if_else || st.else_token == npos || st.children.size() != 2 || st.cond_close <= st.cond_open + 1) return;
    const Stmt& then = st.children[0];
    const Stmt& other = st.children[1];
    std::string condition;
    const usize a = st.cond_open + 1, b = st.cond_close - 1;
    if (p.is(a, "!") && p.is(a + 1, "(") && p.match(a + 1) == b && b > a + 2) condition = std::string(p.range(a + 2, b - 1));
    else condition = "!(" + std::string(p.range(a, b)) + ")";
    // The new then-branch is an if: braces keep the else with the outer if.
    std::string new_then(p.range(other.first, other.last));
    if (other.kind == StmtKind::if_else) new_then = "{ " + new_then + " }";
    Edit e;
    e.kind = MutationKind::negate_if;
    e.begin = p.begin(st.cond_open);
    e.end = p.end(other.last);
    e.text = "(" + condition + ")" + std::string(p.source().substr(p.end(st.cond_close), p.begin(then.first) - p.end(st.cond_close))) + new_then +
             std::string(p.source().substr(p.end(then.last), p.begin(other.first) - p.end(then.last))) + std::string(p.range(then.first, then.last));
    e.description = std::format("negate `if ({})` and swap its branches", snippet(p.range(a, b)));
    out.push_back(std::move(e));
}

void increment_edit(const Parsed& p, usize first, usize last, std::vector<Edit>& out) {
    if (last != first + 1) return;
    std::string text;
    if ((p.is(first, "++") || p.is(first, "--")) && p.word(last)) text = std::string(p.text(last)) + std::string(p.text(first));
    else if (p.word(first) && (p.is(last, "++") || p.is(last, "--"))) text = std::string(p.text(last)) + std::string(p.text(first));
    else return;
    Edit e;
    e.kind = MutationKind::increment;
    e.begin = p.begin(first);
    e.end = p.end(last);
    e.description = std::format("`{}` for `{}`", text, p.range(first, last));
    e.text = std::move(text);
    out.push_back(std::move(e));
}

void statement_edits(const Parsed& p, const Stmt& st, std::vector<Edit>& out) {
    switch (st.kind) {
    case StmtKind::block: move_edits(p, st, out); break;
    case StmtKind::simple:
        declaration_edits(p, st, out);
        if (st.last == st.first + 2) increment_edit(p, st.first, st.first + 1, out);
        break;
    case StmtKind::if_else: negate_edit(p, st, out); break;
    case StmtKind::loop:
        if (st.is_for) {  // the increment of a for loop: after the header's second `;`
            usize semis = 0, k = st.cond_open + 1;
            for (; k < st.cond_close && semis < 2; ++k)
                if (p.is(k, ";")) ++semis;
                else if ((p.is(k, "(") || p.is(k, "[") || p.is(k, "{")) && p.match(k) != npos && p.match(k) < st.cond_close) k = p.match(k);
            if (semis == 2 && k < st.cond_close) increment_edit(p, k, st.cond_close - 1, out);
        }
        break;
    default: break;
    }
    for (const auto& child : st.children) statement_edits(p, child, out);
}

std::vector<Edit> edits_of(const Parsed& p) {
    std::vector<Edit> out;
    for (const auto& body : p.bodies()) {
        statement_edits(p, body, out);
        operator_edits(p, body, out);
    }
    return out;
}

Mutation apply(std::string_view source, Edit&& e) {
    Mutation m;
    m.kind = e.kind;
    m.description = std::move(e.description);
    m.source.reserve(source.size() + e.text.size());
    m.source.append(source.substr(0, e.begin)).append(e.text).append(source.substr(e.end));
    return m;
}

// How often a kind is picked, among the kinds that apply.
int weight(MutationKind kind) {
    switch (kind) {
    case MutationKind::move: return 6;
    case MutationKind::commute: return 3;
    case MutationKind::flip_comparison:
    case MutationKind::negate_if:
    case MutationKind::swap_declarators: return 2;
    case MutationKind::split_declaration:
    case MutationKind::increment: return 1;
    }
    return 1;
}

} // namespace

std::vector<Token> tokenize(std::string_view s) {
    std::vector<Token> out;
    const usize n = s.size();
    usize i = 0, lead = 0;
    bool line_start = true;
    while (i < n) {
        const char c = s[i];
        if (c == '\n') {
            line_start = true;
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            const usize e = s.find("*/", i + 2);
            i = e == std::string_view::npos ? n : e + 2;
            continue;
        }
        Token t;
        t.lead = lead;
        t.begin = i;
        if (c == '#' && line_start) {
            usize j = i;
            while (j < n) {
                if (s[j] == '\n') {
                    usize b = j;
                    while (b > i && (s[b - 1] == '\r' || s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
                    if (b > i && s[b - 1] == '\\') {
                        ++j;
                        continue;
                    }
                    break;
                }
                if (s[j] == '/' && j + 1 < n && s[j + 1] == '*') {
                    const usize e = s.find("*/", j + 2);
                    j = e == std::string_view::npos ? n : e + 2;
                    continue;
                }
                if (s[j] == '/' && j + 1 < n && s[j + 1] == '/') {
                    while (j < n && s[j] != '\n') ++j;
                    break;
                }
                ++j;
            }
            while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\r')) --j;
            t.kind = TokenKind::directive;
            t.end = j;
        } else if (word_start(c)) {
            usize j = i;
            while (j < n && word_char(s[j])) ++j;
            const std::string_view w = s.substr(i, j - i);
            const bool prefix = w == "L" || w == "u" || w == "U" || w == "u8" || w == "R" || w == "LR" || w == "uR" || w == "UR" || w == "u8R";
            if (prefix && j < n && (s[j] == '"' || s[j] == '\'')) {
                if (w.back() == 'R' && s[j] == '"') {
                    const usize open = s.find('(', j + 1);
                    if (open != std::string_view::npos) {
                        const std::string close = ")" + std::string(s.substr(j + 1, open - j - 1)) + "\"";
                        const usize e = s.find(close, open + 1);
                        j = e == std::string_view::npos ? n : e + close.size();
                    } else {
                        j = skip_quoted(s, j);
                    }
                    t.kind = TokenKind::string;
                } else {
                    t.kind = s[j] == '"' ? TokenKind::string : TokenKind::character;
                    j = skip_quoted(s, j);
                }
            } else {
                t.kind = TokenKind::word;
            }
            t.end = j;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            const bool hex = c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X');
            usize j = i + 1;
            while (j < n) {
                const char d = s[j];
                const char before = s[j - 1];
                if ((d == '+' || d == '-') && (hex ? (before == 'p' || before == 'P') : (before == 'e' || before == 'E'))) {
                    ++j;
                    continue;
                }
                if (word_char(d) || d == '.' || (d == '\'' && j + 1 < n && word_char(s[j + 1]))) {
                    ++j;
                    continue;
                }
                break;
            }
            t.kind = TokenKind::number;
            t.end = j;
        } else if (c == '"' || c == '\'') {
            t.kind = c == '"' ? TokenKind::string : TokenKind::character;
            t.end = skip_quoted(s, i);
        } else {
            t.kind = TokenKind::punct;
            t.end = i + 1;
            for (auto p : kPunctuators)
                if (s.substr(i, p.size()) == p) {
                    t.end = i + p.size();
                    break;
                }
        }
        out.push_back(t);
        i = t.end;
        lead = i;
        line_start = false;
    }
    return out;
}

std::string_view to_string(MutationKind kind) {
    switch (kind) {
    case MutationKind::move: return "move";
    case MutationKind::swap_declarators: return "swap_declarators";
    case MutationKind::split_declaration: return "split_declaration";
    case MutationKind::commute: return "commute";
    case MutationKind::flip_comparison: return "flip_comparison";
    case MutationKind::negate_if: return "negate_if";
    case MutationKind::increment: return "increment";
    }
    return "move";
}

std::vector<std::pair<usize, usize>> function_bodies(std::string_view source, std::span<const Token> tokens, std::span<const std::string> names) {
    std::vector<std::pair<usize, usize>> out;
    // The tokens from an offset on: the first that starts there or after it.
    auto first_at = [&](usize offset) { return static_cast<usize>(std::ranges::lower_bound(tokens, offset, {}, &Token::begin) - tokens.begin()); };
    auto named = [&](std::string_view name) {
        return std::ranges::any_of(names, [&](const std::string& n) {
            return n == name || (n.size() > name.size() + 2 && n.ends_with(name) && n.substr(n.size() - name.size() - 2, 2) == "::");
        });
    };
    std::function<void(usize, usize)> visit = [&](usize from, usize to) {
        usize offset = from;
        for (const auto& item : matching::parse_source_items(source.substr(from, to - from))) {
            const usize begin = offset, end = offset + item.text.size();
            offset = end;
            if (item.kind != matching::ItemKind::function && item.kind != matching::ItemKind::block) continue;
            if (item.kind == matching::ItemKind::function && !named(item.name)) continue;
            // Its braces: the item ends with the closing one (a block maybe with a `;` after it).
            const usize lo = first_at(begin), hi = first_at(end);
            if (hi <= lo) continue;
            usize last = hi - 1;
            if (tokens[last].text(source) == ";" && last > lo) --last;
            if (tokens[last].kind != TokenKind::punct || tokens[last].text(source) != "}") continue;
            int depth = 0;
            usize open = last;
            for (usize k = last + 1; k-- > lo;) {
                if (tokens[k].kind != TokenKind::punct) continue;
                const auto x = tokens[k].text(source);
                if (x == "}") ++depth;
                else if (x == "{" && --depth == 0) {
                    open = k;
                    break;
                }
            }
            if (open == last) continue;
            if (item.kind == matching::ItemKind::function) out.emplace_back(open, last);
            else visit(tokens[open].end, tokens[last].begin);
        }
    };
    visit(0, source.size());
    return out;
}

std::vector<Mutation> all_mutations(std::string_view source, std::span<const std::string> names) {
    const Parsed p(source, names);
    std::vector<Mutation> out;
    for (auto& e : edits_of(p)) out.push_back(apply(source, std::move(e)));
    return out;
}

std::optional<Mutation> random_mutation(std::string_view source, std::span<const std::string> names, std::mt19937_64& rng) {
    const Parsed p(source, names);
    auto edits = edits_of(p);
    if (edits.empty()) return std::nullopt;
    // A kind (weighted), then one of its edits.
    std::vector<MutationKind> kinds;
    int total = 0;
    for (const auto& e : edits)
        if (std::ranges::find(kinds, e.kind) == kinds.end()) {
            kinds.push_back(e.kind);
            total += weight(e.kind);
        }
    std::ranges::sort(kinds);
    int pick = static_cast<int>(rng() % static_cast<u64>(total));
    MutationKind kind = kinds.back();
    for (MutationKind k : kinds) {
        if (pick < weight(k)) {
            kind = k;
            break;
        }
        pick -= weight(k);
    }
    std::vector<usize> of_kind;
    for (usize i = 0; i < edits.size(); ++i)
        if (edits[i].kind == kind) of_kind.push_back(i);
    return apply(source, std::move(edits[of_kind[rng() % of_kind.size()]]));
}

PermuteResult permute(const Program& program, const matching::MatchSetup& setup, const Configuration& configuration, const Probe& probe,
                      std::span<const std::string> names, const PermuteOptions& options) {
    PermuteResult r;
    r.source = probe.source;
    matching::MatchSetup s = setup;
    s.cancelled = options.cancelled;
    const int threads = options.threads > 0 ? options.threads : matching::max_parallel_compiles();
    const usize batch = options.batch > 0 ? options.batch : std::max<usize>(8, 2 * static_cast<usize>(std::max(threads, 1)));
    const auto started = std::chrono::steady_clock::now();
    auto cancelled = [&] { return options.cancelled && options.cancelled(); };
    auto out_of_time = [&] { return options.time_limit.count() > 0 && std::chrono::steady_clock::now() - started >= options.time_limit; };
    auto score = [&](const std::string& text) -> std::optional<Score> {
        Probe candidate = probe;
        candidate.source = text;
        auto e = evaluate(program, s, configuration, std::span<const Probe>(&candidate, 1));
        if (e.cancelled) return std::nullopt;
        return e.score;
    };

    const auto first = score(probe.source);
    if (!first) {
        r.cancelled = true;
        return r;
    }
    r.candidates = 1;
    if (options.log) options.log->add("start", *first);
    r.start_score = r.score = *first;
    if (all_mutations(probe.source, names).empty()) {
        r.error = "no edit applies: no statements to reorder or operands to swap in the bodies of the functions";
        return r;
    }

    std::string current = probe.source;
    Score current_score = *first;
    std::vector<std::string> current_steps;
    std::unordered_set<u64> seen{text_key(current)};
    std::mt19937_64 rng(options.seed);
    while (!r.score.complete() && r.candidates < options.max_candidates && !cancelled() && !out_of_time()) {
        struct Candidate {
            std::string source;
            std::vector<std::string> steps;
        };
        std::vector<Candidate> round;
        const usize room = std::min(batch, options.max_candidates - r.candidates);
        // Every single edit not tried yet, in a random order, leaving a quarter of the round to stacks of edits.
        auto singles = all_mutations(current, names);
        for (usize i = singles.size(); i > 1; --i) std::swap(singles[i - 1], singles[rng() % i]);
        const usize single_room = room - room / 4;
        for (auto& m : singles) {
            if (round.size() >= single_room) break;
            if (seen.insert(text_key(m.source)).second) round.push_back({std::move(m.source), {std::move(m.description)}});
        }
        for (usize tries = 0; round.size() < room && tries < room * 8; ++tries) {
            std::string text = current;
            std::vector<std::string> steps;
            const usize depth = 2 + rng() % 2;
            for (usize d = 0; d < depth; ++d) {
                auto m = random_mutation(text, names, rng);
                if (!m) break;
                text = std::move(m->source);
                steps.push_back(std::move(m->description));
            }
            if (!steps.empty() && seen.insert(text_key(text)).second) round.push_back({std::move(text), std::move(steps)});
        }
        if (round.empty()) break;  // every source within reach was tried

        std::vector<std::optional<Score>> scores(round.size());
        parallel_for(round.size(), threads, [&](usize i) { scores[i] = score(round[i].source); }, cancelled);
        std::optional<usize> best;
        for (usize i = 0; i < round.size(); ++i) {  // logged in the order they were made
            if (!scores[i]) continue;
            ++r.candidates;
            if (options.log) options.log->add(join(round[i].steps, "; "), *scores[i]);
            if (!best || scores[i]->better_than(*scores[*best])) best = i;
        }
        if (!best) break;
        const Score& b = *scores[*best];
        // Better, or as good now and then: a plateau has to be crossed sometimes.
        if (b.better_than(current_score) || (b == current_score && rng() % 4 == 0)) {
            current = std::move(round[*best].source);
            current_score = b;
            for (auto& step : round[*best].steps) current_steps.push_back(std::move(step));
            if (current_score.better_than(r.score)) {
                r.score = current_score;
                r.source = current;
                r.steps = current_steps;
            }
        }
    }
    // As good, closer to the start: undo the edits the score does not need (an operand order the search
    // crossed a plateau with).
    auto distance = [&](std::string_view text) {
        const auto d = vm::diff_lines(probe.source, text);
        return d.inserted + d.deleted;
    };
    usize tidy_distance = distance(r.source);
    std::unordered_set<u64> tidied;
    while (tidy_distance > 0 && r.score.better_than(r.start_score) && r.candidates < options.max_candidates && !cancelled()) {
        std::vector<Mutation> closer;
        for (auto& m : all_mutations(r.source, names))
            if (distance(m.source) < tidy_distance && tidied.insert(text_key(m.source)).second) closer.push_back(std::move(m));
        if (closer.size() > options.max_candidates - r.candidates) closer.resize(options.max_candidates - r.candidates);
        if (closer.empty()) break;
        std::vector<std::optional<Score>> scores(closer.size());
        parallel_for(closer.size(), threads, [&](usize i) { scores[i] = score(closer[i].source); }, cancelled);
        std::optional<usize> pick;
        usize pick_distance = tidy_distance;
        for (usize i = 0; i < closer.size(); ++i) {
            if (!scores[i]) continue;
            ++r.candidates;
            if (options.log) options.log->add("tidy: " + closer[i].description, *scores[i]);
            if (r.score.better_than(*scores[i])) continue;
            if (const usize d = distance(closer[i].source); d < pick_distance) {
                pick = i;
                pick_distance = d;
            }
        }
        if (!pick) break;
        r.source = std::move(closer[*pick].source);
        r.score = *scores[*pick];
        r.steps.push_back("tidy: " + closer[*pick].description);
        tidy_distance = pick_distance;
    }
    r.cancelled = cancelled();
    return r;
}

Json to_json(const PermuteResult& result) {
    Json j{{"steps", result.steps},
           {"score", to_json(result.score)},
           {"start_score", to_json(result.start_score)},
           {"candidates", result.candidates},
           {"cancelled", result.cancelled}};
    if (!result.error.empty()) j["error"] = result.error;
    return j;
}

} // namespace decomp::search
