#include "agent/match_session.hpp"

#include "analysis/annotate.hpp"
#include "analysis/demangle.hpp"
#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <format>
#include <regex>

namespace decomp::agent {

namespace {

constexpr usize kMaxListingLines = 400;
constexpr usize kMaxDiffRows = 80;

Json string_prop(const char* description) { return {{"type", "string"}, {"description", description}}; }

Json object_schema(Json properties) {
    Json required = Json::array();
    for (auto it = properties.begin(); it != properties.end(); ++it) required.push_back(it.key());
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}};
}

std::string clip_lines(const std::string& text, usize max_lines) {
    auto lines = split_lines(text);
    if (lines.size() <= max_lines) return text;
    lines.resize(max_lines);
    return join(lines, "\n") + std::format("\n... ({} more lines omitted)\n", split_lines(text).size() - max_lines);
}

} // namespace

const std::string& system_prompt() {
    static const std::string prompt = R"(You are an expert reverse engineer working on a matching decompilation project: rewriting functions of a compiled x86/x64 program as C/C++ source that, compiled with the program's original compiler and flags, produces byte-for-byte identical machine code. This is preservation and interoperability work of the kind done by community decompilation projects; the user is entitled to study this binary.

You work on exactly one target function per conversation. Tools:
- compile_and_diff: compile a complete candidate translation unit and compare the target function with the result. This is your main loop.
- disassemble: annotated disassembly of another function (callees, callers) to learn signatures, structure layouts and conventions.
- read_memory: read data from the target image (strings, tables, constants, initial values of globals).
- lookup_symbol: find symbols by name or address.
- record_note: save a short note for future attempts on this function (what you learned, what did not work).
- submit_result: finish with outcome "matched" and the exact source that matched (it is re-verified; only byte-exact results are accepted), or "give_up" with your best source and the reason.

How to work:
1. Read the brief: annotated disassembly, referenced symbols with their declarations, callers, and notes from earlier attempts.
2. Work out the signature, calling convention and the types the function touches. Use disassemble on callers or callees when the brief is not enough.
3. Write a first complete candidate and call compile_and_diff early. Fix the largest structural differences first (control flow, missing or extra code), then operand-level differences.
4. The candidate must be a complete, self-contained translation unit: declare every external function, global variable and type it uses (extern declarations, struct definitions with the right layout). Declarations must produce the same decorated names shown in the brief: `?add@@YAHHH@Z` is `int __cdecl add(int, int)`; `extern "C"` names like `_entry` need `extern "C"`; arrays mangle like pointers (`?g_table@@3PAHA` can be `int g_table[8]`). No inline assembly. Only include headers the brief lists as available. Define the target function (and static helpers only if the target inlines them).

Matching tips (MSVC and clang-cl):
- Register allocation and instruction order follow declaration order, expression order, temporaries and variable scope. Try equivalent rewrites: operand order (a + b vs b + a), x * 2 vs x << 1, splitting or merging statements, for/while/do-while, early return vs single exit, the order of if/else bodies, and where variables are declared.
- Stack offset differences mean local variable order, size or type differ.
- Signedness and width change instructions: movsx/movzx, jl/jb, idiv/div, sar/shr, imul/mul.
- An inverted branch condition usually means swapped if/else bodies or a negated condition.
- switch statements become jump tables; case order and the default case matter.
- Calls must reach the same functions with the same calling conventions (__cdecl, __stdcall, __fastcall, __thiscall for member functions).
- String literals and floating-point constants are compared by value, so their content must be exact.
- When the diff says only registers or stack offsets differ, you are close: try small reorderings rather than rewriting.
- Listing notation: `imagerel X` (MSVC x64 `[r8+rcx*4+imagerel X]` after `lea r8, __ImageBase`) is an ordinary indexed access to X, such as X[i]. A call annotated "via thunk" goes to the named function; the linker made the thunk, so it is not part of your source.

Keep text between tool calls short; put reasoning into code and notes. When the result is byte-exact, call submit_result with outcome "matched" and that exact source. If you are stuck after many attempts with no improvement, record a note with what you learned and call submit_result with outcome "give_up", your best source, and the reason.)";
    return prompt;
}

MatchSession::MatchSession(const Program& program, const project::Project* project, matching::MatchSetup setup, u64 va,
                           events::EventBus* bus, std::string session_id, int worker)
    : program_(program), project_(project), setup_(std::move(setup)), va_(va), bus_(bus), session_id_(std::move(session_id)),
      worker_(worker) {
    if (const Symbol* s = program.symbols().at(va)) symbol_ = *s;
    else {
        symbol_.va = va;
        symbol_.name = std::format("sub_{:x}", va);
        symbol_.kind = SymbolKind::function;
    }
    if (project_) {
        if (auto best = project_->best_source(symbol_)) best_source_ = *best;
        best_match_ = project_->function_info(va).best_match;
    }
}

void MatchSession::publish(events::Payload payload) const {
    if (bus_) bus_->publish(std::move(payload), worker_);
}

Json MatchSession::tool_schemas() {
    Json t = Json::object();
    t["compile_and_diff"] = object_schema({{"source", string_prop("Complete C/C++ translation unit defining the target function")}});
    t["disassemble"] = object_schema({{"target", string_prop("Function name or address (e.g. \"add\", \"?add@@YAHHH@Z\", \"0x401060\")")}});
    t["read_memory"] = object_schema({
        {"address", string_prop("Address or symbol name, optionally +offset (e.g. \"0x402010\", \"g_table+0x8\")")},
        {"count", {{"type", "integer"}, {"description", "Number of elements to read (bytes for \"bytes\"; ignored for \"string\")"}}},
        {"format", {{"type", "string"},
                    {"enum", {"bytes", "string", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64", "pointer"}},
                    {"description", "How to interpret the memory"}}},
    });
    t["lookup_symbol"] = object_schema({{"query", string_prop("Name, part of a name, or address")}});
    t["record_note"] = object_schema({{"text", string_prop("Short note for future attempts on this function")}});
    t["submit_result"] = object_schema({
        {"outcome", {{"type", "string"}, {"enum", {"matched", "give_up"}}, {"description", "matched (re-verified) or give_up"}}},
        {"source", string_prop("The matching source, or your best attempt when giving up")},
        {"reason", string_prop("Why you are giving up (empty when matched)")},
    });
    return t;
}

std::string MatchSession::tool_description(std::string_view name) {
    if (name == "compile_and_diff")
        return "Compile a complete candidate translation unit with the target's original toolchain and flags, and diff the "
               "target function against the result. Returns compiler errors, or the match percentage, the differing "
               "instructions side by side (target | candidate) and hints. Call this for every candidate you want checked.";
    if (name == "disassemble")
        return "Annotated disassembly of any function in the target program (callers, callees, helpers). Use it to learn "
               "signatures, calling conventions and structure layouts.";
    if (name == "read_memory")
        return "Read data from the target image: string contents, tables, constants, initial values of globals. Use it "
               "when the disassembly references data you need to reproduce exactly.";
    if (name == "lookup_symbol")
        return "Find symbols by exact name, partial name or address; returns addresses, kinds, sizes and readable "
               "signatures. Use it to get the exact declaration of something the function references.";
    if (name == "record_note")
        return "Save a short note for future attempts on this function (what you learned, what did not work). Notes "
               "persist across sessions.";
    if (name == "submit_result")
        return "Finish this function. Use outcome \"matched\" with the exact source once compile_and_diff reported a "
               "byte-exact match (it is re-verified), or \"give_up\" with your best source and the reason when stuck.";
    return {};
}

ToolOutput MatchSession::call(std::string_view tool, const Json& input) {
    if (tool == "compile_and_diff") return compile_and_diff(input);
    if (tool == "disassemble") return disassemble(input);
    if (tool == "read_memory") return read_memory(input);
    if (tool == "lookup_symbol") return lookup_symbol(input);
    if (tool == "record_note") return record_note(input);
    if (tool == "submit_result") return submit_result(input);
    return ToolOutput::error(std::format("unknown tool '{}'", tool));
}

Result<MatchSession::Evaluation> MatchSession::evaluate(const std::string& source) {
    TRY_ASSIGN(auto r, matching::compile_and_diff(program_, setup_, va_, source));
    Evaluation ev;
    publish(events::CompileFinished{session_id_, r.compile.ok, r.compile.cached, r.compile.duration.count(),
                                    static_cast<int>(std::ranges::count_if(r.compile.diagnostics, [](const matching::Diagnostic& d) {
                                        return d.severity == "error" || d.severity == "fatal error";
                                    }))});
    MatchAttempt attempt;
    attempt.source = source;
    attempt.compiled = r.compile.ok;
    if (!r.compile.ok) {
        auto diags = matching::format_diagnostics(r.compile.diagnostics, 20);
        ev.text = std::format("compile: FAILED{}\n{}", r.compile.timed_out ? " (timed out)" : "",
                              diags.empty() ? truncate_utf8(r.compile.output, 4000) : diags);
        attempt.summary = "compile failed";
    } else if (!r.diff) {
        ev.text = std::format("compile: ok\ndiff: {}", r.diff_error);
        attempt.summary = r.diff_error;
    } else {
        const auto& d = *r.diff;
        matching::ReportOptions ro;
        ro.compact = true;
        ro.context = 2;
        ro.max_rows = kMaxDiffRows;
        ev.text = std::format("compile: ok{}\n{}", r.compile.cached ? " (cached)" : "", matching::to_text(d, ro));
        attempt.match_percent = d.match_percent;
        attempt.byte_exact = d.byte_exact;
        attempt.summary = matching::summary_line(d);
        publish(events::DiffComputed{session_id_, d.match_percent, d.byte_exact, attempt.summary});
    }
    {
        std::lock_guard lock(mutex_);
        attempt.number = static_cast<int>(attempts_.size()) + 1;
        bool improved = attempt.compiled && r.diff && attempt.match_percent >= best_match_;
        if (improved) {
            best_match_ = attempt.match_percent;
            best_source_ = source;
        }
        attempts_.push_back(attempt);
        if (project_) {
            (void)project_->record_attempt(symbol_, Json{{"session", session_id_},
                                                         {"attempt", attempt.number},
                                                         {"compiled", attempt.compiled},
                                                         {"match_percent", attempt.match_percent},
                                                         {"byte_exact", attempt.byte_exact},
                                                         {"summary", attempt.summary},
                                                         {"source", source}});
            if (improved) (void)project_->save_best_source(symbol_, source);
        }
        ev.text += std::format("\nattempt {}: best so far {:.1f}%", attempt.number, best_match_);
    }
    ev.result = std::move(r);
    return ev;
}

ToolOutput MatchSession::compile_and_diff(const Json& input) {
    auto source = json_string_or(input, "source", "");
    if (trim(source).empty()) return ToolOutput::error("`source` is empty: pass a complete translation unit.");
    auto ev = evaluate(source);
    if (!ev) return ToolOutput::error("internal error: " + ev.error().message);
    return ToolOutput::ok(ev->text);
}

ToolOutput MatchSession::disassemble(const Json& input) {
    auto target = json_string_or(input, "target", "");
    auto va = program_.resolve(target);
    if (!va) return ToolOutput::error(std::format("unknown function '{}'; try lookup_symbol", target));
    auto fn = annotate_function(program_, *va);
    if (!fn) return ToolOutput::error(fn.error().message);
    return ToolOutput::ok(clip_lines(to_text(*fn, false), kMaxListingLines));
}

ToolOutput MatchSession::read_memory(const Json& input) {
    std::string where = json_string_or(input, "address", "");
    long long count = std::clamp<long long>(json_int_or(input, "count", 16), 1, 4096);
    std::string format = json_string_or(input, "format", "bytes");
    // address or symbol, with optional +offset
    u64 offset = 0;
    std::string base = where;
    if (auto plus = where.rfind('+'); plus != std::string::npos && plus > 0) {
        if (auto off = parse_u64(where.substr(plus + 1))) {
            offset = *off;
            base = where.substr(0, plus);
        }
    }
    auto va = program_.resolve(base);
    if (!va) return ToolOutput::error(std::format("unknown address or symbol '{}'", where));
    u64 addr = *va + offset;
    const auto& image = program_.image();
    if (!image.contains(addr)) return ToolOutput::error(std::format("{:#x} is outside the image", addr));
    std::string out = std::format("{} ({:#x}):\n", program_.describe_address(addr), addr);

    if (format == "string") {
        auto s = image.read_cstring(addr, 4096);
        if (!s) return ToolOutput::error(std::format("no NUL-terminated string at {:#x}", addr));
        return ToolOutput::ok(out + escape_c_string(*s));
    }
    struct Fmt {
        usize size;
        bool is_float, is_signed, is_ptr;
    };
    Fmt f{1, false, false, false};
    if (format == "u16" || format == "i16") f.size = 2;
    else if (format == "u32" || format == "i32" || format == "f32") f.size = 4;
    else if (format == "u64" || format == "i64" || format == "f64") f.size = 8;
    else if (format == "pointer") f.size = pointer_size(image.arch());
    f.is_float = format == "f32" || format == "f64";
    f.is_signed = !format.empty() && format[0] == 'i';
    f.is_ptr = format == "pointer";
    usize total = format == "bytes" ? static_cast<usize>(count) : static_cast<usize>(count) * f.size;
    total = std::min<usize>(total, 4096);
    auto view = image.view(addr, total);
    std::vector<std::byte> zeros;
    if (!view) {
        auto rva = static_cast<u32>(addr - image.image_base());
        auto copy = program_.image().read_rva(rva, total);
        if (!copy) return ToolOutput::error(std::format("cannot read {} bytes at {:#x}", total, addr));
        zeros = std::move(*copy);
        view = ByteSpan(zeros);
        out += "(uninitialized data reads as zero)\n";
    }
    if (format == "bytes") {
        for (usize i = 0; i < total; i += 16) {
            usize n = std::min<usize>(16, total - i);
            out += std::format("{:08x}  {}\n", addr + i, hex_bytes(reinterpret_cast<const u8*>(view->data() + i), n));
        }
        return ToolOutput::ok(out);
    }
    for (usize i = 0; i + f.size <= total; i += f.size) {
        u64 raw = 0;
        for (usize b = 0; b < f.size; ++b) raw |= u64(static_cast<u8>((*view)[i + b])) << (8 * b);
        std::string value;
        if (f.is_float) value = f.size == 4 ? std::format("{}", std::bit_cast<float>(static_cast<u32>(raw))) : std::format("{}", std::bit_cast<double>(raw));
        else if (f.is_ptr) value = std::format("{:#x}{}", raw, image.contains(raw) ? " = " + program_.describe_address(raw) : "");
        else if (f.is_signed) {
            i64 v = static_cast<i64>(raw << (64 - 8 * f.size)) >> (64 - 8 * f.size);
            value = std::format("{}", v);
        } else value = std::format("{} ({:#x})", raw, raw);
        out += std::format("[{}] {}\n", i / f.size, value);
    }
    return ToolOutput::ok(out);
}

ToolOutput MatchSession::lookup_symbol(const Json& input) {
    std::string query(trim(json_string_or(input, "query", "")));
    if (query.empty()) return ToolOutput::error("`query` is empty");
    auto line = [&](const Symbol& s) {
        return std::format("{:#x} {:<8} size {:<5} {}{}{}\n", s.va, to_string(s.kind), s.size, s.display.empty() ? s.name : s.display,
                           s.display != s.name ? "  [" + s.name + "]" : "", s.is_static ? "  (static)" : "");
    };
    if (auto va = program_.resolve(query)) {
        const Symbol* s = program_.symbols().at(*va);
        if (!s) s = program_.symbols().containing(*va);
        if (s) return ToolOutput::ok(line(*s));
        return ToolOutput::ok(std::format("{:#x}: no symbol ({})", *va, program_.describe_address(*va)));
    }
    std::string q = to_lower(query);
    std::string out;
    usize n = 0;
    for (const auto& [va, s] : program_.symbols()) {
        if (to_lower(s.name).find(q) == std::string::npos && to_lower(s.display).find(q) == std::string::npos &&
            to_lower(s.pdb_name).find(q) == std::string::npos)
            continue;
        if (++n > 25) {
            out += "... more matches; refine the query\n";
            break;
        }
        out += line(s);
    }
    if (out.empty()) return ToolOutput::ok(std::format("no symbols matching '{}'", query));
    return ToolOutput::ok(out);
}

ToolOutput MatchSession::record_note(const Json& input) {
    auto text = std::string(trim(json_string_or(input, "text", "")));
    if (text.empty()) return ToolOutput::error("`text` is empty");
    {
        std::lock_guard lock(mutex_);
        session_notes_.push_back(text);
    }
    if (project_) {
        if (auto r = project_->append_note(symbol_, text); !r) return ToolOutput::error("could not save the note: " + r.error().message);
    }
    return ToolOutput::ok("noted");
}

ToolOutput MatchSession::submit_result(const Json& input) {
    std::string outcome = json_string_or(input, "outcome", "");
    std::string source = json_string_or(input, "source", "");
    std::string reason = json_string_or(input, "reason", "");
    if (outcome == "give_up") {
        if (!trim(source).empty() && !best_source_) {
            std::lock_guard lock(mutex_);
            best_source_ = source;
        }
        auto out = ToolOutput::ok(std::format("recorded: gave up ({}). Best match {:.1f}%.", reason.empty() ? "no reason given" : reason, best_match_));
        out.end_session = true;
        out.outcome = {{"outcome", "gave_up"}, {"reason", reason}, {"best_match", best_match_}};
        return out;
    }
    if (outcome != "matched") return ToolOutput::error("`outcome` must be \"matched\" or \"give_up\"");
    if (trim(source).empty()) return ToolOutput::error("`source` is required when submitting a match");
    auto ev = evaluate(source);
    if (!ev) return ToolOutput::error("internal error: " + ev.error().message);
    if (!ev->result.diff || !ev->result.diff->byte_exact)
        return ToolOutput::error("not accepted: the submitted source is not byte-exact.\n" + ev->text);
    {
        std::lock_guard lock(mutex_);
        matched_ = true;
        matched_source_ = source;
        best_match_ = 100.0;
        best_source_ = source;
    }
    if (project_) {
        if (auto r = project_->write_matched_source(symbol_, source); r)
            publish(events::FileWritten{fs::to_utf8(project_->matched_source_path(symbol_)), "matched source"});
    }
    auto out = ToolOutput::ok("accepted: byte-exact match verified.");
    out.end_session = true;
    out.outcome = {{"outcome", "matched"}, {"best_match", 100.0}};
    return out;
}

std::string MatchSession::status_line(int turns_left) const {
    std::lock_guard lock(mutex_);
    return std::format("[status] turns left: {}; attempts: {}; best match: {:.1f}%", turns_left, attempts_.size(), best_match_);
}

std::string MatchSession::brief() const {
    std::string out = "# Target function\n";
    out += std::format("function: {}\n", symbol_.display.empty() ? symbol_.name : symbol_.display);
    out += std::format("symbol:   {}{}\n", symbol_.name, symbol_.pdb_name.empty() || symbol_.pdb_name == symbol_.name ? "" : "  (PDB name: " + symbol_.pdb_name + ")");
    out += std::format("address:  {:#x}, {} bytes{}\n", va_, symbol_.size, symbol_.is_static ? ", static (internal linkage)" : "");
    out += std::format("toolchain: {} ({}), flags: {}\n", setup_.toolchain.name, matching::to_string(setup_.toolchain.kind),
                       join(setup_.flags, " ").empty() ? "(none)" : join(setup_.flags, " "));
    if (!setup_.include_dirs.empty()) {
        std::vector<std::string> headers;
        for (const auto& dir : setup_.include_dirs) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec)) headers.push_back(fs::to_utf8(std::filesystem::relative(it->path(), dir, ec)));
        }
        std::ranges::sort(headers);
        out += std::format("project headers available: {}\n", headers.empty() ? "(none)" : join(headers, ", "));
    }

    auto fn = annotate_function(program_, va_);
    if (!fn) return out + "\n(could not disassemble: " + fn.error().message + ")\n";
    out += "\n# Annotated disassembly\n```\n" + clip_lines(to_text(*fn, false), kMaxListingLines) + "```\n";

    out += "\n# Referenced symbols\n";
    if (fn->callees.empty() && fn->data_refs.empty()) out += "(none)\n";
    for (const auto& c : fn->callees)
        out += std::format("- calls {}: {}{}\n", c.display, c.detail.empty() ? c.name : c.detail, c.name != c.display ? "  [" + c.name + "]" : "");
    for (const auto& d : fn->data_refs) {
        std::string extra;
        if (d.kind == "data") {
            if (const Symbol* s = program_.symbols().at(d.va); s && s->size > 0 && s->size <= 64) {
                if (auto bytes = program_.image().view(d.va, s->size))
                    extra = std::format(", initial bytes: {}", hex_bytes(reinterpret_cast<const u8*>(bytes->data()), bytes->size()));
            }
        }
        out += std::format("- {} {}: {}{}{}\n", d.kind, d.display, display_name(d.name), d.detail.empty() || d.kind == "data" ? "" : " = " + d.detail, extra);
    }
    if (!fn->callers.empty()) out += std::format("\n# Callers\n{}\n", join(fn->callers, ", "));

    if (project_) {
        auto notes = project_->notes(symbol_);
        auto attempts = project_->attempts(symbol_);
        if (!notes.empty() || !attempts.empty() || best_source_) {
            out += "\n# Earlier attempts\n";
            if (!attempts.empty()) out += std::format("{} earlier attempt(s); best match {:.1f}%\n", attempts.size(), best_match_);
            if (!notes.empty()) out += "notes:\n" + notes;
            if (best_source_) out += "best source so far:\n```cpp\n" + *best_source_ + "\n```\n";
        }
    }
    out += "\nWrite a complete candidate translation unit and call compile_and_diff.";
    return out;
}

} // namespace decomp::agent
