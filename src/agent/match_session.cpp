#include "agent/match_session.hpp"

#include "analysis/annotate.hpp"
#include "analysis/demangle.hpp"
#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "matching/unit_source.hpp"
#include "project/types.hpp"

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
- get_type: the exact layout of a struct, class, union or enum as the compiler lays it out: size, bases, vtable slots, each field's offset, size and type (from the project's headers, else the target's PDB).
- record_note: save a short note for future attempts on this function (what you learned, what did not work).
- set_symbol: name a function or global the code shows you the purpose of (a callee, a table, a counter), or correct a symbol's kind or size. Later sessions see the name; the supervisor may have to approve it.
- define_type: put a struct, class, union, enum or typedef into a shared project header (include/types.h unless you name another), so this and later functions can #include it. It is checked against every verified source that includes the header; the supervisor may have to approve it.
- submit_result: finish with outcome "matched" and the exact source that matched (it is re-verified; only byte-exact results are accepted), or "give_up" with your best source and the reason.

How to work:
1. Read the brief: annotated disassembly, referenced symbols with their declarations, callers, and notes from earlier attempts.
2. Work out the signature, calling convention and the types the function touches. Use disassemble on callers or callees when the brief is not enough, and get_type for a type's exact layout. Listing comments such as `this->hp` or `arg_0->area() (virtual, slot 0)` name what an operand reaches through a pointer of known type; the `; types:` line says what the pointers are at entry.
3. Write a first complete candidate and call compile_and_diff early. Fix the largest structural differences first (control flow, missing or extra code), then operand-level differences.
4. The candidate must be a complete, self-contained translation unit: declare every external function, global variable and type it uses (extern declarations, struct definitions with the right layout). Declarations must produce the same decorated names shown in the brief: `?add@@YAHHH@Z` is `int __cdecl add(int, int)`; `extern "C"` names like `_entry` need `extern "C"`; arrays mangle like pointers (`?g_table@@3PAHA` can be `int g_table[8]`). No inline assembly. Only include headers the brief lists as available. Define the target function (and static helpers only if the target inlines them). When the brief has a "# Translation unit" section, your source is composed into that unit's source instead of compiled alone: follow what the section says.

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

Use set_symbol and define_type for facts the code establishes, with the evidence as the reason; they never change the function you are matching, and a declined one is not an error in your work.

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
        if (auto units = project::load_units(*project_); units)
            if (const Unit* unit = project::matching_unit(*project_, *units, symbol_)) unit_ = *unit;
    }
}

std::optional<std::string> MatchSession::written_path() const {
    std::lock_guard lock(mutex_);
    return written_path_;
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
    t["get_type"] = object_schema({{"name", string_prop("The type's name, as the brief or a listing writes it (e.g. \"Player\", \"game::Shape\")")}});
    t["record_note"] = object_schema({{"text", string_prop("Short note for future attempts on this function")}});
    t["set_symbol"] = object_schema({
        {"address", string_prop("The symbol's address or current name (e.g. \"0x401100\", \"sub_401100\", \"data_403010\")")},
        {"name", string_prop("The new name: a C name, or an MSVC decorated name for C++ (\"\" keeps the name)")},
        {"kind", {{"type", "string"}, {"enum", {"", "function", "data"}}, {"description", "The new kind (\"\" keeps the kind)"}}},
        {"size", {{"type", "integer"}, {"description", "The new size in bytes (0 keeps the size)"}}},
        {"reason", string_prop("The evidence: what in the code shows it (recorded with the change)")},
    });
    t["define_type"] = object_schema({
        {"name", string_prop("The type's name (e.g. \"Player\")")},
        {"declaration", string_prop("Its complete definition as C/C++ source: a struct, class, union or enum definition or a typedef, "
                                    "with #pragma pack lines around it if it needs them")},
        {"header", string_prop("The project header under include/ to put it in (\"\" for types.h)")},
        {"reason", string_prop("The evidence: what in the code shows the layout (recorded with the change)")},
    });
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
    if (name == "get_type")
        return "The exact layout of a struct, class, union or enum: its size, bases, vtable slots and virtual methods, and "
               "every field's offset, size and type, as the compiler lays it out. From the project's headers when they declare "
               "the type (the source of truth), else from the target's PDB. Use it to declare a type with the right layout.";
    if (name == "record_note")
        return "Save a short note for future attempts on this function (what you learned, what did not work). Notes "
               "persist across sessions.";
    if (name == "set_symbol")
        return "Name a function or global whose purpose the code shows, or correct a symbol's kind or size (\"\" and 0 keep "
               "a field). Recorded with your reason in symbols.txt's log; later sessions see the change. The supervisor may have "
               "to approve it. The function you are matching, matched functions, and names verified sources use cannot change.";
    if (name == "define_type")
        return "Add or replace a type in a shared project header under include/ (types.h by default), so this and later "
               "functions can #include it instead of declaring it themselves. The header must compile and every verified "
               "source that includes it must stay byte-exact. The supervisor may have to approve it.";
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
    if (tool == "get_type") return get_type(input);
    if (tool == "record_note") return record_note(input);
    if (tool == "set_symbol") return set_symbol(input);
    if (tool == "define_type") return define_type(input);
    if (tool == "submit_result") return submit_result(input);
    return ToolOutput::error(std::format("unknown tool '{}'", tool));
}

Result<MatchSession::Evaluation> MatchSession::evaluate(const std::string& source) {
    int number = 0;
    {
        std::lock_guard lock(mutex_);
        number = static_cast<int>(attempts_.size()) + 1;
    }
    std::vector<std::string> flags = setup_.toolchain.flags;
    flags.insert(flags.end(), setup_.flags.begin(), setup_.flags.end());
    publish(events::CompileStarted{session_id_, setup_.toolchain.name, join(flags, " ")});
    // In a unit, the candidate joins the unit's source as it is on disk now and the whole unit is compiled.
    Evaluation ev;
    matching::CandidateResult r;
    if (unit_) {
        TRY_ASSIGN(auto candidate, project::compile_candidate(*project_, program_, setup_, symbol_, source, &*unit_));
        r = std::move(candidate.result);
        ev.unit = std::move(candidate.unit);
    } else {
        TRY_ASSIGN(r, matching::compile_and_diff(program_, setup_, va_, source));
    }
    publish(events::CompileFinished{session_id_, r.compile.ok, r.compile.cached, r.compile.duration.count(),
                                    static_cast<int>(std::ranges::count_if(r.compile.diagnostics,
                                                                           [](const matching::Diagnostic& d) {
                                                                               return d.severity == "error" || d.severity == "fatal error";
                                                                           })),
                                    r.compile.exit_code, join(r.compile.command, " "), truncate_utf8(r.compile.output, events::kMaxCompileOutput),
                                    setup_.toolchain.name});
    MatchAttempt attempt;
    attempt.source = source;
    attempt.compiled = r.compile.ok;
    if (ev.unit && !ev.unit->rejected.empty()) {
        ev.text = std::format("unit: your source cannot join {}: {}", unit_->source, ev.unit->rejected.front().second);
        attempt.compiled = false;
        attempt.summary = "cannot join the unit's source";
    } else if (!r.compile.ok) {
        // In a unit the lines are the unit source's: show their text, which the model wrote or read.
        std::string diags;
        if (ev.unit) {
            const auto unit_lines = split_lines(ev.unit->content);
            usize shown = 0;
            for (const auto& d : r.compile.diagnostics) {
                if (d.severity == "note") continue;
                if (++shown > 20) break;
                diags += std::format("line {}{}: {}{}: {}\n", d.line, d.column ? std::format(":{}", d.column) : "", d.severity,
                                     d.code.empty() ? "" : " " + d.code, d.message);
                if (d.line > 0 && static_cast<usize>(d.line) <= unit_lines.size())
                    diags += std::format("    | {}\n", trim(unit_lines[static_cast<usize>(d.line) - 1]));
            }
        } else {
            diags = matching::format_diagnostics(r.compile.diagnostics, 20);
        }
        ev.text = std::format("compile: FAILED{}{}\n{}", r.compile.timed_out ? " (timed out)" : "",
                              ev.unit ? std::format(" ({} with your source composed in)", unit_->source) : std::string(),
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
        if (ev.unit)
            if (const auto failing = project::failing_functions(*ev.unit, va_); !failing.empty())
                ev.text += std::format("\nunit: with your source in {}, {} other function(s) there are not byte-exact: {}", unit_->source,
                                       failing.size(), project::describe_checks(program_, failing));
        attempt.match_percent = d.match_percent;
        attempt.byte_exact = d.byte_exact;
        attempt.summary = matching::summary_line(d);
        publish(events::DiffComputed{session_id_, d.match_percent, d.byte_exact, attempt.summary, number, static_cast<int>(d.equal),
                                     static_cast<int>(d.encoding), static_cast<int>(d.operand), static_cast<int>(d.opcode),
                                     static_cast<int>(d.inserted), static_cast<int>(d.deleted)});
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
            (void)project_->record_attempt(
                symbol_, Json{{"session", session_id_},
                              {"attempt", attempt.number},
                              {"origin", "agent"},
                              {"time", std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()))},
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
    const auto headers = header_types();
    auto fn = annotate_function(program_, *va, true, headers ? &headers->catalog : nullptr);
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

std::optional<ToolOutput> MatchSession::approve(ApprovalRequest request, std::string& approval) {
    approval = "auto";
    if (!approvals_) return std::nullopt;
    const std::string action = request.action;
    const ApprovalDecision decision = approvals_->request(std::move(request), setup_.cancelled, worker_);
    if (decision.verdict == "cancelled") return ToolOutput::error("The session ended before the supervisor decided; nothing was changed.");
    if (!decision.approved())
        return ToolOutput::error(std::format("Not done: {} {}{}", action == kSetSymbolAction ? "the symbol change" : "the type definition",
                                             decision.by == "policy" ? "is not allowed by this run's policy"
                                                                     : "was declined by the supervisor",
                                             decision.reason.empty() ? "." : ": " + decision.reason + "."));
    approval = decision.by == "policy" ? "auto" : "approved by " + decision.by;
    return std::nullopt;
}

ToolOutput MatchSession::set_symbol(const Json& input) {
    if (!project_) return ToolOutput::error("set_symbol needs a project; this session has none.");
    const std::string address(trim(json_string_or(input, "address", "")));
    const std::string name(trim(json_string_or(input, "name", "")));
    const std::string kind_text(trim(json_string_or(input, "kind", "")));
    const i64 size = json_int_or(input, "size", 0);
    const std::string reason(trim(json_string_or(input, "reason", "")));
    if (address.empty()) return ToolOutput::error("`address` is empty.");
    if (name.empty() && kind_text.empty() && size == 0) return ToolOutput::error("Nothing to change: give a name, a kind or a size.");
    if (reason.empty()) return ToolOutput::error("Give the reason: what in the code shows it. It is recorded with the change.");
    const auto va = program_.resolve(address);
    if (!va) return ToolOutput::error(std::format("No symbol or address '{}'.", address));
    if (*va == va_)
        return ToolOutput::error("The function this session matches cannot be changed during the session (its history and source are "
                                 "kept under its name). Record a note with what you found instead.");
    const Symbol* current = program_.symbols().at(*va);
    if (!name.empty()) {
        if (name.size() > 512 || std::ranges::any_of(name, [](char c) { return static_cast<unsigned char>(c) <= ' '; }))
            return ToolOutput::error("A name has no spaces or control characters.");
        if (auto other = program_.resolve(name); other && *other != *va)
            return ToolOutput::error(std::format("'{}' already names {:#x}.", name, *other));
    }
    std::optional<SymbolKind> kind;
    if (!kind_text.empty()) {
        kind = symbol_kind_from_string(kind_text);
        if (kind != SymbolKind::function && kind != SymbolKind::data) return ToolOutput::error("`kind` is \"function\", \"data\" or \"\".");
    }
    if (size < 0 || size > 0x1000000) return ToolOutput::error("`size` is a byte count (0 keeps the size).");
    if (!current && name.empty()) return ToolOutput::error(std::format("{:#x} has no symbol yet: a new one needs a name.", *va));
    // What verified sources depend on stays.
    if (current) {
        if (current->kind == SymbolKind::function && project_->function_info(*va).status == project::FunctionStatus::matched)
            return ToolOutput::error(std::format("{} is matched: its verified source depends on its name, kind and size.", current->display));
        if (!name.empty())
            for (const std::string& used : {qualified_name(current->name), current->pdb_name}) {
                if (used.empty()) continue;
                if (const auto sources = project::verified_sources_using(*project_, used); !sources.empty())
                    return ToolOutput::error(std::format("'{}' is used by verified sources ({}): renaming it would stop them matching.", used,
                                                         join(sources, ", ")));
            }
    }
    project::SymbolEdit edit;
    edit.va = *va;
    if (!name.empty()) edit.name = name;
    edit.kind = kind;
    if (size > 0) edit.size = static_cast<u32>(size);

    // What symbols.txt would hold before and after, for the supervisor.
    Symbol after = current ? *current : Symbol{};
    if (!current) {
        after.va = *va;
        after.kind = SymbolKind::function;
    }
    if (edit.name) after.name = *edit.name;
    if (edit.kind) after.kind = *edit.kind;
    if (edit.size) after.size = *edit.size;
    after.source = SymbolSource::agent;
    std::vector<std::string> what;
    if (edit.name) what.push_back(current ? std::format("rename {} to {}", qualified_name(current->name), *edit.name) : std::format("name it {}", *edit.name));
    if (edit.kind && (!current || current->kind != *edit.kind)) what.push_back(std::format("make it {}", to_string(*edit.kind)));
    if (edit.size && (!current || current->size != *edit.size)) what.push_back(std::format("size {:#x}", *edit.size));
    const std::string summary = std::format("{:#x}: {}", *va, what.empty() ? std::string("no change") : join(what, ", "));
    const std::string display = symbol_.display.empty() ? symbol_.name : symbol_.display;
    std::string approval;
    if (auto refused = approve(ApprovalRequest{std::string(kSetSymbolAction), session_id_, display, *va, "symbols.txt", summary,
                                               project::format_symbol_line(after, nullptr) + "\n",
                                               current ? project::format_symbol_line(*current, nullptr) + "\n" : std::string()},
                               approval))
        return *refused;
    auto change = project_->set_symbol(edit, project::ChangeOrigin{SymbolSource::agent, session_id_, reason});
    if (!change) return ToolOutput::error("Not done: " + change.error().message);
    const Symbol& done = change->after ? *change->after : after;
    publish(events::SymbolChanged{*va, change->before ? change->before->name : std::string(), done.name, std::string(to_string(done.kind)),
                                  done.size, "agent", session_id_});
    return ToolOutput::ok(std::format("Done ({}): {}. Recorded in symbols.txt; later sessions see it, while this session's brief and "
                                      "listings keep the names they started with.",
                                      approval, summary));
}

ToolOutput MatchSession::get_type(const Json& input) {
    const std::string name(trim(json_string_or(input, "name", "")));
    if (name.empty()) return ToolOutput::error("Give the type's name.");
    std::string error;
    const auto headers = header_types(&error);
    const TypeCatalog& pdb = program_.pdb_types().catalog;
    const project::HeaderType* declared = headers ? headers->header_of(name) : nullptr;
    const TypeLayout* header = declared ? headers->catalog.find(name) : nullptr;
    const TypeLayout* target = pdb.find(name);
    // A type the headers use from elsewhere (a system header's) has a layout too.
    const TypeLayout* used = !header && !target && headers ? headers->catalog.find(name) : nullptr;
    const TypeLayout* layout = header ? header : target ? target : used;
    if (!layout) {
        std::vector<std::string> similar;
        const std::string needle = to_lower(name);
        for (const TypeCatalog* catalog : {headers ? &headers->catalog : nullptr, &pdb}) {
            if (!catalog) continue;
            for (const TypeLayout& t : catalog->types())
                if (similar.size() < 10 && to_lower(t.name).find(needle) != std::string::npos && std::ranges::find(similar, t.name) == similar.end())
                    similar.push_back(t.name);
        }
        std::string text = std::format("No type {} in the project's headers or the target's PDB.", name);
        if (!similar.empty()) text += " Similar: " + join(similar, ", ") + ".";
        if (!error.empty()) {
            const auto lines = split_lines(error);
            text += " (The project's headers do not compile, so their types are unknown: " + (lines.empty() ? error : lines.front()) + ")";
        }
        return ToolOutput::error(text);
    }
    std::string text;
    if (header) text = std::format("// {} (the project's header)\n", declared->header);
    else if (target) text = "// the target's PDB; no project header declares it yet (define_type can)\n";
    else text = "// used by the project's headers (declared outside include/)\n";
    text += to_text(*layout);
    if (header && target) {
        const auto differences = compare_layouts(*header, *target);
        if (differences.empty()) text += "// the same in the target's PDB\n";
        else text += "// differs from the target's PDB:\n//   " + join(differences, "\n//   ") + "\n";
    }
    return ToolOutput::ok(text);
}

ToolOutput MatchSession::define_type(const Json& input) {
    if (!project_) return ToolOutput::error("define_type needs a project; this session has none.");
    const std::string name(trim(json_string_or(input, "name", "")));
    const std::string declaration = json_string_or(input, "declaration", "");
    const std::string header(trim(json_string_or(input, "header", "")));
    const std::string reason(trim(json_string_or(input, "reason", "")));
    if (name.empty() || trim(declaration).empty()) return ToolOutput::error("Give the type's name and its definition.");
    if (reason.empty()) return ToolOutput::error("Give the reason: what in the code shows the layout. It is recorded with the change.");
    const std::string display = symbol_.display.empty() ? symbol_.name : symbol_.display;
    for (int attempt = 0;; ++attempt) {
        auto change = project::prepare_type_change(*project_, program_, setup_, name, declaration, header);
        if (!change) return ToolOutput::error("Not defined: " + change.error().message);
        std::string approval = "auto";
        if (attempt == 0) {
            const std::string summary =
                std::format("define {} in {} ({}{})", name, change->header,
                            change->base.empty() ? "new header" : change->replaced ? "replaces its definition" : "added",
                            change->sources.empty() ? std::string() : std::format("; {} verified source(s) include it and still verify", change->sources.size()));
            if (auto refused = approve(ApprovalRequest{std::string(kDefineTypeAction), session_id_, display, va_, change->header, summary,
                                                       change->content, change->base},
                                       approval))
                return *refused;
        }
        auto written = project::commit_type_change(*project_, *change, project::ChangeOrigin{SymbolSource::agent, session_id_, reason},
                                                   project::ChangeSubject{symbol_.name, va_, {}, {}});
        if (!written && written.error().code == ErrorCode::conflict && attempt < 3) continue;  // another writer changed the header
        if (!written) return ToolOutput::error("Not defined: " + written.error().message);
        {
            std::lock_guard lock(types_mutex_);
            header_types_loaded_ = false;
            header_types_.reset();
        }
        publish(events::FileWritten{change->header, "type definition", written->size, written->sha1, session_id_, approval});
        return ToolOutput::ok(std::format("Done ({}): {} is defined in {}. Use it with #include \"{}\".", approval, name, change->header,
                                          change->header.substr(std::string_view("include/").size())));
    }
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
    // In a unit, the unit's other functions must stay byte-exact (those that were before it joined).
    if (ev->unit) {
        auto broken = project::broken_functions(program_, setup_, *ev->unit, va_);
        if (!broken) return ToolOutput::error("internal error: " + broken.error().message);
        if (!broken->empty())
            return ToolOutput::error(std::format("not accepted: byte-exact itself, but with your source in {} these functions of the unit "
                                                 "are no longer byte-exact: {}. Change your source so that they compile as before "
                                                 "(the declarations and helpers it adds to the unit's prelude affect them too).",
                                                 unit_->source, project::describe_checks(program_, *broken)));
    }
    std::string approval = "auto";
    if (project_ && approvals_) {
        // The supervisor may want to see (or veto) what lands in the project.
        std::error_code ec;
        std::string path, previous, content = source;
        if (ev->unit) {
            path = unit_->source;
            previous = ev->unit->base;
            content = ev->unit->content;
        } else {
            const auto file = project_->matched_source_path(symbol_);
            if (std::filesystem::exists(file, ec))
                if (auto text = fs::read_text(file)) previous = std::move(*text);
            path = fs::to_utf8(std::filesystem::relative(file, project_->root(), ec));
        }
        const std::string display = symbol_.display.empty() ? symbol_.name : symbol_.display;
        const std::string summary =
            ev->unit ? std::format("byte-exact {} joins {} ({} functions in it{})", display, unit_->source, ev->unit->functions.size(),
                                   previous.empty() ? ", new file" : "")
                     : std::format("byte-exact {} ({} bytes of source{})", display, source.size(),
                                   previous.empty() ? ", new file" : ", replaces the existing file");
        ApprovalRequest request{std::string(kWriteSourceAction), session_id_, display, va_, path, summary, content, std::move(previous)};
        const ApprovalDecision decision = approvals_->request(std::move(request), setup_.cancelled, worker_);
        if (decision.verdict == "cancelled")
            return ToolOutput::error("Verified byte-exact, but the session ended before the supervisor decided whether to save it.");
        if (!decision.approved()) {
            std::lock_guard lock(mutex_);
            declined_.push_back(source);
        }
        if (!decision.approved())
            return ToolOutput::error(std::format("Verified byte-exact, but the supervisor declined saving it{} Adjust the source as "
                                                 "asked and submit it again, or give up with your reasons.",
                                                 decision.reason.empty() ? "." : ": " + decision.reason + "."));
        approval = decision.by == "policy" ? "auto" : "approved by " + decision.by;
    }
    // changes.jsonl says who let the write through when it was not automatic.
    const std::string write_reason = approval == "auto" ? std::string("verified match") : std::format("verified match, {}", approval);
    if (project_ && ev->unit) {
        // Another worker may have changed the unit source since: compose again on top of it.
        project::UnitChange change = std::move(*ev->unit);
        for (int attempt = 0;; ++attempt) {
            auto written = project::commit_unit_change(*project_, change, project::ChangeOrigin{SymbolSource::agent, session_id_, write_reason},
                                                       project::ChangeSubject{symbol_.name, va_, {}, {}});
            if (written) {
                publish(events::FileWritten{unit_->source, "unit source", written->size, written->sha1, session_id_, approval});
                std::lock_guard lock(mutex_);
                written_path_ = unit_->source;
                break;
            }
            if (written.error().code != ErrorCode::conflict || attempt >= 3)
                return ToolOutput::error("Verified byte-exact, but the unit source could not be written: " + written.error().message);
            auto again = project::prepare_unit_change(*project_, program_, setup_, *unit_, {{&symbol_, source}});
            if (!again) return ToolOutput::error("internal error: " + again.error().message);
            const bool exact = std::ranges::any_of(again->verification.functions, [&](const auto& c) { return c.va == va_ && c.byte_exact(); });
            auto broken = exact && again->rejected.empty() ? project::broken_functions(program_, setup_, *again, va_)
                                                           : Result<std::vector<const matching::UnitCheck*>>{};
            if (!broken) return ToolOutput::error("internal error: " + broken.error().message);
            if (!again->rejected.empty() || !again->verification.error.empty() || !exact || !broken->empty())
                return ToolOutput::error(std::format("not accepted: {} changed while your match was being saved, and with its new content your "
                                                     "source is no longer byte-exact or breaks another function there. Compile again and "
                                                     "submit what matches.",
                                                     unit_->source));
            change = std::move(*again);
        }
    }
    {
        std::lock_guard lock(mutex_);
        matched_ = true;
        matched_source_ = source;
        best_match_ = 100.0;
        best_source_ = source;
    }
    if (project_ && !unit_) {
        if (auto r = project_->write_matched_source(symbol_, source, project::ChangeOrigin{SymbolSource::agent, session_id_, write_reason}); r) {
            std::error_code ec;
            publish(events::FileWritten{fs::to_utf8(r->path), "matched source", r->size, r->sha1, session_id_, approval});
            std::lock_guard lock(mutex_);
            written_path_ = fs::to_utf8(std::filesystem::relative(r->path, project_->root(), ec));
        } else {
            log::warn("cannot save the matched source of {}: {}", symbol_.name, r.error().message);
        }
    }
    auto out = ToolOutput::ok("accepted: byte-exact match verified.");
    out.end_session = true;
    out.outcome = {{"outcome", "matched"}, {"best_match", 100.0}};
    return out;
}

std::optional<std::string> MatchSession::unsubmitted_exact_source() const {
    std::lock_guard lock(mutex_);
    if (matched_) return std::nullopt;
    for (auto it = attempts_.rbegin(); it != attempts_.rend(); ++it)
        if (it->byte_exact && std::ranges::find(declined_, it->source) == declined_.end()) return it->source;
    return std::nullopt;
}

std::string MatchSession::status_line(int turns_left) const {
    std::lock_guard lock(mutex_);
    return std::format("[status] turns left: {}; attempts: {}; best match: {:.1f}%", turns_left, attempts_.size(), best_match_);
}

std::shared_ptr<const project::HeaderTypes> MatchSession::header_types(std::string* error) const {
    std::lock_guard lock(types_mutex_);
    if (!header_types_loaded_ && project_) {
        header_types_loaded_ = true;
        header_types_error_.clear();
        auto compiled = project::compile_header_types(*project_, setup_, program_.arch());
        if (compiled) header_types_ = std::make_shared<const project::HeaderTypes>(std::move(*compiled));
        else if (compiled.error().code != ErrorCode::unsupported) header_types_error_ = compiled.error().message;
    }
    if (error) *error = header_types_error_;
    return header_types_;
}

std::string MatchSession::types_section() const {
    std::string error;
    const auto headers = header_types(&error);
    const ProgramTypes& pdb = program_.pdb_types();
    std::string out;
    if (!error.empty()) out += "The project's headers do not compile, so their types are unknown:\n" + clip_lines(error, 12) + "\n";
    if (headers && !headers->declared.empty()) {
        std::vector<std::string> names;
        for (const auto& declared : headers->declared) {
            if (names.size() == 60) {
                names.push_back(std::format("and {} more", headers->declared.size() - 60));
                break;
            }
            const TypeLayout* layout = headers->catalog.find(declared.name);
            names.push_back(layout ? std::format("{} {} ({} bytes)", to_string(layout->kind), declared.name, layout->size) : declared.name);
        }
        out += std::format("The project's headers declare: {}.\n", join(names, ", "));
    }
    // The layouts of the types the function's PDB type names: the headers' (the source of truth), else the PDB's.
    std::string layouts;
    for (const std::string& name : types_of_function(pdb, va_)) {
        const project::HeaderType* declared = headers ? headers->header_of(name) : nullptr;
        const TypeLayout* layout = declared ? headers->catalog.find(name) : nullptr;
        std::string where = declared ? declared->header : std::string();
        if (!layout) {
            layout = pdb.catalog.find(name);
            where = "from the target's PDB; no project header declares it yet (define_type can)";
        }
        if (layout) layouts += "```\n// " + where + "\n" + to_text(*layout) + "```\n";
    }
    if (!layouts.empty()) out += "The types this function's signature names:\n" + clip_lines(layouts, 150);
    return out.empty() ? out : "\n# Types\n" + out;
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

    const auto headers = header_types();
    auto fn = annotate_function(program_, va_, true, headers ? &headers->catalog : nullptr);
    if (!fn) return out + "\n(could not disassemble: " + fn.error().message + ")\n";
    out += "\n# Annotated disassembly\n```\n" + clip_lines(to_text(*fn, false), kMaxListingLines) + "```\n";

    out += "\n# Referenced symbols\n";
    if (fn->callees.empty() && fn->data_refs.empty()) out += "(none)\n";
    // How an import is reached tells how it was declared.
    auto import_note = [](const Reference& r) -> std::string {
        if (r.import_call == "dllimport") return " (through the import table: declare it __declspec(dllimport))";
        if (r.import_call == "thunk") return " (through the linker's import thunk: declare it without __declspec(dllimport))";
        return {};
    };
    for (const auto& c : fn->callees)
        out += std::format("- calls {}: {}{}{}\n", c.display, c.detail.empty() ? c.name : c.detail, c.name != c.display ? "  [" + c.name + "]" : "",
                           import_note(c));
    for (const auto& d : fn->data_refs) {
        std::string extra;
        if (d.kind == "data") {
            if (const Symbol* s = program_.symbols().at(d.va); s && s->size > 0 && s->size <= 64) {
                if (auto bytes = program_.image().view(d.va, s->size))
                    extra = std::format(", initial bytes: {}", hex_bytes(reinterpret_cast<const u8*>(bytes->data()), bytes->size()));
            }
        }
        out += std::format("- {} {}: {}{}{}{}\n", d.kind, d.display, display_name(d.name), d.detail.empty() || d.kind == "data" ? "" : " = " + d.detail, extra,
                           import_note(d));
    }
    if (!fn->callers.empty()) out += std::format("\n# Callers\n{}\n", join(fn->callers, ", "));
    out += types_section();

    if (unit_) {
        const auto text = fs::read_text(project_->root() / fs::from_utf8(unit_->source));
        matching::UnitSource source = matching::UnitSource::parse(text.value_or(""));
        usize total = 0;
        for (const Symbol* f : program_.symbols().functions()) total += f->object == unit_->name ? 1 : 0;
        std::vector<std::string> names;
        for (const auto& f : source.functions) {
            const Symbol* s = program_.symbols().at(f.va);
            names.push_back(s ? qualified_name(s->name) : std::format("{:#x}", f.va));
        }
        const bool c = unit_->source.ends_with(".c") || unit_->source.ends_with(".C");
        out += std::format("\n# Translation unit\nThis function belongs to the unit {} ({} functions). ", unit_->name, total);
        if (!text) out += std::format("Its source will be {} ({}): no function of the unit is matched yet, so yours starts it.\n", unit_->source, c ? "C" : "C++");
        else out += std::format("Its source is {} ({}), which holds {} of them{}.\n", unit_->source, c ? "C" : "C++", names.size(),
                                names.empty() ? "" : ": " + join(names, ", "));
        out += "Your source is composed into it: your definition of the function joins the unit's functions in address order (with "
               "the #pragma lines around it), and your other declarations, types and data join the unit's prelude unless it has them "
               "already. compile_and_diff compiles the whole unit that way, and submit_result also checks that the unit's other "
               "functions stay byte-exact. Define the function at the top level of your source, not inside a class or namespace "
               "block; do not define the prelude's types differently";
        out += c ? "; write C, not C++.\n" : ".\n";
        source.functions.clear();
        if (const std::string prelude = source.render(); !prelude.empty())
            out += "The unit's prelude, which your source shares:\n```" + std::string(c ? "c" : "cpp") + "\n" + clip_lines(prelude, 200) + "```\n";
    }

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
    out += unit_ ? "\nWrite a candidate with the function and what the unit's prelude lacks, and call compile_and_diff."
                 : "\nWrite a complete candidate translation unit and call compile_and_diff.";
    return out;
}

} // namespace decomp::agent
