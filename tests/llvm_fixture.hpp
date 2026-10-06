#pragma once

#include "formats/image.hpp"
#include "matching/match.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace decomp::test {

// Flags the fixture program is compiled with (see tests/fixtures/build_fixtures.sh).
const std::vector<std::string>& fixture_flags();

struct LlvmTools {
    std::string clang_cl, lld_link;
};
std::optional<LlvmTools> find_llvm();

// A clang-cl toolchain + flags matching the fixture build, for `arch`.
matching::MatchSetup clang_setup(Arch arch, const std::string& clang_cl, const std::filesystem::path& work,
                                 const std::optional<std::filesystem::path>& cache = {});

// Builds the fixture program (exe + PDB) into `dir` with the installed LLVM, so tests compare candidates
// against output of the same compiler version. Returns the exe path.
std::optional<std::filesystem::path> build_fixture_program(Arch arch, const LlvmTools& tools, const std::filesystem::path& dir);

// Compiles `sources` with the fixture flags (objects named after each source) and links them with the
// fixture kernel32.lib into <dir>/<name>.exe + .pdb (entry point `entry`). Returns the exe path.
std::optional<std::filesystem::path> build_program(Arch arch, const LlvmTools& tools, const std::filesystem::path& dir,
                                                   const std::vector<std::filesystem::path>& sources, const std::string& name,
                                                   const std::vector<std::string>& extra_link_flags = {},
                                                   const std::vector<std::string>& extra_compile_flags = {});

} // namespace decomp::test
