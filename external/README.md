# Third-party code

| Directory | Upstream | Version | License |
|---|---|---|---|
| `zydis/` (submodule, includes `dependencies/zycore`) | https://github.com/zyantific/zydis | v4.1.1 (zycore v1.5.2) | MIT |
| `raw_pdb/` (submodule) | https://github.com/MolecularMatters/raw_pdb | 43cc59b | BSD-2-Clause |
| `llvm-demangle/` (vendored subset of `llvm/lib/Demangle` and `llvm/include/llvm/Demangle`) | https://github.com/llvm/llvm-project | llvmorg-23.1.2 | Apache-2.0 WITH LLVM-exception |
| `nlohmann/` (single header) | https://github.com/nlohmann/json | v3.12.0 | MIT |
| `doctest/` (single header) | https://github.com/doctest/doctest | v2.5.3 | MIT |
| `CLI11/` (headers) | https://github.com/CLIUtils/CLI11 | v2.7.2 | BSD-3-Clause |

Vendored copies are unmodified. The LLVM subset is the complete `Demangle` library (Microsoft,
Itanium, Rust and D demanglers); it has no dependency on the rest of LLVM. The only added file is
`llvm-demangle/include/llvm/Config/llvm-config.h`, a two-line stand-in for LLVM's CMake-generated header.

Zycore's OS-specific `src/API/*.c` files are not built: Zydis does not use them.
