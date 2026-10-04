#include "viewmodel/difficulty.hpp"

#include "analysis/cfg.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

namespace decomp::vm {

namespace {

// Whether the agent sees a name for a call target.
bool named(const Program& program, u64 va) {
    const Symbol* s = program.symbols().at(va);
    if (!s) return false;
    if (s->kind == SymbolKind::import) return true;
    return !(s->source == SymbolSource::analysis && s->name.starts_with("sub_"));
}

// `edges` receives the destination of every direct call and leaving jump (thunks followed).
FunctionFeatures features_of(const Program& program, const FunctionExtent& ext, const std::vector<x86::Instruction>& ins,
                             std::vector<u64>* edges) {
    FunctionFeatures f;
    f.va = ext.start;
    f.bytes = static_cast<u32>(std::min<u64>(ext.size(), 0xFFFFFFFFu));
    f.instructions = static_cast<u32>(ins.size());
    f.jump_tables = static_cast<u32>(ext.jump_tables.size());
    const Cfg cfg = build_cfg(ins, ext.jump_tables);
    f.blocks = static_cast<u32>(cfg.blocks.size());
    for (const auto& b : cfg.blocks) {
        if (b.loop_header) ++f.loops;
        f.max_loop_depth = std::max(f.max_loop_depth, static_cast<u32>(std::max(b.loop_depth, 0)));
    }
    std::set<u64> known, unknown;
    u32 unknown_sites = 0;
    for (const auto& x : ins) {
        if (x.flow == x86::Flow::call || x.flow == x86::Flow::indirect_call) ++f.calls;
        const bool leaves = x.branch_target && !ext.contains(*x.branch_target) &&
                            (x.flow == x86::Flow::call || x.flow == x86::Flow::jump || x.flow == x86::Flow::cond_jump);
        if (leaves) {
            u64 target = *x.branch_target;
            if (auto dest = program.thunk_destination(target)) target = *dest;
            (named(program, target) ? known : unknown).insert(target);
            if (edges) edges->push_back(target);
        } else if (x.flow == x86::Flow::indirect_call) {
            const Symbol* slot = x.memory_target ? program.symbols().at(*x.memory_target) : nullptr;
            if (slot && slot->kind == SymbolKind::import) known.insert(*x.memory_target);
            else if (x.memory_target) unknown.insert(*x.memory_target);
            else ++unknown_sites;
        }
    }
    f.callees = static_cast<u32>(known.size());
    f.unknown_callees = static_cast<u32>(unknown.size()) + unknown_sites;
    return f;
}

} // namespace

Result<FunctionFeatures> function_features(const Program& program, u64 va) {
    TRY_ASSIGN(auto ext, program.function_extent(va));
    TRY_ASSIGN(auto ins, program.function_instructions(ext));
    return features_of(program, ext, ins, nullptr);
}

const FunctionFeatures* FunctionAnalysis::find(u64 va) const {
    auto it = std::ranges::lower_bound(functions, va, {}, &FunctionFeatures::va);
    return it != functions.end() && it->va == va ? &*it : nullptr;
}

FunctionAnalysis analyze_functions(const Program& program, std::span<const u64> vas, const std::function<bool()>& cancelled,
                                   const std::function<void(usize, usize)>& progress) {
    FunctionAnalysis out;
    std::vector<u64> order(vas.begin(), vas.end());
    std::ranges::sort(order);
    order.erase(std::unique(order.begin(), order.end()), order.end());
    out.functions.reserve(order.size());
    std::unordered_map<u64, std::vector<u64>> callers;  // callee -> calling functions
    std::vector<u64> edges;
    for (usize i = 0; i < order.size(); ++i) {
        if (i % 256 == 0) {
            if (cancelled && cancelled()) {
                out.cancelled = true;
                break;
            }
            if (progress) progress(i, order.size());
        }
        const u64 va = order[i];
        auto ext = program.function_extent(va);
        if (!ext) {
            out.failed.push_back(va);
            continue;
        }
        auto ins = program.function_instructions(*ext);
        if (!ins) {
            out.failed.push_back(va);
            continue;
        }
        edges.clear();
        out.functions.push_back(features_of(program, *ext, *ins, &edges));
        for (u64 target : edges)
            if (target != va) callers[target].push_back(va);
    }
    for (auto& f : out.functions) {
        auto it = callers.find(f.va);
        if (it == callers.end()) continue;
        auto& list = it->second;
        std::ranges::sort(list);
        f.callers = static_cast<u32>(std::unique(list.begin(), list.end()) - list.begin());
    }
    if (progress && !out.cancelled) progress(order.size(), order.size());
    return out;
}

double difficulty(const FunctionFeatures& f) {
    return std::log2(1.0 + f.bytes) + 0.5 * std::log2(1.0 + f.blocks) + std::min<u32>(f.loops, 8) + 0.5 * std::min<u32>(f.max_loop_depth, 4) +
           0.25 * std::min<u32>(f.callees, 20) + std::min<u32>(f.unknown_callees, 10) + 0.5 * std::min<u32>(f.jump_tables, 4);
}

std::string_view difficulty_label(double score) {
    if (score < 8) return "easy";
    if (score < 14) return "medium";
    if (score < 20) return "hard";
    return "very hard";
}

} // namespace decomp::vm
