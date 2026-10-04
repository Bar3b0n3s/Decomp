#include "matching/toolchain.hpp"

#include "core/fs.hpp"
#include "core/hash.hpp"
#include "core/log.hpp"
#include "core/process.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <format>
#include <regex>

namespace decomp::matching {

std::string_view to_string(ToolchainKind kind) {
    switch (kind) {
    case ToolchainKind::msvc: return "msvc";
    case ToolchainKind::clang_cl: return "clang_cl";
    case ToolchainKind::gcc: return "gcc";
    case ToolchainKind::clang: return "clang";
    }
    return "msvc";
}

std::optional<ToolchainKind> toolchain_kind_from_string(std::string_view s) {
    for (auto k : {ToolchainKind::msvc, ToolchainKind::clang_cl, ToolchainKind::gcc, ToolchainKind::clang})
        if (to_string(k) == s) return k;
    return std::nullopt;
}

Json Toolchain::to_json() const {
    Json j;
    j["kind"] = std::string(to_string(kind));
    j["compiler"] = compiler;
    if (!wrapper.empty()) j["wrapper"] = wrapper;
    if (!flags.empty()) j["flags"] = flags;
    if (!include_dirs.empty()) j["include_dirs"] = include_dirs;
    if (!env.empty()) {
        Json e = Json::object();
        for (const auto& [k, v] : env) e[k] = v;
        j["env"] = e;
    }
    if (!env_prepend.empty()) {
        Json e = Json::object();
        for (const auto& [k, v] : env_prepend) e[k] = v;
        j["env_prepend"] = e;
    }
    if (!description.empty()) j["description"] = description;
    if (timeout_seconds != 120) j["timeout_seconds"] = timeout_seconds;
    return j;
}

Result<Toolchain> Toolchain::from_json(std::string name, const Json& j) {
    if (!j.is_object()) return make_error(ErrorCode::parse, "toolchain '{}' must be an object", name);
    Toolchain t;
    t.name = std::move(name);
    auto kind = toolchain_kind_from_string(json_string_or(j, "kind", "msvc"));
    if (!kind) return make_error(ErrorCode::parse, "toolchain '{}': unknown kind '{}' (msvc, clang_cl, gcc, clang)", t.name, json_string_or(j, "kind", ""));
    t.kind = *kind;
    t.compiler = json_string_or(j, "compiler", "");
    if (t.compiler.empty()) return make_error(ErrorCode::parse, "toolchain '{}': \"compiler\" is required", t.name);
    auto strings = [&](const char* key, std::vector<std::string>& out) -> Result<void> {
        auto it = j.find(key);
        if (it == j.end()) return {};
        if (!it->is_array()) return make_error(ErrorCode::parse, "toolchain '{}': \"{}\" must be an array of strings", t.name, key);
        for (const auto& v : *it) out.push_back(v.get<std::string>());
        return {};
    };
    auto pairs = [&](const char* key, std::vector<std::pair<std::string, std::string>>& out) -> Result<void> {
        auto it = j.find(key);
        if (it == j.end()) return {};
        if (!it->is_object()) return make_error(ErrorCode::parse, "toolchain '{}': \"{}\" must be an object", t.name, key);
        for (auto e = it->begin(); e != it->end(); ++e) {
            if (e->is_array()) {
                std::vector<std::string> parts;
                for (const auto& p : *e) parts.push_back(p.get<std::string>());
                out.emplace_back(e.key(), join(parts, std::string(1, path_list_separator())));
            } else {
                out.emplace_back(e.key(), e->get<std::string>());
            }
        }
        return {};
    };
    try {
        TRY(strings("wrapper", t.wrapper));
        TRY(strings("flags", t.flags));
        TRY(strings("include_dirs", t.include_dirs));
        TRY(pairs("env", t.env));
        TRY(pairs("env_prepend", t.env_prepend));
    } catch (const Json::exception& e) {
        return make_error(ErrorCode::parse, "toolchain '{}': {}", t.name, e.what());
    }
    t.description = json_string_or(j, "description", "");
    t.timeout_seconds = static_cast<int>(json_int_or(j, "timeout_seconds", 120));
    return t;
}

std::optional<std::string> find_clang_cl() {
    ProcessSpec probe;
#ifdef _WIN32
    const char* names[] = {"clang-cl.exe"};
    std::vector<std::string> extra = {"C:/Program Files/LLVM/bin/clang-cl.exe"};
#else
    const char* names[] = {"clang-cl"};
    std::vector<std::string> extra;
    for (int v = 30; v >= 14; --v) extra.push_back(std::format("/usr/lib/llvm-{}/bin/clang-cl", v));
    extra.push_back("/usr/local/opt/llvm/bin/clang-cl");
    extra.push_back("/opt/homebrew/opt/llvm/bin/clang-cl");
#endif
    if (auto path = get_env("PATH")) {
        for (auto dir : split(*path, path_list_separator())) {
            for (const char* n : names) {
                auto candidate = fs::from_utf8(dir) / n;
                std::error_code ec;
                if (!dir.empty() && std::filesystem::is_regular_file(candidate, ec)) return fs::to_utf8(candidate);
            }
        }
    }
    for (const auto& e : extra) {
        std::error_code ec;
        if (std::filesystem::exists(fs::from_utf8(e), ec)) return e;
    }
    return std::nullopt;
}

std::optional<std::string> find_llvm_tool(std::string_view name) {
#ifdef _WIN32
    std::string file = std::string(name) + ".exe";
#else
    std::string file(name);
#endif
    std::error_code ec;
    if (auto clang_cl = find_clang_cl()) {
        auto candidate = fs::from_utf8(*clang_cl).parent_path() / fs::from_utf8(file);
        if (std::filesystem::exists(candidate, ec)) return fs::to_utf8(candidate);
    }
    if (auto path = get_env("PATH")) {
        for (auto dir : split(*path, path_list_separator())) {
            if (dir.empty()) continue;
            auto candidate = fs::from_utf8(dir) / fs::from_utf8(file);
            if (std::filesystem::is_regular_file(candidate, ec)) return fs::to_utf8(candidate);
        }
    }
    return std::nullopt;
}

std::filesystem::path ToolchainRegistry::default_path() {
    if (auto overridden = get_env("DECOMP_TOOLCHAINS")) return fs::from_utf8(*overridden);
#ifdef _WIN32
    if (auto appdata = get_env("APPDATA")) return fs::from_utf8(*appdata) / "decomp" / "toolchains.json";
#else
    if (auto xdg = get_env("XDG_CONFIG_HOME"); xdg && !xdg->empty()) return fs::from_utf8(*xdg) / "decomp" / "toolchains.json";
    if (auto home = get_env("HOME")) return fs::from_utf8(*home) / ".config" / "decomp" / "toolchains.json";
#endif
    return "toolchains.json";
}

Result<ToolchainRegistry> ToolchainRegistry::load(const std::optional<std::filesystem::path>& path) {
    ToolchainRegistry reg;
    reg.path_ = path ? *path : default_path();
    std::error_code ec;
    if (std::filesystem::exists(reg.path_, ec)) {
        TRY_ASSIGN(auto text, fs::read_text(reg.path_));
        TRY_ASSIGN(auto j, parse_json(text));
        auto list = j.find("toolchains");
        if (list == j.end() || !list->is_object())
            return make_error(ErrorCode::parse, "{}: expected {{\"toolchains\": {{\"name\": {{...}}}}}}", fs::to_utf8(reg.path_));
        for (auto it = list->begin(); it != list->end(); ++it) {
            auto t = Toolchain::from_json(it.key(), *it);
            if (!t) return std::unexpected(std::move(t.error()).with_context(fs::to_utf8(reg.path_)));
            reg.toolchains_.push_back(std::move(*t));
        }
    }
    if (auto clang_cl = find_clang_cl()) {
        for (auto [name, target] : {std::pair{"clang-cl-x86", "i686-pc-windows-msvc"}, std::pair{"clang-cl-x64", "x86_64-pc-windows-msvc"}}) {
            if (reg.find(name)) continue;
            Toolchain t;
            t.name = name;
            t.kind = ToolchainKind::clang_cl;
            t.compiler = *clang_cl;
            t.flags = {std::format("--target={}", target), "/Zl", "/Brepro"};
            t.description = std::format("auto-detected clang-cl targeting {}", target);
            t.builtin = true;
            reg.toolchains_.push_back(std::move(t));
        }
    }
    return reg;
}

const Toolchain* ToolchainRegistry::find(std::string_view name) const {
    for (const auto& t : toolchains_)
        if (t.name == name) return &t;
    return nullptr;
}

void ToolchainRegistry::upsert(Toolchain toolchain) {
    for (auto& t : toolchains_)
        if (t.name == toolchain.name) {
            t = std::move(toolchain);
            return;
        }
    toolchains_.push_back(std::move(toolchain));
}

bool ToolchainRegistry::remove(std::string_view name) {
    return std::erase_if(toolchains_, [&](const Toolchain& t) { return t.name == name && !t.builtin; }) > 0;
}

Result<void> ToolchainRegistry::save() const {
    Json list = Json::object();
    for (const auto& t : toolchains_)
        if (!t.builtin) list[t.name] = t.to_json();
    return fs::write_text(path_, dump_pretty(Json{{"toolchains", list}}) + "\n");
}

std::vector<Diagnostic> parse_diagnostics(std::string_view output) {
    static const std::regex msvc(R"(^(.*?)\((\d+)(?:,(\d+))?\)\s*:\s*(fatal error|error|warning|note)\s*([A-Z]+\d+)?\s*:\s*(.*)$)");
    static const std::regex gnu(R"(^(.*?):(\d+):(?:(\d+):)?\s*(fatal error|error|warning|note):\s*(.*)$)");
    std::vector<Diagnostic> out;
    for (const auto& raw : split_lines(output)) {
        std::string line(trim(raw));
        std::smatch m;
        Diagnostic d;
        if (std::regex_match(line, m, msvc)) {
            d.file = m[1];
            d.line = std::stoi(m[2]);
            d.column = m[3].matched ? std::stoi(m[3]) : 0;
            d.severity = m[4];
            d.code = m[5].matched ? std::string(m[5]) : std::string();
            d.message = m[6];
        } else if (std::regex_match(line, m, gnu)) {
            d.file = m[1];
            d.line = std::stoi(m[2]);
            d.column = m[3].matched ? std::stoi(m[3]) : 0;
            d.severity = m[4];
            d.message = m[5];
        } else {
            continue;
        }
        out.push_back(std::move(d));
    }
    return out;
}

std::string format_diagnostics(const std::vector<Diagnostic>& diagnostics, usize max) {
    std::string out;
    usize shown = 0;
    for (const auto& d : diagnostics) {
        if (d.severity == "note" && shown >= max / 2) continue;
        if (++shown > max) {
            out += std::format("... {} more\n", diagnostics.size() - max);
            break;
        }
        out += std::format("line {}{}: {}{}: {}\n", d.line, d.column ? std::format(":{}", d.column) : "", d.severity,
                           d.code.empty() ? "" : " " + d.code, d.message);
    }
    return out;
}

Compiler::Compiler(Toolchain toolchain, std::filesystem::path work_dir, std::optional<std::filesystem::path> cache_dir)
    : toolchain_(std::move(toolchain)), work_dir_(std::move(work_dir)), cache_dir_(std::move(cache_dir)) {}

std::vector<std::string> Compiler::command_line(const CompileRequest& request, const std::filesystem::path& source,
                                                const std::filesystem::path& object) const {
    std::vector<std::string> cmd = toolchain_.wrapper;
    cmd.push_back(toolchain_.compiler);
    auto add_includes = [&](const std::string& prefix) {
        for (const auto& d : toolchain_.include_dirs) cmd.push_back(prefix + d);
        for (const auto& d : request.include_dirs) cmd.push_back(prefix + fs::to_utf8(d));
    };
    if (toolchain_.msvc_style()) {
        cmd.push_back("/nologo");
        cmd.push_back("/c");
        cmd.insert(cmd.end(), toolchain_.flags.begin(), toolchain_.flags.end());
        cmd.insert(cmd.end(), request.flags.begin(), request.flags.end());
        if (needs_function_sections(request)) cmd.push_back("/Gy");
        add_includes("/I");
        cmd.push_back("/Fo" + fs::to_utf8(object));
        cmd.push_back(fs::to_utf8(source));
    } else {
        cmd.push_back("-c");
        cmd.insert(cmd.end(), toolchain_.flags.begin(), toolchain_.flags.end());
        cmd.insert(cmd.end(), request.flags.begin(), request.flags.end());
        add_includes("-I");
        cmd.push_back("-o");
        cmd.push_back(fs::to_utf8(object));
        cmd.push_back(fs::to_utf8(source));
    }
    return cmd;
}

// Candidates are always compiled with /Gy: every function gets its own COMDAT section, so it can be
// cut out of the object exactly (no padding, calls to neighbours carry relocations). /Gy changes
// packaging only, not the code generated for a function, so targets built without it still match.
bool Compiler::needs_function_sections(const CompileRequest& request) const {
    if (!toolchain_.msvc_style()) return false;
    std::optional<bool> gy;
    auto scan = [&](const std::vector<std::string>& flags) {
        for (const auto& f : flags) {
            const std::string lower = to_lower(f);
            if (lower == "/gy" || lower == "-gy") gy = true;
            else if (lower == "/gy-" || lower == "-gy-") gy = false;
        }
    };
    scan(toolchain_.flags);
    scan(request.flags);
    return gy != true;
}

std::string Compiler::cache_key(const CompileRequest& request) const {
    Sha1 h;
    h.update(dump_compact(toolchain_.to_json()));
    h.update(join(request.flags, "\x1f"));
    if (needs_function_sections(request)) h.update("\x1f/Gy");
    h.update(request.source);
    // Headers can change between compiles: fold in every include directory's file sizes and timestamps.
    for (const auto& dir : request.include_dirs) {
        std::error_code ec;
        h.update(fs::to_utf8(dir));
        if (!std::filesystem::is_directory(dir, ec)) continue;
        std::vector<std::string> entries;
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            auto size = it->file_size(ec);
            auto time = it->last_write_time(ec).time_since_epoch().count();
            entries.push_back(std::format("{}|{}|{}", fs::to_utf8(it->path()), size, time));
        }
        std::ranges::sort(entries);
        for (const auto& e : entries) h.update(e);
    }
    return to_hex(h.finish());
}

namespace {

class CompileSlots {
public:
    bool acquire(const std::function<bool()>& cancelled) {
        std::unique_lock lock(mutex_);
        while (used_ >= limit_) {
            if (cancelled && cancelled()) return false;
            cv_.wait_for(lock, std::chrono::milliseconds(100));
        }
        ++used_;
        return true;
    }
    void release() {
        {
            std::lock_guard lock(mutex_);
            --used_;
        }
        cv_.notify_one();
    }
    void set_limit(int limit) {
        {
            std::lock_guard lock(mutex_);
            limit_ = std::max(1, limit);
        }
        cv_.notify_all();
    }
    int limit() {
        std::lock_guard lock(mutex_);
        return limit_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int limit_ = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    int used_ = 0;
};

CompileSlots& compile_slots() {
    static CompileSlots slots;
    return slots;
}

} // namespace

void set_max_parallel_compiles(int count) { compile_slots().set_limit(count); }
int max_parallel_compiles() { return compile_slots().limit(); }

Result<CompileResult> Compiler::compile(const CompileRequest& request) const {
    CompileResult result;
    std::string key = cache_key(request);
    std::optional<std::filesystem::path> cached_obj, cached_meta;
    if (cache_dir_) {
        cached_obj = *cache_dir_ / (key + ".obj");
        cached_meta = *cache_dir_ / (key + ".json");
        std::error_code ec;
        if (!request.bypass_cache && std::filesystem::exists(*cached_meta, ec)) {
            auto meta = fs::read_text(*cached_meta).and_then([](const std::string& t) { return parse_json(t); });
            if (meta) {
                result.ok = meta->value("ok", false);
                result.output = meta->value("output", "");
                result.diagnostics = parse_diagnostics(result.output);
                result.cached = true;
                if (!result.ok) return result;
                if (auto data = fs::read_file(*cached_obj)) {
                    result.object = *cached_obj;
                    result.object_data = std::move(*data);
                    return result;
                }
            }
        }
    }

    // Unique across threads and processes sharing the work directory.
    static std::atomic<u64> counter{0};
    auto dir = work_dir_ / std::format("c{}-{}-{}", key.substr(0, 12), current_process_id(), counter.fetch_add(1));
    TRY(fs::create_directories(dir));
    auto source = dir / fs::from_utf8(request.file_name);
    auto object = dir / (toolchain_.msvc_style() ? "candidate.obj" : "candidate.o");
    TRY(fs::write_text(source, request.source));

    ProcessSpec spec;
    spec.argv = command_line(request, source, object);
    spec.cwd = dir;
    spec.timeout = std::chrono::seconds(toolchain_.timeout_seconds);
    for (const auto& [k, v] : toolchain_.env) spec.env.emplace_back(k, v);
    for (const auto& [k, v] : toolchain_.env_prepend) {
        auto current = get_env(k);
        spec.env.emplace_back(k, current && !current->empty() ? v + path_list_separator() + *current : v);
    }
    if (toolchain_.kind == ToolchainKind::msvc)  // keep concurrent /Zi compiles from sharing one mspdbsrv
        spec.env.emplace_back("_MSPDBSRV_ENDPOINT_", "decomp-" + key.substr(0, 16));
    // cl.exe and clang-cl read extra options from CL and _CL_; only the configured flags may apply.
    if (toolchain_.msvc_style())
        for (const char* var : {"CL", "_CL_"})
            if (std::ranges::none_of(toolchain_.env, [&](const auto& kv) { return kv.first == var; }))
                spec.env.emplace_back(var, std::nullopt);
    result.command = spec.argv;

    // Long command lines go through a response file (old cl.exe has small limits).
    if (toolchain_.msvc_style() && build_windows_command_line(spec.argv).size() > 4000) {
        usize first_arg = toolchain_.wrapper.size() + 1;
        std::vector<std::string> args(spec.argv.begin() + static_cast<std::ptrdiff_t>(first_arg), spec.argv.end());
        auto rsp = dir / "args.rsp";
        TRY(fs::write_text(rsp, build_windows_command_line(args)));
        spec.argv.resize(first_arg);
        spec.argv.push_back("@" + fs::to_utf8(rsp));
    }

    spec.cancelled = request.cancelled;
    if (!compile_slots().acquire(request.cancelled)) {
        result.cancelled = true;
        result.output = "[compile cancelled]";
        return result;
    }
    auto proc_result = run_process(spec);
    compile_slots().release();
    TRY_ASSIGN(auto proc, std::move(proc_result));
    result.duration = proc.duration;
    result.timed_out = proc.timed_out;
    result.cancelled = proc.cancelled;
    result.exit_code = proc.exit_code;
    result.output = proc.out + proc.err;
    // MSVC echoes the source file name on success; drop that noise.
    if (toolchain_.msvc_style()) {
        auto lines = split_lines(result.output);
        std::erase_if(lines, [&](const std::string& l) { return trim(l) == request.file_name; });
        result.output = join(lines, "\n");
    }
    result.diagnostics = parse_diagnostics(result.output);
    std::error_code ec;
    result.ok = proc.ok() && std::filesystem::exists(object, ec);
    if (result.ok) {
        TRY_ASSIGN(result.object_data, fs::read_file(object));
    }
    if (proc.timed_out) result.output += std::format("\n[compiler timed out after {}s]", toolchain_.timeout_seconds);
    if (proc.cancelled) result.output += "\n[compile cancelled]";

    if (cache_dir_ && !proc.cancelled) {
        (void)fs::create_directories(*cache_dir_);
        if (result.ok && fs::write_file(*cached_obj, result.object_data)) result.object = *cached_obj;
        if (!proc.timed_out) (void)fs::write_text(*cached_meta, dump_compact(Json{{"ok", result.ok}, {"output", result.output}}));
    }
    if (result.ok) std::filesystem::remove_all(dir, ec);  // keep failed compiles for inspection
    return result;
}

} // namespace decomp::matching
