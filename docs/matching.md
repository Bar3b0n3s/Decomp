# Matching

Matching is the mechanical core of Decomp. Every claim that a function is decompiled rests on it. A
candidate C++ translation unit is compiled with the target's original toolchain, the function is
extracted from the resulting object file, and it is compared with the function in the target binary.
The comparison has to bridge one gap: the candidate is an unlinked object whose address operands are
relocations, while the target is linked code at fixed addresses. Decomp's relocation-aware symbolic diff
turns both sides into canonical instructions whose address operands are symbolic, aligns them, and
classifies every difference. It then reports a match percentage, an `exact` flag and a `byte_exact`
flag, plus targeted hints for the agent. A function is **matched** only when it is `byte_exact`. This
document defines the verdicts, describes the algorithm, lists the MSVC-specific cases, and covers how
Decomp drives compilers.

Status: implemented in the first slice (steps 7-8 in the [roadmap](roadmap.md#first-working-slice)).
The code is in `src/matching/` (`diff.cpp`, `match.cpp`, `toolchain.cpp`) and `src/analysis/`.
Extensions are marked per section as planned, with their phase where one is set.

## What "matching" means

| Verdict | Definition | Used for |
|---|---|---|
| `match_percent` | 0-100 score: a credit per aligned row ([below](#6-verdicts-and-score)) over the length of the longer listing. Exactly 100.0 only when `byte_exact`, otherwise at most 99.9. | Progress, ranking attempts, choosing the best attempt |
| `exact` | Every instruction pairs with one that has the same mnemonic and operands after canonicalization, and every address operand refers to the same thing: the same symbol, label, string or constant, or a jump table with the same targets | An intermediate signal |
| `byte_exact` | `exact`, and every pair of instructions has the same length and identical bytes outside the fields that hold an address operand (those are already proven equivalent symbolically) | The only verdict that marks a function `matched` |

Both flags exist because each hides something the other shows. Canonical text hides encoding choices:
`83 C0 01` and `05 01 00 00 00` both read `add eax, 1`, yet a different compiler version or flag
produced them. Raw bytes cannot compare relocated fields at all, because the candidate's fields hold
zeros or addends while the target's hold final addresses. `byte_exact` combines both: the symbolic
comparison proves the relocated fields equivalent, and the remaining bytes must be identical.

**What is compared:** the function's code bytes on both sides, the instructions they decode to, and,
by content, the data the function references directly: string literals, floating-point and SSE
constants, and jump tables. Named globals and functions are compared by name.

**What is not compared** (in the slice): object-file metadata (timestamps, symbol order, debug
sections, `.drectve`); the function's address and its alignment padding; the contents of referenced
globals beyond their identity; exception-handling tables; and unwind data. Data matching and full
relinking come in Phase 5.

## Inputs

Both sides become a `Side`: the function's name, address and size, and its instructions, each with a
`Ref` for every address-bearing field and a canonical text.

- **Target** (`build_target_side`): the byte range `[start, end)` from `Program::function_extent()`
  ([architecture.md](architecture.md#analysis)): the symbol's size when known, otherwise recursive
  descent. Jump tables inside the range are excluded from decoding. The `SymbolDb` names addresses.
- **Candidate** (`build_candidate_side`): the function's bytes and instructions from its section in the
  compiled COFF object, the relocations inside its range (offset, type, symbol), and the object's other
  sections, which hold string literals, constants and jump tables.

`diff_function()` builds both sides and compares them with `diff_sides()`; `compile_and_diff()` compiles
a source first ([Driving compilers](#driving-compilers)).

## The relocation-aware symbolic diff

### 1. Target side

The function's bytes come from the image. The decoder reports every displacement, immediate and
relative field of an instruction; these fields count as **address operands**:

| Source | Fields |
|---|---|
| Relative branches and calls (`rel8`/`rel32`) | Always. Destination = end of instruction + displacement. |
| RIP-relative displacements (x64) | Always. Destination = end of instruction + displacement. |
| Base relocations (any non-`ABSOLUTE` `.reloc` entry: `HIGHLOW` on x86, `DIR64` on x64) | Displacements and immediates of at least 4 bytes that carry a base relocation |
| No relocation information (`.reloc` stripped or absent, as in EXEs linked `/FIXED`) | **Heuristic:** displacements and immediates of at least 4 bytes whose value lies inside the image's sections, at or above `ImageBase + 0x1000` |
| MSVC x64 jump-table loads (`mov r, [base + i*4 + table_rva]` with `base` = `__ImageBase`) | The displacement, read as the table's RVA |

Other fields of fewer than 4 bytes are never address operands. A heuristic field is only a guess:
when the candidate has no relocation at that field, the two fields are compared as plain values
instead, so an integer constant that happens to look like an address still matches.

Each resolved address becomes a reference (`Ref`), in this order:

1. An address where one of the function's jump tables starts becomes that table; any other address
   inside the function becomes an internal label (step 3).
2. An address without a symbol that holds a linker thunk is replaced by the thunk's destination: an
   incremental-linking `jmp rel32` (followed through chains) or an import stub `jmp [IAT slot]`
   ([thunks](#incremental-linking-and-import-thunks)).
3. An exact symbol, or a containing symbol plus offset (`g_table+0x8`). A string-literal symbol
   (`??_C@...`) at offset 0 becomes the string's bytes, and a constant symbol (`__real@`, `__xmm@`,
   `__ymm@`) at offset 0 becomes the constant's bytes. Any other symbol is compared by name, with its
   PDB name as an alternative.
4. Anything else stays unknown, `unk_<hex address>`.

### 2. Candidate side

The candidate object is parsed with `coff::Object`. Decomp locates the function by the target's
decorated name; if the object does not define it, by any function symbol with an equivalent name (the
same qualified name, such as a static function's undecorated PDB name `helper` and the candidate's
mangled name), or by the name given with `decomp diff --symbol`. When nothing fits, the error lists the
functions the object defines.

- The range runs from the symbol to the next defined symbol of its section (labels excepted) or to the
  section end. With `/Gy` (function-level linking, implied by `/O1` and `/O2`), each function is a
  COMDAT section of its own, and the range is the rest of that section.
- A jump table at the end of the range is split off as data: the first relocated reference from an
  indirect jump into the function's own section, past the jump, marks the end of the code (see
  [jump tables](#jump-tables-inside-text-x86)).
- Candidates are always compiled with `/Gy`: for MSVC-style toolchains the driver appends `/Gy` when
  the flags lack it or turn it off with `/Gy-`. `/Gy` changes how functions are packaged, not the code
  generated for them, so targets built without it still match. Each candidate function is therefore
  a COMDAT of its own: no alignment padding is included and calls to neighbouring functions carry
  relocations. (A `.obj` built elsewhere without `/Gy` and given to `diff --obj` is not supported:
  padding shows as extra rows and same-section calls are not recognized.)

A relocation at the offset of an instruction's field makes that field an address operand. COFF
relocations have no explicit addend: the field's existing contents are the addend. For example,
`mov eax, [g_table+8]` is a `DIR32` relocation to `g_table` with 8 stored in the field. The relocation
type does not matter for instruction fields; the relocation's symbol decides what the operand becomes:

| Relocation symbol | Becomes |
|---|---|
| Undefined (external) | That symbol plus the addend |
| Inside the function's own code | An internal label |
| In another code section | The named symbol defined at that offset |
| A string literal (`??_C@...`), named or through its COMDAT's section symbol | The string's bytes |
| A constant (`__real@`, `__xmm@`, `__ymm@`), named or through its section symbol | The constant's bytes; the size follows from the name (4, 8, 16 or 32 bytes) |
| Any other named symbol outside the function's section | That symbol plus the addend |
| Anonymous data: a section symbol, or a label in the function's own section (MSVC's `$LN` table labels) | A jump table when the data holds relocations back into the function; otherwise a string when it holds a NUL-terminated string of at least 2 bytes (such as an older compiler's `$SG` literal in `.data`); otherwise the named symbol at that offset; otherwise `<section>+<offset>` |

Relocation types matter for jump-table entries only: their size, and whether they are PC-relative
(clang's x64 `REL32` entries, which are adjusted back to the label they point to). Thread-local
variables (`SECREL`) are not supported yet: the target's field is a plain offset into `.tls`, so such
an operand shows as a difference (planned).

### 3. Canonicalization

Each instruction is rendered in Intel syntax (`x86::render`), and its address operands are replaced by
their references. For alignment, every address operand is masked to a placeholder, so the key of an
instruction is its prefix and mnemonic plus these operand templates. References have a kind
(`RefKind`):

| Kind | Key | Equal when | Shown as |
|---|---|---|---|
| `symbol` | Name and offset | The offsets are equal and the names match: identical when the target's name is the linker's own (C++-decorated, or a public that differs from its PDB name), otherwise the same qualified name. The target's PDB name also counts. | Qualified name, `g_table+0x8` |
| `label` | `L<index>`, the destination's position in that side's listing | The two destinations are aligned with each other | `loc_<offset in the function>` |
| `string` | The bytes up to the terminator | Identical bytes. When the target has no string symbol there, the string at the target address is read and compared. | An escaped C string, first 48 bytes |
| `float32`, `float64`, `vector` | The constant's bytes | Identical bit patterns, read at the target address when the target has no constant symbol there | `1.5f`, `0.75`, `const:<hex>` |
| `table` | The targets of every entry, as labels | Same length, and every entry's labels aligned | `switch_table` |
| `unknown` | `unk_<va>` on the target, `<section>+<offset>` on the candidate | Never | The address in hex |

**Linker names compare exactly; readable names by equivalence.** When the target's name is the
linker's own (C++-decorated like `?add@@YAHHH@Z`, or a public such as `_entry` whose PDB name `entry`
differs), the candidate must use exactly that name, after dropping an `__imp_` prefix on either side.
That also checks the declaration: `int add(int, unsigned)` mangles to `?add@@YAHHI@Z` and does not
match, even though its code is identical; a hint names both declarations. This applies to references
and to the matched function's own name. When only a readable name is known (static functions and data,
whose PDB records carry undecorated names, or names from the export table), names are equal when their
qualified names are: `helper` matches the candidate's `?helper@@YAHH@Z`, and `__imp__ExitProcess@4`
matches `_ExitProcess@4`.

**Strings and floats compare by content.** MSVC names string literals by an encoding of their content
(`??_C@_0...`), and floats by their bit pattern (`__real@...`). Older compilers without string pooling
emit anonymous `$SG...` symbols. The target only has addresses, possibly pooled or merged by the
linker. So on the candidate side, Decomp reads the literal or constant from the object's data section.
On the target side, it reads the bytes at the referenced address. The two compare equal when the bytes
are identical. Floats compare by bits, never numerically, so `-0.0`, `0.0` and NaN payloads stay
distinct. Narrow strings are read up to their NUL byte; wide literals (`??_C@_1...`) are read as UTF-16
up to their 0x0000 unit and compared over all their bytes (ref kind `wide_string`).

**Internal branch targets become instruction indices.** A branch whose destination lies inside the
function is keyed `L<index>`, where the index is the destination instruction's position in that side's
listing. Because an insertion earlier in the function shifts every later index, branch operands are
not compared textually. After alignment (step 4), a target branch to `Li` and a candidate branch to
`Lj` are equal exactly when rows `i` and `j` are aligned with each other. A destination that is not the
start of an instruction gets an offset key (`off+<hex>`) instead.

**Jump tables compare as lists of indices.** A table is decoded on both sides into its destinations,
mapped to instruction indices, and compared element by element through the alignment. Table length is
part of the comparison. Three table forms are recognized on the target:

| Form | Dispatch | Entries |
|---|---|---|
| x86 (MSVC, clang-cl) | `jmp [reg*4 + table]` | Absolute addresses |
| clang x64 | `lea base, [rip + table]; movsxd r, [base + i*4]; add r, base; jmp r` | 32-bit offsets from the table |
| MSVC x64 | `lea base, [rip + __ImageBase]; mov r, [base + i*4 + table_rva]; add r, base; jmp r` | 32-bit RVAs |

Entries are read until one points outside the function or into non-code, or until another symbol
starts (at most 4096). Two-level MSVC switches add a byte-sized index table
(`movzx reg, byte ptr [reg + index_table]`); recognizing that table as data is planned (Phase 2).

**Bindings.** When the target address has no symbol (`unknown`) and the candidate references a named
symbol at the aligned operand, the pair is recorded as a binding, for example "the candidate uses
`?g_player@@3PAVPlayer@@A` where the target uses `0x004A3F20`". Bindings are listed in the report
(each pair once) and produce a hint that suggests the name. They do not count as equal: the row stays
an `operand` difference, so the function cannot be `exact` until the address is named. Adding a line
for the address to `symbols.txt` ([project-format.md](project-format.md#symbolstxt)) does that. Checking
that bindings are consistent across the function and recording consistent bindings automatically when a
function matches are planned. *Policy (proposed, open):* in Phase 1 the approval policy may require the
user to confirm new bindings before they are written.

### 4. Alignment

The two instruction sequences are aligned globally (Needleman-Wunsch) on their keys. Pairing two
instructions costs 0 when their keys are equal, 1 when only their mnemonics are equal, and 3 otherwise;
a gap costs 2. For very large functions (more than 25,000,000 cells, n × m) the listings are paired
by position instead. Paired instructions with different mnemonics become `opcode` rows; unpaired ones
become `insert` (candidate only) or `delete` (target only) rows. With the alignment fixed, branch
targets and jump tables are compared through the map from target to candidate instruction indices, as
described above.

### 5. Row kinds

| Row kind | Meaning | Credit |
|---|---|---|
| `equal` | Same mnemonic and operands after canonicalization, and the same bytes outside address fields | 1.0 |
| `encoding` | Same text, but a different length or different bytes outside address fields | 0.9 |
| `operand` | Same mnemonic, at least one operand differs | 0.75 when every differing operand is `register` or `stack`, otherwise 0.5 |
| `opcode` | Paired instructions with different mnemonics (prefixes included) | 0 |
| `insert` | Instruction only in the candidate | 0 |
| `delete` | Instruction only in the target | 0 |

An `operand` row lists each differing operand with a category:

| Category | Meaning |
|---|---|
| `register` | Both operands are registers, and they differ |
| `immediate` | Both operands are immediates, and they differ |
| `stack` | Both are memory operands on `esp`/`ebp` (x86) or `rsp`/`rbp` (x64) with the same base and index, and they differ (usually in the displacement) |
| `memory` | Any other difference in shape, including a different number of operands |
| `symbol` | Same shape, but an address operand refers to something else, or a binding was found |

The credits are initial values and will be tuned against the fixtures.

### 6. Verdicts and score

```
match_percent = 100 * (sum of row credits) / max(n_target, n_candidate)
```

The score is capped at 99.9 unless the function is `byte_exact`, so `100.0` appears only for matched
functions. Text reports show one decimal, and the JSON report rounds to one decimal.

- `exact`: there are no `opcode`, `operand`, `insert` or `delete` rows (`encoding` rows are allowed),
  and the listings are not empty.
- `byte_exact`: `exact`, and there are no `encoding` rows either. For each pair, the bytes of every
  field that holds an address operand on either side are masked, and the rest must be identical, with
  equal instruction lengths. Jump tables in the range compare as tables, not as code bytes.

The one-line summary reads, for example,
`match 57.9% (27/57 equal; 11 operand, 3 opcode, 16 extra, 4 missing) - not matching`. Its verdict is
`MATCHING (byte-exact)`, `equivalent but bytes differ` (exact only) or `not matching`; `extra` counts
`insert` rows, `missing` counts `delete` rows, and `encoding` rows appear as `N encoding`.

### 7. Hints

Hints are short sentences for the agent and the user. A `byte_exact` function has none. Instructions
are referred to by their target index (`target #4`).

| Detected when | Hint |
|---|---|
| Every differing row is an `operand` row with `register` differences only | "Only register allocation differs. Try reordering declarations or statements, changing variable lifetimes, introducing or removing temporaries, or changing an expression's evaluation order." |
| Every differing row is an `operand` row with `stack` differences only | "Only stack offsets differ: local variable order, sizes or types differ (MSVC lays out locals by declaration order and size)." |
| There are `encoding` rows | "N instruction(s) are identical in text but encoded differently (e.g. operand form or immediate size); often a different operand type, signedness, or compiler flag." |
| An `opcode` row pairs a conditional jump with its inverse (the first one) | "Branch condition inverted at target #i (jle vs jnle): swap the if/else bodies or negate the condition." |
| Both listings have the same instructions in a different order | "Same instructions in a different order: statement order or the evaluation order of an expression differs." |
| The instruction counts differ | "Candidate has N more (or fewer) instruction(s) than the target." |
| A branch target differs | "Branch at target #i lands on code that differs between the versions (around loc_30)." or "... goes to a different place (A vs B): the control flow around it differs." |
| A target address has no symbol where the candidate has one | "Target #i references 0x403000, which has no symbol; the candidate uses `?g_counter@@3HA` there. If that is the same object, name the address `?g_counter@@3HA`." |
| Any other address operand differs | "String literal (Constant, Callee or Reference) differs at target #i: target A vs candidate B." |
| One side reads an import's IAT slot and the other calls its name directly | "Target #i calls `_Foo@4` through the import table (`call [__imp_...]`), the candidate through the linker's import thunk: declare it `__declspec(dllimport)`." (or "... declare it without `__declspec(dllimport)`.") |

Reference hints stop after about a dozen. Additional detectors are planned: `signature` (names that
are equivalent but decorated differently, which points to parameter types or the calling convention),
and `gs_cookie`, `chkstk` and `eh_frame` (see [MSVC specifics](#msvc-specifics)).

### 8. Output formats

The report renders the same result two ways (`to_text`, `to_json`). `decomp diff` takes `--compact`
(only differing rows, with `--context` rows of context around each; default 3), `--bytes`
(instruction bytes) and the global `--json`, and is colored on a TTY. Reports are capped at 400 rows.

**Text, for humans and the agent.** The agent's `compile_and_diff` result uses the compact form with 2
rows of context and at most 80 rows. Row markers are ` ` equal, `e` encoding, `~` operand, `!` opcode,
`+` insert and `-` delete; offsets are relative to the function start. The example below compares the
x86 fixture's `scale` with a deliberately mutated candidate:

```
match 87.5% (3/4 equal; 1 operand) - not matching
target ?scale@@YAMM@Z (17 bytes) | candidate ?scale@@YAMM@Z (17 bytes)
     0: fld dword ptr [esp+0x4]                       |    0: fld dword ptr [esp+0x4]
~    4: fmul dword ptr [1.5f]                         |    4: fmul dword ptr [2.5f]  (op0 symbol)
     a: fadd dword ptr [0.25f]                        |    a: fadd dword ptr [0.25f]
    10: ret                                           |   10: ret
hints:
- Constant differs at target #1: target 1.5f vs candidate 2.5f.
```

**JSON** (`decomp --json diff`). It contains the summary, the verdicts, the counts, the visible rows
(`t`/`c` are the target and candidate texts, `ti`/`ci` their indices), the hints and the bindings. Keys
are sorted, so the output is deterministic. A mutated string literal (the output is indented; it is
shown compacted here):

```json
{
  "bindings": [],
  "byte_exact": false,
  "candidate": {"instructions": 2, "name": "?message@@YAPBDXZ", "size": 6},
  "counts": {"encoding": 0, "equal": 1, "extra": 0, "missing": 0, "opcode": 0, "operand": 1},
  "exact": false,
  "hints": [
    "String literal differs at target #0: target \"hello world\" vs candidate \"hello there\"."
  ],
  "match_percent": 75.0,
  "rows": [
    {"c": "mov eax, \"hello there\"", "ci": 0, "kind": "operand",
     "operands": [{"diff": "symbol", "operand": 1}], "t": "mov eax, \"hello world\"", "ti": 0},
    {"c": "ret", "ci": 1, "kind": "equal", "t": "ret", "ti": 1}
  ],
  "summary": "match 75.0% (1/2 equal; 1 operand) - not matching",
  "target": {"address": 4198768, "instructions": 2, "name": "?message@@YAPBDXZ", "size": 6}
}
```

Bindings appear as `{"candidate_symbol": "?g_counter@@3HA", "target_va": 4206592}`.

## MSVC specifics

### `/Gy` and COMDAT sections

With `/Gy`, every function is emitted into its own COMDAT section (`IMAGE_SCN_LNK_COMDAT`). The
section-definition auxiliary record carries the length, a checksum and the selection kind: no
duplicates for ordinary functions, "any" for inline functions and pooled literals. Associative COMDATs
attach data to the function, such as x64 `.pdata`/`.xdata` and debug symbols. Extraction runs from the
function's symbol to the next symbol of its section or the section end, which for a COMDAT is the
whole function ([step 2](#2-candidate-side)). The compile driver always adds `/Gy` to candidate
compiles (see [step 2](#2-candidate-side)), so targets built without it are matched too; only
objects built elsewhere without `/Gy` and passed to `diff --obj` are not supported.

### String literals (`??_C@`)

With string pooling (`/GF`, which `/O1` and `/O2` imply), literals become COMDATs named `??_C@_0...`
(narrow) or `??_C@_1...` (wide). The name encodes the length, a hash and a prefix of the content.
Without pooling, older compilers place literals in `.data` or `.rdata` under anonymous `$SG<n>` local
symbols. Decomp compares both by content. A wrong literal is therefore an `operand` row (category
`symbol`) with a hint that shows both strings. Wide literals are compared over all their UTF-16 units.
A literal placed in a different section, for example because
of a `const` mismatch, is caught in Phase 5 when data placement is verified.

### Floating-point constants (`__real@`, `__xmm@`)

MSVC materializes float and double constants as COMDATs named after their bit pattern
(`__real@3f800000` is `1.0f`, `__real@4008000000000000` is `3.0`). 16-byte SSE constants are named
`__xmm@...` (and 32-byte ones `__ymm@...`). Decomp reads as many bytes at the target address as the
candidate's constant has, and compares bit patterns. A float-versus-double mix-up shows as a constant
difference (`1.5f` against `1.5`).

### Jump tables inside `.text` (x86)

MSVC x86 places a switch's jump table, and for sparse switches a byte index table, directly after the
function's code in `.text`. In the object they sit inside the function's COMDAT, with `DIR32`
relocations pointing back into the function and a `$LN` label on the table. On the target, the entries
are absolute addresses covered by `HIGHLOW` base relocations, when relocations are present. A table
inside the function's range is excluded from decoding: disassembly shows it as data (`switch:` comments
and `switch_table_<address>` operands), and the diff compares it as index lists (see
[step 3](#3-canonicalization)). On the candidate side, the first relocated reference from an indirect
jump into the function's own section marks the end of the code. The byte index table of a two-level
switch is not recognized yet (planned, Phase 2). MSVC x64 tables hold 32-bit image-relative entries
(`ADDR32NB`) indexed through `__ImageBase`, and clang x64 tables hold offsets from the table. The slice
detects tables from the indirect-jump pattern; without a symbol size, recursive descent finds the
table's end by reading entries. Phase 2 hardens this for PDB-less MSVC targets.

### `/OPT:ICF` folding

Identical COMDAT folding makes the linker keep one copy of byte-identical functions (and read-only
data), so several names can share one address. Consequences:

- The PDB may list several procedures at one address. The function is matched once; the other names
  are aliases. The `SymbolDb` keeps one primary name per address and records the other names as
  aliases, which name lookups find; `symbols.txt` stores only the primary name. Phase 5 needs the
  aliases, because relinking has to produce every alias.
- A call in the target may land on a body whose primary name differs from the callee the source used.
  Today a reference compares against the primary name and the PDB name only; accepting any alias is
  planned.
- Folded bodies are identical by definition, so matching any one of them verifies the code at that
  address.

### Incremental-linking and import thunks

Binaries linked with `/INCREMENTAL` (typical for debug builds) route calls through an incremental
linking table of `jmp rel32` thunks. Calls to imported functions take one of two forms. With
`__declspec(dllimport)`, the call is indirect through the IAT slot (`call [__imp__Foo@4]`). Without it,
the call goes to a linker-generated stub, `jmp [__imp__Foo@4]`.

- `Program::thunk_destination()` follows an unnamed `jmp rel32` (through up to four chained jumps) to
  a named function, or a `jmp [IAT slot]` to its import. A call through such a thunk reads as a call
  to the destination, in the annotated listing ("via thunk at ...") and in the diff, so
  `call ILT+0x120` reads as `call ?Update@Player@@QAEXM@Z`.
- `fold_linker_thunks()` moves names off ILT entries when a program is opened: when an export, the
  entry point or an analysis-found function starts with a 5-byte `jmp rel32` to code, and that jump
  sits in a table of such jumps or leads to a function with an equivalent name, the symbol moves to the
  jump's destination. Functions are then named by the code the compiler produced.
- Function discovery recognizes link.exe's table at the start of the code section (a few `int3`
  bytes, then two or more `jmp rel32` thunks into code) and does not report its thunks as functions:
  a call, jump, function pointer or entry point that lands on a thunk stands for the function the
  thunk jumps to.
- Imports compare by their `__imp_` names. How an import is reached says how it was declared: code
  that reads its IAT slot (`call [__imp__Foo@4]`, or `mov esi, [__imp__Foo@4]` then `call esi`) was
  compiled against a `__declspec(dllimport)` declaration; a direct call to `_Foo@4` goes through the
  linker's import thunk and comes from a plain declaration. The annotated listing and the agent's brief
  say which one each import needs, and when the target and the candidate differ the diff gives the
  `dllimport` hint.

### SEH and C++ EH prologs

On x86, functions with C++ exception handling register a frame in the prolog: `push -1`,
`push offset __ehhandler$<fn>`, then a load of `fs:[0]`, a push, and a store to `fs:[0]`. The
`__ehhandler$<fn>` routine is a compiler-generated companion that passes a `__ehfuncinfo$<fn>` table
to `__CxxFrameHandler`. Structured exception handling (`__try`) uses a scope table and
`__except_handler3` or `__except_handler4`. A companion of the function being matched can only exist
in the candidate object under its own name, while the target has only an address. The slice has no
special handling: such a reference compares like any other symbol, so it is equal when the target has
a symbol with an equivalent name (from a PDB, for example) and otherwise a binding suggestion. Binding
companions structurally, and comparing the companion bodies and the `FuncInfo`/scope tables as part of
data matching, are planned (Phase 5). An EH prolog present on only one side will produce the planned
`eh_frame` hint: exception-handling flags, objects with destructors, or `try` blocks. x64 has no prolog
registration (handling is table-based, through `.pdata`/`.xdata`).

### `/GS` security cookies

With `/GS` (available since Visual Studio .NET 2002 and on by default since Visual Studio 2005), a
function with vulnerable local buffers loads `__security_cookie`, XORs it with the frame pointer,
stores it below the locals, and checks it before returning with a call to `__security_check_cookie`.
The compiler's buffer heuristics decide which functions get a cookie. A cookie on only one side
therefore points either to the flags or to the local declarations (an array versus a struct, for
example); the planned `gs_cookie` hint will say which. VC6 predates `/GS`.

### `__chkstk`

Frames larger than a page are allocated through a stack probe. On x86 this is `mov eax, <size>`
followed by `call __chkstk` (the CRT routine is also known as `_alloca_probe`). On x64 it is
`mov eax, <size>`, `call __chkstk`, `sub rsp, rax`. Without a probe, the frame is allocated with
`sub esp, <size>`. A probe on only one side means the total size of the locals differs; the planned
`chkstk` hint will report both sizes.

### Rich header compiler IDs

Images produced by Microsoft linkers (from Visual C++ 6.0 on) carry a Rich header between the DOS
stub and the PE header. It is XOR-masked and is located through its `Rich` and `DanS` markers. Each
entry records a product ID, a build number and a count: which compilers, assemblers, resource
converters and linker made how many of the objects. The key is also a checksum of the DOS header and
stub and of the entries, so an edited header shows. Decomp names every product ID of Microsoft's
enumeration, from Visual C++ 5.0 to the Visual Studio 2015-and-later tools (which share their IDs),
and turns each entry into a tool version and a Visual Studio release
([architecture.md](architecture.md#compiler-identification)). `decomp info` lists the entries and
what the image was built with; the Dashboard and the Binary explorer's Rich tab show the same.

### Choosing the toolchain

The compiler the target's own code came from decides which toolchain matches it. Decomp picks it
from the Rich header: among the compilers of the linker's release, the newest build, because the
runtime and SDK libraries linked into a program were often compiled with an older build of the same
release (a VC6 game's C runtime objects can outnumber the game's own). The suggestion names the
release the way the docs name toolchains (`vc6`, `vs2003`, `vs2019`) and notes what else matters for
matching:

- a Standard or Introductory edition compiler, which has no optimizer: the code is unoptimized
  whatever flags were given;
- objects compiled for link-time code generation (`/GL`) or optimized with a profile (PGO), whose code
  the linker generated across objects;
- other builds of the compiler (usually the runtime libraries), objects from other releases, assembly
  objects, objects without a tool ID, and a checksum that does not match.

A configured toolchain is compared with it through the `@comp.id` symbol that MSVC writes into every
object: `decomp toolchain test` (and the GUI's health check) compiles a probe and reports the
compiler ID it carries and, inside a project, whether it is the same build as the target's compiler,
the same release with another build (a different service pack or update), or another release.
clang-cl writes no compiler ID, so its fit is unknown.

### x64 `.pdata`

On x64, every non-leaf function has a `RUNTIME_FUNCTION` entry in `.pdata` (begin RVA, end RVA,
unwind info RVA). These entries give exact bounds without symbols. The slice already uses them: an
entry without a symbol becomes a function named `sub_<hex address>` with the entry's size. Leaf
functions that neither allocate stack nor save registers may have no entry. Functions split by the
optimizer appear as chained unwind entries; merging them into one function is planned (Phase 2).
Comparing the unwind data itself (`.xdata`) belongs to data matching in Phase 5.

### Whole-program optimization (`/GL`)

Objects compiled with `/GL` contain intermediate code, and the machine code is generated at link time
(`/LTCG`), where cross-function inlining and calling-convention changes happen. Per-function
compile-and-diff cannot reproduce those decisions. The Rich header shows LTCG objects, and the
toolchain suggestion warns about them ([Choosing the toolchain](#choosing-the-toolchain)). Targets
built that way are out of scope for now.

## Driving compilers

### Toolchains and the registry

A `Toolchain` describes how to run one compiler: `{name, kind: msvc | clang_cl | gcc | clang,
compiler, wrapper, flags, include_dirs, env, env_prepend, description, timeout_seconds}`. Toolchains
live in a user-level registry, because paths differ per machine: `%APPDATA%\decomp\toolchains.json`
on Windows and `~/.config/decomp/toolchains.json` on Linux (`DECOMP_TOOLCHAINS` overrides the
location). Projects reference them by name; per-project overrides are planned. When clang-cl is
installed, two auto-detected entries, `clang-cl-x86` and `clang-cl-x64`, are always available. The
file format and example entries for VC6, VS2008, clang-cl and Wine are in
[project-format.md](project-format.md#toolchain-registry).

`decomp toolchain add` creates or replaces an entry, and `decomp toolchain list` shows the registry.
`decomp toolchain test <name>` compiles a probe function (`int decomp_probe(int x) { return x * 3 + 1;
}`) in a temporary directory and prints `OK` or `FAILED`, the command line, the duration, whether the
output parses as a COFF object (with its architecture and function count), and the compiler's output.
It exits with 0 when the compile succeeded. A failed probe shows the compiler's own error, such as a
DLL that is not on `PATH`. It also prints the compiler's version banner (for example the
`Version 12.00.8804` line of VC6's `cl.exe`), the probe object's compiler ID (`@comp.id`) and, inside
a project, how the toolchain fits the target ([Choosing the toolchain](#choosing-the-toolchain)).

### Invocation

For MSVC-style compilers (`msvc`, `clang_cl`), the command line is:

```
[wrapper...] <compiler> /nologo /c <toolchain flags> <project flags> /I<toolchain include dir>... /I<project include dir>... /Fo<dir>/candidate.obj <dir>/candidate.cpp
```

Project flags come after the toolchain's base flags so that they take precedence. `<dir>` is the
compile's working directory, passed as a full path. clang-cl selects the target with
`--target=i686-pc-windows-msvc` or `--target=x86_64-pc-windows-msvc` in its base flags. `gcc` and
`clang` toolchains get `-c <flags> -I<dir>... -o <dir>/candidate.o <dir>/candidate.cpp`, but their
objects cannot be diffed before Phase 7. When an MSVC-style command line is longer than 4000
characters, the arguments after the compiler go into a response file, `@<dir>/args.rsp`. Every compile
has a timeout (`timeout_seconds`, 120 by default). The compiler runs in its own process group on POSIX
and inside a job object on Windows, so a timeout kills its whole process tree. Abort does not reach a
running compile yet (planned); it takes effect when the compile ends.

### Environment and wrappers

Old compilers depend on their environment. VC6's `cl.exe` needs `PATH` to include `MSDev98\Bin`
(for `mspdb60.dll`) and `VC98\Bin`, and needs `INCLUDE` and `LIB`. VS2008 needs `Common7\IDE` on
`PATH` (for `mspdb80.dll`). Toolchain entries express this with:

- `env`: sets a variable;
- `env_prepend`: puts a value in front of the inherited value, joined with the host's path separator.

These become `ProcessSpec.env` overrides, so the parent environment is otherwise inherited. Unsetting
a variable from a toolchain entry is not supported. For MSVC-style toolchains Decomp removes `CL` and
`_CL_`, which `cl.exe` and clang-cl read as extra command-line options, unless the toolchain sets them,
so a developer's shell settings cannot change codegen.

`wrapper` prefixes the command line. Its main use is running MSVC under Wine on Linux
(`["wine"]`). With Wine, the Windows-side search path is extended through `WINEPATH`, `WINEPREFIX`
selects the prefix, and the file paths Decomp passes must be translated to Windows form (`Z:\...`).
The field is part of the toolchain model and is passed through today. Wine support itself (path
translation and testing) is planned.

### Isolating parallel compiles

Each compile runs in a fresh directory under `.decomp/build/` (named after the cache key and a
counter), with fixed file names (`candidate.cpp`, `candidate.obj`), so that nothing leaks between
attempts. The compiler's working directory is that directory, so a PDB written by `/Zi` lands there
too. Newer MSVC versions write PDBs through the `mspdbsrv.exe` server, which concurrent compilers
would otherwise share, so for `msvc` toolchains Decomp sets `_MSPDBSRV_ENDPOINT_` per compile, which
gives each compile its own server instance. `/Z7` (debug information in the object) needs neither. A
successful compile's directory is deleted; a failed one is kept for inspection. Outside a project,
`decomp diff --source` compiles under the system's temporary directory. A global compile gate that
bounds concurrent compiler processes comes with the Phase 1 batch runner
([architecture.md](architecture.md#threading-model)).

### Diagnostics

Compiler output is parsed into `{file, line, column, severity, code, message}`:

| Producer | Form |
|---|---|
| MSVC (classic, including VC6) | `candidate.cpp(12) : error C2065: 'x' : undeclared identifier` |
| MSVC with columns | `candidate.cpp(12,5): error C2065: 'x': undeclared identifier` |
| clang-cl | `candidate.cpp(12,5): error: use of undeclared identifier 'x'` |
| GCC, Clang | `candidate.cpp:12:5: error: 'x' was not declared in this scope` |

Severities are `error`, `fatal error`, `warning` and `note`. Unrecognized lines are dropped from the
list but kept in the raw output. The agent receives up to 20 diagnostics, formatted as
`line 12:5: error C2065: 'x': undeclared identifier` (notes are dropped once 10 are shown), or the
first 4000 bytes of the raw output when nothing was recognized. `decomp diff --source` prints the raw
output. Clickable diagnostics in the diff viewer are Phase 1.

### Compile cache

Compilers are deterministic for identical inputs, so results are cached. The key is the SHA-1 of:

- the toolchain definition as JSON (kind, compiler path, wrapper, flags, include directories,
  environment, description and timeout; not its name);
- the project flags;
- the source bytes;
- for every project include directory, its path and the path, size and modification time of every
  file in it. Until dependency tracking exists, all of them are hashed, which is cheap and never stale.

The compiler's version is not part of the key; recording it is planned (see
[Determinism](#determinism)). The value is the object file plus a small JSON file with the result
(`ok` and the compiler output). Failed compiles are cached too, timeouts are not. In a project, entries
are stored flat under `.decomp/cache/objects/` as `<sha1>.obj` and `<sha1>.json`, and deleting the
directory clears the cache; `decomp diff --source` outside a project and `decomp toolchain test` do not
cache. A hit launches no process and is reported as cached in the `compile_finished` event and in the
tool result (`compile: ok (cached)`), so the transcripts show which attempts actually ran the compiler.

## Determinism

- **Compare code and referenced data only.** Object headers carry timestamps, and debug sections
  carry paths and signatures. None of that is compared.
- **Reproducible fixtures.** The test fixtures are built with `/Brepro` (clang-cl and lld-link) and
  committed, so unit tests need no compiler and produce stable results.
- **File names.** `__FILE__` (and therefore `assert`) embeds the source path in string literals, which
  *are* compared. The candidate is always named `candidate.cpp`, but it is passed to the compiler by
  its full path, which contains the compile directory and changes from compile to compile, so a
  function that uses `__FILE__` cannot match yet. Choosing the compile name to match source paths
  found in the target is planned.
- **Environment hygiene.** The toolchain's environment is applied explicitly, and the compiler runs in
  its own directory, so nothing depends on the caller's current directory. For MSVC-style toolchains
  `CL` and `_CL_` (extra options that `cl.exe` and clang-cl read from the environment) are removed
  unless the toolchain sets them.
- **Source encoding.** Old MSVC versions read source in the system code page. Candidate files are
  written as UTF-8 without a BOM, so non-ASCII bytes in string literals should be written as escapes
  (`"\xE9"`). Both sides compare as bytes, so mistakes surface as string mismatches rather than
  passing silently.
- **Toolchain drift.** The same toolchain name on two machines can mean two service packs. Recording
  the compiler's version banner, adding it to the cache key and showing it with every verification is
  planned. Today the cache key covers the toolchain's definition (including the compiler's path), not
  the compiler binary itself.
- **Deterministic outputs.** Reports and JSON files are sorted and stable. Timestamps appear only in
  run logs, run summaries and notes, never in diffs, `decomp.json` or `symbols.txt`.
- **Isolation.** Compiles share no working files, and concurrent MSVC compiles with different inputs
  get different PDB servers.

## First slice versus later phases

| Area | First slice | Later |
|---|---|---|
| Target formats | PE32, PE32+ | ELF64 (Phase 7) |
| Candidate objects | COFF from MSVC and clang-cl (including `/bigobj`), built with `/Gy` | Objects without `/Gy` (padding, same-section calls); ELF objects (Phase 7) |
| Address fields | Base relocations, relative branches, RIP-relative operands, stripped-`.reloc` heuristic (compared as values where the candidate has no relocation), MSVC x64 image-base-relative operands | — |
| Symbol sources | PDB 7.0 (publics, procedures, data), exports, imports, x64 `.pdata`, `symbols.txt` | MSVC `.map`, RTTI names, library signatures (Phase 2) |
| Data compared | Narrow and wide strings, floats and SSE constants, jump tables | Global initializers, EH and unwind tables, string and float pools, section placement (Phase 5) |
| Thunks | ILT and import thunks followed; names moved off ILT entries; the `dllimport` hint | |
| Jump tables | x86 absolute, clang x64 relative and MSVC x64 RVA tables, compared as index lists | Two-level (byte index) tables; robust in-`.text` bounds for PDB-less MSVC targets (Phase 2) |
| Hints | Register-only, stack-only, encoding, inverted branch, reordering, instruction count, branch target, binding, reference (string, constant, callee) and `dllimport` hints | `signature`, `gs_cookie`, `chkstk`, `eh_frame` |
| Toolchains | Registry with auto-detected clang-cl, `toolchain add`/`list`/`test`, compile cache, MSVC and GCC-style diagnostics; clang-cl round trip (Linux CI), `cl.exe` round trip (Windows CI, being brought up) | Version banner, project overrides, `CL`/`_CL_` removal, Wine wrapper on Linux; flag search and compiler identification (Phase 6) |
| Verification scope | Single functions | Whole translation units and relinking with a SHA-1 check of the result (Phase 5) |
