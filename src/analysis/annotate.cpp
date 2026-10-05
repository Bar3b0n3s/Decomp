#include "analysis/annotate.hpp"

#include "analysis/demangle.hpp"
#include "analysis/eh.hpp"
#include "analysis/typeflow.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <map>
#include <set>

namespace decomp {
namespace {

std::string float_text(const Program& program, u64 va, std::string_view symbol_name) {
    bool is_double = symbol_name.size() == std::string_view("__real@").size() + 16;
    if (is_double) {
        if (auto v = program.image().read<u64>(va)) return std::format("{}", std::bit_cast<double>(*v));
    } else if (auto v = program.image().read<u32>(va)) {
        return std::format("{}f", std::bit_cast<float>(*v));
    }
    return {};
}

bool printable_string(const std::string& s) {
    if (s.size() < 2) return false;
    return std::ranges::all_of(s, [](unsigned char c) { return c >= 0x20 || c == '\n' || c == '\r' || c == '\t'; });
}

std::string short_name(const Symbol& s) {
    if (s.kind == SymbolKind::import) return s.name;
    auto q = qualified_name(s.name);
    return q.empty() ? s.name : q;
}

} // namespace

Reference describe_reference(const Program& program, u64 va) {
    Reference r;
    r.va = va;
    const Symbol* s = program.symbols().at(va);
    if (!s) {
        // Calls through linker thunks read as calls to the destination.
        if (auto dest = program.thunk_destination(va)) {
            r = describe_reference(program, *dest);
            r.va = va;
            const std::string via = std::format("via thunk at {:#x}", va);
            r.detail = r.detail.empty() ? via : r.detail + "; " + via;
            return r;
        }
    }
    i64 offset = 0;
    if (!s) {
        s = program.symbols().containing(va);
        if (s) offset = static_cast<i64>(va - s->va);
    }
    if (s) {
        r.name = offset ? std::format("{}+{:#x}", s->name, offset) : s->name;
        r.display = offset ? std::format("{}+{:#x}", short_name(*s), offset) : short_name(*s);
        r.kind = std::string(to_string(s->kind));
        if (s->kind == SymbolKind::string) {
            if (auto str = program.image().read_cstring(va)) r.detail = escape_c_string(*str);
        } else if (s->kind == SymbolKind::float_const) {
            r.detail = float_text(program, va, s->name);
        } else if (s->kind == SymbolKind::import) {
            r.detail = s->display;
        } else if (s->kind == SymbolKind::function) {
            r.detail = s->display;
        }
        return r;
    }
    r.name = std::format("unk_{:x}", va);
    r.display = std::format("{:#x}", va);
    r.kind = "unknown";
    if (program.image().is_readonly_data(va)) {
        if (auto str = program.image().read_cstring(va, 256); str && printable_string(*str)) {
            r.kind = "string";
            r.detail = escape_c_string(*str);
        }
    }
    if (const auto* sec = program.image().section_at(va); sec && r.detail.empty()) r.detail = "in " + sec->name;
    return r;
}

bool is_address_field(const Program& program, const x86::Instruction& ins, const x86::Field& field) {
    if (field.kind == x86::FieldKind::rel) return true;
    if (field.rip_relative) return true;
    if (field.size < 4) return false;
    const auto& image = program.image();
    if (image.has_relocations()) return image.is_relocated(ins.address + field.offset);
    return image.contains(field.absolute) && field.absolute >= image.image_base() + 0x1000;
}

Result<AnnotatedFunction> annotate_function(const Program& program, u64 start, bool include_callers, const TypeCatalog* header_types) {
    TRY_ASSIGN(auto ext, program.function_extent(start));
    TRY_ASSIGN(auto ins, program.function_instructions(ext));
    auto cfg = build_cfg(ins, ext.jump_tables);
    // Typed pointers name the fields and virtual methods the code reaches.
    const TypeView types(header_types, &program.pdb_types().catalog);
    const EntryTypes entry = entry_types(program, start, types);
    const auto typed = typed_operand_notes(program, ins, cfg, entry, types);

    AnnotatedFunction fn;
    fn.start = ext.start;
    fn.end = ext.end;
    fn.jump_tables = ext.jump_tables;
    if (const Symbol* s = program.symbols().at(start)) {
        fn.name = s->name;
        fn.display = s->display;
        fn.pdb_name = s->pdb_name;
    } else {
        fn.name = std::format("sub_{:x}", start);
        fn.display = fn.name;
    }
    fn.types = entry.description;
    fn.instruction_count = ins.size();
    fn.block_count = cfg.blocks.size();
    fn.loop_count = static_cast<usize>(std::ranges::count_if(cfg.blocks, &BasicBlock::loop_header));

    // Branch targets inside the function get labels.
    std::set<u64> labelled;
    for (const auto& x : ins)
        if (x.branch_target && ext.contains(*x.branch_target) && *x.branch_target != ext.start &&
            (x.flow == x86::Flow::jump || x.flow == x86::Flow::cond_jump || x.flow == x86::Flow::call))  // a call: a __finally block
            labelled.insert(*x.branch_target);
    for (const auto& t : ext.jump_tables)
        for (u64 target : t.targets)
            if (target) labelled.insert(target);
    // So do the places whose address the code holds (where a catch block resumes).
    for (const auto& x : ins)
        for (const auto& f : x.fields)
            if (f.kind != x86::FieldKind::rel && ext.contains(f.absolute) && f.absolute != ext.start && is_address_field(program, x, f) &&
                std::ranges::none_of(ext.jump_tables, [&](const JumpTable& t) { return t.table_va == f.absolute || t.index_va == f.absolute; }))
                labelled.insert(f.absolute);
    auto label_for = [](u64 va) { return std::format("loc_{:x}", va); };

    // Code only exceptions reach: labelled, with what it is.
    std::map<u64, std::vector<std::string>> eh_notes;
    const FunctionEh eh = program.arch() == Arch::x86 ? function_eh(program.image(), program.decoder(), ins) : function_eh_x64(program.image(), start);
    auto where = [&](u64 va) { return ext.contains(va) ? label_for(va) : describe_reference(program, va).display; };
    auto mark = [&](u64 va, std::string note) {
        if (!ext.contains(va)) return;
        labelled.insert(va);
        eh_notes[va].push_back(std::move(note));
    };
    if (eh.cxx) {
        for (const auto& h : eh.cxx->handlers) mark(h.code, std::format("{} (try block {})", catch_clause(program.image(), h), h.try_block));
        for (u64 a : eh.cxx->unwind_actions) mark(a, "unwind code: destroys objects while an exception passes");
        std::vector<std::string> clauses;
        for (const auto& h : eh.cxx->handlers)
            clauses.push_back(std::format("{} at {}", catch_clause(program.image(), h), where(h.code)));
        std::string stub;
        if (eh.stub) stub = std::format(" (handler stub {})", program.symbols().at(eh.stub) ? where(eh.stub) : "__ehhandler$" + fn.name);
        fn.exception_handling.push_back(std::format("C++ exception handling{}: {} try block{}{}{}", stub,
                                                    eh.cxx->try_blocks, eh.cxx->try_blocks == 1 ? "" : "s", clauses.empty() ? "" : "; ",
                                                    join(clauses, ", ")));
    }
    if (eh.seh) {
        for (usize i = 0; i < eh.seh->entries.size(); ++i) {
            const ScopeEntry& e = eh.seh->entries[i];
            const std::string within = e.enclosing >= 0 ? std::format(" in __try {}", e.enclosing) : "";
            if (e.finally) {
                mark(e.handler, std::format("__finally block (__try {})", i));
                fn.exception_handling.push_back(std::format("__try {}{}: __finally at {}", i, within, where(e.handler)));
            } else {
                if (e.filter) mark(e.filter, std::format("__except filter (__try {})", i));
                mark(e.handler, std::format("__except block (__try {})", i));
                fn.exception_handling.push_back(std::format("__try {}{}: __except at {}, filter {}", i, within, where(e.handler),
                                                            e.filter ? "at " + where(e.filter) : std::string("EXCEPTION_EXECUTE_HANDLER")));
            }
        }
    }

    std::map<u64, Reference> callees, data_refs;
    const auto rva_fields = image_relative_fields(program.image(), ins);
    const bool x64 = program.arch() == Arch::x64;
    const i64 slot = x64 ? 8 : 4;
    // The stack pointer relative to its value at entry (return address at [sp_entry]), and ebp/rbp once set up.
    const StackPoints stack = stack_points(ins, x64);

    auto frame_name = [&](i64 offset_from_entry) -> std::string {
        if (offset_from_entry >= slot) return std::format("arg_{:x}", offset_from_entry - slot);
        if (offset_from_entry < 0) return std::format("var_{:x}", -offset_from_entry);
        return "return_address";
    };

    for (usize i = 0; i < ins.size(); ++i) {
        const auto& x = ins[i];
        AnnotatedLine line;
        line.address = x.address;
        line.block = cfg.block_of[i];
        line.bytes = hex_bytes(x.bytes.data(), x.length);
        if (labelled.contains(x.address)) line.label = label_for(x.address);

        std::vector<std::string> notes;
        auto renderer = [&](const x86::Instruction& in, const x86::Field& f) -> std::optional<std::string> {
            const auto field_no = static_cast<usize>(&f - in.fields.data());
            if (f.kind != x86::FieldKind::rel && rva_fields.contains({in.address, field_no})) {
                // Image-base-relative (MSVC x64 `[r8+rcx*4+rva]`): name what the RVA points at.
                const u64 va = program.image().image_base() + static_cast<u64>(f.raw);
                if (std::ranges::any_of(ext.jump_tables, [&](const JumpTable& t) { return t.table_va == va; }))
                    return std::format("switch_table_{:x}", va);
                auto ref = describe_reference(program, va);
                data_refs.emplace(va, ref);
                if (!ref.detail.empty() && ref.kind != "function") notes.push_back(ref.detail);
                return std::format("imagerel {}", ref.display);
            }
            if (f.kind == x86::FieldKind::rel) {
                if (ext.contains(f.absolute)) return label_for(f.absolute);
                auto ref = describe_reference(program, f.absolute);
                if (in.flow == x86::Flow::call || in.flow == x86::Flow::jump || in.flow == x86::Flow::cond_jump) {
                    if (auto dest = program.thunk_destination(f.absolute))
                        if (const Symbol* slot = program.symbols().at(*dest); slot && slot->kind == SymbolKind::import) ref.import_call = "thunk";
                    callees.emplace(f.absolute, ref);
                    if (in.flow != x86::Flow::call) notes.push_back("tail call");
                }
                return ref.display;
            }
            if (!is_address_field(program, in, f)) return std::nullopt;
            if (ext.contains(f.absolute) && !std::ranges::any_of(ext.jump_tables, [&](const JumpTable& t) { return t.table_va == f.absolute; }))
                return label_for(f.absolute);
            auto ref = describe_reference(program, f.absolute);
            // The exception-handling registration, by MSVC's names for it, unless a symbol is there.
            const Symbol* at = program.symbols().at(f.absolute);
            if (!at && eh.cxx && f.absolute == eh.stub) return std::format("__ehhandler${}", fn.name);
            if (!at && eh.seh && f.absolute == eh.seh->va) return std::format("__sehtable${}", fn.name);
            bool is_table = std::ranges::any_of(ext.jump_tables, [&](const JumpTable& t) { return t.table_va == f.absolute; });
            if (is_table) {
                return std::format("switch_table_{:x}", f.absolute);
            }
            // An import's slot is read to call it (`call [__imp_X]`, or `mov esi, [__imp_X]` then `call esi`).
            if (ref.kind == "import") {
                ref.import_call = "dllimport";
                callees.emplace(f.absolute, ref);
            } else if (ref.kind == "function" && in.flow == x86::Flow::indirect_call) {
                callees.emplace(f.absolute, ref);
            } else {
                data_refs.emplace(f.absolute, ref);
            }
            if (!ref.detail.empty() && ref.kind != "function") notes.push_back(ref.detail);
            return ref.display;
        };
        line.text = x86::render(x, renderer);

        // Frame slots for esp/ebp-relative operands.
        const i64 sp = stack.sp[i];
        const std::optional<i64> frame = stack.frame[i];
        for (const auto& op : x.operands) {
            if (op.kind != x86::OperandKind::mem || !op.mem.index.empty()) continue;
            const auto& base = op.mem.base;
            if ((base == "esp" || base == "rsp")) notes.push_back(frame_name(sp + op.mem.disp));
            else if ((base == "ebp" || base == "rbp") && frame) notes.push_back(frame_name(*frame + op.mem.disp));
        }
        notes.insert(notes.end(), typed[i].begin(), typed[i].end());
        for (const auto& t : ext.jump_tables) {
            if (t.jump_va != x.address) continue;
            std::vector<std::string> targets;
            for (u64 target : t.targets) targets.push_back(target ? label_for(target) : "none");
            notes.push_back(std::format("switch: {} cases -> {}", t.targets.size(), join(targets, ", ")));
        }
        if (auto it = eh_notes.find(x.address); it != eh_notes.end()) notes.insert(notes.end(), it->second.begin(), it->second.end());
        const auto& block = cfg.blocks[line.block];
        if (block.first == i && block.loop_header) notes.push_back(std::format("loop header (depth {})", block.loop_depth));
        for (usize s : block.successors)
            if (block.last == i && s <= line.block && cfg.blocks[s].loop_header) notes.push_back("loop back-edge");

        std::ranges::sort(notes);
        notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
        line.comment = join(notes, "; ");
        fn.lines.push_back(std::move(line));
    }

    for (auto& [va, ref] : callees) fn.callees.push_back(ref);
    for (auto& [va, ref] : data_refs) fn.data_refs.push_back(ref);
    if (include_callers)
        for (u64 caller : program.callers_of(start)) fn.callers.push_back(program.describe_address(caller));
    fn.virtual_slots = program.rtti().describe_slots(start);
    return fn;
}

std::string to_text(const AnnotatedFunction& fn, bool with_bytes) {
    std::string out;
    out += std::format("; function: {}\n", fn.display.empty() ? fn.name : fn.display);
    out += std::format("; symbol:   {}{}\n", fn.name, fn.pdb_name.empty() || fn.pdb_name == fn.name ? "" : std::format("  (pdb: {})", fn.pdb_name));
    out += std::format("; range:    {:#x}-{:#x} ({} bytes, {} instructions, {} blocks, {} loops)\n", fn.start, fn.end,
                       fn.end - fn.start, fn.instruction_count, fn.block_count, fn.loop_count);
    if (!fn.callers.empty()) out += std::format("; callers:  {}\n", join(fn.callers, ", "));
    for (const auto& v : fn.virtual_slots) out += std::format("; virtual:  {}\n", v);
    for (const auto& e : fn.exception_handling) out += std::format("; eh:       {}\n", e);
    if (!fn.types.empty()) out += std::format("; types:    {}\n", join(fn.types, ", "));
    for (const auto& c : fn.callees)
        out += std::format("; calls:    {} = {}\n", c.display, c.detail.empty() ? c.name : c.detail);
    for (const auto& d : fn.data_refs)
        out += std::format("; data:     {} ({}){}\n", d.display, d.kind, d.detail.empty() ? "" : " " + d.detail);
    usize prev_block = static_cast<usize>(-1);
    for (const auto& l : fn.lines) {
        if (l.block != prev_block || !l.label.empty()) {
            if (!l.label.empty()) out += l.label + ":\n";
            prev_block = l.block;
        }
        std::string body = with_bytes ? std::format("  {:08x}  {:<24} {}", l.address, l.bytes, l.text)
                                      : std::format("  {:08x}  {}", l.address, l.text);
        if (!l.comment.empty()) {
            if (body.size() < 64) body.append(64 - body.size(), ' ');
            body += " ; " + l.comment;
        }
        out += body + "\n";
    }
    return out;
}

Json to_json(const AnnotatedFunction& fn) {
    Json j;
    j["start"] = fn.start;
    j["end"] = fn.end;
    j["name"] = fn.name;
    j["display"] = fn.display;
    j["pdb_name"] = fn.pdb_name;
    j["instructions"] = fn.instruction_count;
    j["blocks"] = fn.block_count;
    j["loops"] = fn.loop_count;
    j["callers"] = fn.callers;
    j["virtual_slots"] = fn.virtual_slots;
    j["exception_handling"] = fn.exception_handling;
    j["types"] = fn.types;
    auto refs = [](const std::vector<Reference>& list) {
        Json arr = Json::array();
        for (const auto& r : list)
            arr.push_back({{"va", r.va}, {"name", r.name}, {"display", r.display}, {"kind", r.kind}, {"detail", r.detail},
                           {"import_call", r.import_call}});
        return arr;
    };
    j["callees"] = refs(fn.callees);
    j["data"] = refs(fn.data_refs);
    Json lines = Json::array();
    for (const auto& l : fn.lines) {
        Json line{{"address", l.address}, {"bytes", l.bytes}, {"text", l.text}, {"block", l.block}};
        if (!l.label.empty()) line["label"] = l.label;
        if (!l.comment.empty()) line["comment"] = l.comment;
        lines.push_back(std::move(line));
    }
    j["lines"] = std::move(lines);
    return j;
}

} // namespace decomp
