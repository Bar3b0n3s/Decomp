# Phase 1 acceptance

The manual check of the [Phase 1 exit criteria](roadmap.md#phase-1-supervision-gui-and-batch-runner),
done by a person on Windows (the primary platform) and on Linux (X11, or XWayland on a Wayland
desktop). Automated tests cover the same paths with scripted sessions; this checklist adds the real
API, a real target and a person at the controls.

## What you need

- A Release build of `decomp`, `decomp-gui`, `decomp_tests` and `decomp_gui_tests`, with both test
  binaries passing.
- A project for a real target (`decomp init <binary> --toolchain <name> --flag ...`) whose toolchain
  passes `decomp toolchain test <name>`, with at least 25 functions that are not matched yet.
- `ANTHROPIC_API_KEY` set in the environment that starts `decomp-gui`. A run budget of a few dollars
  is enough for the whole checklist.

Without a key, the same steps work with scripted sessions: set Settings > Developer > replay directory
to `tests/replay/run` and use a project made from `tests/fixtures/x86/basic.exe` (13 functions, so
run all of them instead of 20).

## Checklist

### Open a project

- [ ] `decomp-gui --project <dir>` (or File > Open project) opens the project. The Dashboard shows
      the target's identity with the SHA-1 verified against `decomp.json` and the PDB state, and
      progress numbers equal to `decomp status`.
- [ ] Settings shows the API key as present (never its value); Check lists the models.
- [ ] Toolchains and compiles: the project's toolchain passes its health check and shows its version.

### Run 20 or more functions on 4 workers

- [ ] In the Function browser, filter the functions that are not matched, select 20 or more and
      queue them (or Run > Start run for the default selection), with 4 workers and a run budget.
- [ ] The first session starts alone; the others start once it streams its first response. From
      their first turn, later sessions read the prompt prefix from the cache (Cost and usage: cache
      hits; the transcripts' `cache_read_input_tokens`).
- [ ] The top bar shows the run state, spend against the budget and the API light; the status bar
      shows progress, the queue and an ETA.

### Watch live sessions and steer one

- [ ] Run monitor: four workers with their phases, turns and spend; the activity feed; the queue
      with ETAs; the worker timeline; throughput; rate limits.
- [ ] Agent session: open a live session. Thinking summaries and text stream in; each turn shows
      its tool calls, compiles and diffs; the score chart grows.
- [ ] Send guidance: it shows as pending, then appears inline after the next tool results. Retract a
      second one before it is sent: it never appears.
- [ ] Lower the per-function turn limit (Run monitor > Limits): the next status line reports it.
- [ ] Skip a queued function and requeue a finished one.

### Pause and stop

- [ ] Pause one worker, then the whole run, then resume both. Paused workers finish their current
      turn first.
- [ ] Stop the run: running sessions finish their current turn and end `stopped`; the queue keeps
      the rest.

### Review diffs and changes

- [ ] Diff viewer: a function's best attempt as an aligned diff, with row kinds, operand highlights
      and the attempt slider.
- [ ] Set the write policy to "ask" (Changes and approvals) and resume the run: a verified match
      waits with its diff; approve one, deny another with a reason (the agent's next turn shows the
      reason).
- [ ] Changes and approvals lists the approved write ("approved by user"); revert it: the source
      file goes away and the function is back to `nonmatching`.
- [ ] Take over a function (manual mode): edit the source, the diff recompiles in the background,
      Verify and save writes it and marks the function matched.
- [ ] Rename a symbol in the Binary explorer (right-click a byte in the hex view): Symbols and
      provenance lists it with source `user` and the edit in its history.

### Reopen the finished run from its log

- [ ] Let the run finish (or stop it), close `decomp-gui`, start it again: Run > Runs lists the run;
      open it read-only. Every view shows the same data as during the run (the streamed text is in
      the transcripts), and nothing is announced again as new.
- [ ] Resume a stopped run: the rest of the queue runs, and the views show the whole run.
- [ ] `decomp runs show <id>` agrees with the run's `summary.json`, and `decomp status` with the
      Dashboard.

## Recording the result

Note the platform, the target, the run id and anything that did not behave as described, with the
run directory (`.decomp/runs/<id>/`: `events.jsonl`, `sessions/`, `summary.json`), which holds
everything needed to investigate.

# Phase 2 acceptance

The manual check of the [Phase 2 exit criteria](roadmap.md#phase-2-analysis-depth) on a real target
without a usable PDB, such as a game built with Visual C++ 6.0. CI covers the same analysis on a
build of Zydis made with clang-cl and with cl.exe; this checklist adds the target the work is for.

## What you need

- The target executable, and for the measurement the build's map file (link.exe `/MAP`). Without
  one, a hand-checked list of function starts and ends written as a map file (one line per function
  in "Publics by Value", flagged `f`) does the same.
- The static libraries the target was linked with, when you have them (for VC6: `LIBC.LIB`,
  `LIBCMT.LIB` or `MSVCRT.LIB`, `LIBCP.LIB`, MFC and SDK libraries).
- The original compiler, registered as a toolchain (`decomp toolchain add vc6 --kind msvc ...`).

## Checklist

### The compiler

- [ ] `decomp info GAME.EXE` names the compiler the game's code came from (for VC6: "built with
      Visual C++ 6.0: C++ compiler 12.00.<build>") and the linker, with notes such as other
      compilers' objects, a Standard edition compiler or assembly objects; the Rich header's
      checksum matches.
- [ ] In a project for the target, `decomp toolchain test vc6` reports the compiler id of its probe
      and "the same compiler as the target's code" (or the same release with another build: another
      service pack).

### Function bounds

- [ ] `decomp bounds GAME.EXE --truth GAME.MAP --min-exact 95 --show-code` reports at least 95% of
      the functions exact and exits with 0. Note the exact, start-only, missed and extra counts, and
      the shapes of the mismatches it shows.

### Names and context

- [ ] `decomp init GAME.EXE --map GAME.MAP --toolchain vc6 --flag ...` names the functions from the
      map and records their object files (`obj=` in `symbols.txt`); `decomp map import GAME.MAP` does
      the same for a project made without the map.
- [ ] `decomp lib match LIBC.LIB ...` names the runtime functions the game linked and marks them
      `library`; `decomp status` counts them in its `library` row. `decomp analyze` afterwards traces
      the functions again knowing those names (the runtime's `_exit` and `__CxxThrowException@8`
      never return) and reports what changed.
- [ ] If the target was built with `/GR`: `decomp classes --slots` lists its classes, bases and
      vftables, and the Inspector's listing of a virtual function says which vftable slots hold it.
- [ ] In `decomp-gui`: the Dashboard shows what the target was built with; the Binary explorer has
      the Classes and Rich header tabs; the Inspector lists callers through incremental-linking thunks
      and the data that stores a function's address.

## Recording the result

Note the target, the compiler the Rich header names, the `decomp bounds` numbers and the mismatch
shapes `--show-code` shows, and the library match counts.

# Phase 3 acceptance

The manual check of the [Phase 3 exit criteria](roadmap.md#phase-3-project-organization) on a real
target with a PDB, then with the agent at work. CI covers the same steps on the test fixtures.

## What you need

- A target built with a PDB, and the original compiler registered as a toolchain.
- A project for it (`decomp init <binary> --toolchain <name> --flag ...`), and `ANTHROPIC_API_KEY` for
  the agent steps (or the scripted sessions of the Phase 1 checklist on the fixture).

## Checklist

### Units

- [ ] `decomp units` lists one unit per module of the PDB, in the PDB's order (compare with
      `llvm-pdbutil dump --modules <pdb>`), and `decomp status` counts every function in a unit: no
      "(no unit)" row.
- [ ] Code units have sources named after the files the PDB names (`src/<file>.cpp`), and library,
      import and linker units have none.

### Unit sources

- [ ] Run a few functions of one unit (`decomp run --unit <name>` or the Units view's Run). Each match
      lands in the unit's source after a `// FUNCTION:` marker, and `decomp units verify <name>`
      reports every function in it byte-exact.
- [ ] With functions matched in their own files (a project from before Phase 3, or a guessed unit),
      `decomp units emit` moves them into their units' sources, and `decomp units verify` agrees.

### Symbols and types

- [ ] In `decomp-gui`, with the policies for symbol changes and type definitions at "ask", a session
      that calls `set_symbol` or `define_type` waits in Changes and approvals; approving applies it,
      denying returns the reason to the agent.
- [ ] `decomp symbols log --agent` lists the agent's symbol changes with session and reason, and
      `decomp symbols revert --session <id>` undoes one session's; `decomp changes` lists the headers
      `define_type` wrote, and `decomp changes revert <n>` undoes one.

### Queue and cost

- [ ] A new run's queue (Run monitor, or `run.json`) starts with the functions the Function browser
      scores easiest.
- [ ] `decomp status` and `decomp status --json` show each unit's functions and bytes matched and its
      spend, and they add up to the totals; the Units view shows the same numbers.

## Recording the result

Note the target, the number of units and how they compare with the PDB's modules, the functions
matched into unit sources, and anything the agent named or defined.

# Phase 4 acceptance

The manual check of the [Phase 4 exit criteria](roadmap.md#phase-4-types) on a real target with a PDB,
and on one built with `/GR` without a PDB. CI covers the same steps on the test fixtures.

## What you need

- A target built with a PDB, and its original compiler registered as a toolchain (MSVC 7.0 or later:
  older compilers write type records Decomp does not read).
- A project for it (`decomp init <binary> --toolchain <name> --flag ...`).
- For the skeletons: a target built with `/GR` (run-time type information), with its PDB moved away.

## Checklist

### Layouts

- [ ] `decomp types --pdb` lists the PDB's types; `decomp types import --all --dry-run` prints the
      header it would write, and `decomp types import --all` writes `include/types.h` (or reports which
      types do not come back from the compiler with the PDB's layout, and why).
- [ ] `decomp types check` reports every declared type equal to the PDB's: size, field offsets and the
      vtable. Note any type that differs and the reason it gives.
- [ ] Edit a field's type in the header: `decomp types check` names the difference and exits with 1;
      revert the edit (`decomp changes revert <n>` for an import).

### Field names

- [ ] `decomp disasm <member function>` shows `; types:    this = <Class>* (ecx)` (rcx on x64) and the
      fields the code reaches in the comments (`this->...`), and virtual calls by their method's name.
- [ ] Rename a field in the header: the listing names it the header's way.

### get_type

- [ ] In a session (`decomp agent <member function>`), the brief's `# Types` section shows the class's
      layout, and a `get_type` call returns the same layout as `decomp types show <name>`.

### Skeletons

- [ ] With the PDB moved away, `decomp classes` lists the RTTI's classes, and `decomp types skeletons`
      writes their skeletons; they compile with the original compiler (it checked their vtables and
      base offsets against the RTTI before writing).
- [ ] The Types view in `decomp-gui` lists the types, shows a layout, and finds the uses of a type's
      fields.

## Recording the result

Note the target and compiler, how many types the PDB defines, how many imported and how many came back
equal, the differences `decomp types check` reported, and how many classes the skeletons covered.

# Phase 5 acceptance

The manual check of the [Phase 5 exit criteria](roadmap.md#phase-5-units-data-and-full-relink) on a
real target: partially matched, and relinked byte-identically with its unmatched code and data carried
by split objects. CI covers the same steps on the test fixtures and on a real program, the Zydis corpus,
built with clang-cl and lld-link and with cl.exe and link.exe, x86 and x64.

## What you need

- A target with its PDB (or a link map), and its original toolchain registered with the linker that
  made the target, of the same version: `decomp relink` says whether it is (`linker:`). When the
  toolchain's linker is another, set `link.linker` in `decomp.json`.
- A project for it with its units (`decomp units` lists them; `decomp units derive` makes them) and
  some matched functions in unit sources.
- An incrementally linked target (link.exe's default with `/DEBUG`) cannot be relinked identically: its
  incremental jump thunks and padding are the linker's.

## Checklist

### Split objects

- [ ] `decomp relink --all-split` reports `identical to the target`, with the fields it took over from
      the target (timestamps, the PDB's GUID and age). If it differs, note the first difference, its
      unit, and the `linker:` line.

### A partially matched target

- [ ] Complete one unit's source: every function matched into it and its data defined in its prelude,
      by matches and edits, or with `decomp units compose <unit> <file>` from a translation unit written
      by hand. `decomp units check <unit>` reports it `complete`.
- [ ] `decomp relink` links that unit from its source (`source  complete`) and every other unit from its
      split object, and reports `identical to the target`.

### A difference

- [ ] Change an initializer in that unit's source: `decomp units check <unit>` names the section that
      differs, and `decomp relink` splits the unit again (and stays identical).
- [ ] `decomp relink --source <unit>` differs, and names the first differing bytes in the unit's data,
      with the unit and the symbol there. Revert the change.

### The Relink view

- [ ] In `decomp-gui`'s Relink view, Relink shows the units and how each was linked and why; the
      Comparison tab shows the SHA-1s and the fields taken over, and for the relink that differs, the
      first differing bytes and the unit responsible, with the bytes around them in both images; the
      Unit tab shows the unit's check section by section.

## Recording the result

Note the target, its linker and version, the number of units and how many were linked from their
sources, the relinked image's SHA-1, and, for a relink that was not identical, its first difference and
the unit holding it.

# Phase 6 acceptance

The manual check of the [Phase 6 exit criteria](roadmap.md#phase-6-search-helpers) on a real target: its
flags recovered by a flag search, a function brought to byte-exact by the permuter, and its compiler
identified among the toolchains that could have built it. CI covers the same steps on the test fixtures
with clang-cl (x86 and x64), cl.exe (x86 and x64) and MinGW-w64 GCC (x64).

## What you need

- A project on a real target with some verified functions (in unit sources or their own files), and its
  toolchain registered.
- For identification, at least one other toolchain registered that could have built the target (another
  release of the compiler, clang-cl, MinGW-w64 GCC): `decomp toolchain list` shows them.
- For the permuter, a function whose best attempt is close but not byte-exact (an agent session's that
  only an order of statements or declarations keeps from matching), or a matched function's source with
  two independent statements swapped by hand.

## Checklist

### Flag search

- [ ] Note the project's flags (`decomp.json`), then set its optimization level to another one (`/Od` for
      `/O2`). `decomp search flags --verified` finds flags that make every verified function byte-exact
      again; the optimization group is decided, and the project's own alternative is among the equally
      good ones in every group.
- [ ] `decomp search flags <function>` on one verified function: note which groups it decides and which it
      leaves open (more functions decide more).
- [ ] `decomp search flags --verified --apply` sets the flags; `decomp units verify` passes with them.

### Permuter

- [ ] `decomp search permute <function>` on the near-miss: it ends byte-exact (exit code 0), and its edits
      and the diff are the reordering expected. Its run is in `decomp search list`.
- [ ] `--apply` keeps the source as the function's verified source; `decomp units verify` passes.

### Compiler identification

- [ ] `decomp search identify --verified` ranks the target's own toolchain first, ahead of the others,
      with every function byte-exact and its flags among those of the flag search.
- [ ] With the target's toolchain left out (`--candidates` naming the others), no toolchain makes every
      function byte-exact; note how the ranking reads.

### The Search view

- [ ] In `decomp-gui`'s Search view, start a flag search on a verified function: its progress shows while it
      runs, then its groups (chosen, as good, the start), its candidates with the best so far, and its
      settings. A permutation shows its edits and the diff of its start and best sources; an
      identification its ranking. The searches made with `decomp search` are listed too.
- [ ] Use these flags (or the toolchain, or Keep the source) changes `decomp.json` (or the verified
      source) as the CLI's `--apply` does.

## Recording the result

Note the target, the toolchain and flags the flag search recovered and the groups it left open, the
function the permuter matched with its edits and the number of candidates, and the identification's
ranking with each toolchain's best score.
