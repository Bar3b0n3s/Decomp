#include "viewmodel/inspector.hpp"

#include "analysis/cfg.hpp"
#include "core/fs.hpp"
#include "events/events.hpp"

#include <algorithm>
#include <set>

namespace decomp::vm {

Result<Listing> build_listing(const Program& program, u64 va) {
    TRY_ASSIGN(auto annotated, annotate_function(program, va, false));
    TRY_ASSIGN(auto extent, program.function_extent(va));
    TRY_ASSIGN(auto instructions, program.function_instructions(extent));
    const Cfg cfg = build_cfg(instructions, extent.jump_tables);

    Listing out;
    std::vector<AnnotatedLine> lines = std::move(annotated.lines);
    out.function = std::move(annotated);
    out.function.lines.clear();
    out.lines.reserve(lines.size());
    // annotate_function() decodes the same extent, so its lines and the instructions correspond one to
    // one; the address check guards against that ever changing.
    const bool aligned = lines.size() == instructions.size();
    usize prev_block = static_cast<usize>(-1);
    for (usize i = 0; i < lines.size(); ++i) {
        AnnotatedLine& a = lines[i];
        ListingLine l;
        l.address = a.address;
        l.label = std::move(a.label);
        l.bytes = std::move(a.bytes);
        l.comment = std::move(a.comment);
        l.block = a.block;
        l.block_start = i == 0 || a.block != prev_block;
        prev_block = a.block;
        if (a.block < cfg.blocks.size()) {
            const BasicBlock& b = cfg.blocks[a.block];
            l.loop_header = b.loop_header && l.block_start;
            l.loop_depth = b.loop_depth;
            out.max_loop_depth = std::max(out.max_loop_depth, b.loop_depth);
        }
        const x86::Instruction* ins = aligned && instructions[i].address == a.address ? &instructions[i] : nullptr;
        if (ins) {
            l.mnemonic = ins->prefix + ins->mnemonic;
            l.flow = ins->flow;
            if (ins->branch_target) {
                l.target = ins->branch_target;
            } else if (ins->memory_target && program.image().contains(*ins->memory_target)) {
                l.target = ins->memory_target;
            } else {
                for (const auto& f : ins->fields)
                    if (f.kind == x86::FieldKind::imm && is_address_field(program, *ins, f)) {
                        l.target = f.absolute;
                        break;
                    }
            }
            if (l.target) l.inside = extent.contains(*l.target);
        }
        // The rendered text is the mnemonic, a space and the operands.
        if (!l.mnemonic.empty() && a.text.starts_with(l.mnemonic)) {
            l.operands = a.text.size() > l.mnemonic.size() ? a.text.substr(l.mnemonic.size() + 1) : std::string();
        } else {
            const usize space = a.text.find(' ');
            l.mnemonic = a.text.substr(0, space);
            l.operands = space == std::string::npos ? std::string() : a.text.substr(space + 1);
        }
        out.lines.push_back(std::move(l));
    }
    return out;
}

namespace {

std::string function_name(const Program& program, u64 function, u64 fallback) {
    if (function)
        if (const Symbol* s = program.symbols().at(function)) return s->display.empty() ? s->name : s->display;
    return program.describe_address(function ? function : fallback);
}

bool is_function_at(const Program& program, u64 va) {
    const Symbol* s = program.symbols().at(va);
    return s && s->kind == SymbolKind::function;
}

bool is_branch(XrefKind kind) { return kind == XrefKind::call || kind == XrefKind::jump; }

} // namespace

FunctionXrefs function_xrefs(const Program& program, u64 va) {
    FunctionXrefs out;
    std::set<std::pair<u64, u64>> seen;  // (at, target)
    auto add_caller = [&](const Xref& x, u64 target) {
        if (!seen.insert({x.from, target}).second) return;
        XrefRow row;
        row.at = x.from;
        row.function = x.function;
        row.target = target;
        row.kind = x.kind;
        row.name = function_name(program, x.function, x.from);
        row.is_function = x.function != 0;
        out.callers.push_back(std::move(row));
    };
    for (const Xref& x : program.xrefs_to(va)) {
        if (x.kind == XrefKind::pointer) {
            XrefRow row;
            row.at = x.from;
            row.target = x.via ? x.via : va;
            row.kind = x.kind;
            row.name = program.describe_address(x.from);
            out.pointers.push_back(std::move(row));
            continue;
        }
        if (!is_branch(x.kind)) continue;
        // Through an incremental-linking thunk: the row shows the thunk as what was called.
        add_caller(x, x.via ? x.via : va);
    }
    for (const Xref& x : program.xrefs_from(va)) {
        XrefRow row;
        row.at = x.from;
        row.function = va;
        row.target = x.to;
        row.kind = x.kind;
        if (is_branch(x.kind)) {
            const u64 destination = program.thunk_destination(x.to).value_or(x.to);
            row.is_function = is_function_at(program, destination);
            row.name = row.is_function ? function_name(program, destination, destination) : program.describe_address(x.to);
            if (destination != x.to && row.is_function) row.name += " (through a thunk)";
            if (row.is_function) row.target = destination;
            out.callees.push_back(std::move(row));
        } else {
            row.is_function = is_function_at(program, x.to);
            row.name = row.is_function ? function_name(program, x.to, x.to) : program.describe_address(x.to);
            out.data.push_back(std::move(row));
        }
    }
    auto by_address = [](const XrefRow& a, const XrefRow& b) { return a.at != b.at ? a.at < b.at : a.target < b.target; };
    std::ranges::sort(out.callers, by_address);
    std::ranges::sort(out.callees, by_address);
    std::ranges::sort(out.data, by_address);
    std::ranges::sort(out.pointers, by_address);
    return out;
}

AttemptSeries attempt_series(const std::vector<AttemptRecord>& attempts) {
    AttemptSeries s;
    double best = 0;
    for (usize i = 0; i < attempts.size(); ++i) {
        const double score = attempts[i].compiled ? attempts[i].match_percent : 0.0;
        best = std::max(best, score);
        s.score.push(static_cast<double>(i + 1), score);
        s.best.push(static_cast<double>(i + 1), best);
    }
    return s;
}

void add_status_changes(StatusHistory& history, std::string_view text, std::string_view run_id) {
    while (!text.empty()) {
        const usize nl = text.find('\n');
        const std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (line.find("\"status_changed\"") == std::string_view::npos) continue;
        auto json = parse_json(line);
        if (!json) continue;
        auto event = events::event_from_json(*json);
        if (!event) continue;
        const auto* p = std::get_if<events::StatusChanged>(&event->payload);
        if (!p) continue;
        StatusChangeRecord r;
        r.time = event->time;
        r.run = event->run.empty() ? std::string(run_id) : event->run;
        r.function = p->function;
        r.old_status = p->old_status;
        r.status = p->status;
        r.best = p->best;
        history[p->va].push_back(std::move(r));
    }
}

StatusHistory load_status_history(const std::filesystem::path& runs_dir, const std::function<bool()>& cancelled) {
    StatusHistory history;
    std::error_code ec;
    if (!std::filesystem::is_directory(runs_dir, ec)) return history;
    for (const auto& entry : std::filesystem::directory_iterator(runs_dir, ec)) {
        if (cancelled && cancelled()) break;
        if (!entry.is_directory(ec)) continue;
        auto text = fs::read_text(entry.path() / "events.jsonl");
        if (!text) continue;
        add_status_changes(history, *text, fs::to_utf8(entry.path().filename()));
    }
    for (auto& [va, list] : history)
        std::ranges::stable_sort(list, [](const StatusChangeRecord& a, const StatusChangeRecord& b) { return a.time < b.time; });
    return history;
}

} // namespace decomp::vm
