#pragma once

// Compiler identification by probing (docs/matching.md#identifying-the-compiler): the probes compiled by
// every candidate toolchain, each with a small flag search of its own (the "basic" groups: optimization
// level, frame pointers, security checks), and the toolchains ranked by how close their best comes to
// the target. For when the Rich header is missing (a non-Microsoft linker, a stripped image) or does not
// tell between the toolchains that could have built the program.

#include "analysis/program.hpp"
#include "core/json.hpp"
#include "matching/match.hpp"
#include "search/evaluate.hpp"
#include "search/flags.hpp"
#include "search/runs.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace decomp::search {

// The architecture a toolchain compiles for, when its flags (--target=, -m32, -m64), its compiler's name
// (i686-w64-mingw32-gcc, ...\Hostx64\x86\cl.exe) or its own name (msvc-x86) tell.
std::optional<Arch> toolchain_arch(const matching::Toolchain& toolchain);

struct IdentifyOptions {
    std::string preset = "basic";       // the flag groups each toolchain is searched with
    std::vector<std::string> start;     // flags given (the project's): kept for toolchains of their style
    usize max_per_toolchain = 64;       // configurations compiled per toolchain, at most
    int threads = 0;
    std::function<bool()> cancelled;
    CandidateLog* log = nullptr;        // every configuration evaluated, labeled with its toolchain
};

struct ToolchainRank {
    std::string toolchain;
    matching::ToolchainKind kind = matching::ToolchainKind::msvc;
    std::vector<std::string> flags;  // its best configuration's
    Score score;
    usize candidates = 0;
    std::string error;  // why it could not compile the probes
};

struct IdentifyResult {
    std::vector<ToolchainRank> ranking;  // best first; toolchains that compiled nothing last
    usize candidates = 0;
    bool cancelled = false;
    // Whether the first is ahead of the second (a tie identifies nothing).
    bool decided() const;
};

// Ranks `toolchains` (each searched with its own flag groups) by their best score on the probes.
IdentifyResult identify(const Program& program, const matching::MatchSetup& setup, std::span<const Probe> probes,
                        std::span<const matching::Toolchain> toolchains, const IdentifyOptions& options);

Json to_json(const IdentifyResult& result);

} // namespace decomp::search
