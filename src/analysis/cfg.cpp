#include "analysis/cfg.hpp"

#include <algorithm>
#include <map>

namespace decomp {

std::optional<usize> instruction_index(const std::vector<x86::Instruction>& instructions, u64 va) {
    auto it = std::ranges::lower_bound(instructions, va, {}, &x86::Instruction::address);
    if (it == instructions.end() || it->address != va) return std::nullopt;
    return static_cast<usize>(it - instructions.begin());
}

Cfg build_cfg(const std::vector<x86::Instruction>& ins, const std::vector<JumpTable>& jump_tables) {
    Cfg cfg;
    if (ins.empty()) return cfg;
    std::map<u64, const JumpTable*> tables;
    for (const auto& t : jump_tables) tables[t.jump_va] = &t;

    std::vector<bool> leader(ins.size(), false);
    leader[0] = true;
    for (usize i = 0; i < ins.size(); ++i) {
        const auto& x = ins[i];
        auto mark = [&](u64 target) {
            if (auto idx = instruction_index(ins, target)) leader[*idx] = true;
        };
        if (x.flow == x86::Flow::jump || x.flow == x86::Flow::cond_jump) {
            if (x.branch_target) mark(*x.branch_target);
        }
        if (auto t = tables.find(x.address); t != tables.end())
            for (u64 target : t->second->targets) mark(target);
        bool ends = x.flow == x86::Flow::jump || x.flow == x86::Flow::cond_jump || x.flow == x86::Flow::ret ||
                    x.flow == x86::Flow::indirect_jump || x.flow == x86::Flow::trap || x.flow == x86::Flow::halt;
        if (ends && i + 1 < ins.size()) leader[i + 1] = true;
        // Gaps in the address sequence (skipped data) also start a block.
        if (i + 1 < ins.size() && ins[i + 1].address != x.end()) leader[i + 1] = true;
    }

    cfg.block_of.assign(ins.size(), 0);
    for (usize i = 0; i < ins.size(); ++i) {
        if (leader[i]) cfg.blocks.push_back({i, i, {}, {}, false, 0});
        cfg.blocks.back().last = i;
        cfg.block_of[i] = cfg.blocks.size() - 1;
    }

    auto add_edge = [&](usize from, usize to) {
        auto& succ = cfg.blocks[from].successors;
        if (std::ranges::find(succ, to) == succ.end()) {
            succ.push_back(to);
            cfg.blocks[to].predecessors.push_back(from);
        }
    };
    for (usize b = 0; b < cfg.blocks.size(); ++b) {
        const auto& last = ins[cfg.blocks[b].last];
        auto target_block = [&](u64 va) -> std::optional<usize> {
            if (auto idx = instruction_index(ins, va)) return cfg.block_of[*idx];
            return std::nullopt;
        };
        bool falls_through = b + 1 < cfg.blocks.size() && ins[cfg.blocks[b + 1].first].address == last.end();
        switch (last.flow) {
        case x86::Flow::jump:
            if (last.branch_target)
                if (auto t = target_block(*last.branch_target)) add_edge(b, *t);
            break;
        case x86::Flow::cond_jump:
            if (last.branch_target)
                if (auto t = target_block(*last.branch_target)) add_edge(b, *t);
            if (falls_through) add_edge(b, b + 1);
            break;
        case x86::Flow::indirect_jump:
            if (auto t = tables.find(last.address); t != tables.end())
                for (u64 target : t->second->targets)
                    if (auto tb = target_block(target)) add_edge(b, *tb);
            break;
        case x86::Flow::ret:
        case x86::Flow::trap:
        case x86::Flow::halt: break;
        default:
            if (falls_through) add_edge(b, b + 1);
            break;
        }
    }

    // Back edges via DFS from the entry: an edge to a block on the current DFS stack closes a loop. The
    // DFS keeps its own stack, so huge functions cannot overflow a thread's stack (GUI jobs run this for
    // every function on pool threads).
    std::vector<int> state(cfg.blocks.size(), 0);  // 0 = new, 1 = on stack, 2 = done
    std::vector<std::pair<usize, usize>> back_edges;
    std::vector<std::pair<usize, usize>> stack;  // block, index of its next successor to visit
    state[0] = 1;
    stack.emplace_back(0, 0);
    while (!stack.empty()) {
        const usize b = stack.back().first;
        const auto& successors = cfg.blocks[b].successors;
        if (stack.back().second == successors.size()) {
            state[b] = 2;
            stack.pop_back();
            continue;
        }
        const usize s = successors[stack.back().second++];
        if (state[s] == 0) {
            state[s] = 1;
            stack.emplace_back(s, 0);
        } else if (state[s] == 1) {
            back_edges.emplace_back(b, s);
        }
    }
    for (auto [from, header] : back_edges) {
        cfg.blocks[header].loop_header = true;
        // Natural loop body: blocks that reach `from` without passing through `header`.
        std::vector<bool> in_loop(cfg.blocks.size(), false);
        in_loop[header] = true;
        std::vector<usize> work{from};
        while (!work.empty()) {
            usize x = work.back();
            work.pop_back();
            if (in_loop[x]) continue;
            in_loop[x] = true;
            for (usize p : cfg.blocks[x].predecessors) work.push_back(p);
        }
        for (usize i = 0; i < cfg.blocks.size(); ++i)
            if (in_loop[i]) ++cfg.blocks[i].loop_depth;
    }
    return cfg;
}

} // namespace decomp
