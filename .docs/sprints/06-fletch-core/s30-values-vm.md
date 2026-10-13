# Sprint 30: Fletch Values, Bytecode, VM, GC

## Prerequisites

- Sprint 28 — `.docs/fletch-spec.md` frozen. This sprint implements §4
  (values/types), §5 (expressions), §6 (statements), §7 (scoping/closures),
  §8 (functions), §9 (errors, VM half), §10 (`edit` opcodes). Cite sections
  in code comments; never re-decide syntax here.
- Sprint 29 — `lex.c|h`, `ast.h`, `parse.c|h`: `FlSpan`, `FlNode` (≤ 48 B,
  arena-allocated, child arrays sized once), `FlProgram { FlNode **stmts;
  u32 n; bool incomplete; }`, `fl_parse`, `fl_parse_literal`, `fl_ast_dump`,
  expected/found diagnostics, the 20-error cap. The compiler consumes an AST
  that already parsed clean.
- Sprint 0 — `Arena`, `Vec(T)`, `Strmap`, `Interner` (stable `u32` ids in
  first-intern order), `Bytebuf`, `yew_sort_stable`, `yew_log`, `YEW_BUG`,
  exit codes. Assumption beyond s00's entry, as s29 also made it: `DiagCtx`
  + `diag_emit(dc, level, span, fmt, …)` with caret rendering.
- Sprint 1 — unit harness, `make test`, gcc/clang/sanitize/valgrind/
  determinism lanes, `scripts/bans.sh`. Sprint 2 — `yew_utf8_*` and the
  lossless escape policy: **Fletch strings hold raw bytes verbatim** and
  never "clean" U+DC80–DCFF escapes (s02 §4 layer table).
- Binding: `00-decisions.md` (Fletch row: tagged union, interned strings,
  mark-sweep with explicit roots; invariants 3, 5, 7, 8) and
  `02-fletch.md` req 7 (start ~0, config load < 1 ms, motion dispatch ~1 µs
  — s33 gates them; this sprint must not design them away).

## Goals

Land `src/fl/value.c|h`, `src/fl/vm.c|h` (with `src/fl/opcodes.def`),
`src/fl/compile.c|h`, `src/fl/gc.c|h`: the `FlValue` tagged union and its
heap objects, a 60-opcode stack bytecode, a single-pass AST→bytecode
compiler with constant dedup / local slots / by-reference upvalues / jump
patching, the VM loop with a switch reference implementation and an
optional computed-goto twin, and a precise mark-sweep GC with an
enumerated root set and a stress mode. After this sprint a Fletch program
with no stdlib and no editor **runs**: arithmetic, control flow, closures,
lists, maps, `try`/`catch`, `edit` as a plain block, and motion blocks
raising kind `"motion"` against the null host. Deferred: the standard
library and `import` resolution (Sprint 31 — the `IMPORT` opcode
hard-errors naming it), the REPL / `yew fl` / full stack traces / the VM
fuzzer (Sprint 32), editor handles and real motion execution (Sprint 34 —
the five handle tags are reserved and unconstructible), the recorder
(Sprint 35), `--batch` (Sprint 37).

## Deliverables

### 1. `FlValue` — `src/fl/value.h`

```c
typedef enum FlType {
    FL_NIL = 0, FL_BOOL, FL_INT, FL_FLOAT,          /* immediates          */
    FL_STR, FL_LIST, FL_MAP, FL_FN, FL_CLOSURE, FL_NATIVE,  /* GC objects  */
    /* Reserved handles — spec §4. Constructible only from Sprint 34.      */
    FL_BUF, FL_CURSOR, FL_SPAN, FL_WIN, FL_REGEX,
    FL_TYPE_COUNT
} FlType;

typedef struct FlObj FlObj;

typedef struct FlValue {
    u8  t;            /* FlType                                            */
    u8  flags;        /* FL_VF_* — spare; zero today                       */
    u16 rsv0; u32 rsv1;
    union { bool b; i64 i; double f; FlObj *o; } as;
} FlValue;
_Static_assert(sizeof(FlValue) == 16, "FlValue size budget (s30 DoD 2)");
_Static_assert(FL_TYPE_COUNT <= 32,   "type tag fits u8 with room to spare");
```

**Decision: no NaN boxing. The tagged union above is final for 1.0.**
Why, stated once so it is never relitigated:

| Argument | Weight |
|---|---|
| Fletch integers are **i64** (§4). NaN boxing carries ≤ 51 payload bits, so every int past ±2^50 must heap-box or split into a second tag. Boxing the most common value type to save 8 bytes is a net loss. | decisive |
| Pointer payloads assume 47–48 significant address bits. We ship `arm64-linux` and `arm64-macos`, where 52-bit VA and top-byte-ignore are live configurations. *Probably* fine on a shipped target is not fine. | decisive |
| It needs bit-punning plus reliance on quiet-NaN payload preservation — outside the C11-subset box (C99 core + three named extensions, nothing clever), and it costs debuggability: a 16-byte struct prints in gdb and reads in ASan reports; a boxed `u64` needs a pretty-printer nobody maintains. | strong |
| Cost is bounded: 16 B × a 16 384-slot stack = 256 KiB, once. `02-fletch.md` req 7's budgets are dominated by parse+compile and by host work, not value copies — s33 proves it or we revisit *with numbers*. | sufficient |

The two `rsv` fields are deliberate slack: they keep the struct at 16 B
either way and give Sprint 34 somewhere for a handle generation counter
(`{ u32 slot; u32 gen; }`) without a layout change. Do not repurpose them.

```c
#define FL_NIL_V        ((FlValue){ .t = FL_NIL })
#define FL_BOOL_V(x)    ((FlValue){ .t = FL_BOOL,  .as.b = (x) })
#define FL_INT_V(x)     ((FlValue){ .t = FL_INT,   .as.i = (x) })
#define FL_FLOAT_V(x)   ((FlValue){ .t = FL_FLOAT, .as.f = (x) })
#define FL_OBJ_V(tg, p) ((FlValue){ .t = (tg),     .as.o = (FlObj *)(p) })

bool        fl_truthy(FlValue v);            /* §4: only nil/false falsy   */
bool        fl_equal(FlValue a, FlValue b);  /* §4 equality rules          */
const char *fl_type_name(u8 t);              /* "int", "buffer", …         */
u32         fl_hash(FlValue v);              /* map keys: str|int|bool     */
```

`fl_type_name` knows all 15 names including the five reserved handles —
error messages must say `buffer`, not `<type 10>`, from day one.

### 2. Heap objects — `src/fl/value.h`

```c
struct FlObj {                       /* 16 B header, every heap object     */
    u8     t;                        /* FlType of the owner                */
    u8     mark;                     /* GC: 0 white, 1 gray/black          */
    u16    oflags;                   /* FL_OF_INTERNED | FL_OF_FROZEN      */
    u32    aux;                      /* type-specific (str hash, fn arity) */
    FlObj *gc_next;                  /* the all-objects list               */
};

typedef struct { FlObj h; u32 len; char b[]; } FlStr;      /* immutable    */
typedef struct { FlObj h; FlValue *v; u32 n, cap; u32 mods; } FlList;
typedef struct FlMap  FlMap;      /* §3 below                              */
typedef struct FlFn   FlFn;       /* §4 below                              */
typedef struct { FlObj h; FlFn *fn; FlUpval **up; u32 nup; } FlClosure;
typedef struct { FlObj h; FlValue *slot; FlValue closed; struct FlUpval *next; } FlUpval;
typedef struct { FlObj h; FlNativeFn fn; u32 name_id; u8 min_ar, max_ar; u32 caps; } FlNative;
```

`FlStr` uses a C99 flexible array member: one allocation per string, header
and bytes contiguous, `aux` holds the FNV-1a hash computed at construction
(eager, not lazy — map keys need it and a lazy branch costs more than the
walk over bytes we just wrote). Strings are immutable; there is no mutating
string op anywhere in the tree.

`FlList.mods` and the map's counterpart are **structural** modification
counters (push/pop/insert/remove/clear — not element assignment). §7 pins
what they are for.

### 3. `FlMap` — insertion-ordered, deterministically

```c
typedef struct { FlValue k, v; u32 hash; bool dead; } FlMapEnt;
struct FlMap {
    FlObj      h;
    FlMapEnt  *ent;  u32 n, cap;   /* DENSE, append-only: insertion order  */
    u32       *idx;  u32 icap;     /* open-addressed, holds ent index + 1  */
    u32        ndead, mods;
};
```

The same discipline `Strmap` uses in `src/util/` (Cgfried-proven): the
dense array *is* the iteration order, the index array is only a lookup
accelerator. Rules pinned here because determinism (invariant 5) depends on
them and "obvious" alternatives break it:

| Rule | Why |
|---|---|
| `keys()`/`values()`/`entries()`/`for k, v in m` walk `ent[0..n)` skipping `dead` | insertion order, byte-identical across runs and libcs |
| Delete marks `dead` and tombstones the index slot; it never moves entries | moving the last entry into the hole (the usual trick) makes order depend on delete history |
| Re-inserting a deleted key appends at the **end** | the alternative (revive in place) makes order depend on whether a key was ever deleted — invisible, untestable, wrong |
| Compaction (rebuild `ent` + `idx`) when `ndead > n/2`, and only there | bounded amortized cost, one predictable order change: dead rows vanish, live order preserved |
| Growth at 70 % load, capacity powers of two, probing linear | linear probing keeps the compaction walk cache-friendly |

Keys are `string | int | bool` (spec §4). Any other key type raises kind
`"key"` naming the offending type. `fl_hash` on a float key is unreachable
by construction — do not add one "just in case"; the error message is the
feature.

### 4. Functions, chunks, and the line table

```c
typedef struct { u32 pc, line, col; } FlLineRun;   /* sorted by pc         */

typedef struct FlChunk {
    u8        *code;   u32 ncode;
    FlValue   *consts; u32 nconsts;
    FlLineRun *lines;  u32 nlines;
    u32        file_id;
} FlChunk;

struct FlFn {
    FlObj    h;
    FlChunk  ch;
    u32      name_id;        /* 0 = anonymous → traces print "<fn>"        */
    u8       arity, nup;
    u16      max_stack;      /* computed by the compiler (§6)              */
    FlOrigin origin;         /* capability origin, spec §13; used by s31   */
};
```

The compiler emits into a `Bytebuf` + `Vec(FlValue)` and **copies to the
arena at final size** when a function completes. Chunks are then immutable
and shareable. Pitfall (s29 pinned the same one): a growing array with live
pointers into it dangles; the copy-at-completion step is what makes patched
jump sites safe to record as offsets rather than pointers.

Line info is a run table, binary-searched by pc, carrying **line *and*
col** — s32's stack traces print `file:line:col` and cannot reconstruct a
column after the fact. Pin: runs are keyed by *instruction start* pc, and
the VM stores `frame->ip` before dispatching a `CALL`, so a trace lookup is
exact rather than "the instruction after the one that failed".

### 5. Opcode listing — `src/fl/opcodes.def`

One X-macro is the single source of truth for name, operand shape, and
stack effect. The switch dispatcher, the computed-goto label table, the
disassembler, and the compiler's `max_stack` computation are all generated
from it. **This is what keeps the two dispatch paths from drifting.**

```c
/* FL_OP(NAME, operands, effect, doc) — operands: "" | "b" (u8) |
 * "s" (i8) | "w" (u16) | combinations; effect: net stack delta, or
 * FL_EFF_VAR for ops whose delta depends on an operand.               */
FL_OP(CONST,   "w",  +1, "push consts[k]")
FL_OP(NIL,     "",   +1, "push nil")
...
```

| # | Opcode | Operands | Effect | Semantics |
|---|---|---|---|---|
| 1 | `CONST` | u16 k | +1 | push `consts[k]` |
| 2 | `NIL` | — | +1 | |
| 3 | `TRUE` | — | +1 | |
| 4 | `FALSE` | — | +1 | |
| 5 | `INT8` | i8 | +1 | small-int fast path; keeps `0`/`1`/`-1` out of the pool |
| 6 | `NIL_N` | u8 n | +n | declare n locals without initializers |
| 7 | `POP` | — | −1 | |
| 8 | `POPN` | u8 n | −n | block exit |
| 9 | `DUP` | — | +1 | |
| 10 | `GET_LOCAL` | u8 s | +1 | `frame->slots[s]` |
| 11 | `SET_LOCAL` | u8 s | −1 | assignment is a **statement** (§4) — no value left |
| 12 | `GET_UPVAL` | u8 u | +1 | |
| 13 | `SET_UPVAL` | u8 u | −1 | |
| 14 | `CLOSE_UPVALS` | u8 s | var | close open upvalues with slot ≥ s, then `sp = slots + s` |
| 15 | `GET_GLOBAL` | u16 k | +1 | `consts[k]` is the interned name; miss → kind `"name"` |
| 16 | `SET_GLOBAL` | u16 k | −1 | miss → kind `"name"` (no implicit globals, §4) |
| 17 | `DEF_GLOBAL` | u16 k | −1 | module top level only |
| 18 | `ADD` | — | −1 | int/float per §4; also `str+str`, `list+list` |
| 19 | `SUB` | — | −1 | |
| 20 | `MUL` | — | −1 | |
| 21 | `DIV` | — | −1 | int `/` truncates toward zero; ÷0 → kind `"div"` |
| 22 | `MOD` | — | −1 | sign of the dividend; %0 → kind `"div"` |
| 23 | `NEG` | — | 0 | |
| 24 | `NOT` | — | 0 | truthiness, never an error |
| 25 | `EQ` | — | −1 | §4 equality (value for scalars, identity for objects) |
| 26 | `NE` | — | −1 | |
| 27 | `LT` | — | −1 | numbers numerically, strings bytewise |
| 28 | `LE` | — | −1 | |
| 29 | `GT` | — | −1 | |
| 30 | `GE` | — | −1 | |
| 31 | `JUMP` | u16 d | 0 | forward, `ip += d` |
| 32 | `JUMP_BACK` | u16 d | 0 | `ip -= d`; loops only |
| 33 | `JUMP_IF_FALSE` | u16 d | −1 | |
| 34 | `JUMP_IF_TRUE` | u16 d | −1 | |
| 35 | `OR_JUMP` | u16 d | 0 | peek: jump if truthy, leaving the value |
| 36 | `AND_JUMP` | u16 d | 0 | peek: jump if falsy, leaving the value |
| 37 | `CALL` | u8 n | −n | callee is at `sp-n-1`; result replaces it |
| 38 | `RETURN` | — | var | pop frame, TOS to caller |
| 39 | `RETURN_NIL` | — | var | bare `return` and fall-off-the-end |
| 40 | `CLOSURE` | u16 k, then n×(u8 is_local, u8 idx) | +1 | capture per §8 |
| 41 | `LIST` | u16 n | −n+1 | |
| 42 | `MAP` | u16 n | −2n+1 | key,value pairs in source order |
| 43 | `INDEX_GET` | — | −1 | list: int index, kind `"index"` OOR; map: kind `"key"` |
| 44 | `INDEX_SET` | — | −3 | `[c, i, v] → []` |
| 45 | `FIELD_GET` | u16 k | 0 | `.name` ≡ `["name"]` (§4); modules read the same way |
| 46 | `FIELD_SET` | u16 k | −2 | frozen object → kind `"type"` |
| 47 | `ITER_NEW` | — | +2 | pops iterable, pushes 3 hidden slots: subject, cursor, mods snapshot |
| 48 | `ITER_NEXT1` | u8 base, u16 d | +1 | `for x in …`; jump `d` when exhausted |
| 49 | `ITER_NEXT2` | u8 base, u16 d | +2 | `for k, v in …` |
| 50 | `MOTION` | u16 k | +1 | run `consts[k]` (an `FlMotionProg`) on the host; push nil |
| 51 | `IMPORT` | u16 k | +1 | Sprint 31; hard-errors here (§9) |
| 52 | `EDIT_BEGIN` | — | 0 | §10 transaction open; null host = no-op |
| 53 | `EDIT_END` | — | 0 | |
| 54 | `THROW` | — | −1 | raise TOS |
| 55 | `TRY_PUSH` | u16 h | 0 | push a handler record (pc `h`, current sp, frame depth) |
| 56 | `TRY_POP` | — | 0 | |
| 57 | `HALT` | — | 0 | end of a top-level chunk |
| 58 | `LIST_APPEND` | u8 s | −1 | append TOS to the list in slot `s` — list-literal build with > 65535 items and s31's comprehension-free helpers |
| 59 | `NOT_NIL` | — | 0 | `x or y` fast path for nil-coalescing in generated code |
| 60 | `TRACE_LINE` | u16 k | 0 | debug builds only (`FL_VM_CHECKS`); never emitted in release |

`_Static_assert(FL_OP__COUNT == 60)` and `_Static_assert(FL_OP__COUNT <=
0xF0)` — `0xF0–0xFF` is reserved for future superinstructions. **No
superinstructions and no tail-call opcode in 1.0**: fused `GET_LOCAL`+`ADD`
pairs would buy a few percent and cost a second definition of every stack
effect, and tail calls would erase the stack frames s32's traces are made
of. Both are recorded here as deliberate omissions, not oversights.

Encoding: operands are fixed-width, little-endian, immediately following
the opcode. Limits, each a compile-time diagnostic rather than a runtime
surprise: 256 locals per function, 256 upvalues per closure, 65 535
constants per function (`too many constants in one function`), 65 535 bytes
of jump distance (`function too large`), 255 call arguments.

`void fl_disasm_chunk(Bytebuf *out, const FlChunk *ch, const Interner *in);`
— deterministic, one instruction per line, `0042  12:5  GET_LOCAL   3 ; x`,
no pointers. This is the compiler's golden-test surface, exactly as
`fl_ast_dump` was s29's.

### 6. Single-pass compiler — `src/fl/compile.c|h`

```c
typedef struct { u32 name; i32 depth; bool captured; } FlLocal; /* depth -1 = uninitialized */
typedef struct { u8 index; bool is_local; } FlUpvalDesc;

FlFn *fl_compile(FlVm *vm, DiagCtx *dc, const FlProgram *p,
                 u32 file_id, FlOrigin origin);   /* NULL on error */
```

- **Constant pool with dedup**, keyed on the *bit pattern plus the tag*:
  `5` and `5.0` are distinct entries, `0.0` and `-0.0` are distinct
  entries, strings dedup through the intern table. Pitfall: dedup keyed on
  `fl_equal` merges int and float constants (§4 says `int == float` compares
  numerically) and silently changes what `fmt.repr` prints — a bug that
  surfaces three sprints later in a workspace-state file.
- **Local slots**: an array capped at 256, `scope_depth` incremented per
  block. `let x` pushes the local at depth `-1` and only marks it
  initialized after the initializer compiles, so `let x = x` is
  `cannot read 'x' in its own initializer` rather than a silent capture of
  an outer `x`. Shadowing in a *nested* scope is legal (§4); redeclaring in
  the *same* scope is an error.
- **Upvalue capture is by reference** (spec §7, normative via the §14
  `counter` example). `resolve_upvalue` walks enclosing compilers, marks the
  source local `captured`, and dedups: two closures made in the same scope
  share one `FlUpval`, which is what makes the counter example work.
- **The loop variable is a fresh binding per iteration.** `for x in xs {
  fns.push(fn() x) }` yields closures over *distinct* `x`s. The compiler
  emits `CLOSE_UPVALS` at the end of each iteration body, not once at loop
  exit. This is a compiler decision the spec does not make, so it is pinned
  here and tested; the alternative is JavaScript's `var` bug, which people
  file as "closures are broken".
- **Jump patching**: `fl_emit_jump` writes a `0xFFFF` placeholder and
  returns the site offset; `fl_patch_jump` computes the distance and errors
  if it exceeds 65 535. `break`/`continue` record pending sites on a
  per-loop vector.

```c
/* Pitfall: a break inside a try must unwind the handler stack, and a break
 * out of a scope holding captured locals must close them — otherwise the
 * closure keeps pointing at a stack slot the next call reuses. Emit both,
 * in this order, at EVERY break/continue site. */
static void emit_loop_exit(Compiler *c, Loop *lp) {
    for (u32 i = c->ntry; i > lp->try_depth; i--) emit(c, FL_OP_TRY_POP);
    emit_close_upvals(c, lp->slot_floor);      /* also resets sp */
}
```

- **`max_stack`** is accumulated from the opcode table's effects as the
  compiler emits (a running depth plus a high-water mark). The VM then
  bounds-checks *once per call* instead of once per push — the only reason
  the hot loop can skip stack checks and still be memory-safe.
- `macro name = @[…]` compiles as `let name = fn() @[…]` (§4). No separate
  node handling, no separate opcode.
- Compile errors go through `DiagCtx` with s29's caret rendering and are
  **never catchable** (§9: compile errors are diagnostics, runtime errors
  are values). `fl_compile` returns NULL after emitting; the caller reports
  and exits 1.

### 7. The VM — `src/fl/vm.c|h`

```c
#define FL_STACK_MAX  16384          /* 256 KiB of FlValue                 */
#define FL_FRAMES_MAX 256

typedef struct { FlClosure *cl; const u8 *ip; FlValue *slots; } FlFrame;
typedef struct { u32 pc; FlValue *sp; u32 frame; } FlHandler;

struct FlVm {
    FlValue    stack[FL_STACK_MAX]; FlValue *sp;
    FlFrame    frames[FL_FRAMES_MAX]; u32 nframes;
    FlHandler  handlers[64]; u32 nhandlers;
    FlUpval   *open_upvals;              /* descending slot order          */
    FlMap     *globals;
    FlMap     *modules;                  /* Sprint 31 fills; rooted now    */
    FlHandleTab handles;                 /* Sprint 34 fills; rooted now    */
    FlValue    temp[32]; u32 ntemp;      /* §8 protection stack            */
    FlGc       gc;
    const FlHost *host;
    Arena     *arena; Interner *in; DiagCtx *dc;
    u64        steps, step_limit;        /* 0 = unlimited; s32 uses it     */
    FlValue    err;                      /* the in-flight raised value     */
};

bool fl_vm_init(FlVm *vm, Arena *a, Interner *in, DiagCtx *dc);
bool fl_vm_run (FlVm *vm, FlFn *entry, FlValue *out);   /* false = raised  */
void fl_vm_free(FlVm *vm);
```

**Dispatch: the `switch` is the reference implementation.** `&&label` and
the label-address array are GNU extensions, outside the C11 subset in
`00-decisions.md`, so the portable build must be complete and correct on
its own. The computed-goto twin is an optimization behind a probe:

```make
FL_CGOTO := $(shell $(CC) -x c -o /dev/null $(NULL_DEV_SRC) \
              -std=gnu11 -Werror scripts/probe-cgoto.c 2>/dev/null && echo 1)
ifeq ($(FL_CGOTO),1)
build/fl/vm.o: CFLAGS := $(filter-out -std=c11 -pedantic,$(CFLAGS)) \
                         -std=gnu11 -DFL_COMPUTED_GOTO=1
endif
```

Exactly one translation unit relaxes the dialect, and `make FL_CGOTO=0`
forces the reference path. Both bodies come from one `opcodes.def` include:

```c
#if FL_COMPUTED_GOTO
#  define FL_OP(N, o, e, d) &&L_##N,
    static void *const table[256] = {          /* PADDED — see pitfall */
#     include "opcodes.def"
    };
#  define VM_CASE(N) L_##N:
#  define VM_NEXT()  goto *table[read_u8(&ip)]
#else
#  define VM_CASE(N) case FL_OP_##N:
#  define VM_NEXT()  break                     /* falls to the switch  */
#endif
```

Pitfall: the goto table is indexed by a byte read from the chunk, so the
switch's `default:` arm (`YEW_BUG("fl vm: opcode %u at pc %u", …)`) has no
equivalent — pad the table to 256 entries with every unused slot pointing
at a shared `L_BAD_OP` label calling the same `YEW_BUG`. Never leave it
short. **CI runs both**: a `fletch-dispatch` lane builds each setting and
asserts byte-identical output over the s33 corpus; the differential test is
what makes a second dispatcher maintainable.

Raising: `fl_raise(vm, "type", "cannot add %s and %s", …)` builds the error
map (`kind`, `msg`; `trace` added by s32 when it escapes every frame), then
unwinds to the innermost `FlHandler`. Restore order is pinned: close
upvalues → truncate frames → restore `sp` → push the error → jump. An
`edit` block unwound past rolls back through the host (§10) and re-raises;
the null host makes it a no-op. Call depth past `FL_FRAMES_MAX`, stack
depth past `FL_STACK_MAX`, and `steps > step_limit` all raise a catchable
kind **`"limit"`**.

### 8. Spec amendment §16-A1 — the `"limit"` error kind

Spec §9 froze a closed set of 11 runtime kinds, none of which describes
resource exhaustion, and infinite recursion is user-triggerable, so it may
not become `yew_bug`/exit 4 and it may not be misfiled under `"index"`.
s28 §11 provides the amendment mechanism precisely for this. This sprint
files exactly one amendment and no others:

```
| 2026-… | §9 | 11 kinds → 12: add "limit" (VM resource exhaustion:
          call depth, value-stack depth, step limit, io result caps) |
          stack overflow must be catchable and is not any of the 11 |
          <reviewer> |
```

§9's stated count changes 11 → 12; §16 gains the row; s33's conformance
suite asserts the amended count. No section is renumbered. If review
rejects the amendment, the fallback is `yew_bug` — and the fallback is
wrong, so make the argument rather than the workaround.

### 9. GC — `src/fl/gc.c|h`

```c
typedef struct FlGc {
    FlObj  *objects;                 /* all-objects list                   */
    FlObj **gray; u32 ngray, graycap;
    FlStrTab strings;                /* weak: interned FlStr*              */
    size_t  bytes, next_gc;          /* next_gc floor = 256 KiB            */
    bool    stress, pending;
} FlGc;

void *fl_gc_alloc(FlVm *vm, size_t n, FlType t);   /* may set gc.pending   */
void  fl_gc_collect(FlVm *vm);                     /* opcode boundaries    */
void  fl_gc_protect(FlVm *vm, FlValue v);          /* push a temp root     */
void  fl_gc_release(FlVm *vm, u32 n);
```

**The root set, enumerated. Adding a root is a review event.**

| # | Root | Marked by |
|---|---|---|
| 1 | value stack `[vm->stack, vm->sp)` | `mark_value` each slot |
| 2 | call frames `frames[0..nframes)` — each `cl` | closure → fn → chunk consts |
| 3 | open upvalue list | each `FlUpval` (open ones alias the stack, closed ones own a value) |
| 4 | `vm->globals` | map |
| 5 | `vm->modules` (Sprint 31) | map — **rooted now, empty now** |
| 6 | `vm->handles` (Sprint 34) | handle table — **rooted now, empty now** |
| 7 | `vm->temp[0..ntemp)` | the protection stack (§below) |
| 8 | the compiler's in-progress function chain (`vm->compiling`) | compilation allocates `FlFn`/`FlStr` and can therefore trigger a collection |

Roots 5, 6 and 8 exist **before their consumers do**, deliberately: s34
adding a root later is the failure mode where a subsystem lands, works in
tests that never collect, and drops objects in production.

**Collection happens only at opcode boundaries.** `fl_gc_alloc` sets
`gc.pending` when `bytes > next_gc`; the dispatch loop tests it at the top
of each instruction. After a collection `next_gc = bytes * 2`, floored at
256 KiB. Pin (s33 gates it): under the default config the first collection
must not fire during config load — the < 1 ms budget has no room for a mark
phase.

**The handle-protection discipline, pinned now rather than in Sprint 34,
because a value reachable only from a C local is invisible to the
collector.** `gc.h` states these four rules with the why:

1. Native arguments arrive as `FlValue *argv` pointing **into the VM
   stack**, live for the whole call. Natives never copy an `FlObj *` into a
   C local across an allocation.
2. Intermediates are protected: `fl_gc_protect(vm, v)` …
   `fl_gc_release(vm, 1)`, strictly LIFO, capped at 32 (overflow is
   `YEW_BUG` — a leak in the discipline, not a user program).
3. Host callbacks (s34) receive handles by table **slot**, never by
   pointer: slot lookup fails cleanly on a dead object, a stale pointer
   cannot.
4. Under `FL_GC_STRESS` the collector runs at **every** allocation and
   poisons swept memory with `0xDD`. A rule-2 violation then fails on the
   first test that exercises it, under ASan, naming the line; without
   stress mode the same bug surfaces once a year, in a user's config, as a
   wrong answer.

Sweep order is pinned: clear the string table's weak references *before*
freeing white objects, or the table points into poisoned memory for the
rest of the sweep — the most commonly botched line in a mark-sweep
collector. No finalizers, no weak-value maps, no incremental or
generational phases for 1.0; handle weakness against buffer death is the
handle table's job (s34), not the collector's.

### 10. String interning and heap escape — `src/fl/value.c`

```c
#define FL_INTERN_MAX 64             /* bytes; strings <= this are interned */
FlStr *fl_str_new (FlVm *vm, const char *b, u32 n);   /* interns if short   */
FlStr *fl_str_take(FlVm *vm, Bytebuf *bb);            /* builder → FlStr    */
bool   fl_str_eq  (const FlStr *a, const FlStr *b);
```

| Source | Representation |
|---|---|
| identifiers, map-literal keys, `.field` names | s00 `Interner` ids (`u32`), never `FlStr` — the AST and chunks carry ids |
| string literals, and every runtime string ≤ 64 B | interned `FlStr`, one object per distinct content, `==` is a pointer compare on the fast path |
| runtime strings > 64 B (`str.join`, `fmt.f`, `io.read`) | plain heap `FlStr`, hash cached in `h.aux` |

`fl_str_eq` **always** compares `(len, hash, bytes)` with the pointer check
as a fast path only. Pitfall: "interned strings compare by pointer" holds
only if *every* string is interned; with a size threshold a pointer-only
comparison silently reports `false` for two equal 100-byte strings, and the
bug reads as "my map lookup misses sometimes". The threshold exists because
interning a 4 MiB `io.read` result buys a permanent table entry for a
string nobody will look up again. Interned strings are **weak** in the GC
(§9); bytes are stored verbatim including U+DC80–DCFF escapes — no
validation, no normalization, no cleaning, anywhere in `value.c`.

### 11. The host seam — `src/fl/vm.h`

```c
typedef struct FlHost {
    void *ud;
    bool (*motion)    (void *ud, const FlMotionProg *p, FlErr *err);
    bool (*edit_begin)(void *ud, FlErr *err);
    bool (*edit_end)  (void *ud, bool ok, FlErr *err);   /* ok=false: roll back */
} FlHost;
extern const FlHost fl_host_null;
```

`fl_host_null.motion` raises kind `"motion"` (spec §3: a headless VM without
an editor host raises it), which is exactly what makes the spec's §14
`shout` example return `"MOTION"`. `edit_*` are no-ops. Sprint 34 supplies
the real host; nothing in `vm.c` knows what a buffer is.

`FlMotionProg` is a flat, preassembled array built by the compiler from the
`FL_A_MOTION_BLOCK` AST:

```c
typedef struct { u8 kind, ch, flags; u32 count, arg; } FlMotionOp; /* 12 B */
typedef struct { FlObj h; FlMotionOp *op; u32 n; } FlMotionProg;
```

One `FL_OP_MOTION` dispatch runs the whole block. Why: `02-fletch.md` req 7
targets ~1 µs per motion op, and a 20-word recorded macro must not pay 20
VM dispatches plus 20 constant loads. The recorder's round-trip law
(invariant 10) also wants the block to be one addressable object.

### 12. Defer

- **Standard library and real `import`** landed in Sprint 31. This sprint's
  landing guard raised kind `"import"` with `modules land in Sprint 31`;
  current `FL_OP_IMPORT` execution resolves through the module system.
- **REPL, `yew fl`, full stack traces, did-you-mean, the VM fuzzer** →
  Sprint 32. This sprint's uncaught error prints `kind: msg` plus the
  innermost frame only.
- **Editor handles and motion execution** landed in Sprint 34. The five tags
  remain in `FlType` and `fl_type_name`; their constructors now route through
  the generation-checked handle table.
- **Recorder** landed in Sprint 35; **`--batch`** landed in Sprint 37.
- **Bytecode serialization (`.flc`)** → not in 1.0, and `vm.h` says so with
  the reason: no foreign bytecode may enter the VM without a verifier
  (s32 §8 makes this a written rule).

## Testing Strategy

- **Values** (`tests/unit/test_fl_value.c`): the §4 truth table (only
  `nil`/`false` falsy, `0` and `""` truthy); equality across
  int/float/string and list identity; i64 wrapping; `/` truncation for all
  four sign combinations; `%` sign-of-dividend; ÷0 → `"div"`;
  `fl_type_name` for all 15 tags.
- **Map order** (`test_fl_map.c`): 1000 inserts, delete every third,
  reinsert half — exact order asserted twice in-process and once across
  processes, before and after a forced compaction; a float key → `"key"`.
- **Compiler goldens** (`test_fl_compile.c`): `fl_disasm_chunk` goldens for
  every §2 statement and expression form; constant dedup asserted by count
  (`5` and `5.0` are two entries, two `"hi"` are one); the 256-local,
  65 535-constant and jump-distance limits driven over the edge with their
  diagnostic text; the `let x = x` diagnostic.
- **Closures** (`test_fl_closure.c`): the §14 `counter` (9 then 10); two
  closures in one scope sharing one upvalue (mutation visible through
  both); N distinct per-iteration loop captures; upvalues closed by a
  `break` not aliased by the next call.
- **VM** (`test_fl_vm.c`): the §14 example → `total == 17` and `shout` →
  `"MOTION"` under `fl_host_null`; `try`/`catch` restoring `sp` exactly;
  handler nesting to 64; recursion to 256 frames raising a catchable
  `"limit"`.
- **GC** (`test_fl_gc.c`): each root in the §9 table dropped-but-one and
  the object proven to survive (stack only, frame closure only, open
  upvalue only, `temp[]` only, compile chain only); an intentionally
  unprotected native allocation failing under stress (an XFAIL fixture that
  proves the harness detects a discipline violation); a cyclic list
  collected once unrooted; the intern table shrinking back to its floor.
- **GC stress lane**: every unit test and every s33 fixture with
  `FL_GC_STRESS=1` — ~30× slower, so its own lane, not `make test`.
- **Differential dispatch**: both `FL_CGOTO` settings green, with a driver
  byte-comparing the executed-opcode trace of every golden through both.
- **Determinism**: every disassembly twice, byte-compared; the §14 example
  twice, stdout compared.
- **Perf smoke** (numbers only — s33 owns the gates): `fib(24)`, a
  10 000-key map build, 100 000 `FL_OP_MOTION` dispatches against the null
  host.

## Definition of Done

1. `src/fl/{value.c,value.h,vm.c,vm.h,opcodes.def,compile.c,compile.h,
   gc.c,gc.h}` build in the gcc and clang lanes with zero warnings under
   `-std=c11 -pedantic -Wall -Wextra -Werror -Wvla`, and again with
   `FL_CGOTO=1` (where `vm.c` alone uses `-std=gnu11`).
2. `_Static_assert(sizeof(FlValue) == 16)`, `sizeof(FlObj) == 16`,
   `FL_OP__COUNT == 60`, `FL_OP__COUNT <= 0xF0` all present and passing.
3. All 60 opcodes are implemented, disassembled, and reached by at least one
   fixture; a coverage comment table at the top of `test_fl_compile.c` maps
   opcode → test with zero gaps (s33 formalizes; be honest now).
4. Spec §14 example: compiles clean, `total == 17`, `counter` yields 9 then
   10, `shout` returns `"MOTION"` — asserted, not printed.
5. Both dispatch modes produce byte-identical opcode traces over every
   fixture; `make FL_CGOTO=0 test` and `make FL_CGOTO=1 test` both exit 0.
6. Map iteration order is asserted stable across processes and across a
   compaction; deleted-then-reinserted keys land at the end.
7. Every root in the §9 table has a drop-it test; the GC-stress lane is
   green over `make test` plus the s29 corpus; valgrind reports zero
   definite leaks and ASan reports zero use-after-poison.
8. Every `FlObj` allocation remains GC-owned through `fl_gc_alloc` and is
   covered by the enumerated-root and stress tests. Later host-side Fletch
   registries and configuration/recorder state may use the project's checked
   allocation wrappers for non-`FlObj` storage; the repository allocator gate
   continues to reject direct libc allocation.
9. Upvalue semantics: by-reference sharing and per-iteration loop bindings
   each proven by a test whose failure mode is the classic bug.
10. Reserved handle tags landed unconstructible in this sprint; Sprint 34
    subsequently delivered their generation-checked constructors and type
    conversions. Sprint 31 delivered import execution; `FL_OP_IMPORT` remains
    compiled, disassembled, and covered by the 60-opcode conformance ledger.
11. Amendment §16-A1 is filed in `.docs/fletch-spec.md` with reviewer and
    date; §9's stated kind count reads 12.
12. Perf smoke numbers recorded in `tests/perf/baselines/fletch_smoke.txt`
    for both dispatch modes, so s33's gates start from measured ground.
