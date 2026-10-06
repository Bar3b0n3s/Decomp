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
  quoting as a pure function on all platforms. The Windows process implementation has its own tests,
  which run in the Windows CI job (step 13).
- [x] **3. Fixtures.**
  - `tests/fixtures/src/basic.cpp` covers arithmetic, a loop, a switch with a real jump table (the
    cases need different side effects, or the compiler emits a lookup table instead), globals, a
    string literal, float constants, calls, an import, an export and a `thiscall` method;
    `src/other.cpp` is a second translation unit. `candidates/mutated.cpp` changes one thing per
    function.
  - `build_fixtures.sh` builds them with clang-cl and lld-link: `/O2 /Gy /GS- /GR- /EHs-c- /Zl /Z7
    /Brepro`, linked with `/nodefaultlib /entry:entry /subsystem:console /debug /Brepro`. The source
    defines `extern "C" int _fltused = 0;`.
  - Committed for x86 and x64: `basic.exe`, `basic.pdb`, `basic_fixed.exe` (linked `/FIXED`, without
    base relocations), the objects `basic.obj` and `other.obj` (exact candidates) and `mutated.obj`,
    and an import library for `kernel32`, so unit tests need no compiler.
- [x] **4. formats.** PE, COFF and PDB readers. Tests check the fixtures' headers, sections, imports,
  exports, base relocations, Rich header, `.pdata`, PDB records, and COFF sections, symbols and
  relocations against expected values written into the tests.
- [x] **5. arch/x86.** The Zydis decoder and the symbolizing formatter. Tests decode known byte
  sequences for x86 and x64 and check text, flow and fields.
- [x] **6. analysis.** Demangling, `SymbolDb` import (PDB, exports, imports, x64 `.pdata`), bounds,
  jump tables, linker thunks, cross-references, CFG and the annotator. CLI: `info`, `funcs`, `disasm`.
- [x] **7. matching (diff).** The target and candidate sides, `diff_sides` (alignment, row kinds,
  scoring, verdicts, hints, bindings) and the text and JSON reports. Tests: every fixture function is
  `exact` and `byte_exact` against its own object; each mutation (immediate, opcode, wrong global,
  extra instructions, string literal, float constant) produces the expected row kinds and hints;
  unnamed target addresses produce bindings. CLI: `diff --obj`, with `--all`.
- [x] **8. matching (toolchains).** Registry with auto-detected clang-cl, compile driver and cache,
  diagnostics parsers. CLI: `diff --source`, `toolchain list`, `toolchain test` and `toolchain add`.
  An integration round trip builds the fixture program with the installed clang-cl and lld-link,
  compiles the fixture sources again, matches every function and requires `byte_exact`. It is skipped
  when LLVM is missing.
- [x] **9. project.** `decomp init <binary>` (writes `decomp.json`, imports symbols into
  `symbols.txt`); commands resolve the project from the current directory or `-C/--project`; function
  status and history; `decomp status` (progress by bytes and functions, status buckets, spend from
  `symbols.txt`).
- [x] **10. Events backbone.** Event types, `EventBus`, the `RunState` reducer, the JSONL event log,
  and the CLI live progress view (ANSI, or plain lines when not a TTY). Tests: the reducer folds
  synthetic event sequences into the expected state, and log, then replay, gives the same state (the
  log omits stream deltas).
- [x] **11. agent.**
  - Transports and the SSE parser, tested on recorded streams with thinking, signatures, partial tool
    JSON and error events.
  - Client retries, tested on replayed 429 and 529 responses.
  - The append-only `Conversation`. A test asserts that every request extends the previous one
    byte-for-byte and that system and tools never change.
  - Tool registry and validator, match tools, the loop (which emits events), the single-session
    `LoopControl`, the runner and transcripts.
  - CLI: `agent <func> [--replay <file>]`, with the live progress view, `--interactive`,
    `--guidance`, budget overrides and Ctrl+C handling. Verified sources and history are written to
    the project.
  - Scripted replays:
    - wrong source, diff, corrected source, `submit_result`, then `matched`;
    - refusal ends as `refused`;
    - budget exhaustion;
    - supervisor guidance injected mid-run keeps the history append-only.
- [x] **12. Docs.** `README.md`, `docs/architecture.md`, `docs/matching.md`, `docs/agent.md`,
  `docs/ui.md`, `docs/project-format.md` and `docs/roadmap.md` (this set), reconciled with the
  implementation.
- [x] **13. CI.** `.github/workflows/ci.yml` on ubuntu (g++-14) and windows-latest (Visual Studio 2022
  or 2026 through `premake5 vs2022` or `vs2026`, msbuild) builds and runs the unit tests. Linux also
  runs a CLI smoke test that includes a scripted agent run. Windows also runs the MSVC round trip
  (`tests/integration/msvc_roundtrip.ps1`, x86 and x64) with the real `cl.exe`, including a scripted
  agent run. Both jobs are green: every fixture function is byte-exact with the real `cl.exe` on x86
  and x64 (incremental-link thunks, `$LN` and RVA jump tables, image-base-relative operands), and the
  scripted agent run matches `add()` with clang-cl on Linux and with `cl.exe` on Windows.

**Slice exit criteria**

- On Linux, the build with g++-14 succeeds and `bin/Release/decomp_tests` passes, including the
  clang-cl integration tests.
- CLI smoke tests pass:
  - `decomp info tests/fixtures/x86/basic.exe` works;
  - `decomp disasm sum_array tests/fixtures/x86/basic.exe` works;
  - `decomp diff --binary tests/fixtures/x86/basic.exe --obj tests/fixtures/x86/basic.obj --all`
    reports every function byte-exact, `decomp diff ... --source <exact.cpp>` reports 100% and
    `byte_exact`, and a mutated candidate produces classified rows;
  - in a project for `tests/fixtures/x86/basic.exe`, `decomp agent add --replay
    tests/replay/agent_match_add.jsonl` shows the live view, ends `matched` and writes the source to
    the project, and the run's `events.jsonl` replays into the same `RunState`.
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
- Resumable runs. After a restart, the remaining queue continues; interrupted sessions start again with
  a fresh conversation whose brief carries their attempts, notes and best source.
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

The manual part is the checklist in [acceptance.md](acceptance.md).

**Status:** the scope is implemented, and the reducer, replay and headless GUI tests pass in CI on
Linux and Windows, with an end-to-end scripted run of `decomp-gui` under Xvfb on Linux. The manual
checklist needs an API key and a person on each platform.

### Phase 2: Analysis depth

**Goal:** accurate function bounds, names and context for targets without a PDB.

**Scope**

- MSVC `.map` import.
- x64 `.pdata` bounds: merging chained unwind entries. (The slice already turns `.pdata` entries
  without symbols into functions.)
- A compiler table that maps Rich header entries to compiler versions and toolchain suggestions. (The
  slice describes the VC6 to Visual Studio 2005 product IDs.)
- A full cross-reference index. (The slice scans functions with known sizes for callers and data
  references.)
- MSVC in-`.text` jump tables for PDB-less targets, including two-level tables. (The slice reads
  single-level tables.)
- The `dllimport` hint for import calls. (The slice already follows incremental-linking and import
  thunks.)
- MSVC RTTI and vtables mapped to class names.
- COFF `.lib` reading, plus CRT and library signature matching that marks functions as `library`.

**Exit criteria**

- At least 95% of function bounds (start and end both exact) are correct on a real PDB-less VC6
  executable, measured against ground truth such as the build's map file or a hand-checked list.
- For a set of reference binaries, the Rich header maps to the correct compiler version.

**Status:** the scope is implemented. The corpus CI measures is Zydis with C++ and structured
exception handling added (`tests/corpus`). Without their PDBs, discovery finds exact bounds for 99.7%
(x86, 382 of 383) and 100% (x64) of the functions in cl.exe 19.51's PDBs, and for 100% (x86) and 98.8%
(x64) of those in clang-cl's. clang counts x64 funclets as part of their function; cl.exe, and so
discovery, does not. Against the link maps, 100% of the starts are found for both compilers and both
architectures. CI fails below 98%. The Rich header of the cl.exe build names cl.exe's own version,
and unit tests cover reference Rich headers from VC6 to Visual Studio 2022. The VC6 measurement
itself needs a real VC6 executable and its map file; the steps are in
[acceptance.md](acceptance.md#phase-2-acceptance).

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

**Status:** the scope is implemented, and CI covers each exit criterion on the test fixtures.

- Units: on the fixtures, whose PDBs list `basic.obj`, `other.obj`, the import modules and
  `* Linker *`, the derived units are those modules in that order, and every function belongs to one.
  Without a PDB or a map, the analysis guesses units as a starting point: on the Zydis corpus it finds
  a third of the true boundaries (`decomp bounds --units`).
- Unit sources: sessions compose matches into their unit's source and verify the whole unit, and
  `decomp units emit` moves earlier per-function sources into it. After the scripted 4-worker run, CI
  checks with `decomp units verify` that every function in the two unit sources is byte-exact (and
  `decomp units verify` with cl.exe in the Windows round trip).
- `set_symbol` and `define_type` run through the approval gate (ask in the GUI, deny in command-line
  runs unless configured), record their provenance (`symbols.log.jsonl`, `changes.jsonl`), and are
  reverted with `decomp symbols revert`, `decomp changes revert` or the GUI. Tests drive both tools in
  a session with real compiles.
- The queue orders by the difficulty score of each function's code, and `decomp status` lists
  progress and spend per unit (its JSON has a `units` array); the GUI's Units view shows the same and
  runs a unit's functions.

The manual check on a real target is in [acceptance.md](acceptance.md#phase-3-acceptance).

### Phase 4: Types

**Goal:** real types instead of per-function ad hoc declarations.

**Scope**

- Headers in `include/` become the source of truth for types.
- Headers are compiled with the original compiler and debug information, and the exact layouts are
  read back from the type records it writes. (Planned as `/Zi` and the resulting PDB's TPI stream;
  `/Z7` puts the same CodeView records in the object's `.debug$T` section, which needs no PDB server
  and no link, so the compile is one more cached compile like a candidate's.)
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

**Status:** the scope is implemented, and CI covers each exit criterion on the test fixtures.

- Layouts: `decomp types check` compiles the project's headers with the project's toolchain and `/Z7`
  and compares every declared type with the PDB: size, kind, bases and their offsets, vfptr and vbptr,
  vtable entries and the slots of the virtual methods, field offsets, sizes, bits and types, and
  enumerators. CI checks the fixtures' types (a struct; classes with virtual functions, multiple and
  virtual inheritance) declared by hand (`tests/fixtures/include`) and imported from the PDB
  (`decomp types import --all`), with clang-cl for x86 and x64 on Linux and with cl.exe against
  link.exe's PDBs in the Windows round trip, which also imports the types of a program built for the
  hard cases (`tests/fixtures/src/layouts.cpp`: packing, alignment, bitfields, anonymous unions and
  structs, member pointers, pure and overloaded virtuals, other calling conventions).
- Field names: annotated listings follow pointers of known types from `this` and the parameters
  (`this->hp`, `arg_0->area() (virtual, slot 0)`), with the project's headers' names before the
  PDB's; tests check the fixtures' listings for x86 and x64 and the type flow on synthetic code.
- `get_type` returns the layout the compiler made of a type (the headers', else the PDB's), with the
  differences between the two; session briefs show the layouts of the types a function's signature
  names.
- Skeletons: `decomp types skeletons` makes class skeletons from the RTTI (bases at the RTTI's
  offsets, vfptrs, virtual methods by slot with the signatures decorated names give); CI compiles them
  for the RTTI fixture without its PDB (clang-cl, x86 and x64) and for the RTTI cl.exe writes (cl.exe),
  and their vtables and base offsets equal the RTTI's.

The manual check on a real target with a PDB is in [acceptance.md](acceptance.md#phase-4-acceptance).

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

**Status:** the scope is implemented, and CI covers each exit criterion on the test fixtures and on a
real program.

- Per-unit matching: `decomp units check` places each unit source's compiled object where the linker
  would put it (by its functions, the external names the program knows and the targets of its
  relocations) and compares its code and data with the image section by section: the order of the
  functions, initial values and placement of `.data`, `.rdata` and `.bss`, strings and constants pooled
  across units, switch tables, C++ exception-handling and SEH tables, x64 unwind data and `.pdata`, and
  functions identical-COMDAT folding put together.
- Relinking: `decomp relink` links every complete unit from its source and every other one from a split
  object of its original bytes (`formats/coff_writer.hpp` writes them, and import libraries from the
  import table), in the order the image shows, with the target's linker and the flags its headers call
  for; takes over the fields that only record when and how the image was built; compares the SHA-1s;
  and says whether the linker is the version that made the target. Split objects carry what the linker
  reads of the originals besides their bytes: their units' names, the compiler marks link.exe counts
  (`@comp.id`, `@feat.00`), x86 SAFESEH registrations, x64 `.pdata` relocated to its functions, and the
  C common symbols the linker allocates (in the order each linker allocates them). Folded functions are
  folded again (`/opt:icf`).
- CI relinks the fixtures byte-identically: 14 images from split objects alone (plain code and data,
  C++ and structured exception handling, RTTI, a static library, `/FIXED`, hand-written code with and
  without its map), x86 and x64, with LLVM 18's lld-link, which made them; the fixture rebuilt from its
  units' sources partly and wholly; the exception-handling fixture from its source; a program whose
  functions ICF folds within and across units, in every mix of split and source units; and programs
  with C common symbols and with an inline function two units define. The Windows round trip relinks
  the fixture cl.exe and link.exe built, x86 and x64, with link.exe, from split objects and from its
  units' sources.
- A partially matched real program: the corpus (Zydis with C++ and structured exception handling,
  31 units) relinks byte-identically from split objects, and with nine of its units (its decoder and
  the SEH unit among them) built from their own sources and the rest split, x86 and x64, with both
  toolchains: clang-cl and lld-link on Linux, cl.exe and link.exe on Windows.
- When a relink differs, `decomp relink` and its `result.json` name the first differing bytes, the unit
  whose contribution holds them and the symbol there; the Relink view in `decomp-gui` shows them with
  the bytes around them in both images, and each unit's check section by section. Tests force a unit
  with a changed initializer into a relink and find the difference at that global, in that unit.

The manual check on a real target is in [acceptance.md](acceptance.md#phase-5-acceptance).

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

**Status:** the scope is implemented, and CI covers each exit criterion on the test fixtures with
three toolchains.

- Flag search: `decomp search flags` searches groups of alternatives (presets per toolchain style and
  architecture, or given with `--group`) exhaustively when the space is small and by local search from
  the project's flags with seeded restarts otherwise, compiling candidates in parallel and scoring them
  over every function of the probes (verified sources, units' sources, best attempts, files); it reports
  which alternatives do as well as the chosen one, and `--apply` sets the flags. CI starts from `/Od` and
  recovers the fixture's flags (every function byte-exact, the fixture's alternative among the equally
  good ones in every group) with clang-cl, x86 and x64, on Linux and with cl.exe, x86 and x64, on Windows.
- The permuter: `decomp search permute` edits the bodies of a translation unit's target functions at the
  token level (statements and declarations moved, declarators swapped or split, commutative operands
  swapped, comparisons flipped, if/else branches swapped, increments) in seeded rounds, keeping what
  brings the functions closer, and undoes the edits the result does not need. `permute_perturbed.cpp`,
  the fixture `permute.cpp` with statements and declarations reordered, becomes byte-exact in about 60
  compiles with clang-cl (x86 and x64, unit tests and Linux CI) and with cl.exe (x86 and x64, Windows
  CI); `--apply` keeps a result as the function's verified source.
- Compiler identification: `decomp search identify` ranks the registered toolchains for the target's
  architecture by the best score of a small flag search each. `ident.c`, built by cl.exe and link.exe,
  by clang-cl and lld-link, and by MinGW-w64 GCC (linked by lld-link), identifies its toolchain first in
  each case, every function byte-exact (Windows CI; Linux CI and the unit tests with clang-cl and GCC).
  GCC-style candidates are diffed like MSVC-style ones: without function sections, with their direct
  calls to neighbouring functions and their section-relative data references read as the symbols they
  reach.
- The Search view in `decomp-gui` starts the three searches with their progress, lists the project's
  searches (`.decomp/search/`, which `decomp search` writes too) and shows each one's result, candidates
  and settings, and keeps a result in the project.

The manual check on a real target is in [acceptance.md](acceptance.md#phase-6-acceptance).

### Phase 7: More targets

**Goal:** go beyond Windows PE and x86.

**Scope**

- End-to-end support for ELF64 built by GCC or Clang: an ELF `BinaryImage`, symbols from `.symtab`,
  bounds from `.eh_frame`, ELF relocations in the diff, and ELF objects from the `gcc`/`clang`
  toolchain kinds (whose command lines and diagnostics the slice already handles).
- Other ISAs through an ISA-neutral decoder interface and additional decoders.

**Exit criteria**

- ELF64 fixtures built by GCC and by Clang go through `init`, `funcs`, `disasm`, `diff` and agent
  matching end to end on Linux, with byte-exact verification.
- At least one additional ISA decoder passes the decoder tests and the diff tests, with changes
  limited to the new decoder and its relocation mapping.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Old MSVC environments (VC6 needs `PATH` including `MSDev98\Bin`, plus `INCLUDE` and `LIB`) | Toolchain `env` and `env_prepend`, and `decomp toolchain test`. On Linux, a `wrapper` (Wine) later. |
| Non-code nondeterminism (timestamps in object headers) | Compare code and data only; `/Brepro` in the fixtures |
| Stripped relocations | Heuristic address detection (in-image values of 4 bytes or more). Comparing such fields as plain values where the candidate has no relocation is planned. |
| Jump tables inside `.text` (MSVC x86) | Treated as data by bounds and disassembly, and compared as index lists |
| LLM cost and refusals | Budgets, configurable effort, cache reads recorded per turn, fallbacks on by default. Refused functions are marked and never worked around; skipping them in batch runs comes with Phase 1. |
| Append-only violations causing 400s | Enforced by `Conversation` and tested |
| Agent safety | Candidate code is compiled, never run, and writes are confined to project paths |
| No API key, Windows machine or display in the development container | Replay-driven tests; Windows verified by CI; the GUI tested through the reducer and a headless smoke test (Phase 1); live runs done by the user |
| Targets built with link-time code generation (`/GL`) | Visible in the Rich header (`decomp info`); a warning is planned. Out of scope for per-function matching. |
| VC6-era PDB 2.0 files, which raw_pdb does not read | Fall back to exports, map files (Phase 2), user symbols and analysis |
| API and model changes | API code isolated in `agent::Client` and `Conversation`; replay tests pin the protocol. A model capability table and configurable prices are planned (prices live in `agent/cost.cpp`). |
| Code sent to a third-party API | `docs/agent.md` documents exactly what is sent. Only `decomp agent` sends anything, and only when started explicitly. |
| Toolchain drift between machines | Planned: the health check records the compiler's version banner, which is added to the cache key and shown with every verification. Today the cache key covers the toolchain's definition. |
