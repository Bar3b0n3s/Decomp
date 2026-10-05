#include "analysis/discovery.hpp"

#include "analysis/jump_tables.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace decomp {

std::string_view to_string(FunctionEvidence evidence) {
    switch (evidence) {
    case FunctionEvidence::symbol: return "symbol";
    case FunctionEvidence::entry: return "entry";
    case FunctionEvidence::export_table: return "export";
    case FunctionEvidence::unwind: return "unwind";
    case FunctionEvidence::call: return "call";
    case FunctionEvidence::tail_jump: return "tail_jump";
    case FunctionEvidence::address: return "address";
    case FunctionEvidence::gap: return "gap";
    }
    return "gap";
}

namespace {

// Imported functions that never return to their caller.
bool noreturn_import(std::string_view name) {
    static const std::set<std::string_view> names = {
        "ExitProcess", "ExitThread", "FatalExit", "FatalAppExitA", "FatalAppExitW", "FreeLibraryAndExitThread", "RaiseFailFastException",
        "RtlExitUserThread", "RtlExitUserProcess", "exit", "_exit", "_Exit", "quick_exit", "abort", "_amsg_exit", "_invoke_watson",
        "_invalid_parameter_noinfo_noreturn", "_CxxThrowException", "longjmp", "_longjmp", "__std_terminate", "?terminate@@YAXXZ",
        "terminate", "__fastfail", "_cexit_noreturn"};
    return names.contains(name);
}

// Instructions compiled code does not contain: data decoded as code tends to produce them.
bool suspicious(const x86::Instruction& ins) {
    static const std::set<std::string_view> mnemonics = {
        "in",    "out",  "insb", "insw", "insd", "outsb", "outsw", "outsd", "iret", "iretd", "iretq", "cli",  "sti",  "arpl",
        "bound", "les",  "lds",  "aaa",  "aas",  "daa",   "das",   "aam",   "aad",  "into",  "salc",  "retf", "lgdt", "lidt",
        "sgdt",  "sidt", "lldt", "sldt", "ltr",  "str",   "lmsw",  "smsw",  "clts", "invd",  "wbinvd", "rdmsr", "wrmsr", "icebp",
        "hlt",   "int1", "loopne", "loope", "jcxz", "jecxz"};
    if (mnemonics.contains(ins.mnemonic)) return true;
    for (const auto& op : ins.operands)
        if (op.kind == x86::OperandKind::pointer) return true;  // far call or jump
    if (ins.mnemonic == "int" && !ins.operands.empty() && ins.operands[0].imm != 0x29 && ins.operands[0].imm != 0x2C &&
        ins.operands[0].imm != 0x2E && ins.operands[0].imm != 0x2D)
        return true;
    return false;
}

// What discovery needs of an instruction, decoded once.
struct Insn {
    u64 target = 0;    // branch target, or the memory operand's absolute address
    u64 code_ref = 0;  // a code address held in an operand (`push offset f`, `lea rcx, [rip+f]`)
    u8 length = 0;
    x86::Flow flow = x86::Flow::none;
    bool has_branch = false;
    bool has_memory = false;
    bool suspicious = false;
};

struct Body {
    u64 end = 0;
    bool returns = false;       // a ret is reached
    bool unknown_exit = false;  // an indirect jump that is not a switch: a tail call through a pointer
    bool invalid = false;       // undecodable or implausible code
    bool falls_off = false;     // execution runs into the next function's start
    u64 last = 0;               // the instruction that fell off
    bool last_is_call = false;
    std::vector<u64> calls;         // direct call targets
    std::vector<u64> tail_targets;  // direct jumps out of the window
    std::vector<u64> jump_targets;  // direct jumps within the window
    std::vector<u64> code_refs;     // code addresses held in operands
    std::vector<u64> instructions;  // starts
    std::vector<std::pair<u64, u64>> data;  // switch tables in the window
    bool unbounded_table = false;           // a table read without a bound (may shrink as more tables are known)
    u64 import_slot = 0;                    // the function is `jmp [slot]` through the IAT (an import thunk)
};

struct Fn {
    FunctionEvidence evidence = FunctionEvidence::call;
    u64 fixed_end = 0;  // a known size: the end does not come from tracing
    Body body;
    bool traced = false;
    bool noreturn = false;
};

struct CodeRange {
    u64 begin = 0, end = 0;  // the file-backed bytes of an executable section
};

class Discoverer {
public:
    Discoverer(const pe::Image& image, const x86::Decoder& decoder, const SymbolDb& known, const DiscoveryOptions& options)
        : image_(image), decoder_(decoder), known_(known), options_(options) {
        for (const auto& s : image.image_sections())
            if (s.executable && s.file_size > 0) ranges_.push_back({s.va, s.va + s.file_size});
        for (const auto& imp : image.imports()) {
            imported_slots_.insert(imp.iat_va);
            if (noreturn_import(imp.name)) noreturn_slots_.insert(imp.iat_va);
        }
    }

    DiscoveryResult run() {
        seed();
        DiscoveryResult result;
        for (usize pass = 0; pass < 10000; ++pass) {
            result.passes = pass + 1;
            retrace();
            bool changed = new_table_starts_;
            changed |= add_call_targets();
            changed |= add_tail_targets();
            changed |= update_noreturn();
            changed |= drop_false_starts();
            if (!changed) changed = split_tail_jumps();
            if (!changed && options_.gaps) changed = add_gap_functions();
            if (!changed && options_.address_taken) changed = add_address_taken();
            if (!changed) break;
        }
        for (const auto& [start, fn] : fns_) {
            DiscoveredFunction f;
            f.start = start;
            f.end = end_of(start, fn);
            f.evidence = fn.evidence;
            f.noreturn = fn.noreturn;
            f.import_slot = fn.body.import_slot;
            result.functions.push_back(f);
        }
        result.instructions = cache_.size();
        return result;
    }

private:
    const CodeRange* range_of(u64 va) const {
        for (const auto& r : ranges_)
            if (va >= r.begin && va < r.end) return &r;
        return nullptr;
    }

    const Insn* insn(u64 va) {
        if (auto it = cache_.find(va); it != cache_.end()) return it->second.length ? &it->second : nullptr;
        Insn out;
        if (const CodeRange* r = range_of(va)) {
            auto bytes = image_.view(va, static_cast<usize>(std::min<u64>(15, r->end - va)));
            if (bytes)
                if (auto ins = decoder_.decode(*bytes, va)) {
                    out.length = ins->length;
                    out.flow = ins->flow;
                    out.suspicious = suspicious(*ins);
                    if (ins->branch_target) {
                        out.has_branch = true;
                        out.target = *ins->branch_target;
                    } else if (ins->memory_target) {
                        out.has_memory = true;
                        out.target = *ins->memory_target;
                    }
                    out.code_ref = code_reference(*ins);
                }
        }
        auto [it, inserted] = cache_.emplace(va, out);
        return it->second.length ? &it->second : nullptr;
    }

    // A code address an instruction holds as a value (not one it reads from): `push offset f`,
    // `mov [x], offset f`, `lea rcx, [rip+f]`. In an image with relocations only relocated fields count.
    u64 code_reference(const x86::Instruction& ins) const {
        for (const auto& f : ins.fields) {
            if (f.kind == x86::FieldKind::rel || f.size < 4) continue;
            u64 value = 0;
            if (f.kind == x86::FieldKind::imm) {
                value = image_.arch() == Arch::x86 ? static_cast<u64>(static_cast<u32>(f.raw)) : static_cast<u64>(f.raw);
                if (image_.has_relocations() && !image_.is_relocated(ins.address + f.offset)) continue;
            } else if (f.rip_relative && ins.mnemonic == "lea") {
                value = f.absolute;
            } else {
                continue;
            }
            if (range_of(value) && value != ins.address) return value;
        }
        return 0;
    }

    bool is_start(u64 va) const { return fns_.contains(va); }

    u64 end_of(u64 start, const Fn& fn) const { return fn.fixed_end ? fn.fixed_end : std::max(fn.body.end, start + 1); }

    // The end of the bytes a function at `start` may occupy: the next start, or its section's end.
    u64 window_end(u64 start) const {
        const CodeRange* r = range_of(start);
        u64 limit = r ? r->end : start + 1;
        if (auto next = fns_.upper_bound(start); next != fns_.end()) limit = std::min(limit, next->first);
        return limit;
    }

    // Marks the function whose window ends at `va` (its predecessor) for retracing.
    void touch_predecessor(u64 va) {
        auto it = fns_.lower_bound(va);
        if (it == fns_.begin()) return;
        dirty_.insert(std::prev(it)->first);
    }

    void add(u64 va, FunctionEvidence evidence, u64 fixed_end = 0, std::string_view why = {}) {
        if (!range_of(va)) return;
        auto [it, inserted] = fns_.try_emplace(va);
        if (inserted || evidence < it->second.evidence) it->second.evidence = evidence;
        if (fixed_end) it->second.fixed_end = fixed_end;
        if (inserted) {
            dirty_.insert(va);
            touch_predecessor(va);
            log::trace("discovery: function at {:#x} ({}{}{})", va, to_string(evidence), why.empty() ? "" : ": ", why);
        }
    }

    void remove(u64 va, std::string_view why) {
        if (fns_.erase(va) == 0) return;
        noreturn_.erase(va);
        rejected_.insert(va);
        dirty_.erase(va);
        touch_predecessor(va);
        log::trace("discovery: {:#x} is not a function start ({})", va, why);
    }

    void seed() {
        const u64 entry = image_.entry_point();
        for (const auto& [va, s] : known_) {
            if (s.kind != SymbolKind::function || !range_of(va)) continue;
            FunctionEvidence evidence = FunctionEvidence::symbol;
            if (va == entry) evidence = FunctionEvidence::entry;
            else if (s.source == SymbolSource::export_table) evidence = FunctionEvidence::export_table;
            else if (s.source == SymbolSource::analysis && s.size) evidence = FunctionEvidence::unwind;
            add(va, evidence, s.size ? va + s.size : 0);
        }
        if (entry) add(entry, FunctionEvidence::entry);
    }

    // The instructions that ran before `va` along the fall-through chain, oldest first (for switch tables).
    std::vector<x86::Instruction> history(u64 va, const std::unordered_map<u64, u64>& prev) const {
        std::vector<x86::Instruction> out;
        u64 at = va;
        for (int n = 0; n < 16; ++n) {
            auto it = prev.find(at);
            if (it == prev.end()) break;
            at = it->second;
            auto ins = decode_full(at);
            if (!ins) break;
            out.push_back(std::move(*ins));
        }
        std::ranges::reverse(out);
        return out;
    }

    std::optional<x86::Instruction> decode_full(u64 va) const {
        const CodeRange* r = range_of(va);
        if (!r) return std::nullopt;
        auto bytes = image_.view(va, static_cast<usize>(std::min<u64>(15, r->end - va)));
        if (!bytes) return std::nullopt;
        return decoder_.decode(*bytes, va);
    }

    Body trace(u64 start, u64 limit) {
        Body body;
        body.end = start;
        std::vector<u64> work{start};
        std::unordered_set<u64> seen;
        std::unordered_map<u64, u64> prev;  // instruction -> the one that fell through into it
        auto in_window = [&](u64 va) { return va >= start && va < limit; };
        while (!work.empty()) {
            u64 a = work.back();
            work.pop_back();
            u64 from = 0;
            while (true) {
                if (!in_window(a)) {
                    if (from) {
                        body.falls_off = true;
                        body.last = from;
                        const Insn* f = insn(from);
                        body.last_is_call = f && (f->flow == x86::Flow::call || f->flow == x86::Flow::indirect_call);
                    }
                    break;
                }
                if (seen.contains(a)) break;
                const Insn* i = insn(a);
                if (!i || a + i->length > limit) {
                    body.invalid = true;
                    break;
                }
                seen.insert(a);
                if (from) prev[a] = from;
                if (i->suspicious) body.invalid = true;
                body.end = std::max(body.end, a + i->length);
                if (i->code_ref) body.code_refs.push_back(i->code_ref);
                bool stop = false;
                switch (i->flow) {
                case x86::Flow::call:
                    if (i->has_branch) {
                        body.calls.push_back(i->target);
                        if (noreturn_.contains(i->target)) stop = ends_after_noreturn_call(a + i->length, start, limit);
                    }
                    break;
                case x86::Flow::indirect_call:
                    if (i->has_memory && noreturn_slots_.contains(i->target)) stop = ends_after_noreturn_call(a + i->length, start, limit);
                    break;
                case x86::Flow::cond_jump:
                    if (i->has_branch) {
                        if (in_window(i->target)) {
                            work.push_back(i->target);
                            body.jump_targets.push_back(i->target);
                        } else {
                            body.tail_targets.push_back(i->target);
                        }
                    }
                    break;
                case x86::Flow::jump:
                    if (i->has_branch) {
                        if (in_window(i->target) && i->target != start) {
                            work.push_back(i->target);
                            body.jump_targets.push_back(i->target);
                        } else if (i->target != start) {
                            body.tail_targets.push_back(i->target);
                        }
                    }
                    stop = true;
                    break;
                case x86::Flow::indirect_jump: {
                    stop = true;
                    if (i->has_memory && imported_slots_.contains(i->target)) {
                        if (!noreturn_slots_.contains(i->target)) body.unknown_exit = true;  // a tail call into a DLL
                        if (a == start) body.import_slot = i->target;
                        break;
                    }
                    std::optional<JumpTable> table;
                    if (auto jmp = decode_full(a)) {
                        const JumpTableContext ctx{
                            .image = &image_, .symbols = nullptr, .fn_start = start, .fn_limit = limit, .table_starts = &table_starts_};
                        table = read_jump_table(ctx, history(a, prev), *jmp);
                    }
                    if (!table) {
                        body.unknown_exit = true;
                        break;
                    }
                    if (!table->bounded) body.unbounded_table = true;
                    if (table_starts_.insert(table->table_va).second) new_table_starts_ = true;
                    if (table->index_va && table_starts_.insert(table->index_va).second) new_table_starts_ = true;
                    for (u64 t : table->targets)
                        if (in_window(t) && image_.read<u8>(t).value_or(0xCC) != 0xCC) work.push_back(t);  // int3: a case that cannot happen
                    for (const auto& range : table->data_ranges())
                        if (in_window(range.first)) {
                            body.data.push_back(range);
                            body.end = std::max(body.end, std::min(range.second, limit));
                        }
                    break;
                }
                case x86::Flow::ret:
                    body.returns = true;
                    stop = true;
                    break;
                case x86::Flow::trap:
                case x86::Flow::halt: stop = true; break;
                case x86::Flow::none: break;
                }
                if (stop) break;
                from = a;
                a += i->length;
            }
        }
        body.instructions.assign(seen.begin(), seen.end());
        std::ranges::sort(body.instructions);
        return body;
    }

    // After a call that cannot return, the function ends unless the compiler emitted code after it anyway
    // (it does when the callee is not declared noreturn): padding, another function or the window's end
    // follow when it did not.
    bool ends_after_noreturn_call(u64 next, u64 start, u64 limit) const {
        if (next < start || next >= limit || is_start(next)) return true;
        const u8 b = image_.read<u8>(next).value_or(0xCC);
        return b == 0xCC || b == 0x90 || b == 0x00;
    }

    void retrace() {
        // Tables read without a bound stop at the start of the next known table: read them again when
        // more tables are known.
        if (new_table_starts_) {
            new_table_starts_ = false;
            for (const auto& [start, fn] : fns_)
                if (fn.body.unbounded_table) dirty_.insert(start);
        }
        std::set<u64> dirty;
        dirty.swap(dirty_);
        for (u64 start : dirty) {
            auto it = fns_.find(start);
            if (it == fns_.end()) continue;
            Fn& fn = it->second;
            const u64 limit = fn.fixed_end ? std::min(fn.fixed_end, window_end(start)) : window_end(start);
            fn.body = trace(start, std::max(limit, start + 1));
            fn.traced = true;
        }
    }

    // Direct call targets are functions.
    bool add_call_targets() {
        std::vector<std::pair<u64, u64>> found;
        for (const auto& [start, fn] : fns_)
            for (u64 t : fn.body.calls)
                if (!is_start(t) && range_of(t) && !rejected_.contains(t)) found.emplace_back(t, start);
        for (auto [t, from] : found) add(t, FunctionEvidence::call, 0, std::format("called from {:#x}", from));
        return !found.empty();
    }

    // A jump out of a function to code that no function covers starts one (a tail call).
    bool add_tail_targets() {
        std::vector<std::pair<u64, u64>> found;
        for (const auto& [start, fn] : fns_)
            for (u64 t : fn.body.tail_targets)
                if (!is_start(t) && range_of(t) && !covered(t) && !rejected_.contains(t)) found.emplace_back(t, start);
        for (auto [t, from] : found) add(t, FunctionEvidence::tail_jump, 0, std::format("jumped to from {:#x}", from));
        return !found.empty();
    }

    // Inside some function's traced extent.
    bool covered(u64 va) const {
        auto it = fns_.upper_bound(va);
        if (it == fns_.begin()) return false;
        --it;
        return va >= it->first && va < end_of(it->first, it->second);
    }

    bool update_noreturn() {
        // Functions that may not return, then the greatest fixpoint over tail calls.
        std::unordered_map<u64, bool> nr;
        for (const auto& [start, fn] : fns_) {
            const Body& b = fn.body;
            bool candidate = !b.returns && !b.unknown_exit && !(b.falls_off && !b.last_is_call) && !b.invalid;
            for (u64 t : b.tail_targets)
                if (!is_start(t)) candidate = false;
            nr[start] = candidate;
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto& [start, fn] : fns_) {
                if (!nr[start]) continue;
                for (u64 t : fn.body.tail_targets)
                    if (!nr[t]) {
                        nr[start] = false;
                        changed = true;
                        break;
                    }
            }
        }
        // A function that falls into the next one ends with a call that cannot return (unless the callee
        // is seen to return).
        for (const auto& [start, fn] : fns_)
            if (fn.body.falls_off && fn.body.last_is_call)
                if (const Insn* i = insn(fn.body.last); i && i->flow == x86::Flow::call && i->has_branch && is_start(i->target) &&
                                                       !fns_.at(i->target).body.returns)
                    nr[i->target] = true;
        std::vector<u64> flipped;
        for (auto& [start, fn] : fns_) {
            const bool now = nr[start];
            if (now == fn.noreturn) continue;
            fn.noreturn = now;
            flipped.push_back(start);
            if (now) noreturn_.insert(start);
            else noreturn_.erase(start);
        }
        if (flipped.empty()) return false;
        // Their callers (and tail callers) trace differently now.
        const std::set<u64> changed(flipped.begin(), flipped.end());
        for (const auto& [start, fn] : fns_) {
            bool affected = false;
            for (u64 c : fn.body.calls) affected = affected || changed.contains(c);
            for (u64 c : fn.body.tail_targets) affected = affected || changed.contains(c);
            if (affected) dirty_.insert(start);
        }
        return true;
    }

    // A weakly evidenced start that the previous function runs into (or jumps past) is inside it.
    bool drop_false_starts() {
        std::vector<std::pair<u64, std::string>> drop;
        for (auto it = fns_.begin(); it != fns_.end(); ++it) {
            auto next = std::next(it);
            if (next == fns_.end()) break;
            const Fn& f = it->second;
            const u64 s = next->first;
            const Fn& g = next->second;
            if (g.evidence < FunctionEvidence::tail_jump || !f.traced) continue;
            if (f.body.falls_off && !f.body.last_is_call) {
                drop.emplace_back(s, std::format("{:#x} runs into it", it->first));
                continue;
            }
            for (u64 t : f.body.tail_targets)
                if (t > s && t < end_of(s, g)) {
                    drop.emplace_back(s, std::format("{:#x} jumps past it to {:#x}", it->first, t));
                    break;
                }
        }
        for (const auto& [s, why] : drop) remove(s, why);
        return !drop.empty();
    }

    // Padding (bytes no flow reaches) right before a jump target: the target starts another function.
    bool split_tail_jumps() {
        std::vector<std::pair<u64, u64>> found;
        for (const auto& [start, fn] : fns_) {
            if (fn.fixed_end) continue;
            const auto& ins = fn.body.instructions;
            for (u64 t : fn.body.jump_targets) {
                if (t <= start || is_start(t) || rejected_.contains(t)) continue;
                // The bytes before t are not instructions of the function: only padding reaches up to it.
                auto it = std::ranges::lower_bound(ins, t);
                if (it == ins.begin()) continue;
                const u64 prev = *std::prev(it);
                const Insn* p = insn(prev);
                if (!p || prev + p->length >= t) continue;  // contiguous with earlier code: the flow can reach t
                // Only int3 fill separates functions; compilers align code inside a function with nops.
                bool int3_only = true;
                for (u64 b = prev + p->length; b < t && int3_only; ++b) int3_only = image_.read<u8>(b).value_or(0) == 0xCC;
                if (!int3_only) continue;
                if (!x86::ends_block(p->flow) && !(p->flow == x86::Flow::call && p->has_branch && noreturn_.contains(p->target))) continue;
                found.emplace_back(t, start);
            }
        }
        for (auto [t, from] : found) add(t, FunctionEvidence::tail_jump, 0, std::format("past padding in {:#x}", from));
        return !found.empty();
    }

    // Traces a weakly evidenced start; nothing when its code does not look like a function.
    std::optional<Body> plausible(u64 va) {
        if (rejected_.contains(va) || is_start(va) || covered(va) || !range_of(va)) return std::nullopt;
        Body b = trace(va, window_end(va));
        const bool ok = !b.invalid && !(b.falls_off && !b.last_is_call) && !b.instructions.empty() &&
                        (b.returns || !b.tail_targets.empty() || b.unknown_exit || !b.calls.empty() || b.instructions.size() >= 2 ||
                         b.import_slot);
        if (!ok) {
            rejected_.insert(va);
            return std::nullopt;
        }
        return b;
    }

    // The code left between functions, after padding: each piece that traces cleanly is a function. Runs
    // of such functions in one gap are taken in one pass.
    bool add_gap_functions() {
        bool added = false;
        for (const auto& r : ranges_) {
            std::vector<std::pair<u64, u64>> gaps;
            u64 at = r.begin;
            for (auto it = fns_.lower_bound(r.begin); it != fns_.end() && it->first < r.end; ++it) {
                if (at < it->first) gaps.emplace_back(at, it->first);
                at = std::max(at, end_of(it->first, it->second));
            }
            if (at < r.end) gaps.emplace_back(at, r.end);
            for (auto [begin, end] : gaps) added |= scan_gap(begin, end);
        }
        return added;
    }

    bool scan_gap(u64 at, u64 next) {
        bool added = false;
        u64 p = at + padding_length(image_, decoder_, at, next);
        while (p < next) {
            if (auto body = plausible(p)) {
                // Something holds its address: say so, though the gap found it first.
                add(p, referenced(p) ? FunctionEvidence::address : FunctionEvidence::gap);
                Fn& fn = fns_.at(p);
                fn.body = std::move(*body);
                fn.traced = true;
                dirty_.erase(p);
                added = true;
                const u64 e = std::max(fn.body.end, p + 1);
                p = e + padding_length(image_, decoder_, e, next);
                continue;
            }
            // Resume after the next padding byte.
            u64 q = p + 1;
            while (q < next) {
                const u8 before = image_.read<u8>(q - 1).value_or(0);
                if (before == 0xCC || before == 0x90) break;
                ++q;
            }
            p = q + padding_length(image_, decoder_, q, next);
        }
        return added;
    }

    // A code address held in data, a relocation or an instruction of a traced function.
    bool referenced(u64 va) {
        if (!options_.address_taken) return false;
        if (!candidates_built_) {
            build_address_candidates();
            data_refs_ = candidates_;
            candidates_built_ = true;
        }
        if (data_refs_.contains(va)) return true;
        for (const auto& [start, fn] : fns_)
            if (std::ranges::find(fn.body.code_refs, va) != fn.body.code_refs.end()) return true;
        return false;
    }

    bool add_address_taken() {
        if (!candidates_built_) {
            build_address_candidates();
            data_refs_ = candidates_;
        }
        candidates_built_ = true;
        // Code addresses held in the operands of traced code.
        for (const auto& [start, fn] : fns_)
            for (u64 r : fn.body.code_refs) candidates_.insert(r);
        std::vector<u64> found;
        for (u64 c : candidates_)
            if (plausible(c)) found.push_back(c);
        candidates_.clear();
        for (u64 c : found)
            if (!covered(c)) add(c, FunctionEvidence::address);
        return !found.empty();
    }

    void build_address_candidates() {
        const unsigned ptr = pointer_size(image_.arch());
        auto read_pointer = [&](u64 va) -> std::optional<u64> {
            if (ptr == 8) return image_.read<u64>(va);
            return image_.read<u32>(va).transform([](u32 v) { return u64{v}; });
        };
        if (image_.has_relocations()) {
            for (const auto& r : image_.base_relocations())
                if (auto value = read_pointer(image_.image_base() + r.rva); value && range_of(*value)) candidates_.insert(*value);
            return;
        }
        // No relocations: aligned pointer-sized values in the data sections that point into code.
        for (const auto& s : image_.image_sections()) {
            if (s.executable || s.file_size == 0) continue;
            for (u64 va = s.va; va + ptr <= s.va + s.file_size; va += ptr)
                if (auto value = read_pointer(va); value && range_of(*value)) candidates_.insert(*value);
        }
    }

    const pe::Image& image_;
    const x86::Decoder& decoder_;
    const SymbolDb& known_;
    DiscoveryOptions options_;
    std::vector<CodeRange> ranges_;
    std::map<u64, Fn> fns_;
    std::set<u64> dirty_;
    std::unordered_map<u64, Insn> cache_;
    std::set<u64> noreturn_;
    std::set<u64> imported_slots_;
    std::set<u64> noreturn_slots_;
    std::set<u64> rejected_;
    std::set<u64> candidates_;
    std::set<u64> data_refs_;  // code addresses in data and relocations
    bool candidates_built_ = false;
    std::set<u64> table_starts_;  // every switch table (and byte table) found so far
    bool new_table_starts_ = false;
};

} // namespace

DiscoveryResult discover_functions(const pe::Image& image, const x86::Decoder& decoder, const SymbolDb& known, const DiscoveryOptions& options) {
    return Discoverer(image, decoder, known, options).run();
}

u64 padding_length(const BinaryImage& image, const x86::Decoder& decoder, u64 va, u64 limit) {
    u64 at = va;
    while (at < limit) {
        auto b = image.read<u8>(at);
        if (!b) break;
        if (*b == 0xCC || *b == 0x90) {
            ++at;
            continue;
        }
        if (*b == 0x00) {
            // Zero fill: a run of at least four zeros, or one that reaches the limit (the end of a section).
            // Shorter runs are the last bytes of instructions such as `ret 4`.
            u64 z = at;
            while (z < limit && image.read<u8>(z).value_or(1) == 0) ++z;
            if (z - at < 4 && z < limit) break;
            at = z;
            continue;
        }
        // Filler instructions: lea r, [r+0] (8D 49 00, 8D A4 24 00000000, 8D 9B 00000000, 8D 64 24 00 ...),
        // multi-byte nops (0F 1F /0, 66 90, 66 0F 1F ...) and xchg ax, ax.
        auto bytes = image.view(at, static_cast<usize>(std::min<u64>(15, limit - at)));
        if (!bytes) break;
        auto ins = decoder.decode(*bytes, at);
        if (!ins || at + ins->length > limit) break;
        bool filler = ins->mnemonic == "nop";
        if (ins->mnemonic == "lea" && ins->operands.size() == 2 && ins->operands[0].kind == x86::OperandKind::reg &&
            ins->operands[1].kind == x86::OperandKind::mem) {
            const auto& m = ins->operands[1].mem;
            filler = m.disp == 0 && m.index.empty() && x86::gpr_family(m.base) == x86::gpr_family(ins->operands[0].reg) &&
                     ins->operands[0].reg == m.base;
        }
        if (ins->mnemonic == "xchg" && ins->operands.size() == 2 && ins->operands[0].kind == x86::OperandKind::reg &&
            ins->operands[1].kind == x86::OperandKind::reg && ins->operands[0].reg == ins->operands[1].reg)
            filler = true;
        if (!filler) break;
        at += ins->length;
    }
    return at - va;
}

} // namespace decomp
