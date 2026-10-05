#include "project/types.hpp"

#include "analysis/demangle.hpp"
#include "core/fs.hpp"
#include "core/hash.hpp"
#include "core/strings.hpp"
#include "formats/coff.hpp"
#include "matching/source_items.hpp"
#include "matching/toolchain.hpp"
#include "matching/unit_source.hpp"
#include "project/units.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>

namespace decomp::project {

namespace {

bool identifier(std::string_view s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) != 0 || s[0] == '_')) return false;
    return std::ranges::all_of(s, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; });
}

// The file an #include line names: "types.h" for `#include "types.h"` and `#include <types.h>`.
std::optional<std::string> included_file(const matching::SourceItem& item) {
    if (item.kind != matching::ItemKind::preprocessor || item.name != "include") return std::nullopt;
    const std::string_view body = matching::item_body(item);
    const auto open = body.find_first_of("\"<");
    if (open == std::string_view::npos) return std::nullopt;
    const auto close = body.find(body[open] == '"' ? '"' : '>', open + 1);
    if (close == std::string_view::npos) return std::nullopt;
    std::string file(body.substr(open + 1, close - open - 1));
    std::ranges::replace(file, '\\', '/');
    return file;
}

bool includes_header(std::string_view source, std::string_view header) {
    return std::ranges::any_of(matching::parse_source_items(source), [&](const matching::SourceItem& item) {
        const auto file = included_file(item);
        return file && *file == header;
    });
}

std::string function_label(const Program& program, u64 va) {
    const Symbol* s = program.symbols().at(va);
    return s ? qualified_name(s->name) : std::format("{:#x}", va);
}

// A compile's diagnostics with their files: those under one of `dirs` by the name it is shown as and
// their path below it ("include/types.h"), others (the translation unit compiled) by their file name.
std::string diagnostics_with_files(const matching::CompileResult& compiled, const std::vector<std::pair<std::filesystem::path, std::string>>& dirs,
                                   usize max = 10) {
    if (compiled.diagnostics.empty()) return truncate_utf8(compiled.output, 2000);
    std::string out;
    usize shown = 0;
    for (const auto& d : compiled.diagnostics) {
        if (d.severity == "note" && shown >= max / 2) continue;
        if (++shown > max) {
            out += std::format("... {} more\n", compiled.diagnostics.size() - max);
            break;
        }
        std::string file = d.file;
        std::ranges::replace(file, '\\', '/');
        bool mapped = false;
        for (const auto& [dir, name] : dirs) {
            std::string prefix = fs::to_utf8(dir);
            std::ranges::replace(prefix, '\\', '/');
            if (!prefix.ends_with('/')) prefix += '/';
            if (file.starts_with(prefix)) {
                file = name + "/" + file.substr(prefix.size());
                mapped = true;
                break;
            }
        }
        if (!mapped)
            if (const auto slash = file.rfind('/'); slash != std::string::npos) file = file.substr(slash + 1);
        out += std::format("{}:{}{}: {}{}: {}\n", file.empty() ? "?" : file, d.line, d.column ? std::format(":{}", d.column) : "", d.severity,
                           d.code.empty() ? "" : " " + d.code, d.message);
    }
    return out;
}

std::string first_line(std::string_view text) {
    text = trim(text);
    const auto eol = text.find('\n');
    return std::string(eol == std::string_view::npos ? text : text.substr(0, eol));
}

} // namespace

bool valid_header_name(std::string_view header) {
    if (header.empty() || header.size() > 200) return false;
    for (usize start = 0; start <= header.size();) {
        const usize end = std::min(header.find('/', start), header.size());
        const std::string_view part = header.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (!std::ranges::all_of(part, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.'; }))
            return false;
        start = end + 1;
    }
    const auto dot = header.rfind('.');
    if (dot == std::string_view::npos || header.find('/', dot) != std::string_view::npos) return false;
    std::string ext(header.substr(dot));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx";
}

Result<ComposedType> compose_type(std::string_view header_text, std::string_view name, std::string_view declaration) {
    bool declares = false;
    for (const auto& item : matching::parse_source_items(declaration)) {
        switch (item.kind) {
        case matching::ItemKind::comment: break;
        case matching::ItemKind::preprocessor:
            if (item.name != "pragma")
                return make_error(ErrorCode::invalid_argument, "`{}`: only #pragma lines (such as #pragma pack) may come with a type definition",
                                  first_line(matching::item_body(item)));
            break;
        case matching::ItemKind::declaration: {
            const auto types = matching::declared_types(item);
            if (types.empty())
                return make_error(ErrorCode::invalid_argument,
                                  "`{}` declares no type: give a struct, class, union or enum definition, a typedef or a using alias",
                                  first_line(matching::item_body(item)));
            declares = declares || std::ranges::find(types, name) != types.end();
            break;
        }
        case matching::ItemKind::function:
        case matching::ItemKind::block:
            return make_error(ErrorCode::invalid_argument, "`{}`: a type definition holds types only, not functions or blocks",
                              first_line(matching::item_body(item)));
        }
    }
    if (!declares) return make_error(ErrorCode::invalid_argument, "the declaration does not declare {} at its top level", name);
    const std::string decl(trim(declaration));

    ComposedType out;
    if (trim(header_text).empty()) {
        out.text = "#pragma once\n\n" + decl + "\n";
        return out;
    }
    // The items' texts follow one another from the start of the header: the definition takes the place
    // of the first item that declares the type (after its leading comments), and later ones go.
    bool placed = false;
    usize consumed = 0;
    for (const auto& item : matching::parse_source_items(header_text)) {
        consumed += item.text.size();
        const auto types = matching::declared_types(item);
        if (std::ranges::find(types, name) == types.end()) {
            out.text += item.text;
            continue;
        }
        out.replaced = true;
        if (placed) continue;
        const std::string_view body = matching::item_body(item);
        out.text += item.text.substr(0, item.text.size() - body.size());
        out.text += decl;
        placed = true;
    }
    out.text += header_text.substr(std::min(consumed, header_text.size()));
    if (!placed) {
        while (!out.text.empty() && std::isspace(static_cast<unsigned char>(out.text.back())) != 0) out.text.pop_back();
        out.text += "\n\n" + decl + "\n";
    } else if (!out.text.ends_with('\n')) {
        out.text += '\n';
    }
    return out;
}

Result<TypeChange> prepare_type_change(const Project& project, const Program& program, const matching::MatchSetup& setup, std::string_view name,
                                       std::string_view declaration, std::string_view header_name) {
    const std::string header(header_name.empty() ? kDefaultTypesHeader : header_name);
    if (!identifier(name)) return make_error(ErrorCode::invalid_argument, "'{}' is not a type name (a C identifier)", name);
    if (!valid_header_name(header))
        return make_error(ErrorCode::invalid_argument, "'{}' is not a header name under include/ (such as types.h or game/player.h)", header);
    TypeChange change;
    change.name = std::string(name);
    change.header = "include/" + header;
    const auto path = project.root() / fs::from_utf8(change.header);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        TRY_ASSIGN(change.base, fs::read_text(path));
    }
    TRY_ASSIGN(auto composed, compose_type(change.base, name, declaration));
    if (composed.text == change.base) return make_error(ErrorCode::invalid_argument, "{} defines {} exactly so already", change.header, name);
    change.content = std::move(composed.text);
    change.replaced = composed.replaced;

    // The new header goes where the compiler finds it before the project's own.
    TRY_ASSIGN(auto staging, fs::TempDir::create("types", setup.work_dir));
    const auto staged = staging.path() / fs::from_utf8(header);
    TRY(fs::create_directories(staged.parent_path()));
    TRY(fs::write_text(staged, change.content));
    matching::MatchSetup with = setup;
    with.include_dirs.insert(with.include_dirs.begin(), staging.path());

    // It compiles on its own and names the type.
    matching::Compiler compiler(with.toolchain, with.work_dir, with.cache_dir);
    matching::CompileRequest probe;
    probe.source = std::format("#include \"{}\"\nusing decomp_probe_type_ = {};\n", header, name);
    probe.flags = with.flags;
    probe.include_dirs = with.include_dirs;
    probe.file_name = "decomp_type_probe.cpp";
    probe.cancelled = with.cancelled;
    TRY_ASSIGN(const auto compiled, compiler.compile(probe));
    if (!compiled.ok)
        return make_error(ErrorCode::invalid_argument, "{} does not compile with it:\n{}", change.header,
                          diagnostics_with_files(compiled, {{staging.path(), "include"}, {project.root() / "include", "include"}}));

    // The verified sources that include it keep their byte-exact functions.
    std::vector<std::string> broken;
    auto compare = [&](const std::string& where, const std::vector<std::pair<u64, bool>>& before, const std::vector<std::pair<u64, bool>>& after,
                       const std::string& failure) {
        if (!failure.empty()) {
            if (std::ranges::any_of(before, [](const auto& b) { return b.second; })) broken.push_back(std::format("{} no longer compiles: {}", where, failure));
            return;
        }
        for (const auto& [va, exact] : before)
            if (exact && std::ranges::find(after, std::pair{va, true}) == after.end())
                broken.push_back(std::format("{} in {}", function_label(program, va), where));
    };
    auto checks = [](const matching::UnitVerification& v) {
        std::vector<std::pair<u64, bool>> out;
        for (const auto& c : v.functions) out.emplace_back(c.va, c.byte_exact());
        return out;
    };
    TRY_ASSIGN(const auto units, load_units(project));
    for (const auto& unit : units) {
        if (unit.kind != UnitKind::code || unit.source.empty()) continue;
        auto text = fs::read_text(project.root() / fs::from_utf8(unit.source));
        if (!text || !includes_header(*text, header)) continue;
        change.sources.push_back(unit.source);
        std::vector<u64> vas;
        for (const auto& f : matching::UnitSource::parse(*text).functions) vas.push_back(f.va);
        TRY_ASSIGN(const auto before, matching::verify_unit(program, setup, *text, unit.source, vas));
        TRY_ASSIGN(const auto after, matching::verify_unit(program, with, *text, unit.source, vas));
        compare(unit.source, checks(before), checks(after), after.error);
    }
    // Matched functions' own files: src/functions/<name>_<address>.cpp.
    for (const auto& entry : std::filesystem::directory_iterator(project.root() / "src" / "functions", ec)) {
        if (entry.path().extension() != ".cpp") continue;
        const std::string stem = fs::to_utf8(entry.path().stem());
        const auto underscore = stem.rfind('_');
        const auto va = underscore == std::string::npos ? std::nullopt : parse_u64("0x" + stem.substr(underscore + 1));
        if (!va || !program.symbols().at(*va)) continue;
        auto text = fs::read_text(entry.path());
        if (!text || !includes_header(*text, header)) continue;
        const std::string where = "src/functions/" + fs::to_utf8(entry.path().filename());
        change.sources.push_back(where);
        TRY_ASSIGN(const auto before, matching::compile_and_diff(program, setup, *va, *text));
        TRY_ASSIGN(const auto after, matching::compile_and_diff(program, with, *va, *text));
        const bool exact_before = before.diff && before.diff->byte_exact, exact_after = after.diff && after.diff->byte_exact;
        compare(where, {{*va, exact_before}}, {{*va, exact_after}}, after.compile.ok ? std::string() : std::string("compilation failed"));
    }
    if (!broken.empty())
        return make_error(ErrorCode::invalid_argument, "with the new {}, verified functions are no longer byte-exact: {}", change.header,
                          join(broken, "; "));
    return change;
}

Result<WriteReceipt> commit_type_change(const Project& project, const TypeChange& change, const ChangeOrigin& origin, const ChangeSubject& subject) {
    return project.write_project_file(fs::from_utf8(change.header), change.content, origin, subject, change.base);
}

Result<std::vector<std::string>> project_headers(const Project& project) {
    std::vector<std::string> out;
    const auto include = project.root() / "include";
    std::error_code ec;
    if (!std::filesystem::is_directory(include, ec)) return out;
    for (auto it = std::filesystem::recursive_directory_iterator(include, ec); !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string relative = fs::to_utf8(it->path().lexically_relative(include));
        std::ranges::replace(relative, '\\', '/');
        if (valid_header_name(relative)) out.push_back("include/" + relative);
    }
    if (ec) return make_error(ErrorCode::io, "reading {}: {}", fs::to_utf8(include), ec.message());
    std::ranges::sort(out);
    return out;
}

namespace {

// The keyword a declaration item starts with: "struct", "class", "union" or "enum"; "" for others.
std::string leading_keyword(std::string_view body) {
    usize i = 0;
    while (i < body.size() && (std::isalnum(static_cast<unsigned char>(body[i])) != 0 || body[i] == '_')) ++i;
    const std::string_view word = body.substr(0, i);
    return word == "struct" || word == "class" || word == "union" || word == "enum" ? std::string(word) : std::string();
}

// The braces of a block item: its head (`namespace game`, `extern "C"`) and the text between them.
std::optional<std::pair<std::string_view, std::string_view>> block_parts(std::string_view body) {
    const auto open = body.find('{');
    const auto close = body.rfind('}');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) return std::nullopt;
    return std::pair{trim(body.substr(0, open)), body.substr(open + 1, close - open - 1)};
}

void collect_declared_types(std::string_view text, const std::string& scope, std::vector<DeclaredType>& out, int depth) {
    if (depth > 16) return;
    for (const auto& item : matching::parse_source_items(text)) {
        if (item.kind == matching::ItemKind::declaration) {
            const std::string keyword = leading_keyword(matching::item_body(item));
            for (const std::string& name : matching::declared_types(item)) out.push_back({scope + name, keyword});
        } else if (item.kind == matching::ItemKind::block) {
            const auto parts = block_parts(matching::item_body(item));
            if (!parts) continue;
            std::string_view head = parts->first;
            if (head.starts_with("inline")) head = trim(head.substr(6));
            if (head.starts_with("extern")) {
                collect_declared_types(parts->second, scope, out, depth + 1);
            } else if (head.starts_with("namespace")) {
                // `namespace game`, `namespace a::b`; not anonymous namespaces, whose types no other file names.
                const std::string name(trim(head.substr(9)));
                const std::string spaced = replace_all(name, "::", ":");
                if (!name.empty() && std::ranges::all_of(split(spaced, ':'), [](std::string_view part) { return identifier(trim(part)); }))
                    collect_declared_types(parts->second, scope + replace_all(spaced, ":", "::") + "::", out, depth + 1);
            }
        }
    }
}

// The flags that make a toolchain write its types into the object (.debug$T).
Result<std::vector<std::string>> debug_type_flags(const matching::Toolchain& toolchain) {
    switch (toolchain.kind) {
    case matching::ToolchainKind::msvc: return std::vector<std::string>{"/Z7"};
    case matching::ToolchainKind::clang_cl: return std::vector<std::string>{"/Z7", "/clang:-fstandalone-debug"};
    case matching::ToolchainKind::gcc:
    case matching::ToolchainKind::clang: break;
    }
    return make_error(ErrorCode::unsupported, "toolchain '{}' ({}) writes DWARF; reading types back needs CodeView from an MSVC-style compiler",
                      toolchain.name, matching::to_string(toolchain.kind));
}

} // namespace

std::vector<DeclaredType> header_declared_types(std::string_view header_text) {
    std::vector<DeclaredType> out;
    collect_declared_types(header_text, "", out, 0);
    return out;
}

const HeaderType* HeaderTypes::header_of(std::string_view name) const {
    const auto it = std::ranges::find(declared, name, &HeaderType::name);
    return it == declared.end() ? nullptr : &*it;
}

Result<HeaderTypes> compile_header_types(const Project& project, const matching::MatchSetup& setup, Arch arch) {
    HeaderTypes out;
    out.catalog = TypeCatalog(arch == Arch::x64 ? 8 : 4);
    TRY_ASSIGN(const auto headers, project_headers(project));
    if (headers.empty()) return out;
    TRY_ASSIGN(const auto debug_flags, debug_type_flags(setup.toolchain));

    // One translation unit: every header, then a pointer to each type each declares. The headers' hash
    // keys the compile cache on their contents.
    std::string includes, references, contents;
    usize n = 0;
    for (const std::string& header : headers) {
        TRY_ASSIGN(const auto text, fs::read_text(project.root() / fs::from_utf8(header)));
        contents += header + '\0' + text + '\0';
        includes += std::format("#include \"{}\"\n", header.substr(8));
        for (const DeclaredType& type : header_declared_types(text)) {
            if (out.header_of(type.name)) continue;
            out.declared.push_back({type.name, header});
            references += std::format("{}{}* decomp_type_{};\n", type.keyword.empty() ? "" : type.keyword + " ", type.name, n++);
        }
    }
    out.source = std::format("// decomp: the project's types (headers {})\n", sha1_hex(contents)) + includes + references;

    matching::CompileRequest request;
    request.source = out.source;
    request.flags = setup.flags;
    request.flags.insert(request.flags.end(), debug_flags.begin(), debug_flags.end());
    request.include_dirs = setup.include_dirs;
    const auto include = project.root() / "include";
    if (std::ranges::find(request.include_dirs, include) == request.include_dirs.end()) request.include_dirs.insert(request.include_dirs.begin(), include);
    request.file_name = "decomp_types.cpp";
    request.cancelled = setup.cancelled;
    request.bypass_cache = setup.bypass_cache;
    matching::Compiler compiler(setup.toolchain, setup.work_dir, setup.cache_dir);
    TRY_ASSIGN(auto compiled, compiler.compile(request));
    if (!compiled.ok)
        return make_error(ErrorCode::invalid_argument, "the project's headers do not compile:\n{}", diagnostics_with_files(compiled, {{include, "include"}}));
    TRY_ASSIGN(const auto object, coff::Object::parse(std::move(compiled.object_data)));
    const auto section = std::ranges::find(object.sections(), std::string(".debug$T"), &coff::Section::name);
    if (section == object.sections().end())
        return make_error(ErrorCode::unsupported, "{} wrote no type records (.debug$T) for the project's headers", setup.toolchain.name);
    auto stream = codeview::TypeStream::from_debug_t(section->data);
    if (!stream) return std::unexpected(std::move(stream.error()).with_context(std::format("the types {} wrote", setup.toolchain.name)));
    if (const auto first = stream->record(stream->first()); first && first->leaf == 0x1515)
        return make_error(ErrorCode::unsupported, "{} put the types in a PDB (LF_TYPESERVER2): its flags ask for /Zi, which /Z7 must replace",
                          setup.toolchain.name);
    out.catalog = TypeCatalog::from(*stream, arch == Arch::x64 ? 8 : 4);
    return out;
}

} // namespace decomp::project
