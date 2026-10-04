#include "project/project.hpp"

#include "analysis/demangle.hpp"
#include "core/fs.hpp"
#include "core/hash.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"

#include <chrono>
#include <format>

namespace decomp::project {

std::string_view to_string(FunctionStatus status) {
    switch (status) {
    case FunctionStatus::unstarted: return "unstarted";
    case FunctionStatus::in_progress: return "in_progress";
    case FunctionStatus::nonmatching: return "nonmatching";
    case FunctionStatus::matched: return "matched";
    case FunctionStatus::refused: return "refused";
    case FunctionStatus::gave_up: return "gave_up";
    case FunctionStatus::skipped: return "skipped";
    case FunctionStatus::library: return "library";
    }
    return "unstarted";
}

std::optional<FunctionStatus> status_from_string(std::string_view s) {
    for (auto st : {FunctionStatus::unstarted, FunctionStatus::in_progress, FunctionStatus::nonmatching,
                    FunctionStatus::matched, FunctionStatus::refused, FunctionStatus::gave_up, FunctionStatus::skipped,
                    FunctionStatus::library})
        if (to_string(st) == s) return st;
    return std::nullopt;
}

Json Config::to_json() const {
    Json j;
    j["version"] = version;
    j["target"] = {{"path", target}, {"sha1", target_sha1}};
    if (!pdb.empty()) j["target"]["pdb"] = pdb;
    j["toolchain"] = toolchain;
    j["flags"] = flags;
    j["include_dirs"] = include_dirs;
    j["agent"] = {{"model", agent.model},
                  {"effort", agent.effort},
                  {"max_turns", agent.max_turns},
                  {"max_usd_per_function", agent.max_usd_per_function},
                  {"max_tokens_per_function", agent.max_tokens_per_function},
                  {"max_minutes_per_function", agent.max_minutes_per_function},
                  {"fallbacks", agent.fallbacks}};
    return j;
}

Result<Config> Config::from_json(const Json& j) {
    if (!j.is_object()) return make_error(ErrorCode::parse, "decomp.json must be an object");
    Config c;
    c.version = static_cast<int>(json_int_or(j, "version", 1));
    if (c.version != 1) return make_error(ErrorCode::unsupported, "unsupported decomp.json version {}", c.version);
    const Json target = j.value("target", Json::object());
    c.target = json_string_or(target, "path", "");
    c.target_sha1 = json_string_or(target, "sha1", "");
    c.pdb = json_string_or(target, "pdb", "");
    if (c.target.empty()) return make_error(ErrorCode::parse, "decomp.json: target.path is required");
    c.toolchain = json_string_or(j, "toolchain", "");
    if (auto it = j.find("flags"); it != j.end() && it->is_array())
        for (const auto& f : *it) c.flags.push_back(f.get<std::string>());
    if (auto it = j.find("include_dirs"); it != j.end() && it->is_array())
        for (const auto& f : *it) c.include_dirs.push_back(f.get<std::string>());
    const Json a = j.value("agent", Json::object());
    c.agent.model = json_string_or(a, "model", c.agent.model);
    c.agent.effort = json_string_or(a, "effort", c.agent.effort);
    c.agent.max_turns = static_cast<int>(json_int_or(a, "max_turns", c.agent.max_turns));
    c.agent.max_usd_per_function = json_number_or(a, "max_usd_per_function", c.agent.max_usd_per_function);
    c.agent.max_tokens_per_function = json_int_or(a, "max_tokens_per_function", c.agent.max_tokens_per_function);
    c.agent.max_minutes_per_function = static_cast<int>(json_int_or(a, "max_minutes_per_function", c.agent.max_minutes_per_function));
    c.agent.fallbacks = json_bool_or(a, "fallbacks", c.agent.fallbacks);
    return c;
}

std::string safe_function_name(const Symbol& fn) {
    std::string base = fn.pdb_name.empty() ? qualified_name(fn.name) : fn.pdb_name;
    std::string out;
    for (char c : base) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') out += c;
        else if (c == ':') out += '_';
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    if (out.size() > 80) out.resize(80);
    if (out.empty()) out = "fn";
    return std::format("{}_{:x}", out, fn.va);
}

namespace {

std::string quote_if_needed(const std::string& v) {
    if (v.find_first_of(" \t\"=") == std::string::npos && !v.empty()) return v;
    return escape_c_string(v);
}

// Splits a line into whitespace-separated tokens; double-quoted tokens may contain spaces and C escapes.
std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> out;
    usize i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        std::string tok;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            if (line[i] == '"') {
                ++i;
                while (i < line.size() && line[i] != '"') {
                    if (line[i] == '\\' && i + 1 < line.size()) {
                        char n = line[++i];
                        tok += n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n;
                    } else {
                        tok += line[i];
                    }
                    ++i;
                }
                ++i;  // closing quote
            } else {
                tok += line[i++];
            }
        }
        out.push_back(std::move(tok));
    }
    return out;
}

} // namespace

std::string format_symbol_line(const Symbol& s, const FunctionInfo* info) {
    std::string line = std::format("{:#010x} {} {}", s.va, to_string(s.kind), quote_if_needed(s.name));
    if (s.size) line += std::format(" size={:#x}", s.size);
    if (!s.pdb_name.empty() && s.pdb_name != s.name) line += " pdb=" + quote_if_needed(s.pdb_name);
    if (s.is_static) line += " static";
    line += std::format(" source={}", to_string(s.source));
    if (info && s.kind == SymbolKind::function) {
        if (info->status != FunctionStatus::unstarted) line += std::format(" status={}", to_string(info->status));
        if (info->best_match > 0) line += std::format(" best={:.1f}", info->best_match);
        if (info->attempts > 0) line += std::format(" attempts={}", info->attempts);
        if (info->cost_usd > 0) line += std::format(" cost={:.4f}", info->cost_usd);
    }
    return line;
}

Result<std::pair<Symbol, std::optional<FunctionInfo>>> parse_symbol_line(std::string_view line) {
    auto tokens = tokenize(line);
    if (tokens.size() < 3) return make_error(ErrorCode::parse, "expected '<address> <kind> <name> [key=value...]'");
    Symbol s;
    auto va = parse_u64(tokens[0]);
    if (!va) return make_error(ErrorCode::parse, "bad address '{}'", tokens[0]);
    s.va = *va;
    auto kind = symbol_kind_from_string(tokens[1]);
    if (!kind) return make_error(ErrorCode::parse, "unknown kind '{}'", tokens[1]);
    s.kind = *kind;
    s.name = tokens[2];
    s.source = SymbolSource::user;
    std::optional<FunctionInfo> info;
    auto fi = [&]() -> FunctionInfo& {
        if (!info) info.emplace();
        return *info;
    };
    for (usize i = 3; i < tokens.size(); ++i) {
        const auto& t = tokens[i];
        if (t == "static") {
            s.is_static = true;
            continue;
        }
        auto eq = t.find('=');
        if (eq == std::string::npos) return make_error(ErrorCode::parse, "unexpected token '{}'", t);
        std::string key = t.substr(0, eq), value = t.substr(eq + 1);
        if (key == "size") {
            auto v = parse_u64(value);
            if (!v) return make_error(ErrorCode::parse, "bad size '{}'", value);
            s.size = static_cast<u32>(*v);
        } else if (key == "pdb") {
            s.pdb_name = value;
        } else if (key == "source") {
            auto src = symbol_source_from_string(value);
            if (!src) return make_error(ErrorCode::parse, "unknown source '{}'", value);
            s.source = *src;
        } else if (key == "status") {
            auto st = status_from_string(value);
            if (!st) return make_error(ErrorCode::parse, "unknown status '{}'", value);
            fi().status = *st;
        } else if (key == "best") {
            fi().best_match = std::stod(value);
        } else if (key == "attempts") {
            fi().attempts = std::stoi(value);
        } else if (key == "cost") {
            fi().cost_usd = std::stod(value);
        } else {
            return make_error(ErrorCode::parse, "unknown key '{}'", key);
        }
    }
    return std::pair{std::move(s), std::move(info)};
}

Result<Project> Project::find(const std::string& start) {
    std::filesystem::path from = start.empty() ? std::filesystem::current_path() : fs::from_utf8(start);
    auto dir = fs::find_upwards(from, kConfigFile);
    if (!dir) return make_error(ErrorCode::not_found, "no {} found in '{}' or its parents (run `decomp init <binary>` first, or pass the binary explicitly)", kConfigFile, fs::to_utf8(from));
    return load(*dir);
}

Result<Project> Project::load(const std::filesystem::path& root) {
    Project p;
    p.root_ = root;
    TRY_ASSIGN(auto text, fs::read_text(root / kConfigFile));
    TRY_ASSIGN(auto json, parse_json(text));
    auto config = Config::from_json(json);
    if (!config) return std::unexpected(std::move(config.error()).with_context(fs::to_utf8(root / kConfigFile)));
    p.config_ = std::move(*config);
    TRY(p.load_symbols_file());
    return p;
}

Result<void> Project::load_symbols_file() {
    auto path = root_ / kSymbolsFile;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    TRY_ASSIGN(auto text, fs::read_text(path));
    usize line_no = 0;
    for (const auto& raw : split_lines(text)) {
        ++line_no;
        auto line = trim(raw);
        if (line.empty() || line.starts_with('#')) continue;
        auto parsed = parse_symbol_line(line);
        if (!parsed) return make_error(ErrorCode::parse, "{}:{}: {}", fs::to_utf8(path), line_no, parsed.error().message);
        if (parsed->second) functions_[parsed->first.va] = *parsed->second;
        symbol_overrides_.push_back(std::move(parsed->first));
    }
    return {};
}

Result<void> Project::save_config() const { return fs::write_text(root_ / kConfigFile, dump_pretty(config_.to_json()) + "\n"); }

std::vector<std::filesystem::path> Project::include_paths() const {
    std::vector<std::filesystem::path> out;
    for (const auto& d : config_.include_dirs) out.push_back(root_ / fs::from_utf8(d));
    return out;
}

Result<Program> Project::open_program() const {
    std::optional<std::filesystem::path> pdb;
    if (!config_.pdb.empty()) pdb = root_ / fs::from_utf8(config_.pdb);
    TRY_ASSIGN(auto program, Program::open(target_path(), pdb));
    if (!config_.target_sha1.empty()) {
        auto actual = sha1_hex(program.image().data());
        if (actual != config_.target_sha1)
            log::warn("target '{}' SHA-1 is {} but decomp.json expects {}", config_.target, actual, config_.target_sha1);
    }
    for (const auto& s : symbol_overrides_) program.symbols().add(s);
    return program;
}

Result<void> Project::save_symbols(const SymbolDb& symbols) const {
    std::string out = "# decomp symbols: <address> <kind> <name> [size=] [pdb=] [static] [source=] [status=] [best=] [attempts=] [cost=]\n";
    for (const auto& [va, s] : symbols) {
        auto it = functions_.find(va);
        out += format_symbol_line(s, it == functions_.end() ? nullptr : &it->second) + "\n";
    }
    return fs::write_text(root_ / kSymbolsFile, out);
}

FunctionInfo Project::function_info(u64 va) const {
    auto it = functions_.find(va);
    return it == functions_.end() ? FunctionInfo{} : it->second;
}

Result<void> Project::update_function(u64 va, const FunctionInfo& info) {
    functions_[va] = info;
    // Persist by rewriting symbols.txt from the current program state.
    TRY_ASSIGN(auto program, open_program());
    return save_symbols(program.symbols());
}

std::filesystem::path Project::function_dir(const Symbol& fn) const {
    return root_ / ".decomp" / "functions" / fs::from_utf8(safe_function_name(fn));
}

std::filesystem::path Project::matched_source_path(const Symbol& fn) const {
    return root_ / "src" / "functions" / fs::from_utf8(safe_function_name(fn) + ".cpp");
}

Result<void> Project::record_attempt(const Symbol& fn, const Json& attempt) const {
    return fs::append_text(function_dir(fn) / "attempts.jsonl", dump_compact(attempt) + "\n");
}

std::vector<Json> Project::attempts(const Symbol& fn) const {
    std::vector<Json> out;
    auto text = fs::read_text(function_dir(fn) / "attempts.jsonl");
    if (!text) return out;
    for (const auto& line : split_lines(*text))
        if (auto j = parse_json(line)) out.push_back(std::move(*j));
    return out;
}

std::optional<std::string> Project::best_source(const Symbol& fn) const {
    auto text = fs::read_text(function_dir(fn) / "best.cpp");
    if (!text) return std::nullopt;
    return *text;
}

Result<void> Project::save_best_source(const Symbol& fn, const std::string& source) const {
    return fs::write_text(function_dir(fn) / "best.cpp", source);
}

std::string Project::notes(const Symbol& fn) const { return fs::read_text(function_dir(fn) / "notes.md").value_or(""); }

Result<void> Project::append_note(const Symbol& fn, const std::string& note) const {
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return fs::append_text(function_dir(fn) / "notes.md", std::format("- {:%Y-%m-%d %H:%M}: {}\n", now, note));
}

Result<void> Project::write_matched_source(const Symbol& fn, const std::string& source) const {
    return fs::write_text(matched_source_path(fn), source);
}

Result<Project> Project::init(const std::filesystem::path& root, const std::filesystem::path& binary,
                              const std::optional<std::filesystem::path>& pdb, const std::string& toolchain) {
    std::error_code ec;
    if (std::filesystem::exists(root / kConfigFile, ec))
        return make_error(ErrorCode::invalid_argument, "'{}' already contains a {}", fs::to_utf8(root), kConfigFile);
    TRY(fs::create_directories(root));
    TRY_ASSIGN(auto program, Program::open(binary, pdb));

    Project p;
    p.root_ = root;
    auto abs_root = std::filesystem::absolute(root, ec);
    auto rel = std::filesystem::relative(std::filesystem::absolute(binary, ec), abs_root, ec);
    p.config_.target = fs::to_utf8(ec || rel.empty() ? std::filesystem::absolute(binary) : rel);
    p.config_.target_sha1 = sha1_hex(program.image().data());
    if (program.pdb_path()) {
        auto prel = std::filesystem::relative(std::filesystem::absolute(*program.pdb_path(), ec), abs_root, ec);
        p.config_.pdb = fs::to_utf8(ec || prel.empty() ? *program.pdb_path() : prel);
    }
    p.config_.toolchain = toolchain;
    p.config_.include_dirs = {"include"};
    TRY(fs::create_directories(root / "include"));
    TRY(fs::create_directories(root / "src" / "functions"));
    TRY(p.save_config());
    TRY(p.save_symbols(program.symbols()));
    TRY(fs::write_text(root / ".gitignore", "/.decomp/\n"));
    return Project::load(root);
}

} // namespace decomp::project
