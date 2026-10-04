# Roadmap

Decomp is built in increments that each end in something usable and tested. The **first working
slice** goes from an empty repository to a tool that can load a PE target, disassemble and annotate a
function, compile a candidate with the original toolchain, diff it with relocation awareness, and run
the built-in agent loop end to end (tested offline through replays), with an event backbone and a
CLI progress view. **Phases 1-7** then add the supervision GUI and a batch runner, deeper analysis,
project organization, types, whole-program verification through relinking, search helpers, and more
target formats. Every phase below has a goal, a scope and explicit exit criteria. The document ends
with the main risks and how they are mitigated.

## First working slice

Each step builds, passes its tests, and lands before the next step starts. Checked boxes reflect the
repository at the time of writing; tick the others as they land.

- [x] **1. Scaffold.** `premake5.lua`, `premake/deps.lua`, the submodules (Zydis, raw_pdb), the
  vendored libraries, `.gitignore`, `.gitmodules`, an empty `decomp` main and the doctest runner.
  Builds with g++-14 through `premake5 gmake`.
- [x] **2. core.** Error and `Result`, logging, file system helpers, `ByteReader`, SHA-1, processes,
  JSON helpers. Tests cover process exit codes, output capture and timeouts, and Windows argument
  quoting as a pure function on all platforms. The Windows process implementation is written; it is
  verified once CI exists (step 13).
- [ ] **3. Fixtures.**
  - `tests/fixtures/src/basic.cpp` covers arithmetic, a loop, a switch with a real jump table (the
    cases need different side effects, or the compiler emits a lookup table instead), globals, a
    string literal, a float constant, calls and a `thiscall` method.
  - `build_fixtures.sh` builds it with clang-cl and lld-link: `/O2 /Gy /GS- /GR- /Z7 /Brepro`, linked
    with `/nodefaultlib /entry:entry /subsystem:console /debug`. The source defines
    `extern "C" int _fltused = 0;`.
  - Committed for x86 and x64: `basic.exe`, `basic.pdb`, and precompiled candidate objects (exact
    copies and deliberate mutations), so unit tests need no compiler.
- [ ] **4. formats.** PE, COFF and PDB readers. Tests compare against expected values that were
  captured once with llvm-readobj and llvm-pdbutil when the fixtures were built, and stored as JSON.
- [ ] **5. arch/x86.** The Zydis decoder and the symbolizing formatter. Tests decode known byte
  sequences for x86 and x64 and check text, flow and fields.
- [ ] **6. analysis.** Demangling, `SymbolDb` import (PDB, exports, imports), bounds, CFG and the
  annotator. CLI: `info`, `funcs`, `disasm`.
- [ ] **7. matching (diff).** `ObjFunction`, `TargetFunction`, `Normalizer`, `Differ`, scoring and
  reports. Tests: exact candidates are `exact` and `byte_exact`; each mutation (operand, opcode,
  reorder, encoding-only, wrong symbol) produces the expected row kinds and hints. CLI: `diff --obj`.
- [ ] **8. matching (toolchains).** Registry, compile driver and cache, diagnostics parsers. CLI:
  `diff --source`, `toolchain list` and `toolchain test`. An integration round trip compiles the
  fixture source with clang-cl, matches every function and requires `byte_exact`. It is skipped when
  the toolchain is missing.
- [ ] **9. project.** `decomp init <binary>` (writes `decomp.json`, imports symbols into
  `symbols.txt`); commands resolve the project from the current directory or `--project`; function
  status and history; `decomp status` (progress by bytes and functions, status buckets, spend from run
  summaries).
- [ ] **10. Events backbone.** Event types, `EventBus`, the `RunState` reducer, the JSONL event log,
  and the CLI live `--progress` renderer (ANSI, or plain lines when not a TTY). Tests: the reducer
  folds synthetic event sequences into the expected state, and log, then replay, gives an identical
  state.
- [ ] **11. agent.**
  - Transports and the SSE parser, tested on recorded streams with thinking, signatures, partial tool
    JSON and error events.
  - Client retries, tested on replayed 429 and 529 responses.
  - The append-only `Conversation`. A test asserts that every request extends the previous one
    byte-for-byte and that system and tools never change.
  - Tool registry and validator, match tools, the loop (which emits events), the single-session
    `RunController`, transcripts.
  - CLI: `agent <func> [--replay <file>] [--progress]`. Verified sources and history are written to
    the project.
  - Scripted replays:
    - wrong source, diff, corrected source, `submit_result`, then `matched`;
    - refusal ends as `refused`;
    - budget exhaustion;
    - supervisor guidance injected mid-run keeps the history append-only.
- [x] **12. Docs.** `README.md`, `docs/architecture.md`, `docs/matching.md`, `docs/agent.md`,
  `docs/ui.md`, `docs/project-format.md` and `docs/roadmap.md` (this set).
- [ ] **13. CI.** `.github/workflows/ci.yml` on ubuntu (g++-14) and windows-latest (VS 2022,
  `premake5 vs2022`, msbuild) builds and runs the unit tests. Windows also runs the round-trip test
  with the real `cl.exe`. Iterate until green.

**Slice exit criteria**

- On Linux, the build with g++-14 succeeds and `bin/Release/decomp_tests` passes, including the
  clang-cl integration test.
- CLI smoke tests pass:
  - `decomp info tests/fixtures/x86/basic.exe` works;
  - `decomp disasm ... sum_array` works;
  - `decomp diff ... --source <exact.cpp>` reports 100% and `byte_exact`, and a mutated source
    produces classified rows;
  - `decomp agent ... --replay tests/replay/match_add.jsonl --progress` shows the live view, ends
    `matched` and writes the source to the project, and the run's `events.jsonl` replays into the same
    `RunState`.
- CI is green on ubuntu and windows, including the Windows round trip with the real `cl.exe`.
- Live (by the user, on Windows, with `ANTHROPIC_API_KEY`): `decomp agent <func>` on a fixture
  function matches within budget, and the transcript shows `cache_read_input_tokens > 0` from the
  second turn on.

## Phases after the slice

### Phase 1: Supervision GUI and batch runner

**Goal:** run many functions in parallel under full supervision.

**Scope**

- A multi-worker `RunController`:
  - a work queue and N workers;
  - a shared rate limiter fed by the API's rate-limit headers;
  - a staggered start, so later workers read the shared prompt prefix from the cache;
  - live concurrency and budget changes;
  - an approvals queue with per-action policies.
- Resumable runs. After a restart, the remaining queue continues. Whether interrupted sessions resume
  from their last committed turn or restart with their history is open.
- `decomp-gui`, with the top bar, the status bar, the notification center, and every view in
  [ui.md](ui.md) except the later-phase ones: Dashboard, Run monitor, Agent session, Diff viewer,
  Function browser and inspector, Binary explorer, Symbols and provenance, Changes and approvals, Cost
  and usage, Toolchains and compiles, Logs and errors, Settings.
- Dear ImGui (docking), ImPlot, GLFW and ImGuiColorTextEdit vendored and built by premake. A headless
  GUI smoke test runs in CI.

**Exit criteria**

- On Windows and on Linux, a user can do all of the following:
  - open a project;
  - run 20 or more functions on 4 workers;
  - watch live sessions and steer one of them;
  - pause and stop;
  - review diffs and changes;
  - reopen the finished run from its log.
- Reducer, replay and headless GUI tests pass in CI on both platforms.

### Phase 2: Analysis depth

**Goal:** accurate function bounds, names and context for targets without a PDB.

**Scope**

- MSVC `.map` import.
- x64 `.pdata` bounds.
- A compiler table that maps Rich header entries to compiler versions and toolchain suggestions.
- A cross-reference index.
- MSVC in-`.text` jump tables, including two-level tables.
- Resolution of incremental-linking and import thunks.
- MSVC RTTI and vtables mapped to class names.
- COFF `.lib` reading, plus CRT and library signature matching that marks functions as `library`.

**Exit criteria**

- At least 95% of function bounds (start and end both exact) are correct on a real PDB-less VC6
  executable, measured against ground truth such as the build's map file or a hand-checked list.
- For a set of reference binaries, the Rich header maps to the correct compiler version.

### Phase 3: Project organization

**Goal:** organize work and sources the way the original program was organized.

**Scope**

- A translation-unit model, seeded from PDB section contributions or, without a PDB, from heuristics
  (address ranges, data locality, call graph).
- One source per unit, replacing one file per function.
- A queue ordered by estimated difficulty.
- The units view.
- The agent tools `set_symbol` and `define_type`, under the approval policy and with provenance.
- A cost report per unit.

**Exit criteria**

- On a target with a PDB, the derived units equal the PDB's module list, and every function is
  assigned to its unit.
- Matched functions are emitted into per-unit sources, and every function in a unit still verifies
  `byte_exact` from the unit source.
- `set_symbol` and `define_type` work end to end, with approvals, provenance and revert.
- The queue orders by difficulty, and `decomp status` (including `--json`) reports progress and cost
  per unit.

### Phase 4: Types

**Goal:** real types instead of per-function ad hoc declarations.

**Scope**

- Headers in `include/` become the source of truth for types.
- Headers are compiled with the original compiler and `/Zi`, and the exact layouts are read back from
  the resulting PDB (TPI stream).
- Field names in annotations: `[ecx+0Ch]` becomes `this->health`.
- The `get_type` tool.
- RTTI-derived class skeletons.
- The types view.

**Exit criteria**

- For every type in the fixtures and in one real target with a PDB, the layout compiled from the
  project headers (size, field offsets, vtable layout) equals the layout in the target's PDB.
- Annotated listings show field names for known types.
- `get_type` returns exact layouts.
- Class skeletons generated from RTTI compile with the original toolchain.

### Phase 5: Units, data and full relink

**Goal:** verify the whole program, not just individual functions.

**Scope**

- Per-unit matching: function order, `.data`/`.rdata`/`.bss` contents and placement, string pools and
  float pools.
- Exception-handling and unwind tables, and the aliases created by identical-COMDAT folding.
- A COFF writer that produces split objects carrying the code and data not yet matched.
- Relinking with the original linker, with a SHA-1 comparison against the original.
- The relink status view.

**Exit criteria**

- The x86 and x64 fixture executables, rebuilt from matched sources with their original linker, are
  byte-identical to the originals (equal SHA-1).
- A partially matched real target relinks byte-identically, with unmatched code and data carried by
  split objects.
- When a relink is not identical, the relink view reports the first differing bytes and the unit
  responsible.

### Phase 6: Search helpers

**Goal:** mechanical search for the last few percent and for unknown build settings.

**Scope**

- Compiler-flag search.
- A token-level source permuter.
- Compiler identification by probing candidate compilers, for when the Rich header is missing or
  ambiguous.
- The permuter and flag-search view.

**Exit criteria**

- Given a matched function's source, flag search recovers the fixture's flags from a candidate set.
- The permuter turns deliberately perturbed but equivalent fixture sources (statement order,
  declaration order) into byte-exact matches.
- Compiler identification ranks the correct toolchain first for fixtures built by at least three
  different toolchains.

### Phase 7: More targets

**Goal:** go beyond Windows PE and x86.

**Scope**

- End-to-end support for ELF64 built by GCC or Clang: an ELF `BinaryImage`, symbols from `.symtab`,
  bounds from `.eh_frame`, ELF relocations in the diff, and the `gcc`/`clang` toolchain kinds with
  their diagnostics.
- Other ISAs through additional `arch::Decoder` implementations.

**Exit criteria**

- ELF64 fixtures built by GCC and by Clang go through `init`, `funcs`, `disasm`, `diff` and agent
  matching end to end on Linux, with byte-exact verification.
- At least one additional ISA decoder passes the decoder tests and the diff tests, with changes
  limited to the new decoder and its relocation mapping.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Old MSVC environments (VC6 needs `PATH` including `MSDev98\Bin`, plus `INCLUDE` and `LIB`) | Toolchain `env` and `decomp toolchain test`. On Linux, a `wrapper` (Wine) later. |
| Non-code nondeterminism (timestamps in object headers) | Compare code and data only; `/Brepro` in the fixtures |
| Stripped relocations | Heuristic address detection is marked as such, and mismatches are flagged only where the candidate has a relocation at that field |
| Jump tables inside `.text` (MSVC x86) | Treated as data by bounds and disassembly, and compared as index lists |
| LLM cost and refusals | Budgets, configurable effort, cache verification, fallbacks on by default. Refused functions are marked and skipped, never worked around. |
| Append-only violations causing 400s | Enforced by `Conversation` and tested |
| Agent safety | Candidate code is compiled, never run, and writes are confined to project paths |
| No API key, Windows machine or display in the development container | Replay-driven tests; Windows verified by CI; the GUI tested through the reducer and a headless smoke test; live runs done by the user |
| Targets built with link-time code generation (`/GL`) | Detected from the Rich header and reported; out of scope for per-function matching |
| VC6-era PDB 2.0 files, which raw_pdb does not read | Fall back to exports, map files (Phase 2), user symbols and analysis |
| API and model changes | API code isolated in `anthropic::Client` with a model capability table; replay tests pin the protocol; prices are configurable |
| Code sent to a third-party API | `docs/agent.md` documents exactly what is sent. Only `decomp agent` sends anything, and only when started explicitly. |
| Toolchain drift between machines | The version banner is recorded by the health check, included in the cache key, and shown with every verification |
