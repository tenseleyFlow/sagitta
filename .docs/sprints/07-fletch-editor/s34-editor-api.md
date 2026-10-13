# Sprint 34: The Editor API — Handles, Bindings, `edit{}`, Hooks

## Prerequisites

- Sprint 13 — `src/edit/cmd.c`: `CmdId`, `CmdDesc` (`name`, `fn`, `arity`,
  `flags`, `help`), `CmdCtx` (`ed`, `win`, `count`, `count_given`, `iarg`,
  `sarg`/`sarg_len`, `source`), `CmdSource` incl. `YEW_SRC_FLETCH`,
  `yew_cmd_register/lookup/desc/invoke/count/at`, the flag set
  (`REPEATABLE`, `TAKES_COUNT`, `RECORDABLE`, `NEEDS_WIN`,
  `CHANGES_BUFFER`, `PROMPTS`, `DEFERRED`), `scripts/check-cmd-dispatch.sh`,
  the one-dispatch-surface doctrine. **This sprint is that doctrine's
  third consumer and adds nothing beside it.**
- Sprint 10 — `EditCtx`, `yew_edit_insert/delete`, `UndoTree`,
  `yew_undo_begin(ut, YewTxnReason)` / `yew_undo_end` / `yew_undo_abort` /
  `yew_undo_boundary`, `YEW_TXN_MACRO` (reserved *for this sprint*),
  `yew_undo_current`. Note: s17's `yew_mc_run` sketch spells the same API
  `yew_undo_begin(undo) → UndoTxn*` / `yew_undo_commit(undo, t)`. s10 is
  authoritative; if the landed code uses s17's spelling, adapt the calls —
  the *semantics* (depth counter, outermost commits, abort inverse-applies)
  are what bind.
- Sprint 9 — `Cursor`, `CursorSet`, `MarkSet`, `MarkId {u32 id, gen}`,
  `yew_marks_adjust`; Sprint 7 — `TextBuf`, `Span {u64 lo, hi}`, `TextIter`.
- Sprint 12 — `Registers`, `RegVal`, `yew_reg_get/yank/delete`.
- Sprint 14 — `Ed`, `Buffer`, `Win`, `Workspace`, `yew_loop_run` (§2's
  numbered loop steps are the hook dispatch points), `yew_msg`.
- Sprints 22/23 — `Pane`, `Tab.tab_id`, `Tab.buffer_id` ("BufferList
  handle"). **Assumption beyond index entries**: buffers are addressable by
  a stable `u32 buffer_id` and windows by a stable `u32 win_id` (s10's
  `EditCtx.win_id` implies it). If s22/s23 landed pointers only, this
  sprint adds the id tables in `src/edit/ed.c`; §1 depends on them.
- Sprint 20 — `YewRe`, `yew_re_compile`, `YewReInput`, `YewReMatch`,
  `yew_re_search`. Sprint 21 — search state.
- Sprints 28–33 — `.docs/fletch-spec.md` frozen; lexer/parser/AST
  (`FL_A_MOTION`, `FL_A_EDIT`), `FlVm`, `FlValue`, `FlChunk`, `FlGc`,
  stdlib, `yew fl`, conformance suite. **Assumptions** (s30–s33 index
  entries name subsystems, not APIs): `fl_gc_root_add(FlGc*, FlValue*)` /
  `fl_gc_root_provider(FlGc*, void (*mark)(FlGc*, void*), void*)` exist for
  explicit roots; `fl_error_raise(FlVm*, const char *kind, const char *fmt,
  …)` raises a spec §9 error map; `fl_native_register(FlVm*, const char
  *dotted_name, FlNativeFn, u8 min, u8 max)` installs host builtins.
- Binding: `02-fletch.md` requirements 4, 5, 8; `00-decisions.md`
  invariants 1, 3, 4, 8, 10; `01-architecture.md` §Modal engine.

## Goals

Land `src/fl/{handle,origin,flapi,flhook,motion_tab}.c|h`: the five editor
handle types as **non-pointer** `FlValue`s over a generation-checked handle
table (a handle to a closed buffer errors cleanly and can never dangle),
the origin/capability model spec §13 froze, and the binding layer where
**every** mutating binding routes through `yew_cmd_invoke` with a capability
check — no privileged side doors. Also: motion-block execution (spec §3's
dispatch half), `edit{}` mapped onto s10 transactions with flatten-nesting
and rollback-on-escape, and `on(event, fn)` with the frozen event
inventory, contained hook errors, and a pinned reentrancy guard. The macro
recorder is Sprint 35; `bind()`, `set`, the option *table*, and config
loading are Sprint 36; `--batch` is Sprint 37; plugin origins are Sprint 54
(this sprint ships the origin machinery they plug into). No new keybindings
here: everything is reachable from `yew fl` and, from Sprint 37, `--batch`.

## Deliverables

### 1. Handles are ids, not pointers — `src/fl/handle.h`, `handle.c`

```c
typedef enum { FL_H_NONE = 0, FL_H_BUF, FL_H_CUR, FL_H_SPAN,
               FL_H_WIN, FL_H_RE, FL_H__N } FlHandleKind;

/* The FlValue payload for a handle: 64 bits, no pointer, ever. */
typedef struct { u32 slot; u32 gen; } FlHandle;   /* tag carries the kind */

typedef struct FlHandleSlot {
    u8  kind;            /* FL_H_NONE = free                              */
    u32 gen;             /* bumped on free; a stale gen never resolves    */
    u32 next_free;
    union {
        u32 buf;                                   /* FL_H_BUF: buffer_id */
        struct { u32 win; u32 index; }      cur;    /* FL_H_CUR           */
        struct { u32 buf; MarkId lo, hi; }  span;   /* FL_H_SPAN          */
        u32 win;                                   /* FL_H_WIN: win_id    */
        struct { Arena a; YewRe *re; }      re;     /* FL_H_RE: owned     */
    } as;
} FlHandleSlot;

typedef struct FlHandleTable {
    Vec(FlHandleSlot) slots;  u32 free_head;  u64 live;
} FlHandleTable;

FlValue fl_h_make(FlHandleTable *t, FlHandleKind k, const FlHandleSlot *init);
bool    fl_h_free(FlHandleTable *t, FlValue v);           /* explicit close */
/* THE resolver: NULL + a raised spec-§9 error on any failure.            */
Buffer *fl_h_buf  (FlVm *vm, FlValue v);
Win    *fl_h_win  (FlVm *vm, FlValue v);
Cursor *fl_h_cur  (FlVm *vm, FlValue v, Win **out_win);
bool    fl_h_span (FlVm *vm, FlValue v, Buffer **out_buf, Span *out);
const YewRe *fl_h_re(FlVm *vm, FlValue v);
```

**Two independent guards, because they catch different bugs.** The slot
`gen` guards *handle-slot reuse*: a script keeping an `FlValue` after
`fl_h_free` sees `gen` mismatch. The stored **id** guards *object death*:
`fl_h_buf` looks `as.buf` up in the `BufferList`, and a closed buffer is
simply absent, so a handle taken before `buf.close()` fails at resolution
with `kind: "handle"`, msg `buffer 7 is closed`. Neither guard subsumes the
other — drop the id indirection and a recycled `Buffer*` slot detonates;
drop the generation and a recycled *handle* slot points at a live but
wrong object.

**Pitfall — a raw pointer inside a script value is a time bomb.** The
whole design is one sentence: *no `FlValue` ever contains an editor
pointer.* The grep gate (DoD 4) is `grep -n 'FlValue' src/fl/handle.h`
showing no `Buffer *`, `Win *`, `Cursor *`, `TextBuf *` field, and
`_Static_assert(sizeof(FlHandle) == 8)`.

**Generation wraparound.** `gen` is `u32` and starts at 1. On free, `gen++`;
if it would wrap to 0 the slot is **retired** — marked `FL_H_NONE` and never
returned to the free list. Why: a wrapped generation makes a 4-billion-free
old handle valid again, which is worse than leaking one 32-byte slot.

**GC contract (s30).** Handles are scalars with no heap children, so the
collector needs no knowledge of them and cannot collect a live buffer. What
the collector *must* see is the mirror problem — `FlValue`s the **host**
retains: hook closures (§6), `bind()` closures (s36), option `on_change`
closures (s36), and any slot payload that ever becomes an `FlValue`. Two
mechanisms, both landed here:

```c
void fl_gc_host_root_add   (FlGc *g, FlValue *slot);   /* stable address  */
void fl_gc_host_root_remove(FlGc *g, FlValue *slot);
/* provider form for vectors that move: handle table + hook table */
void fl_gc_root_provider(FlGc *g, void (*mark)(FlGc *, void *), void *ctx);
```

`fl_handle_gc_mark` and `fl_hook_gc_mark` are registered at boot. A host
`FlValue` reachable only from C and not rooted is collected mid-hook — the
classic embedding crash; the ASan lane runs a forced-GC-every-allocation
mode (`YEW_FL_GC_STRESS=1`) over the whole script suite to find it.

`FL_H_RE` is the only **owning** kind: it holds its own `Arena` with the
compiled program (s20 `YewRe` is arena-owned and immutable). `fl_h_free`
destroys the arena. Regex handles are the one place a leak is possible, so
the script tests compile 10 000 regexes in a loop and assert bounded RSS.

**Span handles are mark-backed.** `as.span` stores two s09 `MarkId`s, not
offsets, so a span survives edits made through it: `s.prepend("(")` then
`s.append(")")` still brackets the same text. Bias per s09: `lo` biases
left, `hi` biases right (an insert at either end grows the span). Marks are
released by `fl_h_free` and by buffer close; a span whose marks were
collapsed by a delete resolves to a zero-length span at the deletion point
— defined, not an error.

### 2. Origins and capabilities — `src/fl/origin.h`, `origin.c`

**Sprint 31 already declared `FlOriginKind`/`FlOrigin` (s31 §12) for the
module cache; this sprint does not redeclare them.** It moves the existing
declaration into `src/fl/origin.h` verbatim and builds the registry on top:

```c
/* From s31 — unchanged, relocated to origin.h. FL_/Fl prefixes per the
 * symbol ledger; do NOT introduce a parallel YEW_ORIGIN_* spelling.      */
typedef enum { FL_ORIGIN_BUILTIN, FL_ORIGIN_CONFIG, FL_ORIGIN_WORKSPACE,
               FL_ORIGIN_PLUGIN,  FL_ORIGIN_CLI,    FL_ORIGIN_REPL } FlOriginKind;
typedef struct { u8 kind; u32 path_id; u32 caps; } FlOrigin;

/* New here: the registry that gives each loaded origin a stable id and a
 * human label for diagnostics and s54's capability prompts.              */
u32  fl_origin_register(Ed *ed, FlOriginKind k, const char *label, u32 caps);
const char *fl_origin_label(const Ed *ed, u32 origin_id);
u32  fl_origin_of_frame(FlVm *vm);    /* defining module of the CALLER    */
bool fl_cap_check(FlVm *vm, u32 need);/* false + kind "capability" raised */
```

- Pitfall — **enum kind and registry id are different numbers.** The kind
  enum starts at `FL_ORIGIN_BUILTIN = 0` (s31's `fl_cap_origin` walks
  frames skipping builtins, so builtin needs no particular value). The
  *registry id* 0 is reserved for the user config, which is the number s54
  depends on. Conflating the two is how a plugin would be granted the user
  config's capabilities; `origin.h` states both facts in its header comment
  and a unit test asserts `fl_origin_register` never returns 0 for a
  non-config origin.

Spec §13, pinned verbatim in `origin.h`'s header comment: **the check reads
the origin of the calling function's defining module, not the current stack
top.** `fl_origin_of_frame` walks to the frame whose function's chunk
carries an origin (host natives are transparent), so a plugin calling a
user-config helper gains nothing. Origin 0 is the user config (s54 depends
on that number). Grants at 1.0: user/builtin/cli/repl = all; workspace =
all after s36's trust prompt; plugin = prompted per capability (s54).

### 3. The binding layer — `src/fl/flapi.h`, `flapi.c`

Every binding is a row, not a function:

```c
typedef enum { FL_ARG_NONE, FL_ARG_INT, FL_ARG_STR, FL_ARG_COUNT,
               FL_ARG_HANDLE_WIN, FL_ARG_HANDLE_BUF } FlArgSlot;

typedef struct FlBindDesc {
    const char *fl_name;      /* "buf.insert", "win.split"                 */
    const char *cmd;          /* "ed.edit.insert.text"; NULL = pure query   */
    u8   recv;                /* FlHandleKind of arg 0; FL_H_NONE = free fn */
    u8   nmin, nmax;
    u8   argmap[3];           /* FlArgSlot per script arg after the receiver */
    u32  caps;                /* required YewCap bitmask; 0 = none          */
    FlNativeFn query;         /* non-NULL iff cmd == NULL                   */
} FlBindDesc;

extern const FlBindDesc fl_api[];  extern const u32 fl_api_len;
```

**The law (review-rejection rule, teeth in DoD 5).** A binding row with
`cmd != NULL` is dispatched by one shared thunk:

```c
/* The ONE place a Fletch call becomes an editor effect.  Marshal, check
 * the capability, then hand off.  Nothing here touches TextBuf, EditCtx,
 * CursorSet, or Registers directly — because a direct mutation would (a)
 * skip yew_undo_record, breaking undo atomicity, and (b) skip the s13
 * recorder tap, making invariant 10 quietly false.  Two invariants, one
 * shortcut, zero symptoms until a user loses work.                      */
static FlValue fl_dispatch(FlVm *vm, const FlBindDesc *d, FlValue *a, u8 n) {
    if (d->caps && !fl_cap_check(vm, d->caps)) return FL_NIL;   /* raised  */
    CmdCtx cx = { .ed = fl_ed(vm), .source = YEW_SRC_FLETCH, .count = 1 };
    if (!fl_marshal(vm, d, a, n, &cx)) return FL_NIL;           /* raised  */
    CmdStatus st = yew_cmd_invoke(d->cmd_id, &cx);              /* choke   */
    return fl_status_to_value(vm, d, st, &cx);
}
```

`d->cmd_id` is resolved **once at boot** by `yew_cmd_lookup`; an unknown
name is `YEW_BUG` at startup, so a typo in the table cannot ship. Rows with
`cmd == NULL` are **read-only queries** (`buf.len`, `cur.line`,
`win.cursors`) that may read the model directly; they are forbidden to
write. The distinction is mechanical and gated:

```sh
# DoD 5 — the mutation choke point, enforced
grep -rn 'yew_edit_insert\|yew_edit_delete\|yew_textbuf_\|yew_undo_record\|\
yew_reg_set\|yew_reg_yank\|yew_reg_delete\|yew_cset_' src/fl/ | grep -v '^src/fl/flapi.c:.*/\*'
# must print nothing.  The only writer in src/fl/ is yew_cmd_invoke.
```

When a Fletch operation has no registry command, the fix is to **register
the command**, never to bypass. Commands this sprint adds to `cmd.c` for
exactly that reason:

| New command | Arity | Flags | Serves |
|---|---|---|---|
| `ed.edit.insert.at` | STR | CHANGES_BUFFER, RECORDABLE | `span.prepend/append`, `buf.insert` |
| `ed.edit.delete.span` | NONE | CHANGES_BUFFER, RECORDABLE | `span.delete` |
| `ed.edit.replace.span` | STR | CHANGES_BUFFER, RECORDABLE | `span.replace` |
| `ed.buf.open` / `ed.buf.close` | STR / NONE | — | `buf.open`, `buf.close` |
| `ed.cursor.set` | NONE (`iarg` = offset) | NEEDS_WIN | `cur.goto` |
| `ed.opt.get` / `ed.opt.set` | STR / STR | — | §4's `opt.*` (table: Sprint 36) |
| `ed.fl.eval` | STR | — | `ed.eval` from E mode (s18's `:fl` slot) |

`CmdDesc` gains one field this sprint, used by §8 and by Sprint 35:

```c
const char *word;   /* motion-space CMDWORD, e.g. "yank"; NULL = not one */
```

Registration validates: `word` is `[a-z][a-z0-9_]{0,15}`, globally unique
(`YEW_BUG` on collision), and **required** whenever `YEW_CMD_RECORDABLE` is
set. That bijection is what Sprint 35's round-trip law stands on; enforcing
it at registration means the law cannot be broken by adding a command.

### 4. API surface

Receiver-style calls are sugar over `.` on a handle (spec §4). Errors are
spec §9 kinds; every row that mutates names the `ed.*` command it invokes.

**`buf.*`** — receiver `FL_H_BUF`; free functions on the `buf` module.

| Fletch | Args | Command | Result |
|---|---|---|---|
| `buf.current()` | — | query | buffer handle of the focused window |
| `buf.list()` | — | query | list of buffer handles, workspace order |
| `buf.open(path)` | str | `ed.buf.open` | buffer handle; `fs.read` cap |
| `b.close(force)` | bool? | `ed.buf.close` | nil; dirty + !force → kind `"io"` |
| `b.save(path?)` | str? | `ed.file.save` | nil; `fs.write` cap |
| `b.path()` `b.name()` `b.lang()` | — | query | str / nil |
| `b.len()` `b.lines()` | — | query | int (bytes / line count) |
| `b.text(span?)` | span? | query | str (whole buffer or span) |
| `b.line(n)` | int | query | str, no EOL bytes; 1-based |
| `b.span(lo, hi)` | int,int | query | span handle, byte offsets |
| `b.line_span(n)` | int | query | span handle |
| `b.insert(off, s)` | int,str | `ed.edit.insert.at` | nil |
| `b.delete(span)` | span | `ed.edit.delete.span` | nil |
| `b.find(re, from?)` | regex,int? | query | span handle or nil |
| `b.find_all(re)` | regex | query | list of span handles |
| `b.dirty()` `b.readonly()` | — | query | bool |
| `b.mark(off)` | int | query | mark-backed zero-width span |

**`win.*`** — receiver `FL_H_WIN`.

| Fletch | Command | Result |
|---|---|---|
| `win.current()` / `win.list()` | query | window handle / list |
| `w.buf()` | query | buffer handle |
| `w.cursors()` | query | **list of cursor handles**, ascending (s17 order) |
| `w.selection()` | query | span handle of the primary selection, or nil |
| `w.split(dir)` | `ed.pane.split_h/v` | new window handle |
| `w.close()` | `ed.pane.close` | nil |
| `w.focus()` | `ed.win.focus` | nil |
| `w.viewport()` | query | map `{top, rows, cols}` |
| `w.set_cursors(list)` | `ed.cursor.set_many` | nil (one transaction) |

**Cursor-list ops** — receiver `FL_H_CUR`. Multi-cursor *is* a list you map
over (`02-fletch.md` requirement 5); the list is a snapshot, and stale
entries resolve to `nil` after a merge (s17 merges cursors), which is
defined behavior scripts must handle:

| Fletch | Command | Result |
|---|---|---|
| `c.pos()` `c.line()` `c.col()` | query | int (byte off / 1-based line / GCol) |
| `c.anchor()` `c.span()` | query | int / span handle or nil |
| `c.goto(off)` | `ed.cursor.set` | nil |
| `c.move(unit, dir, n?)` | `ed.move.unit.*` | nil |
| `c.word()` `c.line_span()` | query | span handle (s16 unit engines) |
| `c.insert(s)` | `ed.edit.insert.text` | nil |
| `c.is_primary()` `c.valid()` | query | bool |

**Span ops** — receiver `FL_H_SPAN`.

| Fletch | Command | Result |
|---|---|---|
| `s.lo()` `s.hi()` `s.len()` | query | int |
| `s.text()` | query | str |
| `s.lines()` | query | list of str, no EOL bytes |
| `s.replace(str)` | `ed.edit.replace.span` | nil |
| `s.replace_lines(list)` | `ed.edit.replace.span` | nil; EOLs from `FileMeta` |
| `s.prepend(str)` / `s.append(str)` | `ed.edit.insert.at` | nil |
| `s.delete()` | `ed.edit.delete.span` | nil |
| `s.yank(reg?)` | `ed.edit.yank` | nil |
| `s.valid()` | query | bool (false once marks are gone) |

**Generic invocation and marshalling.** Every registry command is callable
without a wrapper row — that is how the API stays complete as commands are
added, and how E-mode/`:`-only commands stay scriptable:

```fletch
ed.run("ed.tab.goto", { count: 3 })
ed.run("ed.file.save", { sarg: "out.txt", win: w })
let names = ed.commands()          # list of maps {name, help, flags, word}
```

| Map key | `CmdCtx` field | Rules |
|---|---|---|
| `count` | `count`, `count_given = true` | int 1…`YEW_COUNT_MAX`, else kind `"type"` |
| `iarg` | `iarg` | int |
| `sarg` | `sarg`, `sarg_len` | str; **not NUL-terminated**, length carried |
| `win` | `win` | window handle; default = focused |
| any other key | — | kind `"key"`, naming the key |

`ed.run` refuses `YEW_CMD_DEFERRED` (kind `"name"`, message quoting the
sprint) and — from Sprint 37 — `YEW_CMD_INTERACTIVE` under `--batch`.
`CmdStatus` maps to errors: `ERR_ARG` → `"type"`, `ERR_STATE` → `"user"`,
`ERR_IO` → `"io"`, `ERR_DEFERRED` → `"name"`; `YEW_CMD_OK` → `nil`.

**Options.** `opt.get(name)` / `opt.set(name, value)` route through
`ed.opt.get` / `ed.opt.set`. The *typed option table* (scopes, validators,
`on_change`) is Sprint 36; this sprint ships the value marshalling
(`bool↔bool`, `int↔int`, `str↔string`, `enum↔string`) and an `OptProvider`
seam so Sprint 36 replaces the provider, not `flapi.c`:

```c
typedef struct OptProvider {
    bool (*get)(Ed *, const char *n, u32 nlen, OptVal *out);
    bool (*set)(Ed *, const char *n, u32 nlen, const OptVal *v, const char **err);
    u32  (*list)(Ed *, const char **out, u32 max);   /* insertion order  */
} OptProvider;
void yew_opt_provider_set(Ed *ed, const OptProvider *p);
```

The provider shipped here covers the options earlier sprints already own as
C state (`tabwidth`, `wrap`, `chord_timeout_ms`, `clipboard.sync`,
`undo.bytes_max`, `undo.break_on_newline`, `registers.ring_depth`,
`errorbells`, `ambiguous_wide`, `scrolloff`) — real values, not a stub. An
unknown name raises kind `"name"` listing near matches.

### 5. `edit{}` — transactions over s10

`FL_A_EDIT` compiles (s30) to `FL_OP_TXN_ENTER` / `FL_OP_TXN_LEAVE` around
the block body; the VM calls into `flapi.c`:

```c
typedef struct FlTxn {
    u32 depth;                  /* flatten counter, NOT a stack of txns   */
    Vec(UndoTree *) enlisted;   /* buffers touched, in enlistment order   */
    u32 gc_root;                /* the frame's rooted values              */
} FlTxn;

void fl_txn_enter (FlVm *vm);
void fl_txn_leave (FlVm *vm, bool unwinding);
void fl_txn_enlist(FlVm *vm, Buffer *b);    /* called by every mutating cmd */
```

Pinned semantics (spec §10 is normative; this is its implementation):

| Situation | Behavior |
|---|---|
| outermost `edit{}` enters | `depth` 0→1; each buffer opens `yew_undo_begin(ut, YEW_TXN_MACRO)` on first mutation |
| nested `edit{}` | **flattens** — `depth++` only; no second transaction, no boundary |
| block completes normally at `depth == 1` | `yew_undo_end` on each enlisted tree, in enlistment order |
| error escapes the block at `depth > 1` | nothing; the error keeps unwinding |
| error escapes at `depth == 1` | `yew_undo_abort` on each enlisted tree in **reverse** enlistment order, then **re-raise** unchanged |
| `try{ }` **inside** the block catches | no rollback — the error never escaped |
| `try{ edit{ } } catch e{ }` | rollback happens first, then the catch sees the error and a pre-block buffer |
| mutation outside any `edit{}` | an **implicit** transaction spanning the outermost Fletch entry (key-bound call, hook, `ed.run`, batch script) — there is always exactly one transaction |
| headless VM with no `Ed` | `edit` is a plain block (spec §10) |

**Why flatten rather than nest.** s10's `UndoTree` already keeps a depth
counter where only the outermost `end` commits, and undo is a *user-facing*
history: nested undo steps a user cannot reach by pressing `u` are not
history, they are bookkeeping. Flattening also makes "one macro run = one
undo step" true regardless of how a macro is factored into helper
functions — refactoring a Fletch function must never change how many times
the user presses `u`.

**Pitfall — multi-buffer transactions.** A single `edit{}` may touch two
buffers (`for b in buf.list() { … }`). Each gets its own transaction node
because `UndoTree` is per-buffer; they commit and roll back **together**.
Enlistment happens at the first mutation, not at `enter`, so a block that
reads ten buffers and writes one opens one transaction. Rolling back in
reverse enlistment order matters only for marks shared across the set, but
it costs nothing and makes the order a decision instead of an accident.

### 6. `on(event, fn)` — the event inventory

```c
typedef struct FlHook { u32 event; u32 origin; FlValue fn;
                        u32 errs; bool disabled; u32 ledger_id; } FlHook;
u32  fl_hook_add(FlVm *vm, u32 event, FlValue fn);   /* ledger id         */
bool fl_hook_remove(Ed *ed, u32 ledger_id);
void yew_hook_fire(Ed *ed, u32 event, const FlValue *args, u8 n);
```

Registrations append to the origin-tagged **registration ledger** in `Ed`
(`{u32 origin_id; RegKind kind; u32 handle;}`) that Sprint 36 uses for
config reload and Sprint 54 generalizes to plugins. `REG_HOOK` is defined
here; `REG_BIND`/`REG_OPTION`/`REG_CMD` are defined but only produced from
Sprint 36 on.

| Event | Payload | Dispatch point (s14 §2 loop step) |
|---|---|---|
| `buf.open` | buffer | `yew_file_load` success, before the first render — step 3 or startup |
| `buf.change` | buffer, span | **coalesced**: once per input burst, after step 3, with the union of the burst's damage |
| `buf.save` | buffer | inside `ed.file.save`, **before** the write; may edit — joins the save's transaction |
| `buf.saved` | buffer | after `yew_file_save` returns success and the journal is discarded |
| `buf.close` | buffer | before teardown; handles for that buffer are still resolvable |
| `mode.enter` / `mode.leave` | str mode name | inside `yew_mode_enter`, leave before enter |
| `win.focus` | window | on focus change (s22), after layout is dirty |
| `cursor.move` | window | **coalesced**: once per burst, after step 3, only if any cursor's `pos` changed |
| `ws.open` | workspace | after state restore, before the first paint |
| `ws.close` | workspace | on quit, before buffers are torn down; the last chance to save |
| `ed.idle` | — | step 4, once per idle period (≥ 500 ms with no input) |

Pinned naming: **there is no `ws.quit`** — quitting fires `ws.close`, so the
pair is symmetric and Sprint 54's frozen table needs no amendment.
Sprint 54's Deliverable-5 table omits `cursor.move`; it is part of this
inventory and its namespacing covers it by name.

**Why `buf.change` and `cursor.move` are coalesced.** Firing them per edit
puts arbitrary user code inside the keypress path, and a 40 KB paste is one
burst but thousands of ops — per-op dispatch turns invariant 4's 5 ms budget
into seconds. Coalescing is the same rule the renderer already obeys
(one render per burst); hooks get the same contract, stated in the header.

Dispatch order per event: origin 0 (user config) first, then remaining
origins in registration order — deterministic (invariant 5). A hook's error
never stops dispatch to later subscribers.

### 7. Hook error containment and the reentrancy guard

Every hook runs in a protected frame. On an uncaught error:

1. the enclosing transaction rolls back (§5) — a `buf.save` hook that throws
   leaves the pre-hook content and **the save proceeds** (invariant 1);
2. the stack trace goes to `yew_log(ERROR)`;
3. `yew_msg(WARN)`: `hook "buf.save" error (2/5): <first line>`;
4. `hook->errs++`; at `hooks.error_limit` (option, default **5**) the
   **individual registration** is set `disabled` — not removed, so a
   Sprint 36 reload still tears it down by ledger id — with
   `hook "buf.save" disabled after 5 errors`;
5. dispatch continues to the next subscriber. The editor never exits.

**Reentrancy — pinned.** `FlHookTable` carries `u16 in_flight[FL_EV__N]`.

- A fire of event `E` while `E` is already dispatching is **dropped**, with
  one `yew_log(WARN)` per event per session. Dropping, not queueing: a
  `buf.change` hook that edits the buffer would queue a change that queues a
  change; the queue terminates only by luck, and "your editor froze" is a
  worse outcome than "your hook ran once".
- Cross-event recursion is bounded by `YEW_HOOK_DEPTH_MAX = 8`; exceeding it
  is treated exactly like a throwing hook (contained, counted, disabled),
  so no new error kind and no new user-facing concept.
- Teardown masking: `yew_hook_fire` skips origins currently being torn down
  (`Ed.origin_mask`) — Sprint 54's plugin-disable path depends on it, and
  Sprint 36's reload needs it for the same reason.

### 8. Motion-block execution — spec §3's dispatch half

`FL_A_MOTION_BLOCK` evaluates to `nil` and executes word by word through
`src/fl/motion_tab.c`:

| AST motion | Resolution | Notes |
|---|---|---|
| `FL_M_UNIT` `l w b c` | `ed.mode.enter` with `sarg` = `"L" "W" "B" "I"`… | `c` sets the *char* unit without entering I mode |
| `FL_M_ARROW` `> < ^ v` | `ed.move.unit.{next,prev,up,down}` | `alt` flag → the `.alt` variants |
| `FL_M_H(...)` | `ed.mode.enter "H"`, inner words, `ed.mode.escape` | selection survives to the next word |
| `FL_M_INSERT` `i"…"` | `ed.edit.insert.text` with `sarg` = the string | one command per block, not per grapheme |
| `FL_M_DEL` | `ed.edit.delete.unit` | selection-or-unit, per s17 |
| `FL_M_ESC` | `ed.mode.escape` | |
| `COUNT n` | `cx.count = n; cx.count_given = true` | applies to the **next** word only |
| `FL_M_WORD` | `yew_cmd_by_word(w)` → `CmdId` | unknown word → kind `"name"`, "no command has word 'x'" |

`u32 yew_cmd_by_word(const char *w, u32 len)` is a `Strmap` built at
registration time. Executing a motion block without an editor host raises
kind `"motion"` (spec §3), which is what `yew fl` already does today.

**Pitfall — motion execution is not a second dispatcher.** Every row above
ends in `yew_cmd_invoke`; `motion_tab.c` contains no editing code and no
`Ed` field writes. That is what makes a recorded macro and a hand-typed
motion block indistinguishable to the editor — the precondition for
Sprint 35's law.

### 9. Spec amendment A2 (`fletch-spec` §16)

**Numbering:** amendments are numbered globally in sprint order and
registered in s28's amendment table. **A1** is Sprint 30's `"limit"` kind;
this is **A2**. (Both amend §16, and both were independently drafted as
"A1" before the ledger existed — that collision is resolved here.)

`.docs/fletch-spec.md` §16 gains one row. §9's kind list is **11 base + A1
`"limit"` + A2 `"handle"` = 13**; s33's conformance count assertion is
updated in the same commit:

| Date | § | Old → New | Reason | Approver |
|---|---|---|---|---|
| (sprint date) | 9 | 12 kinds → 13 (`"handle"` added, on top of A1's `"limit"`) | a stale/closed editor handle is neither a type error nor an index error; folding it into either makes `catch` unable to distinguish "you passed the wrong thing" from "the buffer closed under you" | (reviewer) |

No other spec change. In particular `set { … }` from `02-fletch.md`'s
pre-freeze sketch is **not** grammar: §2 has no `set_stmt`, so the config
spells it `set({ tabwidth: 4 })` (Sprint 36 pins this).

### 10. Defer

- `bind()` / `unbind()`, the typed option table, `set(map)`, config loading
  and reload, the trust prompt → **Sprint 36**. `opt.set` works now over the
  §4 provider; `bind` is registered `YEW_CMD_DEFERRED` naming 36.
- Macro recorder, `record.c`, CMDWORD *emission* → **Sprint 35** (the `word`
  field and `yew_cmd_by_word` land here; the reverse map is trivial there).
- `--batch`, the `YEW_CMD_INTERACTIVE` flag's enforcement, stdio contract →
  **Sprint 37**. The flag is *defined* here and set on `YEW_CMD_PROMPTS`
  commands; nothing enforces it until 37.
- Plugin origins, capability prompts, the event bus namespacing →
  **Sprint 54** (origins, ledger, and hook masking ship here).
- `fs.read`/`fs.write`/`shell`/`net` **enforcement points outside flapi**
  (the `io` module) → Sprint 31's surface; this sprint checks the caps it
  owns and nothing more.
- Syntax/theme handles, LSP handles, AI handles → Campaigns 08/09/10; the
  `FlHandleKind` enum is closed at five for 1.0 and adding one is a review
  event.

## Testing Strategy

- **Unit** (`tests/unit/test_fl_handle.c`): free-then-use for all five
  kinds; slot reuse with a stale generation; generation wraparound retiring
  a slot; `buf.close` making every handle to it resolve to kind `"handle"`;
  span marks surviving 1 000 random edits vs an offset oracle; span whose
  marks are collapsed reporting a zero-length span; 10 000 regex
  handle create/free cycles with bounded RSS.
- **Unit** (`test_fl_marshal.c`): every `ed.run` map key incl. rejection
  rows; `sarg` carrying embedded NULs byte-exactly; count clamp; every
  `CmdStatus` → error-kind mapping; `YEW_CMD_DEFERRED` refused.
- **Unit** (`test_fl_origin.c`): `fl_origin_of_frame` through a
  plugin→user-helper→native chain returns the *plugin* origin (the whole
  capability model in one assertion); denied access raises `"capability"`.
- **Script** (`tests/script/api_*.fl`, run via `yew fl` this sprint and via
  `--batch` from Sprint 37): every §4 table row exercised at least once;
  `edit{}` matrix — flatten, error-escape rollback, `try` inside vs outside,
  multi-buffer commit and rollback, implicit transaction; hook matrix — each
  of the 11 events fires with the right payload, a throwing hook is
  contained and the 5th disables it, a `buf.change` hook that edits does not
  recurse, `buf.save` hook edits land in the file.
- **GC stress**: the whole script suite re-run under `YEW_FL_GC_STRESS=1`
  (collect at every allocation) — proves every host-retained `FlValue` is
  rooted. This is the lane that catches the embedding's classic crash.
- **Fuzz** (`tests/fuzz/fuzz_flapi.c`): random `ed.run` calls with random
  maps against a scratch buffer under ASan — no crash, no `YEW_BUG`, every
  failure a spec §9 error; handles fuzzed with random `{slot, gen}` pairs
  (crafted `FlValue`s from the test hook) — every one resolves or errors,
  none dereferences.
- **Perf**: 10⁶ `cur.pos()` query calls < 300 ms; 10⁵ `b.insert` calls
  inside one `edit{}` produce exactly one undo node and stay within
  `undo.bytes_max` accounting. Motion-op dispatch ≤ 1 µs
  (`02-fletch.md` requirement 7) measured over a 10⁶-word motion block.
- **Determinism**: `ed.commands()` and `buf.list()` byte-identical across
  two runs; hook dispatch order pinned by a multi-origin fixture.

## Definition of Done

1. `make test` green on gcc and clang at `-std=c11 -pedantic -Wall -Wextra
   -Werror`; ASan/UBSan/valgrind/determinism lanes clean; the GC-stress
   lane green over the whole script suite.
2. `_Static_assert(sizeof(FlHandle) == 8)`; `grep -n 'Buffer \*\|Win \*\|
   Cursor \*\|TextBuf \*' src/fl/handle.h` finds no struct field — handles
   carry no pointers.
3. Dead-handle matrix green: for all five kinds, use-after-free, use-after
   slot-reuse, and use-after-object-close each raise kind `"handle"` and
   never dereference (ASan-clean under the crafted-handle fuzzer).
4. Generation wraparound retires the slot; a unit test forces `gen =
   0xFFFFFFFF` and asserts the slot never returns to the free list.
5. **The side-door gate**: the Deliverable-3 grep prints nothing;
   `scripts/check-cmd-dispatch.sh` still passes; a seeded direct
   `yew_edit_insert` call in `flapi.c` makes `make test` fail. Every
   `FlBindDesc` row with `cmd != NULL` resolves at boot (YEW_BUG on typo),
   and every row with `cmd == NULL` is proven read-only by the same grep.
6. Every `YEW_CMD_RECORDABLE` command has a unique, valid `word`;
   registering a recordable command without one is `YEW_BUG` (unit-tested),
   and `yew_cmd_by_word` round-trips against `yew_cmd_desc(id)->word` for
   every registered command.
7. `edit{}` matrix: all 9 rows of §5 asserted, incl. multi-buffer commit and
   reverse-order rollback; a nested `edit{}` produces exactly **one** undo
   node (`yew_undo_dump` node count asserted); refactoring a script into
   helper functions does not change the node count.
8. All 11 events fire at their documented dispatch points; `buf.change` and
   `cursor.move` fire **exactly once** per input burst under a 4 000-key
   paste fixture (asserted by a counting hook).
9. Containment: a hook throwing in each of the 11 events leaves the editor
   running, rolls back, and disables at exactly 5; a self-triggering
   `buf.change` hook runs once and logs one drop line.
10. Capability model: the plugin→user-helper chain test proves the check
    reads the defining module's origin; `fl_cap_check` denial raises
    `"capability"` and is catchable.
11. Spec amendment A2 present in `.docs/fletch-spec.md` §16; §9 lists 13
    kinds; s33's count assertion updated in the same commit.
12. Perf gates: motion-op dispatch ≤ 1 µs; 10⁶ query calls < 300 ms;
    baselines committed.
13. ≥ 200 assertions across the three unit files; ≥ 25 script tests.
14. The landing guards named their owning sprints rather than silently
    stubbing: Sprint 36 delivered `bind`/`unbind`, Sprint 35 delivered macro
    record/replay, and Sprint 37 delivered the `--batch` refusal policy.
