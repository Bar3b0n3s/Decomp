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
Decomp drives compilers deterministically.

Status: designed for the first slice (steps 7-8 in the [roadmap](roadmap.md#first-working-slice)), with
the extensions noted per section. Field names in examples are illustrative until the code lands.

## What "matching" means

| Verdict | Definition | Used for |
|---|---|---|
| `match_percent` | 0-100 score computed from weighted row differences | Progress, ranking attempts, choosing the best attempt |
| `exact` | Every row is `equal` after canonicalization, referenced data compares equal (strings, floats, jump tables), and every symbol binding is consistent | An intermediate signal |
| `byte_exact` | `exact`, and every byte is equal once each relocated field (already proven symbolically equivalent) is replaced with the target's bytes | The only verdict that marks a function `matched` |

Both flags exist because each hides something the other shows. Canonical text hides encoding choices:
`83 C0 01` and `05 01 00 00 00` both read `add eax, 1`, yet a different compiler version or flag
produced them. Raw bytes cannot compare relocated fields at all, because the candidate's fields hold
zeros or addends while the target's hold final addresses. `byte_exact` combines both: the symbolic
comparison proves the relocated fields equivalent, and the remaining bytes must be identical.

**What is compared:** the function's code bytes on both sides, the instructions they decode to, and,
by content, the data the function references directly: string literals, floating-point constants and
jump tables. Named globals and functions are compared by name.

**What is not compared** (in the slice): object-file metadata (timestamps, symbol order, debug
sections, `.drectve`); the function's address and its alignment padding; the contents of referenced
globals beyond their identity; exception-handling tables; and unwind data. Data matching and full
relinking come in Phase 5.

## Inputs

- **`TargetFunction`**: the byte range `[start, end)` from the image, as determined by `find_bounds()`
  ([architecture.md](architecture.md#analysis)), together with its decoded instructions, its
  address-bearing fields with resolved targets, the jump tables it uses, and the `SymbolDb` for
  naming.
- **`ObjFunction`**: the candidate function's bytes and instructions from the compiled object,
  together with the COFF relocations inside its range (offset, type, symbol name, embedded addend)
  and the object's own data sections, which hold string literals, constants and jump tables.

## The relocation-aware symbolic diff

### 1. Target side

The function's bytes come from the image. The **address-bearing fields** (the displacement,
immediate and relative fields reported by the decoder that actually hold an address) come from these
sources:

| Source | Fields | Confidence |
|---|---|---|
| Base relocations (`.reloc`: `HIGHLOW` on x86, `DIR64` on x64) | Absolute addresses in immediates and displacements | Certain |
| Relative branches and calls (`rel8`/`rel32`, Zydis `raw.imm[i].is_relative`) | Destination = end of instruction + displacement | Certain |
| RIP-relative displacements (x64) | Destination = end of instruction + displacement | Certain |
| Stripped `.reloc` (common in older EXEs, which were usually linked `/FIXED`) | Immediates and displacements whose value lies inside `[ImageBase, ImageBase + SizeOfImage)` | **Heuristic**, marked as such |
| Image-relative fields (RVAs, which never get base relocations) | Interpreted only where the candidate has a `DIR32NB`/`ADDR32NB` relocation at the aligned field, or a known pattern applies (x64 jump tables) | Candidate-guided |

Heuristic fields are only reported as mismatches when the candidate has a relocation at that field. If
the candidate holds a plain constant with the same bytes, the field is equal. Integers that happen to
look like addresses are therefore harmless.

Each resolved address is named through the `SymbolDb`, in order:

1. An address inside the function becomes an internal label (step 3).
2. An exact symbol, or a containing symbol plus offset (`g_table+8`).
3. An import slot becomes its `__imp_` name.
4. A string or float is identified by content, when the candidate's aligned operand is a literal or
   constant.
5. Anything else stays `unk_<va>`.

### 2. Candidate side

The candidate object is parsed with `coff::Object`. Decomp locates the function by the target's
decorated name, which the candidate must define exactly, together with its section and offset:

- With `/Gy` (function-level linking, implied by `/O1` and `/O2`), each function is a COMDAT section
  of its own, and the range is the symbol's offset to the section end.
- Without `/Gy`, functions share `.text`, and the range runs from the symbol to the next function
  symbol in the section (COFF type `0x20`) or to the section end.
- In both cases, trailing alignment padding (`int3`/`nop` runs) is trimmed, and a jump table at the
  end of the range is split off as data (see [jump tables](#jump-tables-inside-text-x86)).

Relocations inside the range give the candidate's address-bearing fields. COFF relocations have no
explicit addend: the field's existing contents are the addend. For example, `mov eax, [g_table+8]` is
a `DIR32` relocation to `g_table` with 8 stored in the field. Below, P is the address of the field.

| Machine | Type | Field | Value | Target-side counterpart |
|---|---|---|---|---|
| I386 | `DIR32` | 32-bit | VA of symbol + addend | `HIGHLOW` base relocation, or heuristic |
| I386 | `DIR32NB` | 32-bit | RVA of symbol + addend | None; read as RVA (candidate-guided) |
| I386 | `REL32` | 32-bit | symbol + addend - (P + 4) | Relative branch or call |
| I386 | `SECREL` | 32-bit | Offset of symbol in its section (thread-local variables) | Offset into `.tls` |
| I386 | `SECTION` | 16-bit | Section index of symbol (mostly debug data) | Section of the target address |
| AMD64 | `ADDR64` | 64-bit | VA of symbol + addend | `DIR64` base relocation |
| AMD64 | `ADDR32NB` | 32-bit | RVA of symbol + addend | None; read as RVA (candidate-guided) |
| AMD64 | `REL32` | 32-bit | symbol + addend - (P + 4) | Relative branch, call or RIP-relative operand |
| AMD64 | `REL32_1` ... `REL32_5` | 32-bit | symbol + addend - (P + 4 + N), where N immediate bytes follow the field | RIP-relative operand of an instruction with a trailing immediate |

`ABSOLUTE` entries are ignored, and AMD64 `SECREL` (thread-local variables) is handled like I386
`SECREL`. Any other type is reported in the diff header as unsupported, and that field is compared as
raw bytes.

### 3. Canonicalization

The `Normalizer` turns each instruction into a mnemonic (with prefixes such as `rep` and `lock`) and a
list of operand tokens. Registers are tokens. Immediates are values normalized to the operand size.
Memory operands are tokenized as `size seg:[base + index*scale + disp]`, and when the base is
`esp`/`ebp` (x86) or `rsp`/`rbp` (x64) the operand is classed as a *stack* operand. Every
address-bearing operand becomes a **`SymRef` key**:

| `SymRef` kind | Example key | Compared by |
|---|---|---|
| Named symbol (+ offset) | `?g_table@@3PAHA+8` | Exact decorated name and offset |
| Import slot | `__imp__MessageBoxA@16` | Name |
| String literal | `str:"Hello\n"`, `wstr:"Name"` | Bytes up to and including the terminator |
| Float constant | `f32:0x3f800000`, `f64:0x4008000000000000`, `x128:...` | Exact bit pattern |
| Internal label | `L12` | Through the alignment (below) |
| Jump table | `jt[L3,L7,L7,L9]` | Element-wise, through the alignment |
| Thread-local | `tls:?t_count@@3HA` | Name |
| Unknown address | `unk_004a3f20` | Binding (below) |

**Names compare by their exact decorated form.** That form encodes the calling convention and the
parameter types, so a wrong declaration in the candidate shows up as a symbol mismatch. When two
names differ but demangle to the same qualified name, the report says so (see the
[`signature` hint](#7-hints)).

**Strings and floats compare by content.** MSVC names string literals by an encoding of their content
(`??_C@_0...`), and floats by their bit pattern (`__real@...`). Older compilers without string pooling
emit anonymous `$SG...` symbols. The target only has addresses, possibly pooled or merged by the
linker. So on the candidate side, Decomp reads the literal or constant from the object's data section.
On the target side, it reads the same number of bytes at the referenced address (for strings, up to
the terminator, narrow or wide according to the candidate). The two compare equal when the bytes are
identical. Floats compare by bits, never numerically, so `-0.0`, `0.0` and NaN payloads stay distinct.

**Internal branch targets become instruction indices.** A branch whose destination lies inside the
function is written `jcc L<index>`, where the index is the destination instruction's position in that
side's listing. Because an insertion earlier in the function shifts every later index, branch operands
are not compared textually. After alignment (step 4), a target branch to `Li` and a candidate branch to
`Lj` are equal exactly when rows `i` and `j` are aligned with each other. A destination that falls
inside an instruction is flagged.

**Jump tables compare as lists of indices.** A table referenced by an indirect jump
(`jmp [reg*4 + table]`, or the x64 `__ImageBase`-relative form) is decoded on both sides into its
destinations, mapped to instruction indices, and compared element by element through the alignment.
Two-level MSVC switches add a byte-sized index table (`movzx reg, byte ptr [reg + index_table]`),
which compares as raw bytes. Table length is part of the comparison.

**Bindings.** When the target address has no symbol (`unk_<va>`, or a placeholder name created by
analysis) and the candidate references a named symbol at the aligned position, the pair is a binding,
for example "candidate uses `?g_player@@3PAVPlayer@@A` where the target uses `0x004A3F20`". Bindings
are collected over the whole function. They are **consistent** when each candidate symbol always pairs
with the same address, each address always pairs with the same symbol, and the address does not
already belong to another named symbol. Consistent bindings count as equal for the verdicts and are
listed in the report as binding suggestions. Inconsistent bindings make `operand(sym)` rows. When a
function is accepted as matched, its consistent bindings are recorded in `symbols.txt` with
`source=agent`. *Policy (proposed, open):* in Phase 1 the approval policy may require the user to
confirm new bindings before they are written.

### 4. Alignment

1. **Myers diff on mnemonics.** The two mnemonic sequences are diffed. Runs of equal mnemonics become
   paired rows; operands may still differ there. Everything else forms replace hunks (deletions from
   the target, insertions from the candidate).
2. **Pairing inside replace hunks.** Each hunk is aligned with a small dynamic program (weighted edit
   distance). The cost of pairing two instructions is the weight of the row kind they would produce
   (step 5), and a gap costs the insert/delete weight. Hunks are short, so the quadratic cost is
   negligible. Unpaired instructions become `insert` or `delete` rows.
3. **Second pass for branch operands.** With the alignment fixed, branch-target and jump-table
   operands are compared through the alignment map, as described above.

### 5. Row kinds

| Row kind | Meaning | Default weight |
|---|---|---|
| `equal` | Same mnemonic and operands after canonicalization | 0 |
| `operand(stack)` | Only a stack displacement differs | 1 |
| `operand(reg)` | Only register names differ | 5 |
| `operand(imm)` | An immediate differs | 10 |
| `operand(mem)` | A non-stack memory operand differs (base, index, scale or displacement) | 10 |
| `operand(sym)` | An address operand refers to a different symbol or content, or the binding is inconsistent | 20 |
| `opcode` | Paired instructions with different mnemonics | 50 |
| `insert` | Instruction only in the candidate | 100 |
| `delete` | Instruction only in the target | 100 |

A row with several differing operands takes the sum of their weights, capped at the `opcode` weight.
The weights follow the spirit of asm-differ's scorer: cheap for differences that usually come from
local layout or register choice, expensive for structural ones. They are initial values and will be
tuned against the fixtures.

### 6. Verdicts and score

```
match_percent = 100 * max(0, 1 - total_weight / (100 * max(n_target, n_candidate)))
```

The score is shown floored to one decimal, so `100.0` appears only for `exact` functions.

- `exact`: every row is `equal`, all referenced data compares equal, and all bindings are consistent.
- `byte_exact`: only checked when `exact`. Rows pair one-to-one, so for each pair Decomp copies the
  target's bytes over every relocated field of the candidate instruction (the field must sit at the
  same offset with the same size), then requires the instruction bytes and lengths to be identical.
  Jump tables in the range compare as table data, not as code bytes.

### 7. Hints

Hints are short, structured explanations for the agent and the user. They name the rows they refer to.

| Hint | Detected when | Usually means |
|---|---|---|
| `regalloc` | All differing rows are `operand(reg)` and the register mapping is a consistent permutation | Declaration order, expression shape or variable types; not compiler flags |
| `stack_layout` | All differing rows are `operand(stack)` with a consistent offset mapping | Local variable order, sizes or types (an array versus scalars, for example) |
| `branch_polarity` | A conditional jump pairs with its inverse and the taken and fall-through blocks are swapped | Negate the condition or swap the `if`/`else` bodies |
| `reorder` | Inserted and deleted instructions form the same multiset within a window | Statement order or evaluation order |
| `encoding` | `exact` but not `byte_exact` | Compiler version or flags, not the source |
| `binding` | Consistent bindings exist | Name suggestions for unnamed target addresses |
| `unresolved` | Inconsistent or conflicting symbol pairings | The candidate references the wrong global or function |

Additional detectors, planned after the core set: `signature` (names differ but demangle to the same
qualified name, which points to parameter types or calling convention); `gs_cookie`, `chkstk`,
`dllimport` and `eh_frame` (see [MSVC specifics](#msvc-specifics)).

### 8. Output formats

`Report` renders the same result two ways.

**Text, for humans** (`decomp diff`; colored on a TTY). The example below is a register-allocation-only
difference in a fixture function:

```
sum_array  ?sum_array@@YAHPBHH@Z  0x00401030  23 bytes
match 97.0%  exact: no  byte_exact: no   equal 4  operand 6  opcode 0  insert 0  delete 0

  #  off  target                |  off  candidate
  0  00   mov  ecx, [esp+8]    ~|  00   mov  edx, [esp+8]    reg
  1  04   xor  eax, eax        =|  04   xor  eax, eax
  2  06   test ecx, ecx        ~|  06   test edx, edx        reg
  3  08   jle  L9              =|  08   jle  L9
  4  0a   mov  edx, [esp+4]    ~|  0a   mov  ecx, [esp+4]    reg
  5  0e   add  eax, [edx]      ~|  0e   add  eax, [ecx]      reg
  6  10   add  edx, 4          ~|  10   add  ecx, 4          reg
  7  13   dec  ecx             ~|  13   dec  edx             reg
  8  14   jne  L5              =|  14   jne  L5
  9  16   ret                  =|  16   ret

hints:
  regalloc  Only register allocation differs: candidate edx/ecx are target ecx/edx (rows 0, 2, 4-7).
```

Options select raw bytes, relocation markers and differing rows only.

**Compact JSON, for the agent** (the `compile_and_diff` result; `decomp diff --json` emits the full
form). It contains the header, the hints, the bindings, and the differing rows with 2 rows of context
on each side. The output is capped in length, and `omitted_rows` counts what was cut. Keys are sorted,
so the output is deterministic.

```json
{
  "bindings": [],
  "byte_exact": false,
  "counts": {"delete": 0, "equal": 4, "insert": 0, "opcode": 0, "operand": 6},
  "exact": false,
  "function": "?sum_array@@YAHPBHH@Z",
  "hints": [
    {"kind": "regalloc", "rows": [0, 2, 4, 5, 6, 7],
     "text": "Only register allocation differs: candidate edx/ecx are target ecx/edx."}
  ],
  "match_percent": 97.0,
  "omitted_rows": 0,
  "rows": [
    {"c": "mov edx, [esp+8]", "i": 0, "kind": "operand", "sub": "reg", "t": "mov ecx, [esp+8]"},
    {"i": 1, "kind": "equal", "t": "xor eax, eax"},
    {"c": "test edx, edx", "i": 2, "kind": "operand", "sub": "reg", "t": "test ecx, ecx"},
    {"i": 3, "kind": "equal", "t": "jle L9"},
    {"c": "mov ecx, [esp+4]", "i": 4, "kind": "operand", "sub": "reg", "t": "mov edx, [esp+4]"},
    {"c": "add eax, [ecx]", "i": 5, "kind": "operand", "sub": "reg", "t": "add eax, [edx]"},
    {"c": "add ecx, 4", "i": 6, "kind": "operand", "sub": "reg", "t": "add edx, 4"},
    {"c": "dec edx", "i": 7, "kind": "operand", "sub": "reg", "t": "dec ecx"},
    {"i": 8, "kind": "equal", "t": "jne L5"},
    {"i": 9, "kind": "equal", "t": "ret"}
  ]
}
```

## MSVC specifics

### `/Gy` and COMDAT sections

With `/Gy`, every function is emitted into its own COMDAT section (`IMAGE_SCN_LNK_COMDAT`). The
section-definition auxiliary record carries the length, a checksum and the selection kind: no
duplicates for ordinary functions, "any" for inline functions and pooled literals. Associative COMDATs
attach data to the function, such as x64 `.pdata`/`.xdata` and debug symbols. Extraction uses the
symbol's own section when it is a COMDAT. Otherwise the range runs to the next function symbol, as
described in [step 2](#2-candidate-side). `/O1` and `/O2` imply `/Gy` in all supported versions.

### String literals (`??_C@`)

With string pooling (`/GF`, which `/O1` and `/O2` imply), literals become COMDATs named `??_C@_0...`
(narrow) or `??_C@_1...` (wide). The name encodes the length, a hash and a prefix of the content.
Without pooling, older compilers place literals in `.data` or `.rdata` under anonymous `$SG<n>` local
symbols. Decomp compares both by content. A wrong literal is therefore an `operand(sym)` row that shows
both strings. A literal placed in a different section, for example because of a `const` mismatch, is
caught in Phase 5 when data placement is verified.

### Floating-point constants (`__real@`, `__xmm@`)

MSVC materializes float and double constants as COMDATs named after their bit pattern
(`__real@3f800000` is `1.0f`, `__real@4008000000000000` is `3.0`). 16-byte SSE constants are named
`__xmm@...`. Decomp reads 4, 8 or 16 bytes at the target address, according to the candidate's
constant, and compares bit patterns. A float-versus-double mix-up shows as `f32:...` against
`f64:...`.

### Jump tables inside `.text` (x86)

MSVC x86 places a switch's jump table, and for sparse switches a byte index table, directly after the
function's code in `.text`. In the object they sit inside the function's COMDAT, with `DIR32`
relocations pointing back into the function. On the target, the entries are absolute addresses
covered by `HIGHLOW` base relocations, when relocations are present. Bounds detection must stop
decoding before the table. Disassembly shows the table as data, and the diff compares it as index
lists (see [step 3](#3-canonicalization)). MSVC x64 tables hold 32-bit image-relative entries
(`ADDR32NB`) indexed through `__ImageBase`. The slice detects tables from the indirect-jump pattern.
Phase 2 hardens this for PDB-less MSVC targets, where the table's end also has to be found without a
symbol size.

### `/OPT:ICF` folding

Identical COMDAT folding makes the linker keep one copy of byte-identical functions (and read-only
data), so several names can share one address. Consequences:

- The PDB may list several procedures at one address. The function is matched once; the other names
  are aliases. The slice's `SymbolDb` keeps one symbol per address, and recording aliases is an open
  item that Phase 5 needs, because relinking has to produce every alias.
- A call in the target may land on a body whose primary name differs from the callee the source
  used. Canonicalization must accept any alias name for a call target.
- Folded bodies are identical by definition, so matching any one of them verifies the code at that
  address.

### Incremental-linking and import thunks

Binaries linked with `/INCREMENTAL` (typical for debug builds) route calls through an incremental
linking table of `jmp rel32` thunks. Calls to imported functions take one of two forms. With
`__declspec(dllimport)`, the call is indirect through the IAT slot (`call [__imp__Foo@4]`). Without it,
the call goes to a linker-generated stub, `jmp [__imp__Foo@4]`. On the target side, Decomp follows a
single `jmp` thunk to name the call's real destination, so `call ILT+0x120` reads as
`call ?Update@Player@@QAEXM@Z`. If the target calls through the IAT and the candidate calls the stub,
or the reverse, the `dllimport` hint names the declaration to fix. Imports themselves compare by their
`__imp_` names from the slice onward. Thunk resolution is Phase 2.

### SEH and C++ EH prologs

On x86, functions with C++ exception handling register a frame in the prolog: `push -1`,
`push offset __ehhandler$<fn>`, then a load of `fs:[0]`, a push, and a store to `fs:[0]`. The
`__ehhandler$<fn>` routine is a compiler-generated companion that passes a `__ehfuncinfo$<fn>` table
to `__CxxFrameHandler`. Structured exception handling (`__try`) uses a scope table and
`__except_handler3` or `__except_handler4`. A companion of the function being matched can only exist
in the candidate object under its own name, while the target has only an address. In the slice,
references to these companions bind structurally: a companion whose name embeds the matched
function's name may bind to the aligned target address. Comparing the companion bodies and the
`FuncInfo`/scope tables is part of data matching in Phase 5. An EH prolog present on only one side
produces the `eh_frame` hint: exception-handling flags, objects with destructors, or `try` blocks.
x64 has no prolog registration (handling is table-based, through `.pdata`/`.xdata`).

### `/GS` security cookies

With `/GS` (available since Visual Studio .NET 2002 and on by default since Visual Studio 2005), a
function with vulnerable local buffers loads `__security_cookie`, XORs it with the frame pointer,
stores it below the locals, and checks it before returning with a call to `__security_check_cookie`.
The compiler's buffer heuristics decide which functions get a cookie. A cookie on only one side
therefore points either to the flags or to the local declarations (an array versus a struct, for
example), and the `gs_cookie` hint says which. VC6 predates `/GS`.

### `__chkstk`

Frames larger than a page are allocated through a stack probe. On x86 this is `mov eax, <size>`
followed by `call __chkstk` (the CRT routine is also known as `_alloca_probe`). On x64 it is
`mov eax, <size>`, `call __chkstk`, `sub rsp, rax`. Without a probe, the frame is allocated with
`sub esp, <size>`. A probe on only one side means the total size of the locals differs, and the
`chkstk` hint reports both sizes.

### Rich header compiler IDs

Images produced by Microsoft linkers usually carry a Rich header between the DOS stub and the PE
header. It is XOR-masked and is located through its `Rich` and `DanS` markers. Each entry records a
product ID, a build number and a count: which compiler front ends, linkers and assemblers built how
many of the objects. That identifies the exact MSVC version and service pack, separately for C and
C++ objects, and reveals objects built with link-time code generation. The slice decodes and displays
the entries. Phase 2 maps them to toolchain suggestions through a compiler table, and Phase 6 probes
candidate compilers when the header is absent.

### x64 `.pdata`

On x64, every non-leaf function has a `RUNTIME_FUNCTION` entry in `.pdata` (begin RVA, end RVA,
unwind info RVA). These entries give exact bounds without symbols, and Phase 2 uses them for bounds.
Leaf functions that neither allocate stack nor save registers may have no entry. Functions split by
the optimizer appear as chained unwind entries and are merged into one function. Comparing the unwind
data itself (`.xdata`) belongs to data matching in Phase 5.

### Whole-program optimization (`/GL`)

Objects compiled with `/GL` contain intermediate code, and the machine code is generated at link time
(`/LTCG`), where cross-function inlining and calling-convention changes happen. Per-function
compile-and-diff cannot reproduce those decisions. The Rich header shows LTCG objects, and Decomp
warns when the target contains them. Targets built that way are out of scope for the slice.

## Driving compilers

### Toolchains and the registry

A `Toolchain` describes how to run one compiler:
`{name, kind: msvc | clang_cl | gcc | clang, compiler, wrapper argv, env set/prepend, base flags,
include dirs, obj format}`. Toolchains live in a user-level registry, because paths differ per
machine: `%APPDATA%\decomp\toolchains.json` on Windows and `~/.config/decomp/toolchains.json` on
Linux. Projects reference them by name and may override entries. The file format and example entries
for VC6, VS2008, clang-cl and Wine are in [project-format.md](project-format.md#toolchain-registry).

`decomp toolchain list` shows the registry. `decomp toolchain test` runs a health check per toolchain:
the compiler starts with its environment, its version banner is recorded (for example the
`Version 12.00.8804` line of VC6's `cl.exe`), a small known TU compiles, and the resulting object
parses. A failed check names the missing piece, such as a DLL not on `PATH` or an empty `INCLUDE`.

### Invocation

For MSVC-style compilers (`msvc`, `clang_cl`), the command line is:

```
[wrapper...] <compiler> <base flags> <project flags> /I<include dir>... /c /Fo<dir>\candidate.obj <dir>\candidate.cpp
```

Project flags come after the toolchain's base flags so that they take precedence. clang-cl selects the
target with `--target=i686-pc-windows-msvc` or `--target=x86_64-pc-windows-msvc` in its base flags.
GCC and Clang (`-c -o`) follow in Phase 7. On Windows, long command lines go through a response file.
Every compile has a timeout. On Windows the compiler runs inside a job object, so a timeout or Abort
kills its whole process tree.

### Environment and wrappers

Old compilers depend on their environment. VC6's `cl.exe` needs `PATH` to include `MSDev98\Bin`
(for `mspdb60.dll`) and `VC98\Bin`, and needs `INCLUDE` and `LIB`. VS2008 needs `Common7\IDE` on
`PATH` (for `mspdb80.dll`). Toolchain entries express this with:

- `env.set`: replaces a variable (a `null` value unsets it);
- `env.prepend`: puts entries in front of the inherited value, joined with the host's path separator.

These become `ProcessSpec.env` overrides, so the parent environment is otherwise inherited. Decomp
always removes `CL` and `_CL_`, which MSVC reads as extra command-line options, so a developer's shell
settings cannot change codegen.

`wrapper` prefixes the command line. Its main use is running MSVC under Wine on Linux
(`["wine"]`). With Wine, the Windows-side search path is extended through `WINEPATH`, `WINEPREFIX`
selects the prefix, and the file paths Decomp passes must be translated to Windows form (`Z:\...`).
The field is part of the toolchain model from the slice onward. Wine support itself (path translation
and testing) comes after the slice.

### Isolating parallel compiles

Each compile runs in a fresh directory under `.decomp/build/`, with fixed file names
(`candidate.cpp`, `candidate.obj`), so that nothing leaks between attempts or workers. When the flags
include `/Zi`, the PDB goes into the same directory (`/Fd`). Newer MSVC versions write PDBs through the
`mspdbsrv.exe` server, which concurrent compilers would otherwise share, so Decomp also sets a unique
`_MSPDBSRV_ENDPOINT_` per worker, which gives each worker its own server instance. `/Z7` (debug
information in the object) needs neither. A global compile gate bounds the number of concurrent
compiler processes ([architecture.md](architecture.md#threading-model)).

### Diagnostics

Compiler output is parsed into `{file, line, col, severity, code, msg}`:

| Producer | Form |
|---|---|
| MSVC (classic, including VC6) | `candidate.cpp(12) : error C2065: 'x' : undeclared identifier` |
| MSVC with columns | `candidate.cpp(12,5): error C2065: 'x': undeclared identifier` |
| clang-cl | `candidate.cpp(12,5): error: use of undeclared identifier 'x'` |
| GCC, Clang | `candidate.cpp:12:5: error: 'x' was not declared in this scope` |

Unrecognized lines are kept in the raw log. The agent receives the first errors, capped in count and
length. The diff viewer makes each diagnostic clickable.

### Compile cache

Compilers are deterministic for identical inputs, so results are cached. The key is the SHA-1 of:

- the toolchain fingerprint: name, kind, compiler path, the version recorded by the health check, the
  wrapper and the environment overrides;
- the complete flag list and include directories;
- the source bytes;
- the contents of the project's include directories. Until dependency tracking exists, all of them
  are hashed, which is cheap and never stale.

The value is the object file plus the `CompileOutput` metadata, including failed compiles with their
diagnostics. Entries are content-addressed under `.decomp/cache/`, and deleting the directory clears
the cache. A hit launches no process and is reported as cached in the compile events, so the UI and
transcripts show which attempts actually ran the compiler.

## Determinism

- **Compare code and referenced data only.** Object headers carry timestamps, and debug sections
  carry paths and signatures. None of that is compared.
- **Reproducible fixtures.** The test fixtures are built with `/Brepro` (clang-cl and lld-link) and
  committed, so unit tests need no compiler and produce stable results.
- **Stable file names.** `__FILE__` (and therefore `assert`) embeds the source path in string
  literals, which *are* compared. Compiles use fixed relative names in a fresh directory. If the
  target contains source paths, the agent sees them in the referenced strings, and the compile name
  can be chosen to match. How that is configured is open.
- **Environment hygiene.** `CL` and `_CL_` are removed, the toolchain's environment is applied
  explicitly, and nothing depends on the caller's current directory.
- **Source encoding.** Old MSVC versions read source in the system code page. Candidate files are
  written as UTF-8 without a BOM, so non-ASCII bytes in string literals should be written as escapes
  (`"\xE9"`). Both sides compare as bytes, so mistakes surface as string mismatches rather than
  passing silently.
- **Toolchain drift.** The same toolchain name on two machines can mean two service packs. The health
  check records the version banner. It is part of the cache key and shown in the UI, so a
  verification made with a different build is visible.
- **Deterministic outputs.** Reports and JSON files are sorted and stable. Timestamps appear only in
  event logs and history records, never in diffs or project files.
- **Isolation.** Parallel compiles share no files, PDBs or PDB servers.

## First slice versus later phases

| Area | First slice | Later |
|---|---|---|
| Target formats | PE32, PE32+ | ELF64 (Phase 7) |
| Candidate objects | COFF from MSVC and clang-cl | ELF objects (Phase 7) |
| Address fields | Base relocations, relative branches, RIP-relative operands, stripped-`.reloc` heuristic, candidate-guided RVAs | - |
| Symbol sources | PDB 7.0, exports, imports, `symbols.txt` | MSVC `.map`, `.pdata` bounds, RTTI names, library signatures (Phase 2) |
| Data compared | Strings, floats, jump tables | Global initializers, EH and unwind tables, string and float pools, section placement (Phase 5) |
| Thunks | Imports by `__imp_` name | ILT and import-stub resolution (Phase 2) |
| Jump tables | Indirect-jump tables read as data and compared as index lists | Robust in-`.text` bounds for PDB-less MSVC targets (Phase 2) |
| Hints | `regalloc`, `stack_layout`, `branch_polarity`, `reorder`, `encoding`, `binding`, `unresolved` | `signature`, `gs_cookie`, `chkstk`, `dllimport`, `eh_frame` |
| Toolchains | Registry, health check, cache, MSVC and clang diagnostics; clang-cl round trip (Linux and Windows CI), `cl.exe` round trip (Windows CI) | Wine wrapper on Linux; flag search and compiler identification (Phase 6) |
| Verification scope | Single functions | Whole translation units and relinking with a SHA-1 check of the result (Phase 5) |
