# Project format

A Decomp project is a directory that holds everything about decompiling one target binary. The parts
meant for git are its configuration (`decomp.json`), its symbol database (`symbols.txt`), shared
headers (`include/`) and verified sources (`src/functions/`). Working data stays local under a
gitignored `.decomp/` directory: per-function attempt history and notes, run logs and transcripts,
build directories and the compile cache. Compilers are machine-specific, so they live in a
**user-level toolchain registry** outside the project, and projects refer to them by name. Decomp
writes JSON with sorted keys and `symbols.txt` sorted by address, and it replaces whole files through
a temporary file plus rename, so a project diffs and merges cleanly. This document specifies each
file, the function status values and the toolchain registry.

Status: implemented in the first slice (step 9 of the [roadmap](roadmap.md#first-working-slice)).
Translation-unit organization replaces the one-file-per-function source layout in Phase 3. Anything
marked *planned* does not exist yet.

## Layout

```
<project>/
    decomp.json                 configuration                                     (committed)
    symbols.txt                 symbols: addresses, names, kinds, sizes, status   (committed)
    include/                    shared headers                                    (committed)
    src/functions/              verified sources, one .cpp per function           (committed)
    .gitignore                  written by init: /.decomp/                        (committed)
    .decomp/                    working data                                      (gitignored)
        functions/<fn>/         attempts.jsonl, best.cpp, notes.md
        runs/<run-id>/          events.jsonl, sessions/<fn>.jsonl, summary.json
        build/                  per-compile working directories
        cache/objects/          compile cache
```

`<fn>` is the function's [key](#function-keys-fn), such as `add_401060`.

The target binary and its PDB usually live outside the project and are referenced by path. They are
often not redistributable, and `target.sha1` lets every checkout confirm that it has the right file.

`decomp init <binary>` creates the project in the current directory, or in `--dir <dir>` (created if
needed). It writes `decomp.json`, `symbols.txt` and `.gitignore`, creates the empty `include/` and
`src/functions/` directories, and refuses to run where a `decomp.json` already exists. Its other
options are `--toolchain <name>`, `--flag <flag>` (repeatable) and `--pdb <file>`. Every other command
finds the project by searching upward from the current directory for `decomp.json`, or upward from
the directory given with the global option `-C <dir>` (`--project <dir>`). Global options go before
the command name (`decomp -C game status`), and `init` ignores `-C`.

## `decomp.json`

The example below is shown exactly as `decomp init ../bin/GAME.EXE --toolchain vs2008 --flag /O2
--flag /EHsc --flag /MD` writes it: keys sorted, two-space indentation, a trailing newline.

```json
{
  "agent": {
    "effort": "high",
    "fallbacks": true,
    "max_minutes_per_function": 30,
    "max_tokens_per_function": 0,
    "max_turns": 40,
    "max_usd_per_function": 5.0,
    "model": "claude-opus-5-5"
  },
  "flags": [
    "/O2",
    "/EHsc",
    "/MD"
  ],
  "include_dirs": [
    "include"
  ],
  "target": {
    "path": "../bin/GAME.EXE",
    "pdb": "../bin/GAME.pdb",
    "sha1": "0123456789abcdef0123456789abcdef01234567"
  },
  "toolchain": "vs2008",
  "version": 1
}
```

| Key | Type | Meaning |
|---|---|---|
| `version` | integer | Format version; must be `1` |
| `target.path` | string | Required. The target binary, relative to the project directory (absolute paths are accepted; `init` writes a relative path whenever one exists) |
| `target.sha1` | string | SHA-1 of the target. When the project loads its target and the SHA-1 differs, Decomp logs a warning and continues; refusing to run on a mismatch is planned. |
| `target.pdb` | string | Optional. The PDB, relative to the project directory. If it is set but cannot be used (missing, unreadable, or its GUID and age do not match the image's CodeView record), loading fails. If it is absent, Decomp looks next to the binary for the file named in the CodeView record, then for `<stem>.pdb`, and ignores a PDB that does not match. |
| `toolchain` | string | The name of a toolchain in the [registry](#toolchain-registry) |
| `flags` | array of strings | The target's code-generation flags, passed after the toolchain's base flags |
| `include_dirs` | array of strings | Project include directories (relative), passed to the compiler as `/I` |
| `agent` | object | Agent settings ([agent.md](agent.md#configuration)). Omitted keys use the defaults shown above. |

`decomp init` fills in `target` (path, SHA-1, and the PDB when one was given with `--pdb` or found
next to the binary and matched), sets `toolchain` and `flags` from `--toolchain` and `--flag`, and
sets `include_dirs` to `["include"]`. Without `--toolchain` or `--flag` it prints a reminder to set
them in `decomp.json`. Recovering flags automatically is part of Phase 6. The API key is never stored
here.

## `symbols.txt`

One symbol per line, sorted by address, after a header comment that names the fields:

```
# decomp symbols: <address> <kind> <name> [size=] [pdb=] [static] [source=] [status=] [best=] [attempts=] [cost=]
```

An excerpt of the file that `init` writes for the x86 test fixture, after one agent session matched
`add`:

```
0x00400000 data __ImageBase source=analysis
0x00401000 function ?Hit@Player@@QAEXH@Z size=0x1e pdb=Player::Hit source=pdb_public
0x00401060 function ?add@@YAHHH@Z size=0xf pdb=add source=pdb_public status=matched best=100.0 attempts=3 cost=0.0729
0x00401080 function ?sum_array@@YAHPBHH@Z size=0x6b pdb=sum_array source=pdb_public
0x00401160 function helper size=0xb static source=pdb
0x004011e0 function _entry size=0xee pdb=entry source=pdb_public
0x00402004 float __real@3fc00000 size=0x4 source=pdb_public
0x00402010 string ??_C@_0M@LACCCNMM@hello?5world?$AA@ size=0xc source=pdb_public
0x00402110 import __imp__ExitProcess@4 size=0x4 source=pdb_public
0x00403000 data ?g_counter@@3HA size=0x4 pdb=g_counter source=pdb_public
0x00403028 data s_calls size=0x4 static source=pdb
```

Addresses have at least eight hex digits, so x64 addresses are longer:

```
0x140001030 function ?add@@YAHHH@Z size=0xa pdb=add source=pdb_public
```

| Field | Meaning |
|---|---|
| address | Virtual address (not RVA): `0x` and at least 8 lowercase hex digits |
| kind | `function`, `data`, `string`, `float`, `import`, `label` or `unknown` (`func` is also accepted when reading) |
| name | The decorated name as the linker sees it (`?Update@Player@@QAEXM@Z`, `_main`, `_WinMain@16`, `@fn@8`, `__imp__MessageBoxA@16`), or the best name known: the PDB records of static functions and data carry undecorated names. A name containing a space, tab, `"` or `=` is written in double quotes with C escapes. Demangled forms are derived, not stored. |
| `size=` | Size in bytes, in hex; omitted when unknown |
| `pdb=` | The undecorated name from the PDB's procedure or data record, when it differs from the name |
| `static` | Internal linkage (`S_LPROC32` procedures and module-local data) |
| `source=` | Where the name came from, in increasing order of trust: `analysis`, `import`, `export`, `pdb_public`, `pdb`, `agent`, `user`. A line without `source=` is read as `user`. |
| `status=` | For functions: a [function status](#function-status); omitted for `unstarted` |
| `best=` | For functions: the best match percentage reached by agent sessions, one decimal |
| `attempts=` | For functions: the number of compile attempts made by agent sessions |
| `cost=` | For functions: the agent spend on the function so far, in USD |

Notes:

- `init` writes every symbol Decomp derives from the image, its PDB and the analysis: imports,
  exports, PDB public symbols, procedures and data, `.pdata` entries on x64 (functions without any
  other name are called `sub_<hex address>`), `__ImageBase`, and the entry point (`entry` when it
  has no other name). Exports and the entry point that land on incremental-linking thunks are moved
  to the functions behind them ([matching.md](matching.md#incremental-linking-and-import-thunks)).
- When a project loads, its lines are applied on top of the symbols derived from the image and PDB.
  A line takes over the name at its address when its source is at least as trusted as the derived
  one; otherwise its name is kept as an alias. To rename something, edit the name and set
  `source=user` (or drop `source=`); to name an unnamed address, for example the address of a
  [binding](matching.md#3-canonicalization) suggestion, add a line for it.
- Blank lines and lines starting with `#` are ignored. An unknown key, or a bare token other than
  `static`, is a parse error that stops the project from loading and names the file and line.
- Decomp rewrites the whole file from its symbol database whenever a function's state changes, that
  is, when an agent session starts and when it ends. Comments added by hand are not kept. Other names
  at the same address (aliases, for example from identical-COMDAT folding) are kept in memory for name
  lookups but not written ([matching.md](matching.md#opticf-folding)).

## `include/`

Shared headers: declarations of types, globals and functions used by more than one source. `init`
creates the directory empty and lists it in `include_dirs`. In the slice, the agent writes
self-contained translation units and does not change headers; its brief lists the files found in the
include directories. The headers enter the compile cache key through their paths, sizes and
modification times ([matching.md](matching.md#compile-cache)). From Phase 3, the `define_type` tool
edits headers under the approval policy. From Phase 4, headers are the source of truth for types, and
their layouts are checked against the PDB.

## `src/functions/`

Verified sources, one `.cpp` per function in the slice, named `<fn>.cpp` (for example
`src/functions/add_401060.cpp`). A file is written when the agent's `submit_result` is accepted, and
it is **exactly** the translation unit that verified byte-exact. Decomp adds no banner or comment,
because even a comment can shift `__LINE__`. The history (session, attempts, scores) lives in
`.decomp/`. `decomp diff --source` never writes here; a "verify and save" for hand-written sources is
part of the Phase 1 GUI. Phase 3 groups functions into translation units, with one source per unit as
in the original program.

## `.decomp/`

Working data, local to the machine and gitignored. Deleting it loses history, run logs and cached
compiles, but never verified sources or symbols (each function's status, best score, attempt count
and spend are also in `symbols.txt`).

### Function keys (`<fn>`)

`<fn>` is a filesystem-safe name that stays readable and is unique per address. It starts from the
function's PDB name, or, without one, from the qualified name inside its decorated name. Each `:`
becomes `_`, any other run of characters that are not letters, digits or `_` becomes a single `_`,
and the result is cut to 80 characters. Then `_` and the address in lowercase hex are appended:
`Player::Hit` at `0x401000` becomes `Player__Hit_401000`, and `add` at `0x401060` becomes
`add_401060`. The same key names the history directory, the session transcripts and the source file.
Renaming a symbol changes its key; existing files are not moved.

### `functions/<fn>/`

| File | Contents |
|---|---|
| `attempts.jsonl` | One JSON object per compile attempt of an agent session, that is, every `compile_and_diff` call and the verification compile of every `submit_result` with `matched`. Fields: `attempt` (numbered from 1 within the session), `session`, `compiled`, `match_percent`, `byte_exact`, `summary` (the diff's one-line summary, or why there is no diff) and the full `source`. `decomp diff` does not record attempts. |
| `best.cpp` | The source of the best attempt so far. An attempt replaces it when it compiled, was diffed, and scored at least the function's previous best (`best=` in `symbols.txt`), so among equal scores the most recent wins. For a matched function it equals the verified source. |
| `notes.md` | Notes from the agent's `record_note`, one per line: `- YYYY-MM-DD HH:MM: <text>` (UTC) |

An `attempts.jsonl` line from the scripted replay of the CI smoke test:

```json
{"attempt":1,"byte_exact":false,"compiled":true,"match_percent":68.75,"session":"2026-10-04T02-08-33-f11d-401060","source":"extern int g_counter;\n\n__declspec(noinline) int add(int a, int b) { return a - b + g_counter; }\n","summary":"match 68.8% (2/4 equal; 1 operand, 1 opcode) - not matching"}
```

This history feeds future briefs: the number of earlier attempts and the best score, the notes and the
best source go into the next session's first message ([agent.md](agent.md#prompts)).

### `runs/<run-id>/`

A run is one invocation of `decomp agent`, which works on one function in the slice. The run ID is the
UTC start time plus four random hex digits, such as `2026-10-04T15-30-12-3f9a`. With
`decomp agent --log-dir <dir>` the run goes to `<dir>/<run-id>/` instead; without a project and
without `--log-dir`, no run files are written.

| File | Contents |
|---|---|
| `events.jsonl` | Every event of the run except the high-volume `stream_delta` events, one JSON object per line, in sequence order ([ui.md](ui.md#events)). `RunState::replay` folds it back into the run's state ([ui.md](ui.md#replay-of-past-runs)). |
| `sessions/<fn>.jsonl` | The transcript of the session for one function: the first request in full, the messages each later request appended, every response with its usage and cost, tool calls, retries, guidance, pauses and the outcome ([agent.md](agent.md#transcripts-and-event-logs)). It never contains the API key. |
| `summary.json` | Totals for the run: its status, model and effort, cost, and per function the outcome, best match, turns, cost and token usage. `decomp status` does not read it; it uses `symbols.txt`. |

The `summary.json` of the CI smoke test's scripted run (`"replay": true`; a live run has `false`):

```json
{
  "cost_usd": 0.072854,
  "effort": "high",
  "finished": "2026-10-04T02:08:33Z",
  "functions": [
    {
      "best_match": 100.0,
      "cost_usd": 0.072854,
      "detail": "'submit_result' ended the session",
      "display": "int __cdecl add(int, int)",
      "function": "?add@@YAHHH@Z",
      "matched": true,
      "outcome": "matched",
      "turns": 4,
      "usage": {
        "cache_creation_input_tokens": 8270,
        "cache_read_input_tokens": 20900,
        "input_tokens": 31,
        "output_tokens": 1360
      },
      "va": 4198496
    }
  ],
  "model": "claude-opus-5-5",
  "replay": true,
  "run": "2026-10-04T02-08-33-f11d",
  "started": "2026-10-04T02:08:33Z",
  "status": "completed"
}
```

`status` is `completed`, `stopped`, `aborted` or `error`; the function's `outcome` is one of the
outcomes in [agent.md](agent.md#the-loop).

### `build/` and `cache/`

- `build/` holds one fresh working directory per compile, named `c<first 12 hex digits of the cache
  key>-<n>`: `candidate.cpp`, `candidate.obj`, and `args.rsp` when the command line is long. The
  directory of a successful compile is deleted right away; a failed compile's directory is kept for
  inspection, and nothing prunes those yet.
- `cache/objects/` is the compile cache: `<sha1>.obj` plus `<sha1>.json` with the compile result
  (`ok` and the compiler's `output`), keyed by the SHA-1 described in
  [matching.md](matching.md#compile-cache). Failed compiles are cached too, except timeouts. Deleting
  the directory is always safe.

## Function status

| Status | Meaning | Set by |
|---|---|---|
| `unstarted` | No attempt has scored yet. This is the default, and it is not written to `symbols.txt`. | `init`; the runner, when a session ends without any scored attempt |
| `in_progress` | An agent session is working on it | The runner, when a session starts; replaced when the session ends. After a crash it stays in `symbols.txt` until the next session on that function ends. |
| `nonmatching` | At least one attempt scored above 0%, none matched; the best percentage is in `best=` | The runner, when a session ends without a match, give-up or refusal (budget, turn limit, no result, stop, abort or error) |
| `matched` | A verified byte-exact source is in `src/functions/` | `submit_result` verification. A matched function stays matched, whatever later sessions do. |
| `refused` | The model declined the request, including after fallbacks when they are enabled | The runner |
| `gave_up` | The agent called `submit_result` with `give_up` | The agent |
| `skipped` | Excluded from work | Editing `symbols.txt`. Nothing else sets or reads it yet; selecting work for a batch is Phase 1. |
| `library` | Library or runtime code that is not meant to be decompiled | Editing `symbols.txt`; library signature matching from Phase 2 |

```
unstarted --agent--> in_progress --+--> matched
                                   +--> nonmatching --agent--> in_progress ...
                                   +--> gave_up
                                   +--> refused
                                   +--> back to the previous status (no attempt scored)
any status --edit symbols.txt--> skipped | library | unstarted (reset; history kept)
```

`decomp agent` runs on a function whatever its status. Skipping refused, skipped and library functions
belongs to the Phase 1 batch runner. Re-verifying matched functions after a change of toolchain or
flags is open.

## Toolchain registry

Compilers differ per machine, so they are registered per user rather than per project:

| Location | Used when |
|---|---|
| `$DECOMP_TOOLCHAINS` | The variable is set (any platform) |
| `%APPDATA%\decomp\toolchains.json` | Windows |
| `$XDG_CONFIG_HOME/decomp/toolchains.json`, or `~/.config/decomp/toolchains.json` when `XDG_CONFIG_HOME` is unset or empty | Linux and macOS |

Projects refer to a toolchain by name (`decomp.json` → `toolchain`); per-project overrides of registry
fields are planned. `decomp toolchain add <name>` creates or replaces an entry (`--kind`,
`--compiler`, and the repeatable `--wrapper`, `--flag`, `--include`, `--env NAME=VALUE`,
`--env-prepend NAME=VALUE`, plus `--description`). `decomp toolchain list` shows the registry's path
and entries, and `decomp toolchain test <name>` compiles a probe
([matching.md](matching.md#toolchains-and-the-registry)).

When clang-cl is installed, the registry also contains two auto-detected entries, unless the file
defines entries with the same names: `clang-cl-x86` (flags `--target=i686-pc-windows-msvc /Zl
/Brepro`) and `clang-cl-x64` (`--target=x86_64-pc-windows-msvc /Zl /Brepro`). Decomp looks for
clang-cl on `PATH`, then in `C:\Program Files\LLVM\bin` on Windows, or in `/usr/lib/llvm-<N>/bin`
(N from 30 down to 14), `/usr/local/opt/llvm/bin` and `/opt/homebrew/opt/llvm/bin` elsewhere.
Auto-detected entries are never written to the file.

| Field | Type | Meaning |
|---|---|---|
| `kind` | string | `msvc` (the default), `clang_cl`, `gcc` or `clang`. The first two get MSVC-style command lines. `gcc` and `clang` get `-c`/`-o` command lines, but their ELF objects cannot be diffed until Phase 7. |
| `compiler` | string | Required. The compiler executable: an absolute path, or a name found through `PATH`. With a `wrapper`, the path as the wrapped program sees it. |
| `wrapper` | array of strings | Optional prefix for the command line, such as `["wine"]` |
| `flags` | array of strings | Base flags for every compile with this toolchain, passed before the project's flags. Decomp adds `/nologo` and `/c` itself for MSVC-style kinds. |
| `include_dirs` | array of strings | Extra include directories, passed before the project's |
| `env` | object | Variables to set, as `"NAME": "value"`. A value may also be an array of strings, which is joined with the host's path separator. |
| `env_prepend` | object | Values to put in front of a variable's inherited value, joined with the host's path separator. Same value forms as `env`. |
| `description` | string | Free text |
| `timeout_seconds` | integer | Compile timeout; default 120 |

The example registry below has entries for clang-cl, VC6 and VS2008 on Windows, and VC6 under Wine
on Linux. The paths are examples. It is shown as Decomp writes it: keys sorted, and array values of
`env` and `env_prepend` stored joined.

```json
{
  "toolchains": {
    "clang-cl": {
      "compiler": "C:\\Program Files\\LLVM\\bin\\clang-cl.exe",
      "flags": [
        "--target=i686-pc-windows-msvc",
        "/Brepro"
      ],
      "kind": "clang_cl"
    },
    "vc6": {
      "compiler": "C:\\VS6\\VC98\\Bin\\CL.EXE",
      "env": {
        "INCLUDE": "C:\\VS6\\VC98\\Include;C:\\VS6\\VC98\\MFC\\Include;C:\\VS6\\VC98\\ATL\\Include",
        "LIB": "C:\\VS6\\VC98\\Lib;C:\\VS6\\VC98\\MFC\\Lib"
      },
      "env_prepend": {
        "PATH": "C:\\VS6\\Common\\MSDev98\\Bin;C:\\VS6\\VC98\\Bin"
      },
      "kind": "msvc"
    },
    "vc6-wine": {
      "compiler": "C:\\VS6\\VC98\\Bin\\CL.EXE",
      "env": {
        "INCLUDE": "C:\\VS6\\VC98\\Include",
        "LIB": "C:\\VS6\\VC98\\Lib",
        "WINEDEBUG": "-all",
        "WINEPATH": "C:\\VS6\\Common\\MSDev98\\Bin;C:\\VS6\\VC98\\Bin",
        "WINEPREFIX": "/home/me/.wine-vc6"
      },
      "kind": "msvc",
      "wrapper": [
        "wine"
      ]
    },
    "vs2008": {
      "compiler": "C:\\Program Files (x86)\\Microsoft Visual Studio 9.0\\VC\\bin\\cl.exe",
      "env": {
        "INCLUDE": "C:\\Program Files (x86)\\Microsoft Visual Studio 9.0\\VC\\include;C:\\Program Files\\Microsoft SDKs\\Windows\\v6.0A\\Include",
        "LIB": "C:\\Program Files (x86)\\Microsoft Visual Studio 9.0\\VC\\lib;C:\\Program Files\\Microsoft SDKs\\Windows\\v6.0A\\Lib"
      },
      "env_prepend": {
        "PATH": "C:\\Program Files (x86)\\Microsoft Visual Studio 9.0\\Common7\\IDE;C:\\Program Files (x86)\\Microsoft Visual Studio 9.0\\VC\\bin"
      },
      "kind": "msvc"
    }
  }
}
```

Notes on the entries:

- **VC6.** `cl.exe` needs `mspdb60.dll` from `Common\MSDev98\Bin` on `PATH`. Without it, the compiler
  fails before it reads the source. `INCLUDE` and `LIB` replace whatever the user's environment has.
- **VS2008.** `Common7\IDE` provides `mspdb80.dll`. The Windows SDK headers come from the SDK version
  that shipped with Visual Studio (`v6.0A`).
- **clang-cl.** Used for the test fixtures; on Linux the auto-detected `clang-cl-x86` and
  `clang-cl-x64` entries usually suffice. Use `--target=x86_64-pc-windows-msvc` for x64 targets. Code
  that includes Windows or CRT headers also needs MSVC's `INCLUDE`; the fixtures avoid headers
  entirely.
- **VC6 under Wine (planned).** `wrapper` runs the compiler through `wine`. `WINEPREFIX` selects the
  prefix that holds the installation, `WINEPATH` extends the Windows-side `PATH`, and
  `WINEDEBUG=-all` keeps Wine's diagnostics out of the compiler output. Decomp already passes the
  wrapper through, but it does not yet translate the file paths it passes to Windows form (`Z:\...`),
  so `cl.exe` receives host paths such as `/Fo/home/...`. Wine support is planned
  ([matching.md](matching.md#environment-and-wrappers)).

The compiler inherits Decomp's environment, with the entry's `env` and `env_prepend` applied on top.
For `msvc` toolchains Decomp also sets `_MSPDBSRV_ENDPOINT_` per compile, so that concurrent `/Zi`
compiles do not share one `mspdbsrv.exe`. Decomp does not yet remove `CL` and `_CL_`, which MSVC
reads as extra command-line options (planned), so make sure they are not set where Decomp runs.

## Git-friendliness

- **What to commit:** `decomp.json`, `symbols.txt`, `include/` and `src/functions/`. `init` writes a
  `.gitignore` that excludes `.decomp/`. Keep the target binary out of the repository unless you are
  allowed to redistribute it.
- **Deterministic output:** JSON with sorted keys and two-space indentation; `symbols.txt` sorted by
  address, with at least eight hex digits per address; LF line endings; UTF-8 without a BOM; a
  trailing newline on `decomp.json` and every line of `symbols.txt`.
- **Small diffs:** an agent session changes one line of `symbols.txt` (the function's status, best
  score, attempt count and spend), and a newly matched function adds one file. The detailed history
  stays in `.decomp/`.
- **Atomic writes:** whole files (`decomp.json`, `symbols.txt`, sources, `best.cpp`, `summary.json`,
  cache entries) are written to a temporary sibling and renamed into place, so an interrupted run
  never leaves a half-written file. Logs, transcripts, `attempts.jsonl` and `notes.md` are appended.
  While a session runs, its function is `in_progress` in `symbols.txt`; after a crash that status
  remains until the next session on the function ends.
- **Machine independence:** toolchains are referenced by name, and paths in `decomp.json` are relative
  when a relative path exists. Nothing secret is stored in the project.
- **Merge-friendly:** the symbol file is line-based with one symbol per line, so concurrent work on
  different functions merges without conflicts.
- **Verified means committed as verified:** sources are written exactly as they matched, so the
  committed file is the file that compiled to the original bytes.
