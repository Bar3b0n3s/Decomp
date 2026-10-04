#pragma once

#include "analysis/program.hpp"
#include "arch/x86/decoder.hpp"

#include <vector>

namespace decomp {

struct BasicBlock {
    usize first = 0;  // instruction indices, inclusive
    usize last = 0;
    std::vector<usize> successors;  // block indices
    std::vector<usize> predecessors;
    bool loop_header = false;  // target of a back edge
    int loop_depth = 0;
};

struct Cfg {
    std::vector<BasicBlock> blocks;
    std::vector<usize> block_of;  // instruction index -> block index

    usize block_at_instruction(usize index) const { return block_of[index]; }
};

// Builds the CFG of one function. Branch targets outside the function (tail calls) get no edge;
// jump tables provide the successors of their indirect jumps.
Cfg build_cfg(const std::vector<x86::Instruction>& instructions, const std::vector<JumpTable>& jump_tables = {});

// Index of the instruction starting at `va`, if any.
std::optional<usize> instruction_index(const std::vector<x86::Instruction>& instructions, u64 va);

} // namespace decomp
