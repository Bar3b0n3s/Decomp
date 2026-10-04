# Decomp

Decomp is a tool for AI-assisted *matching decompilation* of x86 and x86-64 binaries. It rewrites
functions from an existing executable as C++ that compiles **byte-for-byte identical** with the
binary's original compiler, using a built-in Claude agent that writes a candidate, compiles it with the
original toolchain, diffs the result against the binary and iterates until the bytes match. Matching
decompilation yields source that is mechanically proven equivalent to the original program, which is
what preservation, porting and research projects need. Doing it by hand is slow, and most of the time
goes into that same edit-compile-compare loop, which Decomp automates while a person supervises.

> **Status: early.** The first working slice is in progress. The repository currently contains the
> build scaffold, the third-party dependencies and the `core` module. The commands in the quickstart
> are the planned interface and are being implemented. See [docs/roadmap.md](docs/roadmap.md).

## Key ideas

- **Byte matching.** A function counts as matched only when the candidate's machine code equals the
  original byte for byte, with relocated fields resolved. Anything short of that is reported as a
  score plus the specific differences.
- **Relocation-aware diff.** The candidate is compiled on its own; the original was linked at fixed
  addresses. Decomp compares them as instructions whose address operands are symbolic: named symbols,
  string and float contents, branch targets as instruction indices. See [docs/matching.md](docs/matching.md).
- **Built-in agent.** Decomp calls the Claude API itself and owns the loop. There is no MCP server and
  no external agent framework. Each function gets one conversation with tools for compiling, diffing
  and reading the binary, plus budgets, retries, transcripts and cost accounting.
  See [docs/agent.md](docs/agent.md).
- **Supervision.** Every request, thinking summary, tool call, compile, diff, file write and dollar is
  recorded as an event. Live and past runs use the same views, and the user can pause, stop, steer,
  approve or take over at any time. The CLI has a live progress view; the desktop GUI arrives in Phase 1.
  See [docs/ui.md](docs/ui.md).
- **No external applications.** Decomp does not depend on disassembler suites, build systems or
  scripting runtimes. Its libraries are vendored and built from source. The one external program is
  the target's original compiler (later also its linker), which Decomp drives.

## Building

Decomp is written in C++23 and built with [premake5](https://github.com/premake/premake-core)
(5.0.0-beta8 or newer). Generated project files and intermediates go to `build/`, and binaries go to
`bin/<config>/`, where `<config>` is `Debug` or `Release`.

### Get the source

Zydis and raw_pdb are git submodules:

```sh
git clone --recursive https://github.com/Bar3b0n3s/Decomp.git
# or, in an existing clone:
git submodule update --init --recursive
```

### Windows (primary platform)

Requirements: Visual Studio 2022 **17.10 or newer** (MSVC 19.40+) with the C++ desktop workload, and
`premake5.exe` on `PATH`.

```bat
premake5 vs2022
```

Open `build\Decomp.sln`, select `Release|x64` and build. From a Developer Command Prompt you can build
without the IDE:

```bat
msbuild build\Decomp.sln /m /p:Configuration=Release /p:Platform=x64
```

The executables link the C runtime statically, so `bin\Release\decomp.exe` runs on its own.

### Linux

Requirements: GCC 14+ or Clang 19+, GNU make, the libcurl development package and premake5.

```sh
sudo apt install g++-14 libcurl4-openssl-dev      # Debian/Ubuntu package names
premake5 gmake
make -C build config=release_x64 CC=gcc-14 CXX=g++-14 -j"$(nproc)"
```

Use `config=debug_x64` for a debug build, or `CC=clang-19 CXX=clang++-19` for Clang. In premake
beta8 the generator is called `gmake`; earlier betas called it `gmake2`. If no premake binary is
available for your distribution, build it from source:

```sh
git clone --depth 1 --branch v5.0.0-beta8 https://github.com/premake/premake-core.git
make -C premake-core -f Bootstrap.mak linux       # produces premake-core/bin/release/premake5
```

### Running the tests

The unit and integration tests are a single [doctest](https://github.com/doctest/doctest) binary:

```sh
bin/Release/decomp_tests                          # Windows: bin\Release\decomp_tests.exe
bin/Release/decomp_tests -tc="*quoting*"          # run matching test cases only
```

Tests use committed fixtures under `tests/fixtures/` and need no compiler. Integration tests that need
a real toolchain (such as the clang-cl round trip) are skipped when that toolchain is not installed.

## Quickstart (planned CLI)

```sh
# Inspect any binary; no project needed
decomp info path/to/GAME.EXE

# Create a project in the current directory. This writes decomp.json and imports symbols
# (PDB, exports, imports) into symbols.txt. Then set "toolchain" and "flags" in decomp.json.
mkdir game && cd game
decomp init ../GAME.EXE

# Check the original compiler (registry format: docs/project-format.md)
decomp toolchain list
decomp toolchain test vc6

# Explore the target
decomp funcs                                      # functions with size and status
decomp disasm sum_array                           # annotated disassembly of one function

# Compile a candidate with the original toolchain and diff it against the target
decomp diff sum_array --source sum_array.cpp
decomp diff sum_array --obj sum_array.obj         # diff an object compiled elsewhere

# Let the agent match a function (needs ANTHROPIC_API_KEY, see below)
decomp agent sum_array --progress

# Overall progress: bytes and functions matched, status buckets, spend
decomp status
```

Commands find the project by searching upward from the current directory for `decomp.json`, or take
`--project <dir>`. Every command accepts `--json` for machine-readable output.

To see the agent loop without an API key, replay a recorded session:

```sh
decomp agent <func> --replay tests/replay/match_add.jsonl --progress
```

## The agent and your API key

`decomp agent` sends requests to the Claude API (`https://api.anthropic.com/v1/messages`) and reads the
key from the `ANTHROPIC_API_KEY` environment variable:

```sh
export ANTHROPIC_API_KEY=<your key>               # PowerShell: $env:ANTHROPIC_API_KEY = "<your key>"
```

The key stays in memory. It is never written to project files, transcripts, event logs or log files,
and the UI only shows whether a key is present. Every other command works without a key, and
`--replay` runs the agent offline. The default model is `claude-opus-5-5`, and budgets cap spend per
function and per run. What is sent, what it costs and how to change the defaults is described in
[docs/agent.md](docs/agent.md).

## Documentation

| Document | Contents |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Goals, pipeline, modules, data flow, threading, errors, build, platforms |
| [docs/matching.md](docs/matching.md) | What "matched" means, the relocation-aware diff, MSVC specifics, driving compilers |
| [docs/agent.md](docs/agent.md) | The built-in agent: API usage, tools, loop, budgets, refusals and fallbacks, cost |
| [docs/ui.md](docs/ui.md) | The supervision UI, view by view, and its event-driven architecture |
| [docs/project-format.md](docs/project-format.md) | Project files, symbol file, history, toolchain registry |
| [docs/roadmap.md](docs/roadmap.md) | First slice checklist, Phases 1-7 with exit criteria, risks |

## Repository layout

```
premake5.lua, premake/   build scripts (third-party projects in premake/deps.lua)
src/core/                errors (Result, TRY), logging, files, bytes, SHA-1, processes, JSON
src/formats/ arch/x86/ analysis/ matching/ events/ agent/ project/   (first slice, in progress)
src/cli/                 the decomp command
src/gui/                 decomp-gui (Phase 1)
tests/                   unit, integration, fixtures, replay
external/                third-party code
docs/                    design documentation
```

## Third-party code

Decomp builds on Zydis/Zycore, raw_pdb, LLVM's Demangle library, nlohmann/json, doctest and CLI11.
Phase 1 adds Dear ImGui, ImPlot, GLFW and ImGuiColorTextEdit. Versions, upstream locations and licenses
are listed in [external/README.md](external/README.md), and each library keeps its license file next to it.
