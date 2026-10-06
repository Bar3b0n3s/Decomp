#include "matching/unit_source.hpp"

#include "analysis/demangle.hpp"
#include "core/strings.hpp"
#include "formats/coff.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace decomp::matching {

namespace {

// The text without the blank lines before it and the whitespace after it (comments stay).
std::string_view trimmed(std::string_view text) {
    usize start = 0;
    for (usize i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') start = i + 1;
        else if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r') break;
    }
    text.remove_prefix(std::min(start, text.size()));
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    return text;
}

// Whether the text starts after a blank line.
bool after_blank_line(std::string_view text) {
    int newlines = 0;
    for (char c : text) {
        if (c == '\n') ++newlines;
        else if (c != ' ' && c != '\t' && c != '\r') break;
    }
    return newlines >= 2;
}

// A function's head without `__declspec(naked)`: MSVC takes it only on a definition.
std::string without_naked(std::string head) {
    for (usize at = head.find("__declspec"); at != std::string::npos; at = head.find("__declspec", at + 1)) {
        usize i = at + 10;
        auto skip_space = [&] {
            while (i < head.size() && (head[i] == ' ' || head[i] == '\t')) ++i;
        };
        skip_space();
        if (i >= head.size() || head[i] != '(') continue;
        ++i;
        skip_space();
        if (head.compare(i, 5, "naked") != 0) continue;
        i += 5;
        skip_space();
        if (i >= head.size() || head[i] != ')') continue;
        ++i;
        skip_space();
        head.erase(at, i - at);
        return head;
    }
    return head;
}

bool is_conditional(const SourceItem& item) {
    if (item.kind != ItemKind::preprocessor) return false;
    for (std::string_view d : {"if", "ifdef", "ifndef", "elif", "else", "endif"})
        if (item.name == d) return true;
    return false;
}

UnitSource::Function make_function(u64 va, std::string text) {
    UnitSource::Function f;
    f.va = va;
    f.text = std::string(trimmed(text));
    for (const auto& item : parse_source_items(f.text))
        if (item.kind == ItemKind::function) {
            f.name = item.name;
            f.is_static = item.is_static;
            break;
        }
    return f;
}

} // namespace

UnitSource UnitSource::parse(std::string_view text) {
    UnitSource u;
    std::vector<std::pair<usize, usize>> markers;  // (start of the marker line, start of the next line)
    std::vector<u64> addresses;
    for (usize pos = 0; pos < text.size();) {
        usize eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        const std::string_view line = text.substr(pos, eol - pos);
        if (line.starts_with(kFunctionMarker))
            if (auto va = parse_u64(trim(line.substr(kFunctionMarker.size())))) {
                markers.emplace_back(pos, std::min(eol + 1, text.size()));
                addresses.push_back(*va);
            }
        pos = eol + 1;
    }
    u.prelude = parse_source_items(text.substr(0, markers.empty() ? text.size() : markers.front().first));
    for (usize k = 0; k < markers.size(); ++k) {
        const usize end = k + 1 < markers.size() ? markers[k + 1].first : text.size();
        u.functions.push_back(make_function(addresses[k], std::string(text.substr(markers[k].second, end - markers[k].second))));
    }
    std::ranges::stable_sort(u.functions, {}, &Function::va);
    return u;
}

std::string UnitSource::render() const {
    std::string out;
    for (const auto& item : prelude) {
        const std::string_view text = trimmed(item.text);
        if (text.empty()) continue;
        if (!out.empty() && after_blank_line(item.text)) out += '\n';
        out += text;
        out += '\n';
    }
    for (const auto& f : functions) {
        if (!out.empty()) out += '\n';
        out += std::format("{}{:#010x}\n", kFunctionMarker, f.va);
        out += trimmed(f.text);
        out += '\n';
    }
    return out;
}

const UnitSource::Function* UnitSource::find(u64 va) const {
    auto it = std::ranges::find(functions, va, &Function::va);
    return it == functions.end() ? nullptr : &*it;
}

bool UnitSource::remove(u64 va) { return std::erase_if(functions, [&](const Function& f) { return f.va == va; }) > 0; }

Result<void> compose_function(UnitSource& unit, u64 va, std::span<const std::string> names, std::string_view source, bool join) {
    const auto items = parse_source_items(source);
    std::optional<usize> def;
    for (usize i = 0; i < items.size() && !def; ++i)
        if (items[i].kind == ItemKind::function && std::ranges::find(names, items[i].name) != names.end()) def = i;
    if (!def)
        return make_error(ErrorCode::invalid_argument,
                          "the source has no definition of {} at its top level: a unit source holds functions defined outside class and "
                          "namespace blocks",
                          names.empty() ? std::string("the function") : names.front());
    // The directives that belong with the definition: #pragma and #line before it, #pragma after it.
    usize first = *def, last = *def;
    while (first > 0 && items[first - 1].kind == ItemKind::preprocessor && (items[first - 1].name == "pragma" || items[first - 1].name == "line"))
        --first;
    while (last + 1 < items.size() && items[last + 1].kind == ItemKind::preprocessor && items[last + 1].name == "pragma") ++last;
    std::string text;
    for (usize k = first; k <= last; ++k) text += items[k].text;
    UnitSource::Function entry = make_function(va, text);
    entry.name = items[*def].name;
    entry.is_static = items[*def].is_static;

    unit.remove(va);
    // The entry is the function's definition: one the prelude has (another function's source brought it)
    // becomes a declaration, so the functions before the entry still see it (a member function's class
    // declares it already). A static definition makes declarations without `static` conflict.
    for (auto it = unit.prelude.begin(); it != unit.prelude.end();) {
        if (it->name.empty() || it->name != entry.name) {
            ++it;
            continue;
        }
        if (it->kind == ItemKind::function) {
            const std::string_view body = item_body(*it);
            const auto brace = body.find('{');
            if (entry.name.find("::") == std::string::npos && brace != std::string_view::npos) {
                auto declaration = parse_source_items(without_naked(std::string(trimmed(body.substr(0, brace)))) + ";");
                if (declaration.size() == 1) {
                    *it++ = std::move(declaration.front());
                    continue;
                }
            }
            it = unit.prelude.erase(it);
        } else if (entry.is_static && it->function_declaration && !it->is_static) {
            it = unit.prelude.erase(it);
        } else {
            ++it;
        }
    }
    if (!join) {
        auto at = std::ranges::upper_bound(unit.functions, va, {}, &UnitSource::Function::va);
        unit.functions.insert(at, std::move(entry));
        return {};
    }
    std::set<std::string> seen;
    for (const auto& p : unit.prelude) seen.insert(normalized(item_body(p)));
    for (const auto& f : unit.functions)
        for (const auto& item : parse_source_items(f.text)) seen.insert(normalized(item_body(item)));
    auto is_static_in_unit = [&](const std::string& name) {
        if (entry.is_static && entry.name == name) return true;
        for (const auto& f : unit.functions)
            if (f.is_static && f.name == name) return true;
        for (const auto& p : unit.prelude)
            if (p.is_static && p.name == name && (p.kind == ItemKind::function || p.function_declaration)) return true;
        return false;
    };
    auto defined_in_unit = [&](const std::string& name) {
        if (entry.name == name) return true;
        for (const auto& f : unit.functions)
            if (f.name == name) return true;
        for (const auto& p : unit.prelude)
            if (p.kind == ItemKind::function && p.name == name) return true;
        return false;
    };
    for (usize i = 0; i < items.size(); ++i) {
        if (i >= first && i <= last) continue;
        const SourceItem& item = items[i];
        if (item.kind == ItemKind::comment) continue;
        if (!is_conditional(item)) {
            if (item.kind == ItemKind::function && !item.name.empty() && defined_in_unit(item.name)) continue;
            if (item.function_declaration && !item.is_static && is_static_in_unit(item.name)) continue;
            if (!seen.insert(normalized(item_body(item))).second) continue;
        }
        unit.prelude.push_back(item);
    }
    auto at = std::ranges::upper_bound(unit.functions, va, {}, &UnitSource::Function::va);
    unit.functions.insert(at, std::move(entry));
    return {};
}

std::vector<std::string> definition_names(const Symbol& function) {
    std::vector<std::string> out;
    auto add = [&](std::string name) {
        if (!name.empty() && std::ranges::find(out, name) == out.end()) out.push_back(std::move(name));
    };
    add(qualified_name(function.name));
    add(function.pdb_name);
    add(undecorate(function.name));
    add(function.name);
    return out;
}

bool UnitVerification::all_byte_exact() const {
    return error.empty() && std::ranges::all_of(functions, [](const UnitCheck& c) { return c.byte_exact(); });
}

Result<UnitVerification> verify_unit(const Program& program, const MatchSetup& setup, const std::string& source, std::string_view file_name,
                                     std::span<const u64> vas) {
    UnitVerification out;
    Compiler compiler(setup.toolchain, setup.work_dir, setup.cache_dir);
    CompileRequest req;
    req.source = source;
    req.flags = setup.flags;
    req.include_dirs = setup.include_dirs;
    req.cancelled = setup.cancelled;
    req.bypass_cache = setup.bypass_cache;
    const auto slash = file_name.find_last_of("/\\");
    req.file_name = std::string(slash == std::string_view::npos ? file_name : file_name.substr(slash + 1));
    TRY_ASSIGN(out.compile, compiler.compile(req));
    if (!out.compile.ok) {
        out.error = out.compile.cancelled ? "the compile was cancelled" : out.compile.timed_out ? "the compiler timed out" : "compilation failed";
        return out;
    }
    auto obj = coff::Object::parse(out.compile.object_data);
    if (!obj) {
        out.error = "cannot read the compiled object: " + obj.error().message;
        return out;
    }
    for (u64 va : vas) {
        UnitCheck check;
        check.va = va;
        if (auto d = diff_function(program, va, *obj)) check.diff = std::move(*d);
        else check.error = d.error().message;
        out.functions.push_back(std::move(check));
    }
    return out;
}

} // namespace decomp::matching
