# Project format

A Decomp project is a directory that holds everything about decompiling one target binary. The parts
meant for git are its configuration (`decomp.json`), its symbol database (`symbols.txt`), its
translation units (`units.txt`), shared headers (`include/`) and verified sources (`src/functions/`). Working data stays local under a
gitignored `.decomp/` directory: per-function attempt history and notes, run logs and transcripts,
build directories and the compile cache. Compilers are machine-specific, so they live in a
**user-level toolchain registry** outside the project, and projects refer to them by name. Decomp
writes JSON with sorted keys and `symbols.txt` sorted by address, and it replaces whole files through
a temporary file plus rename, so a project diffs and merges cleanly. This document specifies each
file, the function status values and the toolchain registry.

Status: implemented in the first slice (step 9 of the [roadmap](roadmap.md#first-working-slice)),
extended in Phase 1 with locks, change logs and batch-run directories, and in Phase 3 with translation
units: `units.txt` and one source per unit. Anything marked *planned* does not exist yet.

## Layout

```
<project>/
    decomp.json                 configuration                                     (committed)
    symbols.txt                 symbols: addresses, names, kinds, sizes, status   (committed)
    units.txt                   translation units in link order                   (committed)
    include/                    shared headers                                    (committed)
    src/<unit source>           unit sources: a unit's matched functions          (committed)
    src/functions/              verified sources of functions without one         (committed)
    .gitignore                  written by init: /.decomp/                        (committed)
    .decomp/                    working data                                      (gitignored)
        functions/<fn>/         attempts.jsonl, best.cpp, notes.md
        runs/<run-id>/          run.json, summary.json, events.jsonl, sessions/, run.lock
        changes.jsonl           every file Decomp wrote into the project
        blobs/<sha1>            the content those writes replaced
        symbols.log.jsonl       every symbol edit
        project.lock            held while symbols.txt and the logs are written
        active-run.lock         held by the process that runs agent sessions
        build/                  per-compile working directories
        cache/objects/          compile cache
        relink/                 the last relink: objects/, libs/, out/, link.rsp, result.json
```

`<fn>` is the function's [key](#function-keys-fn), such as `add_401060`.

The target binary and its PDB usually live outside the project and are referenced by path. They are
often not redistributable, and `target.sha1` lets every checkout confirm that it has the right file.

`decomp init <binary>` creates the project in the current directory, or in `--dir <dir>` (created if
needed). It writes `decomp.json`, `symbols.txt`, `units.txt` and `.gitignore`, creates the empty
`include/` and `src/functions/` directories, and refuses to run where a `decomp.json` already exists. Its other
options are `--toolchain <name>`, `--flag <flag>` (repeatable), `--pdb <file>` and `--map <file>` (the
build's link map, see [Link maps](#link-maps)). Every other command
finds the project by searching upward from the current directory for `decomp.json`, or upward from
the directory given with the global option `-C <dir>` (`--project <dir>`). Global options may come
before or after the command name (`decomp -C game status` or `decomp status -C game`); `init` creates
the project in `--dir`, else in the `-C` directory, else in the current directory.

### Link maps

A link map (`link.exe /MAP`, also written by `lld-link /map`) names every public and static symbol of
the build with its address and object file. For a target without a usable PDB, VC6 games among
them, it is the best source of names and function starts there is. `decomp init <binary> --map
<file>` reads it when the project is created; `decomp map import <file>` reads it into an existing
project. Either way the map must describe the target: its entry point has to be the image's (a map of
another build is refused), and a different link timestamp gets a warning.

- Every symbol takes the map's name unless a PDB, the agent or the user named it (`source=map`; the
  map's name becomes an alias), and records its object file (`obj=`). Static symbols are marked
  `static`. Symbols the map places in code are functions when the map flags them `f`, as link.exe
  does; without the flag they are labels (assembly labels), which start no function. A map without
  any flags (lld-link writes none) makes every symbol in code a function.
- The map's function starts guide the analysis, which measures every function's size again.
- `decomp map import` (and `decomp analyze`, which does the same without a map) refuses to run while
  a run is active. Functions only the analysis knew, with no work recorded, are found again from
  scratch; the others are kept. A function the map renames keeps its work: its directory under
  `.decomp/functions/` and its matched source under `src/functions/` are renamed with it. Both
  commands print what changed: functions added, removed, resized and renamed.

## `decomp.json`

The example below is shown exactly as `decomp init ../bin/GAME.EXE --toolchain vs2008 --flag /O2
--flag /EHsc --flag /MD` writes it: keys sorted, two-space indentation, a trailing newline.

```json
{
  "agent": {
    "approvals": {},
    "effort": "high",
    "fallbacks": true,
    "max_minutes_per_function": 30,
    "max_tokens_per_function": 0,
    "max_turns": 40,
    "max_usd_per_function": 5.0,
    "max_usd_per_run": 0.0,
    "model": "claude-opus-5-5",
    "workers": 4
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
| `target.sha1` | string | SHA-1 of the target. When the project loads its target and the SHA-1 differs, every command that opens the project fails with an error: the project's symbols and results describe the old binary. If the new binary is intended, update the value. |
| `target.pdb` | string | Optional. The PDB, relative to the project directory. If it is set but cannot be used (missing, unreadable, or its GUID and age do not match the image's CodeView record), loading fails. If it is absent, Decomp looks next to the binary for the file named in the CodeView record, then for `<stem>.pdb`, and ignores a PDB that does not match. |
| `toolchain` | string | The name of a toolchain in the [registry](#toolchain-registry) |
| `flags` | array of strings | The target's code-generation flags, passed after the toolchain's base flags |
| `include_dirs` | array of strings | Project include directories (relative), passed to the compiler as `/I` |
| `agent` | object | Agent settings ([agent.md](agent.md#configuration)). Omitted keys use the defaults shown above. |
| `link` | object | Optional. How `decomp relink` links the target again ([Relinking](#relinking)): `linker` (a path or a name; default the toolchain's: lld-link for clang-cl, link.exe beside cl.exe for MSVC), `flags` (passed after the flags taken from the image, such as `/opt:icf` or an `/alternatename:` the original link had), `libraries` (linked after the objects: import libraries, static libraries; relative to the project, or a bare name the linker finds through `LIB`) |

`decomp init` fills in `target` (path, SHA-1, and the PDB when one was given with `--pdb` or found
next to the binary and matched), sets `toolchain` and `flags` from `--toolchain` and `--flag`, and
sets `include_dirs` to `["include"]`. Without `--toolchain` or `--flag` it prints a reminder to set
them in `decomp.json`. Recovering flags automatically is part of Phase 6. The API key is never stored
here.

## `symbols.txt`

One symbol per line, sorted by address, after a header comment that names the fields:

```
# decomp symbols: <address> <kind> <name> [size=] [pdb=] [static] [source=] [obj=] [status=] [best=] [attempts=] [cost=]
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
| `source=` | Where the name came from, in increasing order of trust: `analysis`, `library` (a static library's function the code matches, see `decomp lib match`), `import`, `export`, `map`, `pdb_public`, `pdb`, `agent`, `user`. A line without `source=` is read as `user`. |
| `obj=` | The symbol's [unit](#unitstxt): the object file it was linked from, as a link map names it (`main.obj`, `LIBC:printf.obj` for a library member). It comes from the PDB's section contributions, the link map, a library match or the analysis. Edit it to move a symbol to another unit. |
| `status=` | For functions: a [function status](#function-status); omitted for `unstarted` |
| `best=` | For functions: the best match percentage reached by agent sessions, one decimal |
| `attempts=` | For functions: the number of compile attempts made by agent sessions |
| `cost=` | For functions: the agent spend on the function so far, in USD |

Notes:

- `init` writes every symbol Decomp derives from the image, its PDB, its link map and the analysis:
  imports, exports, PDB public symbols, procedures and data, the map's symbols, the functions the
  analysis finds without a PDB, `.pdata` entries on x64 (functions without any other name are called
  `sub_<hex address>`), `__ImageBase`, and the entry point (`entry` when it has no other name). Exports and the entry point that land on incremental-linking thunks are moved
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

## `units.txt`

The program's translation units: the object files it was linked from, in link order, one per line
after a header comment. Each symbol's `obj=` in `symbols.txt` names its unit. `init` derives them;
`decomp units derive` derives them again (`--force` when the project has units from a PDB, a map or
the user), and `decomp units` lists them with the progress and spend of their functions (`--json`
for the details). The file `init` writes for the x86 test fixture:

```
# decomp units, in link order: <name> [kind=] [source=] [origin=]
basic.obj source=src/basic.cpp origin=pdb
other.obj source=src/other.cpp origin=pdb
kernel32:kernel32.dll kind=import origin=pdb
Import:kernel32.dll kind=import origin=pdb
"* Linker *" kind=linker origin=pdb
```

| Field | Meaning |
|---|---|
| name | The object as a link map names it: `player.obj`, `LIBCMT:printf.obj` for a member of `LIBCMT.lib` (a library match's `LIBCMT.LIB:printf.obj` is written so too), `kernel32:KERNEL32.dll` for an import library's member, `Import:KERNEL32.dll`, `* Linker *`. Quoted like a symbol name when it contains a space. |
| `kind=` | `code` (the default; the program's own code), `library` (a static library's member), `import` (import stubs and descriptors) or `linker` (what the linker made) |
| `source=` | A code unit's source file, relative to the project: the file the PDB's line information names for the module (`src/basic.cpp`), else the object's stem with `.c` when its functions all have C names and `.cpp` otherwise. Directories of the original path are added where two units would share a file name (never a drive, `.` or `..`; characters other than letters, digits and `_-.+` and spaces become `_`). A source must be a C or C++ file (`.c`, `.cc`, `.cpp`, `.cxx`) under `src/` but not `src/functions/`, written with forward slashes and without `.` or `..`: `units.txt` with another is refused. |
| `origin=` | Where the unit came from, in increasing order of trust: `analysis`, `map`, `pdb`, `user`. A line without `origin=` was written by hand (`user`). |

Where the units come from, best first:

- **The PDB.** One unit per module, in module order, which is the link order; each function and data
  symbol belongs to the module whose section contribution holds its address. The units are exactly
  the PDB's module list.
- **A link map.** The object files the map names for its symbols (`init --map`, `decomp map import`),
  ordered by their first address. A function the map does not name belongs to the unit of the
  functions around it, or of the function before it when they differ.
- **The analysis**, with neither. Functions a library match named keep their library member, linker
  and import thunks go to `* Linker *` and the import units, and the rest is cut into units of
  consecutive functions: where no call, data both use or data placed close together ties the two
  sides, once the unit has four functions, and where the source file the functions' strings name
  (`__FILE__` in asserts and log messages) changes. A unit that names its source file takes its name
  (`player.obj`, `src/player.cpp`); the others are called after their first function
  (`unit_00401000.obj`). This is a starting point to correct by hand: on the Zydis corpus, which has
  no such strings, a third of the true boundaries are found and a fifth of the cuts are true ones
  (`decomp bounds <binary> --truth <pdb> --units` measures it).

`decomp analyze`, `decomp map import` and `decomp lib match` derive the units again while the analysis
alone made them, so a map or a library match replaces the guesses. Units from a PDB, a map or the user
stay; `decomp units derive --force` replaces all but the user's. To split, merge or rename units, edit
`units.txt` (a line without `origin=` stays through derivations) and the `obj=` of the symbols that
move.

## `include/`

Shared headers: declarations of types, globals and functions used by more than one source. `init`
creates the directory empty and lists it in `include_dirs`. Sessions write self-contained translation
units, and their brief lists the files found in the include directories. The headers enter the compile
cache key through their paths, sizes and modification times
([matching.md](matching.md#compile-cache)).

The agent's `define_type` tool adds a type to a header here (`types.h` unless it names another, such as
`game/player.h`), or replaces the header's definition of the type in place, under the approval policy
([agent.md](agent.md#define_type)). A new header starts with `#pragma once`. Before it is written, the
new header must compile and name the type, and every verified source that includes it (unit sources and
functions' own files) must keep its byte-exact functions byte-exact. Each write is recorded in
[`.decomp/changes.jsonl`](#changesjsonl-and-blobs) and can be reverted. Header names are relative to
`include/`, written with forward slashes, without `.` or `..`, and end in `.h`, `.hh`, `.hpp` or
`.hxx`.

The headers are the source of truth for the program's types. What the compiler makes of them is read
back from its own debug information (`compile_header_types()` in `project/types.hpp`): one translation
unit includes every header, in path order, and points a variable at each type they declare (at their
top level, in named namespaces and in `extern "C"` blocks: struct, class, union and enum definitions and
forward declarations, typedefs and `using` aliases), so that the compiler writes each type's
definition. It is compiled with the project's toolchain, flags and include directories plus `/Z7` (and,
for clang-cl, `-fstandalone-debug`, without which clang writes a class's definition only where its
vtable or constructor is), and the layouts come from the object's type records (`.debug$T`): size,
bases, vfptr and vbptr, vtable slots, fields with offsets, bits and types, enumerators
([architecture.md](architecture.md#types)). The compile is cached like any other, keyed on the headers'
contents.

- `decomp types` lists the types the headers declare, with their kind, size and header, and whether
  the target's PDB has the same layout; `--pdb` lists the PDB's types instead (`--filter` narrows
  either).
- `decomp types show <name>` prints a type's layout from the headers (else, or with `--pdb`, from the
  PDB), and how the two differ.
- `decomp types check` compares every declared type with the PDB's layout of the same name and lists
  each difference ("field speed at +0x8, expected +0x4", "virtual slot 1: reset, expected none"); it
  exits with 1 when any type differs.
- `decomp types import <name>...` declares the PDB's types in a header (`types.h` unless `--header`
  names another), with what they need: the types they hold or derive from are defined first, the ones
  they only point to are declared forward, a nested type comes with its class, and the headers that
  define the existing types they use are included. `--all` takes every type the PDB defines except
  templates, anonymous types, the compiler's own and those defined in compilers' and SDKs' headers
  (where the PDB records the file, Visual C++ 8.0 and later); `--from <text>` keeps those defined in
  files whose path contains the text. `--dry-run` prints the header instead of writing it. Types the
  headers define already are left alone. Before the header is written it is compiled and every type it
  defines must have the PDB's layout, and the verified sources that include it must keep their
  byte-exact functions; the write is recorded in [`.decomp/changes.jsonl`](#changesjsonl-and-blobs).

- `decomp types skeletons [<class>...]` declares class skeletons from the target's RTTI, for targets
  without a PDB (`analysis/skeletons.hpp`): every class the RTTI names unless some are given. A
  skeleton has the class's bases where RTTI puts them, the vfptr it adds, and the virtual methods it
  introduces in slot order: named after the functions in its vftable, with the signatures their
  decorated names give (`virtual int area() const;`), a destructor for a deleting destructor, `vf2`
  for a function without a name, `= 0` for `_purecall`; overrides where the base's slot has the same
  method, and the other member functions the symbols name. Fields are unknown: a class that another
  base follows gets a `char` array that fills it up to that base's offset. Before the header is
  written, the skeletons are compiled, and each class's vtables (their entry counts) and its direct
  bases' offsets must be what RTTI says.

The declarations rebuild what the type records flatten (`analysis/declarations.hpp`): the members of
anonymous unions and structs (a member that starts back inside the previous one opens a union), anonymous
member types (inline, with the member), unnamed bitfields where bits are skipped and `: 0` where a unit
ends early, `#pragma pack` when offsets are tighter than the fields' alignment, `__declspec(align)` when
the size is larger than they need, methods with their signatures (virtual ones in slot order,
`__stdcall` and the like where not the default, `= 0`), static members, nested types, and enums with
explicit values. Struct and class keep their keyword (decorated names differ, `PAUPlayer@@` and
`PAVPlayer@@`); every member is public; enums are plain (`enum class` leaves no trace in the records).

Session briefs list the types the headers declare and show the layouts of the types the function's
signature names in the PDB (its class, and what its parameters and return value are or point to): the
headers' layout when a header declares the type, else the PDB's. Toolchains of the GCC kinds write DWARF,
which is not read; Visual C++ before 7.0 writes 16-bit type indices, which are not read either.

## Unit sources

A code unit's matched functions live in its source file (`source=` in `units.txt`, such as
`src/basic.cpp`), as the original program's source file held them. A unit source is a prelude
(directives, declarations, types, data) followed by the matched functions in address order, each after a
marker line with its address:

```cpp
#define NOINLINE __declspec(noinline)
extern int g_counter;
int add(int a, int b);
static int s_calls = 0;
NOINLINE static int helper(int x);

// FUNCTION: 0x00401060
NOINLINE int add(int a, int b) { return a + b + g_counter; }

// FUNCTION: 0x004010f0
NOINLINE int dispatch(int op, int v) {
    ...
}

// FUNCTION: 0x00401160
NOINLINE static int helper(int x) {
    ++s_calls;
    return x * 3 + 1;
}
```

A function joins its unit's source by composition: the translation unit that verified byte-exact for
it gives its definition (with the `#pragma` and `#line` directives right before and after it) as the
function's entry, and its other items join the prelude unless the unit has them already: the same
item (comments and spacing aside), a definition of the same function, or a `static` declaration of a
function the item declares without `static` (which would conflict). Conditional directives (`#if` ...
`#endif`) always join. A definition another function's source brought into the prelude (a static helper
its caller needs) gives way to the function's own entry and stays behind as a declaration, so the
functions before it still see it. The definition must be at the top level of its source: a function
defined inside a class or namespace block cannot be placed yet.

The unit source is then compiled once, as its file name (`.c` compiles as C), and every function in it
is diffed against the target. A unit source is written only when the functions it adds are byte-exact
in it and every function that was byte-exact in it before still is: the unit source is the proof, since
composing can change what a function compiles to (an earlier function's definition may now be visible
to it, for example).

Agent sessions and the GUI's Verify and save put a match into its unit's source when the unit is a
code unit with a source path that the project trusts: it comes from a PDB or a map, the user wrote
it, or its source file exists already ([agent.md](agent.md#translation-units)). The functions of a unit
that the analysis only guessed go to `src/functions/` until `decomp units emit` (or the user) starts
the unit's source. A session composes onto the unit source as it is on disk, and the write fails if
another writer changed the file in between; the session then composes onto the new content and
verifies again.

- `decomp units verify [unit...]` compiles the unit sources and diffs every function they hold; the exit
  code is 2 when one is not byte-exact.
- `decomp units emit [unit...]` moves the verified sources of matched functions (`src/functions/`) into
  their units' sources: per unit, in address order, for every code unit with a source path (guessed
  ones too). Functions that do not compose, or are not byte-exact in the unit, keep their own files
  and the rest is tried again without them. A unit source is written only when every function in it is
  byte-exact. Each write and each removed file is recorded in `changes.jsonl`.
- `decomp units check [unit...]` checks the unit as a whole ([matching.md](matching.md#units)): it
  compiles the unit source, places the object's code and data where the linker would put them and
  compares them with the target. A unit is **complete** when every function the unit has (its `obj=`)
  is in its source and the object fills the unit's place in the image exactly: its functions in the
  same order, its data with the same contents in the same places, the same strings and constants
  (pooled ones shared with other units), and the same exception-handling and unwind tables. The data
  is part of the prelude: the unit's globals defined there with their initial values, in the order the
  original source had them. The exit code is 2 when a unit is not complete.
- `decomp units compose <unit> <file> [--dry-run]` makes a unit's source from a whole translation unit
  (an original source file, one written by hand): each function of the unit is composed from it after
  its marker and the rest becomes the prelude. The result is checked like `units check`, written
  (recorded in `changes.jsonl`), and when the unit is complete its functions are marked matched.

## Relinking

`decomp relink` links the whole target again and compares the result with it: the verification of the
whole program, not just of its functions. Every complete code unit is linked from its source; every
other unit (incomplete, without a source, a static library's member) from a *split object* that
carries its original code and data; imports come from import libraries written from the target's import
table (or `link.libraries`). The original linker links them in `units.txt` order with flags taken from
the target's headers ([architecture.md](architecture.md#key-flow-decomp-relink)).

```
$ decomp relink
  basic.obj                    split   7 of 12 functions in its source
  other.obj                    source  complete
identical to the target: SHA-1 3b10acac... (1 units from source, 1 split); taken over from the
original: COFF header TimeDateStamp, debug directory entry 0 TimeDateStamp, ..., CodeView GUID
```

The fields that only record when and how an image was built (the COFF header's and the debug
directory's timestamps, the PDB's GUID and age, the export and resource directories' timestamps, a
repro hash) cannot come out of any source: they are taken over from the target before the SHA-1s are
compared (the checksum is then computed again), and the result lists them. Everything else must be the
same byte for byte. When it is not, `relink` names the first differing bytes in the target's sections,
with the unit whose contribution holds them and the symbol there, the header fields that differ and the
differing bytes per section. The exit code is 0 when the relink is identical and 2 otherwise.

| Option | Meaning |
|---|---|
| `--source <unit>` | Link the unit from its source even when its check fails (repeatable): to see what it breaks |
| `--split <unit>` | Carry the unit's original bytes even when its source is complete (repeatable) |
| `--all-split` | Every unit from its original bytes: tests the relink itself |
| `--toolchain <name>` | Compile (and link) with another toolchain than the project's |

`.decomp/relink/` holds the last relink: `objects/` (the compiled and split objects, in link order),
`libs/` (the import libraries written), `link.rsp` (the linker's arguments), `out/` (the relinked image
and its PDB) and `result.json`:

| Field | Meaning |
|---|---|
| `time` | When it ran (UTC) |
| `identical` | Whether the relinked image equals the target once the identity fields are taken over |
| `units[]` | Per unit in link order: `unit`, `kind`, `mode` (`source`, `split`, `linker`), `reason`, `bytes` (the size of its contributions), `object`, and for units with a source the `check` (`decomp units check --json`'s record) |
| `libraries` | The import libraries written |
| `notes` | What the relink could not provide (a name a compiled object needs that nothing defines, an export or entry point without a symbol) |
| `link` | `ok`, `exit_code`, `output`, `command`, `duration_ms` |
| `image` | The relinked image, relative to the project |
| `comparison` | `identical`, `original_sha1`, `relinked_sha1` (stamped), `relinked_unstamped_sha1`, `original_size`, `relinked_size`, `stamped[]` (`name`, `offset`, `original`, `relinked`), `differing_bytes`, `differences[]` (`offset`, `size`, `where`, `rva`, `unit`, `symbol`, `original`, `relinked` bytes), `sections[]` (`name`, `differing_bytes`, `first_rva`, `first_unit`, `size_differs`) and `first`, the first difference in section contents |

Split objects reproduce the target's layout because each carries its unit's contributions, the input
sections the linker placed, with their names, alignments and COMDAT-ness: a PDB lists them exactly
(its section contributions, named by the linker's COFF group records); without one they are cut from the
units of the symbols, which is enough while every unit is split, and as good as the units are once some
are built from source. link.exe writes a Rich header counting the objects of each compiler: split
objects carry the `@comp.id` of the compiler the PDB says made their originals, and import libraries
the import library tool's.

## `src/functions/`

Verified sources of functions whose matches do not go into a unit source ([Unit sources](#unit-sources)):
functions in no code unit with a source path, functions of guessed units whose source has not been
started, and matched functions not yet emitted into their unit's source. There is one `.cpp` per
function, named `<fn>.cpp` (for example `src/functions/add_401060.cpp`). A file is written when the
agent's `submit_result` is accepted (or when a session ends with a byte-exact attempt it did not
submit), or by the GUI's Verify and save, once the `write_source` approval policy lets it through
([agent.md](agent.md#approvals)). It is **exactly** the translation unit that verified byte-exact.
Decomp adds no banner or comment, because even a comment can shift `__LINE__`. Every write is
recorded in [`.decomp/changes.jsonl`](#changesjsonl-and-blobs) and can be reverted. The history
(session, attempts, scores) lives in `.decomp/`. `decomp diff --source` never writes here.
`decomp units emit` moves these files into their units' sources.

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
| `attempts.jsonl` | One JSON object per compile attempt of an agent session, that is, every `compile_and_diff` call and the verification compile of every `submit_result` with `matched`. Fields: `attempt` (numbered from 1 within the session), `session`, `origin` (`agent`; `user` for attempts made by hand in the GUI), `time` (UTC), `compiled`, `match_percent`, `byte_exact`, `summary` (the diff's one-line summary, or why there is no diff) and the full `source`. `decomp diff` does not record attempts. |
| `best.cpp` | The source of the best attempt so far. An attempt replaces it when it compiled, was diffed, and scored at least the function's previous best (`best=` in `symbols.txt`), so among equal scores the most recent wins. For a matched function it equals the verified source. |
| `notes.md` | Notes from the agent's `record_note`, one per line: `- YYYY-MM-DD HH:MM: <text>` (UTC) |

An `attempts.jsonl` line from the scripted replay of the CI smoke test:

```json
{"attempt":1,"byte_exact":false,"compiled":true,"match_percent":68.75,"origin":"agent","session":"2026-10-04T02-08-33-f11d-401060","source":"extern int g_counter;\n\n__declspec(noinline) int add(int a, int b) { return a - b + g_counter; }\n","summary":"match 68.8% (2/4 equal; 1 operand, 1 opcode) - not matching","time":"2026-10-04T02:08:33.412Z"}
```

This history feeds future briefs: the number of earlier attempts and the best score, the notes and the
best source go into the next session's first message ([agent.md](agent.md#prompts)).

### `runs/<run-id>/`

A run is one invocation of `decomp agent` (one function) or one batch run of `decomp run` or
`decomp-gui` (many functions; [agent.md](agent.md#batch-runs)). The run ID is the UTC start time plus
four random hex digits, such as `2026-10-04T15-30-12-3f9a`. Both commands write the same files. With
`decomp agent --log-dir <dir>` the run goes to `<dir>/<run-id>/` instead; without a project and
without `--log-dir`, its files go to a temporary directory that is removed when it ends.

| File | Contents |
|---|---|
| `events.jsonl` | Every event of the run except the high-volume `stream_delta` events, one JSON object per line, in sequence order ([ui.md](ui.md#events)). `RunState::replay` folds it back into the run's state ([ui.md](ui.md#replay-of-past-runs)); `decomp runs show` and the GUI's past runs read it. A resumed run appends to it, continuing the numbering. |
| `sessions/<fn>.jsonl` | The transcript of a function's first session in the run, and `sessions/<fn>.<n>.jsonl` of its n-th (after a requeue or a resume): a `session` header, the first request in full, the messages each later request appended, every response with its usage and cost, tool calls, retries, guidance, pauses and the outcome ([agent.md](agent.md#transcripts-and-event-logs)). It never contains the API key. |
| `summary.json` | What the run did: totals, and per function the outcome of its latest session plus the totals of all its sessions. Batch runs rewrite it as functions finish; `decomp runs show <id>` computes the same from `events.jsonl`. `decomp status` does not read it; it uses `symbols.txt`. |
| `run.json` | Batch runs: settings, status and queue, rewritten at every transition (below) |
| `run.lock` | Batch runs: held by the process running the run. A run whose `run.json` says it is live but whose lock is free was interrupted, and `decomp runs list` shows it as `interrupted`. |

The `summary.json` of a scripted batch run (`"replay": true` in `run.json`), with one of its 13
functions:

```json
{
  "cost_usd": 0.61896,
  "effort": "high",
  "finished": "2026-10-04T14:14:17Z",
  "functions": [
    {
      "best_match": 100.0,
      "cost_usd": 0.04892,
      "detail": "'submit_result' ended the session",
      "display": "public: void __thiscall Player::Hit(int)",
      "function": "?Hit@Player@@QAEXH@Z",
      "matched": true,
      "outcome": "matched",
      "session": "2026-10-04T14-14-16-df8f-401000",
      "sessions": 1,
      "turns": 2,
      "usage": {
        "cache_creation_input_tokens": 6100,
        "cache_read_input_tokens": 6100,
        "input_tokens": 2400,
        "output_tokens": 380
      },
      "va": 4198400
    }
  ],
  "functions_matched": 8,
  "functions_planned": 13,
  "functions_worked": 13,
  "model": "claude-opus-5-5",
  "project": "pf",
  "run": "2026-10-04T14-14-16-df8f",
  "sessions": 13,
  "started": "2026-10-04T14:14:16Z",
  "status": "completed",
  "usage": {
    "cache_creation_input_tokens": 79300,
    "cache_read_input_tokens": 79300,
    "input_tokens": 31200,
    "output_tokens": 4090
  },
  "workers": 4
}
```

`status` is `completed`, `stopped`, `aborted`, `budget_exhausted` or `error`; a function's `outcome`
is one of the outcomes in [agent.md](agent.md#the-loop).

The `run.json` of the same run, with one queue entry:

```json
{
  "counts": {"done": 13, "matched": 8, "pending": 0, "running": 0, "skipped": 0},
  "created": "2026-10-04T14:14:16Z",
  "effort": "high",
  "id": "2026-10-04T14-14-16-df8f",
  "limits": {"max_seconds": 1800, "max_tokens": 0, "max_turns": 40, "max_usd": 5.0},
  "model": "claude-opus-5-5",
  "policies": {},
  "project": "pf",
  "queue": [
    {
      "difficulty": 2.807354922057604,
      "display": "int __cdecl read_counter(void)",
      "name": "?read_counter@@YAHXZ",
      "outcome": "matched",
      "sessions": 1,
      "state": "done",
      "va": 4198512
    }
  ],
  "replay": true,
  "replay_dir": "tests/replay/run",
  "run_budget_usd": 0.0,
  "selection": {"all": true, "filter": "", "functions": [], "include_finished": false, "statuses": []},
  "spent_usd": 0.61896,
  "status": "completed",
  "updated": "2026-10-04T14:14:17Z",
  "version": 1,
  "workers": 4
}
```

| Key | Meaning |
|---|---|
| `status` | `starting`, `running`, `paused`, `stopping` or `aborting` while live; then `completed`, `stopped`, `aborted` or `budget_exhausted` |
| `queue` | Every function of the run in dispatch order: `state` (`pending`, `running`, `done` or `skipped`), the latest `outcome`, the number of `sessions`, the `difficulty` estimate and `pinned` when pinned |
| `model`, `effort`, `workers`, `limits`, `run_budget_usd`, `policies` | The settings, including changes made during the run; a resume starts from them |
| `selection`, `replay`, `replay_dir` | How the functions were chosen, and the scripts of a scripted run (so that a resume finds them) |
| `counts`, `spent_usd`, `created`, `updated` | Progress for `decomp runs list` |

### `changes.jsonl` and `blobs/`

Every file Decomp writes into the project (verified sources, in unit sources and `src/functions/`,
and the headers `define_type` writes) is recorded in `changes.jsonl`, one JSON object per write, and the content it
replaced is kept in `blobs/<sha1 of that content>`, so any write can be undone:

```json
{"function":"?scale@@YAMM@Z","path":"src/functions/scale_401180.cpp","previous_sha1":null,"reason":"verified match","session":"2026-10-04T14-14-16-df8f-401180","sha1":"e5f5b03613cdcf0060ce0290b4b5b0d5b3fab780","size":71,"source":"agent","time":"2026-10-04T14:14:17.022Z","va":4198784}
```

| Field | Meaning |
|---|---|
| `path` | Relative to the project directory, with forward slashes. Decomp writes, and reverts, only paths inside the project and outside `.decomp/`. |
| `sha1`, `size` | The content written (`sha1` is `null` when a revert removed the file) |
| `previous_sha1` | The content replaced, kept in `blobs/`; `null` for a new file |
| `source`, `session`, `reason` | Who wrote it: `agent` (with its session) or `user`, and why. A match saved after the supervisor's approval says so (`verified match, approved by user`). |
| `function`, `va`, `time` | The function and when (UTC) |
| `unit`, `functions` | For a unit source: the unit, and the addresses of the functions the written source holds |

A record with `sha1: null` removed the file (`decomp units emit` removes the files it took in).
`decomp changes` lists the changes with their numbers (`--json` for the records). Reverting a change
(`decomp changes revert <n>`, `Project::revert_change`, the GUI's Changes and approvals view) restores the
content it replaced, or removes the file when there was none, and appends its own record with
`reason: "revert"`; the reverted content goes to `blobs/` too. Only a file still holding the content
of that change can be reverted, so later changes are reverted first. A function of the change that no
longer has a verified source, in its unit's source or its own file, goes back to `nonmatching`; its
attempts and best source stay.

### `symbols.log.jsonl`

Every symbol edit made through Decomp's project API (`Project::set_symbol`: renames, new symbols, kind
and size changes, removals) appends `{time, va, before, after, source, session, reason}`, where `before`
and `after` hold the symbol's `name`, `kind`, `size` and `source` (`null` when it did not exist). The
agent's `set_symbol` tool records `source: "agent"` with its session and the evidence it gave as the
reason ([agent.md](agent.md#set_symbol)). `symbols.txt` holds the result; the log holds the provenance.
Edits made to `symbols.txt` by hand are not logged. A renamed function's history
(`.decomp/functions/<fn>/`) and own source move to its new key.

`decomp symbols log` lists the edits with their numbers (`--agent` for the agent's, `--json` for the
records). `decomp symbols revert <n>...` (or `--session <id>` for every edit of an agent session) undoes
edits, newest first, as the GUI's Symbols view does: each symbol gets back what it had before, recorded
as a user edit. A revert is refused when a later edit changed the symbol.

### Locks

| File | Held | Purpose |
|---|---|---|
| `project.lock` | Exclusively while `symbols.txt`, the logs beside it, a function's history (attempts, best source, notes) or a verified source under `src/` are written | Workers and processes never interleave writes; each write starts from the latest `symbols.txt` on disk, and a unit source write checks that the file still holds what the change was composed onto |
| `active-run.lock` | By the process that runs agent sessions (`decomp agent`, `decomp run`, `decomp-gui` during a run) | One live run per project |
| `runs/<id>/run.lock` | By the process running that batch run | Tells a live run from an interrupted one; a run is resumed by one process at a time |

The locks are advisory file locks (`flock` on Linux, `LockFileEx` on Windows) that the system releases
when a process dies, so a crash never leaves a project locked. The lock files themselves stay.

### `build/` and `cache/`

- `build/` holds one fresh working directory per compile, named `c<first 12 hex digits of the cache
  key>-<process id>-<n>` (unique across workers and processes): `candidate.cpp`, `candidate.obj`, and
  `args.rsp` when the command line is long. The
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
| `in_progress` | An agent session is working on it | The runner announces it (a `status` event) when a session starts, but never writes it to `symbols.txt`, so a crash leaves nothing stale. An older project may still contain it; the next session's end replaces it. |
| `nonmatching` | At least one attempt scored above 0%, none matched; the best percentage is in `best=` | The runner, when a session ends without a match, give-up or refusal (budget, turn limit, no result, stop, abort or error) |
| `matched` | A verified byte-exact source is in the project: in its unit's source, or in `src/functions/` | `submit_result` verification. A matched function stays matched, whatever later sessions do. |
| `refused` | The model declined the request, including after fallbacks when they are enabled | The runner |
| `gave_up` | The agent called `submit_result` with `give_up` | The agent |
| `skipped` | Excluded from work | Editing `symbols.txt`. The default batch selection leaves it out. |
| `library` | Library or runtime code that is not meant to be decompiled | Editing `symbols.txt`; `decomp lib match` (library signature matching) |

```
unstarted --agent--> in_progress --+--> matched
                                   +--> nonmatching --agent--> in_progress ...
                                   +--> gave_up
                                   +--> refused
                                   +--> back to the previous status (no attempt scored)
any status --edit symbols.txt--> skipped | library | unstarted (reset; history kept)
```

`decomp agent`, and `decomp run` given function names, run on a function whatever its status. The
default selection of `decomp run --all` and of the GUI leaves out matched, refused, skipped and library
functions, linker thunks and functions without a size or recoverable extent (`--status` chooses by
status instead). Re-verifying matched functions after a change of toolchain or flags is open.

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

- **What to commit:** `decomp.json`, `symbols.txt`, `units.txt`, `include/` and `src/`. `init` writes a
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
  `in_progress` is only announced while a session runs and is never written to `symbols.txt`.
- **Machine independence:** toolchains are referenced by name, and paths in `decomp.json` are relative
  when a relative path exists. Nothing secret is stored in the project.
- **Merge-friendly:** the symbol file is line-based with one symbol per line, so concurrent work on
  different functions merges without conflicts.
- **Verified means committed as verified:** sources are written exactly as they matched, so the
  committed file is the file that compiled to the original bytes.
