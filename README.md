# Decomp

Decomp is a tool for AI-assisted *matching decompilation* of x86 and x86-64 binaries. It rewrites
functions from an existing executable as C++ that compiles **byte-for-byte identical** with the
binary's original compiler, using a built-in Claude agent that writes a candidate, compiles it with the
original toolchain, diffs the result against the binary and iterates until the bytes match. Matching
decompilation yields source that is mechanically proven equivalent to the original program, which is
what preservation, porting and research projects need. Doing it by hand is slow, and most of the time
goes into that same edit-compile-compare loop, which Decomp automates while a person supervises.

> **Status: the first working slice and the Phase 1 runner and GUI are implemented.** The `decomp`
> command loads PE targets and their PDBs, annotates disassembly, compiles candidates with the original
> toolchain, diffs them with relocation awareness, and runs the built-in agent on one function
> (`decomp agent`) or on many with several workers (`decomp run`), with transcripts, event logs, a live
> progress view, budgets, approvals and resumable runs. `decomp-gui` opens projects and starts,
> watches, steers and reopens runs. CI is green on Linux and Windows, including a round trip with the
> real MSVC `cl.exe` for x86 and x64. See [docs/roadmap.md](docs/roadmap.md).

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
- **Supervision.** Every request, response, tool call, compile, diff, file write and dollar is recorded
  in the run's event log and the session's transcript. The CLI shows a live progress view, and the
  user can steer, pause, stop or abort (`--interactive`, Ctrl+C). The desktop GUI, `decomp-gui`, shows
  live and past runs in the same views, with approvals, budgets, notifications and manual take-over.
  See [docs/ui.md](docs/ui.md).
- **No external applications.** Decomp does not depend on disassembler suites, build systems or
  scripting runtimes. Its libraries are vendored and built from source. The one external program is
  the target's original compiler (later also its linker), which Decomp drives.

## Building

Decomp is written in C++23 and built with [premake5](https://github.com/premake/premake-core)
(5.0.0-beta8 or newer). Generated project files and intermediates go to `build/`, and binaries go to
`bin/<config>/`, where `<config>` is `Debug` or `Release`.

### Get the source

Zydis, raw_pdb and the GUI libraries (Dear ImGui, ImPlot, GLFW, ImGuiColorTextEdit) are git
submodules:

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

Use `premake5 vs2026` for Visual Studio 2026. Open `build\Decomp.sln`, select `Release|x64` and build.
From a Developer Command Prompt you can build without the IDE:

```bat
msbuild build\Decomp.sln /m /p:Configuration=Release /p:Platform=x64
```

The executables link the C runtime statically, so `bin\Release\decomp.exe` and
`bin\Release\decomp-gui.exe` run on their own.

### Linux

Requirements: GCC 14+ or Clang 19+, GNU make, the libcurl development package, the X11 development
headers (for `decomp-gui`, which uses X11; on Wayland desktops it runs through XWayland) and premake5.

```sh
sudo apt install g++-14 libcurl4-openssl-dev \
    libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev   # Debian/Ubuntu
premake5 gmake
make -C build config=release_x64 CC=gcc-14 CXX=g++-14 -j"$(nproc)"
```

Use `config=debug_x64` for a debug build, or `CC=clang-19 CXX=clang++-19` for Clang. Run
`premake5 gmake` again after adding source files. In premake beta8 the generator is called `gmake`;
earlier betas called it `gmake2`. If no premake binary is available for your distribution, build it
from source:

```sh
git clone --depth 1 --branch v5.0.0-beta8 https://github.com/premake/premake-core.git
make -C premake-core -f Bootstrap.mak linux       # produces premake-core/bin/release/premake5
```

### Running the tests

The unit and integration tests are a [doctest](https://github.com/doctest/doctest) binary, and the
GUI's tests (the shell and every view rendered headless, without a window or GPU) are another:

```sh
bin/Release/decomp_tests                          # Windows: bin\Release\decomp_tests.exe
bin/Release/decomp_tests -tc="*quoting*"          # run matching test cases only
bin/Release/decomp_gui_tests
```

Most tests use committed fixtures under `tests/fixtures/` and need no compiler. Tests that compile
real code (the clang-cl round trip and the agent tests that compile candidates) need clang-cl and
lld-link (LLVM 18 is what CI installs) and are skipped when they are not installed. On Windows,
`tests/integration/msvc_roundtrip.ps1 -Arch x86` (or `x64`), run from a Developer PowerShell after a
Release build, builds the fixture program with the real `cl.exe` and `link.exe` and checks that every
function diffs byte-exact.

## Quickstart

```sh
# Inspect any binary; no project needed
decomp info path/to/GAME.EXE
decomp funcs path/to/GAME.EXE

# Register the original compiler (registry format: docs/project-format.md).
# An installed clang-cl is detected automatically as clang-cl-x86 and clang-cl-x64.
decomp toolchain add vc6 --kind msvc --compiler 'C:\VS6\VC98\Bin\CL.EXE' \
    --env 'INCLUDE=C:\VS6\VC98\Include' --env-prepend 'PATH=C:\VS6\Common\MSDev98\Bin;C:\VS6\VC98\Bin'
decomp toolchain list
decomp toolchain test vc6

# Create a project in the current directory: decomp.json (target, toolchain, flags) and
# symbols.txt (symbols from the PDB, exports and imports)
mkdir game && cd game
decomp init ../path/to/GAME.EXE --toolchain vc6 --flag /O2 --flag /Gy

# Explore the target
decomp funcs                                      # functions with address, size and symbol source
decomp disasm sum_array                           # annotated disassembly of one function

# Compile a candidate with the original toolchain and diff it against the target
decomp diff sum_array --source sum_array.cpp      # exit code 0 = byte-exact, 2 = differs, 3 = compile failed
decomp diff sum_array --obj sum_array.obj         # diff an object compiled elsewhere

# Let the agent match a function (needs ANTHROPIC_API_KEY, see below)
decomp agent sum_array

# ...or many: every function not matched yet, 4 at a time, at most $20 in all
decomp run --all --workers 4 --run-budget-usd 20
decomp runs list                                  # the project's runs; `decomp run --resume <id>` continues one

# Overall progress: functions and code bytes matched, status buckets, spend
decomp status

# The supervision GUI: open the project, start a run, watch and steer it
decomp-gui --project .
```

Commands find the project by searching upward from the current directory for `decomp.json`. Global
options may come before or after the command name: `-C <dir>` (`--project <dir>`) starts the search in another
directory, `--json` makes most commands print JSON (`decomp --json status`), and `-v` or `-q` change
the log level. `decomp <command> --help` lists every option.

To see the agent loop without an API key, replay a scripted session. The model's side is scripted,
but the compiles are real, so this needs clang-cl. From the repository root:

```sh
decomp init tests/fixtures/x86/basic.exe --dir /tmp/basic --toolchain clang-cl-x86 \
    --flag /O2 --flag /Gy --flag /GS- --flag /GR- --flag /EHs-c-
decomp -C /tmp/basic agent add --replay tests/replay/agent_match_add.jsonl
decomp -C /tmp/basic status
```

The session ends `matched`, the verified source is in `/tmp/basic/src/functions/add_401060.cpp`, and
the run's event log, transcript and summary are under `/tmp/basic/.decomp/runs/`.

A whole scripted batch, in the CLI or the GUI (scripts for every fixture function: 8 match, the rest
give up):

```sh
decomp -C /tmp/basic run --all --workers 4 --replay-dir tests/replay/run
decomp-gui --project /tmp/basic --replay-dir tests/replay/run --run-all
```

## The agent and your API key

`decomp agent` sends requests to the Claude API (`https://api.anthropic.com/v1/messages`, or the base
URL in `ANTHROPIC_BASE_URL`) and reads the key from the `ANTHROPIC_API_KEY` environment variable:

```sh
export ANTHROPIC_API_KEY=<your key>               # PowerShell: $env:ANTHROPIC_API_KEY = "<your key>"
```

Without a key, `decomp agent` and `decomp run` stop with an error, and `decomp-gui` refuses to start a
run (its Settings view shows whether a key is present and can check it, but never shows the key). The
key stays in memory: it is only sent in the request header, and it is never written to project files,
transcripts, event logs, GUI settings or log files. Every other command works without a key, and
`--replay` (`--replay-dir` for runs) runs the agent offline. The default model is
`claude-opus-5-5`; per-function budgets cap turns, spend, tokens and time, and Ctrl+C stops a session
after the current turn (twice: aborts). What is sent, what it costs and how to change the defaults is
described in [docs/agent.md](docs/agent.md).

## Documentation

| Document | Contents |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Goals, pipeline, modules, data flow, threading, errors, build, platforms |
| [docs/matching.md](docs/matching.md) | What "matched" means, the relocation-aware diff, MSVC specifics, driving compilers |
| [docs/agent.md](docs/agent.md) | The built-in agent: API usage, tools, loop, budgets, refusals and fallbacks, cost |
| [docs/ui.md](docs/ui.md) | The supervision GUI, view by view, its event-driven architecture, and CLI parity |
| [docs/project-format.md](docs/project-format.md) | Project files, symbol file, history, toolchain registry |
| [docs/roadmap.md](docs/roadmap.md) | First slice checklist, Phases 1-7 with exit criteria, risks |

## Repository layout

```
premake5.lua, premake/   build scripts (third-party projects in premake/deps.lua)
src/core/                errors (Result, TRY), logging, files, bytes, SHA-1, processes, JSON
src/formats/             PE, COFF and PDB readers
src/arch/x86/            x86 and x64 decoding over Zydis
src/analysis/            symbols, bounds, CFG, annotation, demangling
src/matching/            toolchains, compile driver and cache, the diff
src/events/              events, event bus, RunState and snapshots, progress view
src/agent/               the built-in agent: transports, client, conversation, tools, loop, rate gate, approvals
src/project/             decomp.json, symbols.txt, history, locks, change logs
src/run/                 batch runs: selection, work queue, run directories, the run controller
src/viewmodel/           what the GUI's views show, computed without ImGui
src/cli/                 the decomp command
src/gui/                 decomp-gui: the shell, the views, and (platform/) GLFW and OpenGL
tests/                   unit, integration, fixtures, replay
external/                third-party code
docs/                    design documentation
```

## Third-party code

Decomp builds on Zydis/Zycore, raw_pdb, LLVM's Demangle library, nlohmann/json, doctest and CLI11;
`decomp-gui` adds Dear ImGui, ImPlot, GLFW, ImGuiColorTextEdit, stb_image_write and two fonts.
Versions, upstream locations and licenses are listed in [external/README.md](external/README.md), and
each library keeps its license file next to it.
