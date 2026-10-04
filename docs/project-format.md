# Project format

A Decomp project is a directory that holds everything about decompiling one target binary. The parts
meant for git are its configuration (`decomp.json`), its symbol database (`symbols.txt`), shared
headers (`include/`) and verified sources (`src/functions/`). Working data stays local under a
gitignored `.decomp/` directory: per-function attempt history and notes, run logs and transcripts,
build directories and the compile cache. Compilers are machine-specific, so they live in a
**user-level toolchain registry** outside the project, and projects refer to them by name. Everything
Decomp writes is deterministic (sorted keys, sorted lines, stable formatting) and atomic (temporary
file plus rename), so a project diffs and merges cleanly. This document specifies each file, the
function status values and the toolchain registry.

Status: the first slice implements this layout (step 9 of the
[roadmap](roadmap.md#first-working-slice)). Translation-unit organization replaces the one-file-per-
function source layout in Phase 3. Key names follow the plan, and details marked open may change during
implementation.

## Layout

```
<project>/
    decomp.json             configuration                                   (committed)
    symbols.txt             symbols: addresses, names, kinds, sizes, statuses (committed)
    include/                shared headers                                  (committed)
    src/functions/          verified sources, one .cpp per function          (committed)
    .decomp/                working data                                    (gitignored)
        functions/<fn>/     attempts.jsonl, best.cpp, notes.md
        runs/<run-id>/      events.jsonl, sessions/<fn>.jsonl, summary.json
        build/              per-compile working directories
        cache/              compile cache
```

The target binary and its PDB usually live outside the project and are referenced by path. They are
often not redistributable, and `target.sha1` lets every checkout confirm that it has the right file.

`decomp init <binary>` creates `decomp.json` and `symbols.txt` in the current directory (or in
`--project <dir>`). Every other command finds the project by searching upward from the current
directory for `decomp.json`, unless `--project` is given.

## `decomp.json`

The example below is shown exactly as Decomp writes it: keys sorted, two-space indentation, a trailing
newline.

```json
{
  "agent": {
    "budgets": {
      "function": {
        "max_turns": 30,
        "max_usd": 5.0
      },
      "run": {
        "max_usd": 50.0
      }
    },
    "effort": "high",
    "fallbacks": "default",
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
| `version` | integer | Format version; currently `1` |
| `target.path` | string | The target binary, relative to the project directory (absolute paths are accepted) |
| `target.sha1` | string | SHA-1 of the target. It is checked whenever the project loads, and runs are refused on a mismatch. |
| `target.pdb` | string or `null` | The PDB. Its GUID and age must match the image's CodeView record. |
| `toolchain` | string | The name of a toolchain in the [registry](#toolchain-registry) |
| `flags` | array of strings | The target's code-generation flags, passed after the toolchain's base flags |
| `include_dirs` | array of strings | Project include directories (relative), passed to the compiler as `/I` |
| `agent` | object | Agent settings ([agent.md](agent.md#configuration)). Omitted keys use the defaults. |

`decomp init` fills in `target` (path, SHA-1, and the PDB when it is found next to the binary or at
the path named in its CodeView record) and leaves `toolchain` and `flags` for the user to set.
Recovering flags automatically is part of Phase 6. The API key is never stored here.

## `symbols.txt`

One symbol per line, sorted by address:

```
<address> <kind> <name> [<key>=<value> ...]
```

An x86 example:

```
0x00401000 func ?add@@YAHHH@Z size=0x8 status=matched source=pdb
0x00401030 func ?sum_array@@YAHPBHH@Z size=0x17 status=nonmatching source=pdb
0x00401050 func ?Update@Player@@QAEXM@Z size=0x8c status=unstarted source=pdb
0x004010e0 func sub_004010e0 size=0x41 status=unstarted source=analysis
0x00401200 func _memset size=0x5e status=library source=pdb
0x00402000 import __imp__MessageBoxA@16 size=0x4 source=import
0x00403010 string ??_C@_0M@KPLPPDAC@Hello?5world?$AA@ size=0xc source=pdb
0x00403020 float __real@3f800000 size=0x4 source=pdb
0x00404000 data ?g_player@@3PAVPlayer@@A size=0x4 source=agent
```

An x64 line uses 16 hex digits:

```
0x0000000140001000 func ?add@@YAHHH@Z size=0x4 status=matched source=pdb
```

| Field | Meaning |
|---|---|
| address | Virtual address (not RVA), `0x` plus 8 hex digits for PE32 or 16 for PE32+ |
| kind | `func`, `data`, `string`, `float`, `import` or `label` |
| name | The decorated name exactly as the linker sees it: `?Update@Player@@QAEXM@Z`, `_main`, `_WinMain@16`, `@fn@8`, `__imp__MessageBoxA@16`. Decorated names contain no whitespace. Demangled forms are derived, not stored. |
| `size=` | Size in bytes, in hex |
| `status=` | For `func` only: a [function status](#function-status). `in_progress` is never written. |
| `source=` | Where the name came from: `pdb`, `export`, `import`, `user` or `agent` |

Notes:

- Functions found by recursive descent without any name get a placeholder name, `sub_<address>`, and
  `source=analysis`. Neither the placeholder scheme nor the `analysis` source is in the original plan
  (whose list ends at `agent`); both are open.
- The slice keeps one symbol per address. Recording aliases created by identical-COMDAT folding is
  open ([matching.md](matching.md#opticf-folding)).
- Decomp preserves unknown `key=value` attributes when it rewrites the file. Blank lines and lines
  starting with `#` are ignored (planned).
- The best match percentage, attempt counts and spend are not stored here. They are derived from
  `.decomp/`, which keeps this file's diffs to real changes.

## `include/`

Shared headers: declarations of types, globals and functions used by more than one source. In the
slice, the agent writes self-contained translation units and does not change headers. Project headers
are available through `include_dirs`, and their contents are part of the compile cache key
([matching.md](matching.md#compile-cache)). From Phase 3, the `define_type` tool edits headers under
the approval policy. From Phase 4, headers are the source of truth for types, and their layouts are
checked against the PDB.

## `src/functions/`

Verified sources, one `.cpp` per function in the slice, named `<fn>.cpp`. Each file is **exactly** the
translation unit that verified byte-exact. Decomp adds no banner or comment, because even a comment
can shift `__LINE__`. The metadata (when it matched, toolchain, flags, session) lives in `.decomp/`.
Phase 3 groups functions into translation units, with one source per unit as in the original program.

## `.decomp/`

Working data, local to the machine and gitignored. Deleting it loses history and cached compiles but
never verified sources or symbols.

### Function keys (`<fn>`)

`<fn>` is a filesystem-safe key that sorts by address and stays readable, for example
`00401030_sum_array` or `00401050_Player_Update`: the address digits, then a sanitized short name.
The same key names the history directory, the session transcripts and the source file. The exact
format, and what happens to the key when a symbol is renamed, are open.

### `functions/<fn>/`

| File | Contents |
|---|---|
| `attempts.jsonl` | One JSON object per compile attempt, from the agent or the user. Fields: attempt number (increasing across sessions), time, run and session IDs, `origin` (`agent` or `user`), toolchain and flags fingerprint, the full source and its SHA-1, whether it compiled, the diagnostic count, `match_percent`, `exact`, `byte_exact`, row counts, hint kinds, compile and diff durations, `cached`. |
| `best.cpp` | The best attempt so far: highest `match_percent`, and among equal scores the most recent. For a matched function it equals the verified source. |
| `notes.md` | Notes from `record_note` and from the user, each under a heading with the time, the session and the author |

This history feeds future briefs: the best previous attempt and the notes go into the next session's
first message ([agent.md](agent.md#prompts)).

### `runs/<run-id>/`

A run is one invocation of the agent over a selection of functions. The run ID is a UTC timestamp plus
a short random suffix, such as `20261004-153012-3f9a` (format open).

| File | Contents |
|---|---|
| `events.jsonl` | Every event of the run, one JSON object per line, in sequence order. Opening a past run replays this file ([ui.md](ui.md#replay-of-past-runs)). |
| `sessions/<fn>.jsonl` | The full transcript of the session for one function: the frozen prefix, every message as sent, per-turn response metadata, tool executions and the outcome ([agent.md](agent.md#transcripts-and-event-logs)). It never contains the API key. |
| `summary.json` | Totals for the run, read by `decomp status` and the dashboards |

An example `summary.json`:

```json
{
  "config": {
    "effort": "high",
    "model": "claude-opus-5-5"
  },
  "duration_s": 1834,
  "finished": "2026-10-04T16:02:46Z",
  "outcomes": {
    "budget_exhausted": 1,
    "error": 0,
    "gave_up": 1,
    "matched": 6,
    "refused": 0
  },
  "prompt_version": 1,
  "run": "20261004-153012-3f9a",
  "started": "2026-10-04T15:30:12Z",
  "tokens": {
    "cache_read": 4120000,
    "cache_write": 310000,
    "input": 52000,
    "output": 640000
  },
  "turns": 96,
  "usd": 15.38
}
```

### `build/` and `cache/`

- `build/` holds one fresh working directory per compile (`candidate.cpp`, `candidate.obj`, the
  compile log and the PDB if one is produced). Recent directories are kept for inspection and older
  ones are pruned automatically; the retention policy is open.
- `cache/` is the content-addressed compile cache: `cache/<first two hex digits>/<sha1>.obj` plus a
  `.json` with the `CompileOutput` metadata. Deleting it is always safe
  ([matching.md](matching.md#compile-cache)).

## Function status

| Status | Meaning | Set by |
|---|---|---|
| `unstarted` | No attempts yet | `init`, or a user reset |
| `in_progress` | A session is working on it. This exists only in live run state and is never written to `symbols.txt`. | The runner |
| `nonmatching` | At least one attempt compiled, none byte-exact; the best percentage is in the history | The runner, when a budget runs out, a session is stopped, or an error occurs after attempts |
| `matched` | A verified byte-exact source is in `src/functions/` | `submit_result` verification, or "verify and save" in manual mode |
| `refused` | The model declined the request, including after fallbacks. Later runs skip it until it is requeued. | The runner |
| `gave_up` | The agent called `submit_result` with `give_up` and a reason | The agent |
| `skipped` | The user excluded it | The user |
| `library` | Library or runtime code that is not meant to be decompiled | The user; library signature matching from Phase 2 |

```
unstarted --run--> in_progress --+--> matched
                                 +--> nonmatching --run--> in_progress ...
                                 +--> gave_up
                                 +--> refused
any status --user--> skipped | library | unstarted (reset; history kept)
```

A matched function stays matched unless the user resets it. Re-verifying matched functions after a
change of toolchain or flags is open.

## Toolchain registry

Compilers differ per machine, so they are registered per user rather than per project:

| Platform | Location |
|---|---|
| Windows | `%APPDATA%\decomp\toolchains.json` |
| Linux | `~/.config/decomp/toolchains.json` (`$XDG_CONFIG_HOME/decomp/toolchains.json` when that variable is set) |

Projects refer to a toolchain by name (`decomp.json` → `toolchain`). A project can override fields of
a registry entry, for example to add include directories; the override schema is open.
`decomp toolchain list` shows the registry, and `decomp toolchain test` runs the health checks
([matching.md](matching.md#toolchains-and-the-registry)).

| Field | Type | Meaning |
|---|---|---|
| `kind` | string | `msvc` or `clang_cl`; later `gcc` and `clang` |
| `compiler` | string | The compiler executable. With a `wrapper`, the path as the wrapped program sees it (for Wine, a Windows path inside the prefix). |
| `wrapper` | array of strings | Optional prefix for the command line, such as `["wine"]` |
| `env.set` | object | Variables to set; a `null` value unsets the variable |
| `env.prepend` | object of arrays | Entries to put in front of the inherited value, joined with the host's path separator |
| `flags` | array of strings | Base flags for every compile with this toolchain |
| `include_dirs` | array of strings | Extra include directories |
| `obj_format` | string | `coff` (ELF later) |

The example registry below has entries for VC6, VS2008 and clang-cl on Windows, and VC6 under Wine on
Linux. The paths are examples. Keys are sorted, as Decomp writes them.

```json
{
  "toolchains": {
    "clang-cl": {
      "compiler": "C:\\Program Files\\LLVM\\bin\\clang-cl.exe",
      "flags": [
        "--target=i686-pc-windows-msvc",
        "/nologo"
      ],
      "kind": "clang_cl",
      "obj_format": "coff"
    },
    "vc6": {
      "compiler": "C:\\VS6\\VC98\\Bin\\CL.EXE",
      "env": {
        "prepend": {
          "PATH": [
            "C:\\VS6\\Common\\MSDev98\\Bin",
            "C:\\VS6\\VC98\\Bin"
          ]
        },
        "set": {
          "INCLUDE": "C:\\VS6\\VC98\\Include;C:\\VS6\\VC98\\MFC\\Include;C:\\VS6\\VC98\\ATL\\Include",
          "LIB": "C:\\VS6\\VC98\\Lib;C:\\VS6\\VC98\\MFC\\Lib"
        }
      },
      "flags": [
        "/nologo"
      ],
      "kind": "msvc",
      "obj_format": "coff"
    },
    "vc6-wine": {
      "compiler": "C:\\VS6\\VC98\\Bin\\CL.EXE",
      "env": {
        "set": {
          "INCLUDE": "C:\\VS6\\VC98\\Include",
          "LIB": "C:\\VS6\\VC98\\Lib",
          "WINEDEBUG": "-all",
          "WINEPATH": "C:\\VS6\\Common\\MSDev98\\Bin;C:\\VS6\\VC98\\Bin",
          "WINEPREFIX": "/home/me/.wine-vc6"
        }
      },
      "flags": [
        "/nologo"
      ],
      "kind": "msvc",
      "obj_format": "coff",
      "wrapper": [
        "wine"
      ]
    },
    "vs2008": {
      "compiler": "C:\\Program Files (x86)\\Microsoft Visual Studio 9.0\\VC\\bin\\cl.exe",
      "env": {
        "prepend": {
          "PATH": [
            "C:\\Program Files (x86)\\Microsoft Visual Studio 9.0\\Common7\\IDE",
            "C:\\Program Files (x86)\\Microsoft Visual Studio 9.0\\VC\\bin"
          ]
        },
        "set": {
          "INCLUDE": "C:\\Program Files (x86)\\Microsoft Visual Studio 9.0\\VC\\include;C:\\Program Files\\Microsoft SDKs\\Windows\\v6.0A\\Include",
          "LIB": "C:\\Program Files (x86)\\Microsoft Visual Studio 9.0\\VC\\lib;C:\\Program Files\\Microsoft SDKs\\Windows\\v6.0A\\Lib"
        }
      },
      "flags": [
        "/nologo"
      ],
      "kind": "msvc",
      "obj_format": "coff"
    }
  },
  "version": 1
}
```

Notes on the entries:

- **VC6.** `cl.exe` needs `mspdb60.dll` from `Common\MSDev98\Bin` on `PATH`. Without it, the compiler
  fails before it reads the source. `INCLUDE` and `LIB` replace whatever the user's environment has.
- **VS2008.** `Common7\IDE` provides `mspdb80.dll`. The Windows SDK headers come from the SDK version
  that shipped with Visual Studio (`v6.0A`).
- **clang-cl.** Used for the test fixtures. On Linux the compiler path is a native one (for example
  `/usr/lib/llvm-18/bin/clang-cl`). Use `--target=x86_64-pc-windows-msvc` for x64 targets. Code that
  includes Windows or CRT headers also needs MSVC's `INCLUDE`; the fixtures avoid headers entirely.
- **VC6 under Wine (later).** `wrapper` runs the compiler through `wine`. `WINEPREFIX` selects the
  prefix that holds the installation, `WINEPATH` extends the Windows-side `PATH`, and `WINEDEBUG=-all`
  keeps Wine's diagnostics out of the compiler output. Decomp translates the file paths it passes to
  Windows form (`Z:\...`). Wine support is planned after the slice
  ([matching.md](matching.md#environment-and-wrappers)).

Decomp removes `CL` and `_CL_` from every compile's environment, whatever the entry says, because MSVC
reads them as extra options.

## Git-friendliness

- **What to commit:** `decomp.json`, `symbols.txt`, `include/` and `src/functions/`. Add `.decomp/` to
  the project's `.gitignore`, and keep the target binary out of the repository unless you are allowed
  to redistribute it.
- **Deterministic output:** JSON with sorted keys and two-space indentation; `symbols.txt` sorted by
  address with fixed-width hex; LF line endings; UTF-8 without a BOM; a trailing newline on every file.
- **Small diffs:** a status change touches one line, and a newly matched function adds one file and
  changes one line. Volatile data (percentages, attempts, spend) stays in `.decomp/`.
- **Atomic writes:** every file is written to a temporary sibling and renamed into place, so an
  interrupted run never leaves a half-written file. `in_progress` is never persisted, so a crash
  leaves no stale state.
- **Machine independence:** toolchains are referenced by name, and paths in `decomp.json` are relative.
  Nothing machine-specific or secret is stored in the project.
- **Merge-friendly:** the symbol file is line-based with one symbol per line, so concurrent work on
  different functions merges without conflicts.
- **Verified means committed as verified:** sources are written exactly as they matched, so the
  committed file is the file that compiled to the original bytes.
