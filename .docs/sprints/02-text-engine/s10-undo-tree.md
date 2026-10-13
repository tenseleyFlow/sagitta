# Sprint 10: The Undo Tree

## Prerequisites

- Sprint 7 — `TextBuf`, `PieceNode` (COW refcounts, `node_own`), `Span`,
  `ByteOff`/`LineNo`, `TextIter`, `tb->gen`, `yew_textbuf_check`. This
  sprint adds one primitive to `piece.c` (§3 `yew_textbuf_insert_span`).
- Sprint 8 — `Journal`, `yew_journal_record/sync/discard`, the SAGJ record
  shape s08 pinned as *"the shape Sprint 10's undo tree deserialization
  reuses"*; `FileMeta`; the CRC-32 table in `journal.c` (reused, not
  re-implemented).
- Sprint 9 — `Cursor`, `CursorSet` (`yew_cset_adjust`), `MarkSet`,
  `MarkId {u32 id, gen}`, `yew_marks_adjust`, and s09's statement that
  adjustment happens at **a single edit choke point** — this sprint builds
  that choke point and makes undo its third consumer.
- Sprint 0/1 — `Arena`, `Vec(T)`, `Bytebuf`, `yew_log`, `YEW_BUG`, unit
  harness, CI lanes.
- Binding: `00-decisions.md` Undo row (*"Operation-log undo **tree** (not a
  stack) … transactions group multi-cursor/macro edits into one undo step;
  serialized into workspace state"*); `01-architecture.md` Text engine.
- Assumption beyond index entries: no `Buffer` aggregate exists yet (s07–s09
  shipped free-standing types). This sprint defines `EditCtx` as the thing
  that binds them; `01-architecture.md`'s `Buffer` embeds it in Sprint 14.

## Goals

Land `src/text/undo.c|h` and the single edit choke point `src/text/edit.c|h`:
an op log whose transactions form a **tree**, not a stack — editing after an
undo grows a new branch and the redo path survives, so no keystroke a user
made is ever destroyed. Pinned here: node/op layout, the grouping matrix
(one insert burst, one multi-cursor edit, one macro run, one shell filter =
exactly one node), undo/redo/branch navigation including jump-by-time and
jump-by-state, what a transaction stores about cursors and marks, the memory
budget and trimming policy, and the versioned `.sagu` format Sprint 25
references. Keybindings are Sprint 13/14; the `ed.undo.*` command surface is
Sprint 18; the **branch picker UI** is Sprint 26 (its query API and a
headless dump ship here); multi-cursor transactions are wired in Sprint 17,
macro in 34/35, shell filters in 19 — all four have their `YewTxnReason` and
their grouping row here, so those sprints wire rather than design.

## Deliverables

### 1. The edit choke point — `src/text/edit.h`, `src/text/edit.c`

```c
typedef struct EditCtx {
    TextBuf   *tb;
    MarkSet   *marks;
    CursorSet *cset;        /* the INITIATING window's cursors (s22 id)  */
    u32        win_id;      /* 0 until Sprint 22; stored in the txn      */
    Journal   *jrnl;        /* NULL for scratch buffers                  */
    UndoTree  *undo;
    FileMeta   *meta;       /* refreshed after save; NULL for scratch    */
} EditCtx;

void yew_edit_insert(EditCtx *ec, ByteOff at, const u8 *bytes, u64 len);
void yew_edit_delete(EditCtx *ec, Span range);
```

These are the **only** paths that may mutate a `TextBuf` owned by an
`EditCtx`. Each performs, in this exact order: (1) capture, for deletes, the
removed bytes via `TextIter` into the undo blob arena **before** the tree
changes; (2) mutate (`yew_textbuf_insert`/`_delete`); (3) `yew_marks_adjust`
in pre-op coordinates, once; (4) `yew_cset_adjust`; (5)
`yew_journal_record`; (6) `yew_undo_record`, which consults §4 to decide
whether the op opens a node or extends the open one.

**Pitfall — one op stream, three consumers.** s09 pinned that
`yew_marks_adjust` runs from *the* choke point. Any later sprint calling
`yew_textbuf_insert` directly on a buffer with an `EditCtx` double-shifts
marks or silently loses undo. Grep gate (DoD 11):
`yew_textbuf_insert|yew_textbuf_delete` appears outside `piece.c` only in
`edit.c` and `undo.c`.

### 2. Why a tree (the headline, and what it costs)

Every mainstream editor's undo is a stack: undo three times, type one
character, and the three states you undid are gone forever — a data loss the
user cannot see coming and cannot recover from, the same class of failure
invariant 1 outlaws on disk and convention alone tolerates in memory.

yew's model: a transaction is a **node** whose parent is the state it
applied to. Undo moves to the parent; editing at a node that already has
children appends a **new sibling** and the existing children stay reachable.
vim's `undotree` proved the structure; yew goes further at the three
points where vim leaves it inert — navigation is a first-class command
surface (§7), branches are inspectable and switchable through a real picker
(Sprint 26), and the tree is serialized per workspace (§9) so branches
survive a quit. Costs, stated honestly: memory (§8 bounds it) and one UI
concept to learn — mitigated by plain `undo`/`redo` doing exactly the boring
thing for a user who never branches.

### 3. Op records and where payload bytes live

```c
typedef enum { YEW_OP_INS = 1, YEW_OP_DEL = 2 } YewOpKind;  /* == SAGJ op */

typedef struct UndoOp {
    u8  kind;          /* YewOpKind                                      */
    u8  src;           /* INS: PieceSrc of the payload; DEL: 0           */
    u64 off;           /* pre-op byte offset within this transaction     */
    u64 len;
    u64 payload;       /* INS: Span.lo in that store; DEL: blob offset   */
} UndoOp;
_Static_assert(sizeof(UndoOp) <= 32, "op bloat");
```

Payload storage is asymmetric, and the asymmetry is the design:

| Direction | Needs bytes? | Where they come from |
|---|---|---|
| undo an INS | no — just delete `[off, off+len)` | — |
| redo an INS | yes | `add` store span `[payload, payload+len)` — **already immortal** (s07: the add buffer is append-only and never freed) |
| undo a DEL | yes | copied into `UndoTree.blobs` at record time |
| redo a DEL | no — just delete `[off, off+len)` | — |

**Why INS payloads are not copied:** the bytes are in the add store forever
by s07's construction, so copying doubles the memory cost of typing for no
gain. **Why DEL payloads are copied:** removed bytes are a *sequence of
spans* across `orig` and `add`; a piece list per delete is a second tree
implementation, and a flat copy is what the fuzzer checks byte-for-byte.
Only blob bytes count against the §8 budget; add-store growth is s07's
documented, user-bounded behavior. One new `piece.c` primitive:

```c
/* splice a piece referencing an EXISTING store span — no append */
void yew_textbuf_insert_span(TextBuf *tb, ByteOff at, u8 src, Span s);
```

**Pitfall — undo/redo must not grow the add store.** If redo re-appends the
bytes, an undo/redo/undo/redo loop leaks linearly in the cycle count. Redo
re-splices the *original* span. Unit test: 100 000 cycles leave
`tb->add.len` unchanged after the first redo.

The same rule applies to deletes. `UndoTree.replay_spans` is parallel to
`UndoTree.ops`: the first undo of a DEL inserts its blob bytes through the
ordinary append path and records the resulting add-store span; every later
undo re-splices that span. The blob remains the persistent payload and the
replay span is only a runtime cache. Without it, repeated undo of a delete
would grow the add store just as surely as a broken INS redo.

### 4. Transaction grouping — the matrix

```c
typedef enum {
    YEW_TXN_TYPE = 0,  /* insert burst (I mode)         mergeable        */
    YEW_TXN_ERASE,     /* backspace/delete burst        mergeable        */
    YEW_TXN_PASTE,     /* one paste                     atomic           */
    YEW_TXN_CUT,       /* delete of a unit/selection    atomic           */
    YEW_TXN_MULTI,     /* one multi-cursor edit  (s17)  atomic           */
    YEW_TXN_MACRO,     /* one macro / edit{} run (s34)  atomic           */
    YEW_TXN_FILTER,    /* one shell filter       (s19)  atomic           */
    YEW_TXN_REPLACE,   /* one confirm-replace run(s21)  atomic           */
    YEW_TXN_LSP,       /* one workspace edit     (s47)  atomic           */
    YEW_TXN_EXTERNAL,  /* reload / journal merge        atomic           */
    YEW_TXN_REASON_MAX
} YewTxnReason;

void yew_undo_begin(EditCtx *ec, YewTxnReason why);  /* nestable        */
void yew_undo_end(EditCtx *ec);
void yew_undo_boundary(UndoTree *ut);   /* next op must open a new node */
void yew_undo_abort(EditCtx *ec);       /* inverse-apply, drop the node */
bool yew_undo_reopen(EditCtx *ec, YewTxnReason expect);  /* §7, s12    */
```

**Only `YEW_TXN_TYPE` and `YEW_TXN_ERASE` ever merge.** Everything else opens
and closes exactly one node — that sentence is what makes "one macro run =
one undo step" and "one multi-cursor edit = one undo step" true by
construction instead of by every caller remembering.

Merge predicate for the two mergeable reasons — an op extends the open node
iff **all** hold:

| # | Condition |
|---|---|
| 1 | same `reason` as the open node |
| 2 | `now_mono_ms - node->t_last_ms < YEW_UNDO_BURST_MS` (default 400) |
| 3 | contiguous: TYPE requires `off == prev.off + prev.len`; ERASE requires `off + len == prev.off` (backspace) or `off == prev.off` (forward delete) |
| 4 | node's total op payload `< YEW_UNDO_BURST_BYTES` (default 4096) |
| 5 | no boundary event since the last op (table below) |
| 6 | `ut->depth == 0` (not inside an explicit `begin`) |

Boundary events — each calls `yew_undo_boundary()`:

| Event | Sprint that raises it |
|---|---|
| leaving I mode (Esc) | 14 |
| any command-driven cursor motion between two inserts | 14 |
| newline inserted (option `undo.break_on_newline`, default **true**) | 14 |
| save (`yew_file_save` success) — also sets the save point | this sprint |
| buffer switch / window focus change | 22/23 |
| explicit `ed.undo.boundary` command | 18 |
| any op whose reason is not the open node's | this sprint |

**Pitfall — group at record time, not commit time.** Deciding at close lets a
macro's first op merge into the typing burst before it (the macro's `begin`
has not reached the merge test yet). `yew_undo_record` consults the matrix on
every op.

**Pitfall — `CLOCK_MONOTONIC` for grouping, `CLOCK_REALTIME` for display.**
Wall-clock grouping merges an hour of typing into one node the moment NTP
steps the clock or the laptop resumes. `t_last_ms` is monotonic; `t_wall`
exists only for §7's time travel and is clamped there.

The lifecycle takes `EditCtx`, rather than only `UndoTree`, because commit
captures the after-cursor set and abort must inverse-apply through marks,
cursors, and the crash journal. A borrowed context backpointer in the tree
would outlive stack-owned headless contexts and is forbidden.

Nesting: `begin`/`end` keep a depth counter and only the outermost `end`
commits; unbalanced depth at teardown is `YEW_BUG`. `yew_undo_abort`
inverse-applies the open node's ops in reverse, discards it, and restores the
`before` cursor snapshot — `02-fletch.md` requirement 4 ("uncaught error =
rollback"), available before Fletch exists.

### 5. Node layout

```c
typedef struct { u64 pos, anchor, goal; } CursorRec;          /* 24 B    */
typedef struct { u32 mark_id, mark_gen; u64 rel_off; } MarkRepair;
typedef struct { u32 at, len; } UndoRepairRun; /* parallel to ops       */
typedef struct { Span span; u8 src; bool valid; } UndoReplaySpan;

typedef struct UndoNode {
    u32 id, parent;            /* id: monotonic, NEVER reused (see below) */
    u32 first_child, next_sibling, redo_child;   /* redo_child: §7        */
    u32 depth;                 /* root = 0; makes LCA O(depth)            */
    u32 ops_at, n_ops;         /* slice of UndoTree.ops                   */
    u64 blob_lo, blob_hi;      /* slice of UndoTree.blobs — the trim unit */
    u32 cur_before, n_before;  /* slices of UndoTree.cursors              */
    u32 cur_after,  n_after;
    u32 rep_at, n_rep;         /* slice of UndoTree.repairs               */
    u32 win_id;
    i64 t_wall;                /* CLOCK_REALTIME s, clamped monotone (§7) */
    u64 t_last_ms;             /* CLOCK_MONOTONIC ms of this node's LAST op */
    u8  reason, flags;   /* YEW_TXN_TRIMMED | YEW_TXN_SAVED | YEW_TXN_DEAD */
} UndoNode;

typedef struct UndoTree {
    Vec(UndoNode) nodes;   /* index = id-1; dead nodes remain as tombstones */
    Vec(UndoOp) ops;  Vec(CursorRec) cursors;  Vec(MarkRepair) repairs;
    Vec(UndoRepairRun) repair_runs; /* one exact repair slice per op    */
    Vec(UndoReplaySpan) replay_spans; /* lazy DEL add-store span cache  */
    Bytebuf blobs;
    u32 root, cur, saved, open;   /* node ids; open = 0 when none          */
    u32 depth;                    /* begin/end nesting                     */
    u64 bytes_live, bytes_dead;   /* accounting for §8                     */
    u64 gen;                      /* bumped by trim/compact; §8 pitfall    */
} UndoTree;
```

**Node id is the commit sequence number** — assigned in commit order, never
reused — so "the state before/after this one in time" (§7) is an id scan, the
picker's sort order is free, and serialization needs no remapping. Trimming
leaves tombstones (`YEW_TXN_DEAD`); it never compacts ids. `root` is a real
node with `n_ops == 0` standing for the buffer as loaded (post journal
replay, if any); `cur` is the current state.

### 6. What a transaction stores about cursors and marks — pinned

**Cursors: snapshotted, both sides.** `cur_before` is the initiating window's
`CursorSet` when the node opened, in pre-transaction coordinates;
`cur_after` is the set at commit, in post coordinates. Undo restores
`before`, redo restores `after`. Stored per cursor: `pos`, `anchor`,
`goal_col` — exactly the three fields of an s09 `Cursor`, nothing else — with
`primary` encoded as record order (index 0), since restore re-`normalize`s
anyway. `win_id` names the captured window; restore rules:

| Situation | Rule |
|---|---|
| that window still exists and shows this buffer | restore into it |
| it is gone, or shows another buffer now | restore into the focused window showing this buffer |
| no window shows this buffer (headless / `--batch`) | apply to `ec->cset` directly |
| other windows on this buffer | untouched — they already tracked via `yew_cset_adjust` at the choke point |

**Marks: NOT snapshotted, with one exception.** Undo replays inverse ops
through the same choke point, so `yew_marks_adjust` moves every mark back
for free; snapshotting would double-move them and cost O(m) per keystroke.
The exception is the case inverse application cannot repair: s09 pins that a
delete collapses marks inside the range to the deletion point, destroying
their offsets. So the choke point records a `MarkRepair{id, gen, rel_off}`
for **each mark actually collapsed** — empty for essentially every keystroke.
Undo re-applies `pos = off + rel_off` after the inverse insert; redo
re-collapses via the ordinary forward adjust. Pitfall: match `mark_gen`
before repairing, or a reused slab slot teleports a different mark.

`UndoRepairRun` is indexed one-to-one with `UndoOp` and owns that op's slice
of `repairs`. A node-level flat repair slice alone is insufficient: an erase
burst or atomic transaction can contain several deletes at different
offsets, and inverse replay must know which `off` each `rel_off` belongs to.

### 7. Navigation API

```c
bool yew_undo(EditCtx *ec);                     /* to parent            */
bool yew_redo(EditCtx *ec);                     /* into redo_child      */
bool yew_undo_to(EditCtx *ec, u32 node_id);     /* any node, via LCA    */
bool yew_undo_state(EditCtx *ec, i64 delta);    /* older/newer by id    */
bool yew_undo_time(EditCtx *ec, i64 wall_secs); /* "10 minutes ago"     */
u32  yew_undo_branch_cycle(UndoTree *ut, i32 d);/* rotate redo_child    */
u32  yew_undo_current(const UndoTree *ut);
bool yew_undo_at_save_point(const UndoTree *ut);
```

- `redo_child` is set whenever a child is created **or** traversed, so plain
  `redo` after `undo` retraces the path you just walked — the boring case
  stays boring — while `branch_cycle` rotates siblings when you want the
  other future. Pin: "most recently visited", not "newest", not "first".
- `yew_undo_to(target)`: walk `cur` and `target` to equal depth, then in
  lockstep to the LCA; inverse-apply `cur → LCA`, forward-apply
  `LCA → target`. **Pitfall:** the forward path is discoverable only by
  walking *up* from `target`, so push it on a `Vec(u32)` stack and apply in
  reverse. Undo depth is unbounded — `YEW_PIECE_MAX_DEPTH` does not apply.
- `yew_undo_state(delta)`: `delta < 0` → the live node with the greatest id
  `< cur` (vim's `g-`); `delta > 0` → the least id `> cur` (`g+`). It
  crosses branches by design: "the state I was in before, chronologically"
  is what a user means when the redo path is not where their work went. A
  tombstone-skipping scan, then `yew_undo_to`.
- `yew_undo_time(wall_secs)`: the live node with the greatest
  `t_wall <= wall_secs`, ties to the greatest id. **Pitfall:** NTP can step
  the wall clock backwards, making `t_wall` non-monotonic in id order and
  breaking both the search and the tie rule. On commit store
  `t_wall = max(now, parent->t_wall)` and `yew_log(WARN)` on the clamp; id
  order and time order then agree permanently.
- None of these are transactions — they move `cur` and never create nodes.
  They do bump `tb->gen`, adjust marks and cursors through the choke point,
  journal the inverse ops (an undo is a real edit to the crash journal), and
  call `yew_journal_sync`.

Branch-picker query API — the picker *surface* is Sprint 26; its data is here
so s26 writes a renderer, not an algorithm:

```c
typedef struct {
    u32 id, parent, depth, n_children;  u8 reason;  i64 t_wall;
    u64 bytes_ins, bytes_del;
    bool on_current_path, is_current, is_saved, is_trimmed;
} UndoNodeInfo;

u32  yew_undo_list(const UndoTree *ut, UndoNodeInfo *out, u32 max);/*id order*/
u32  yew_undo_children(const UndoTree *ut, u32 id, u32 *out, u32 max);
void yew_undo_describe(const UndoTree *ut, u32 id, i64 now, char *b, u64 n);
void yew_undo_dump(const UndoTree *ut, FILE *out);   /* headless, tests */
```

`yew_undo_describe` renders e.g. `#42  +18 −0  typed   3m ago  *saved`, and
takes `now` as an argument rather than calling `time(NULL)` — deterministic
text the determinism lane can pin.

### 8. Memory bounds and trimming

Accounting: `bytes_live = live blobs + nodes·sizeof(UndoNode) + ops·32 +
cursors·24 + repairs·16 + repair-runs·8 + replay-spans·sizeof(UndoReplaySpan)`.
Budget `undo.bytes_max` default **64 MiB per
buffer**; floor `undo.min_nodes` default **200** — never trimmed below that
many live nodes at any budget, because pasting a 200 MiB blob must not cost
a user their morning's history.

Protected set **P** = root→`cur` path ∪ root→`saved` path ∪ the
`YEW_UNDO_MIN_NODES` highest-id live nodes. Trim loop, at transaction commit
only (never mid-edit), while `bytes_live > budget`:

1. **Prune the oldest dead-end branch.** Find the lowest-id live node whose
   entire subtree is disjoint from P; mark that subtree dead and free its
   ops/blobs/cursors/repairs. A subtree is closed downward, so every
   survivor keeps its parent — **trimming can never break the parent
   chain**, the invariant that makes ids and the serialized format safe.
2. **Else re-root.** With no off-path branch left, the root has exactly one
   live child `c` (on the protected path). Free `c`'s ops, blobs, and
   `cur_before`; mark the old root dead; `c` becomes root with
   `YEW_TXN_TRIMMED` and `parent = 0`. Undo from `c` now returns false and
   the UI says "oldest state" rather than pretending. Refuse when live nodes
   ≤ `YEW_UNDO_MIN_NODES` or `c == cur`.
3. **Else** log once per buffer (`undo: over budget, nothing trimmable`) and
   stay over. The budget is a soft ceiling; correctness wins.

Blob compaction: freed ranges leave holes in an append-only `Bytebuf`. When
`bytes_dead > bytes_live`, `yew_undo_compact()` copies live blobs forward,
rewrites `blob_lo/hi` and DEL `UndoOp.payload`, and bumps `ut->gen`.
**Pitfall:** any `UndoNodeInfo`, picker snapshot, or blob pointer taken
before a compact is stale — same `gen` discipline as s07's iterators, and a
stale read is `YEW_BUG`. Compaction runs only at a transaction boundary.

Trimming is deterministic (id order, no clock input), so the determinism lane
runs a fixed op script twice and byte-compares `yew_undo_dump`.

### 9. Serialization — `.sagu` v1

The tree persists to a **sidecar binary file**, not into the workspace state
literal: `$XDG_STATE_HOME/yew/workspaces/<ws_hash>/undo/<fnv64(realpath)>.sagu`.
Why a sidecar: undo payloads are arbitrary binary (invalid UTF-8, NULs, which
s02 policy says round-trip), the state file is Fletch *data literals* and
would have to hex-encode tens of MiB, and s25 rewrites that file atomically
on every workspace change. Sprint 25's literal carries only the reference —
`undo: { file: "<hash>.sagu", version: 1 }` — and **Sprint 25 freezes that
enclosing state format**; the `.sagu` layout below is frozen *here*, and
changing it is a version bump plus a migration note, never a silent
reinterpretation.

```
header  : "YEWU" u32 version=1  u32 flags
          u32 root_id, cur_id, saved_id, anchor_id
          u32 node_count                       /* live nodes only        */
          u64 anchor_len, u64 anchor_fnv64     /* content AT anchor      */
          u64 cur_len,    u64 cur_fnv64        /* content AT cur         */
node    : u32 id, parent, redo_child, depth
          u8  reason, u8 flags, u16 _pad0, u32 win_id
          i64 t_wall
          u32 n_ops
            op× : u8 kind, u8 _pad1, u16 _pad2, u64 off, u64 len,
                  u64 plen, plen payload bytes,
                  u32 n_rep, MarkRepair×            /* owned by this op  */
          u32 n_before, CursorRec×                  /* 3×u64 each        */
          u32 n_after,  CursorRec×
          u32 crc32                                 /* over this record  */
trailer : u32 node_crc_xor, "UGAS"
```

- Little-endian throughout; CRC-32 is `journal.c`'s table (invariant 7).
- **Payload bytes are written for both directions.** An INS payload is an
  add-store span in memory, but the add store is not persisted, so
  serialization materializes those `len` bytes from the store; DEL payloads
  come from the blob arena. `plen == len` today; the field exists so a
  future version can elide.
- `anchor_id` = `saved` when a save point exists, else `root`.
- Restore validation, in order: hash the loaded content (after s08 file load
  *and* journal replay). Equals `cur_len`/`cur_fnv64` → restore with `cur`
  as-is. Else equals `anchor_len`/`anchor_fnv64` → restore with
  `cur := anchor_id`. Else **drop the tree**, log, and show a one-line
  status message — never partially apply, same doctrine as s08's stale
  journal. Unknown `version` → drop with a log line.
- Nodes are written in id order, so parents precede children and load is one
  forward pass with no fixup phase.
- Write budget `undo.persist_bytes_max`, default **16 MiB**. Over it,
  serialize a subtree chosen by the §8 trim ordering and set
  `flags |= YEWU_TRUNCATED`; the in-memory tree is never modified by saving.
- Write is atomic — s08's tmp + fsync + rename + dir-fsync primitive,
  reused, not re-implemented.

### 10. Save points and the journal

`yew_file_save` success sets `ut->saved = ut->cur`, sets `YEW_TXN_SAVED` on
that node, and calls `yew_journal_discard` (s08). `yew_undo_at_save_point()`
drives the statusline (s15) and tab (s23) modified markers: the marker is
"cur != saved", so undoing back to the saved state clears it — which a dirty
*flag* cannot do. `yew_journal_sync` is called on every transaction commit
and every navigation move; this is the wiring s08 deferred to Sprint 10.

### 11. Defer

- Keybindings (`u`, `g-`, `g+`) → Sprint 13/14; `ed.undo.*` commands and
  `:earlier`/`:later` → Sprint 18.
- **Branch picker UI** → Sprint 26 (§7's query API is complete now);
  `yew_undo_dump` is the interim surface and stays as a test/`--batch` tool.
- Multi-cursor transactions → Sprint 17: `yew_undo_begin(YEW_TXN_MULTI)`
  from a `CursorSet` with `len > 1` hard-errors naming Sprint 17 until then.
  Macro/`edit{}` → 34; shell filters → 19; replace batches → 21; LSP
  workspace edits → 47. Each already has its reason code and §4 row.
- Workspace state file *referencing* the `.sagu` → Sprint 25; this sprint
  reads and writes `.sagu` through the direct API, tested headless.
- Undo-in-region (Emacs selective undo) → not for 1.0; do not reserve a
  reason code speculatively.

## Testing Strategy

- **Unit** (`test_undo.c`): each §4 merge condition driven positive and
  negative; each boundary event; nesting depth 3 with only the outermost
  committing; `yew_undo_abort` restoring content *and* cursors; the §3
  100 000-cycle add-store-stability case; undo-to-root reproducing load
  content byte-exact.
- **Unit** (`test_undo_tree.c`): the branch case — edit A, undo, edit B,
  assert A is live and `yew_undo_to(A)` reproduces A byte-exactly;
  `redo_child` after every traversal; `yew_undo_state(±1)` crossing a
  branch; `yew_undo_time` over a synthetic `t_wall` series containing a
  backwards clock step (the clamp must fire); LCA over a 5-deep, 3-wide
  tree vs hand-computed paths.
- **Unit** (`test_undo_trim.c`): budget forced to 4 KiB — trim order,
  P never trimmed, `min_nodes` blocking re-root, every live node's parent
  live after 10 000 random edits, `yew_undo` false at a re-rooted root;
  forced compaction with every DEL op still yielding identical bytes.
- **Unit** (`test_undo_serial.c`): round-trip a 500-node tree with binary
  payloads (NULs, invalid UTF-8, a 1 MiB paste) — `dump(load(save(T)))
  == dump(T)`; each node's CRC corrupted in turn; truncation at every
  64-byte boundary; bumped version; all three restore-validation outcomes.
- **Unit** (`test_undo_marks.c`): `MarkRepair` exactness; slab slot reused
  between delete and undo (gen mismatch skips); 1 000 marks × 1 000 random
  edit/undo ops vs an O(m) oracle.
- **Determinism**: a fixed 5 000-op script run twice gives byte-identical
  `yew_undo_dump` output and byte-identical `.sagu` files.
- **Fuzz**: undo/redo/branch ops join Sprint 11's differential campaign
  (properties P6–P8 there); this sprint ships a seeded 10 000-op smoke.
- **Sanitizers**: ASan/UBSan on all five unit binaries; valgrind clean —
  blob compaction and tombstones are the leak/UAF surfaces.

## Definition of Done

1. `make test` green under gcc and clang at `-std=c11 -pedantic -Wall
   -Wextra -Werror`, zero warnings; ASan/UBSan/valgrind/determinism green.
2. `sizeof(UndoOp) <= 32` by `_Static_assert`; no per-node malloc.
3. Branch survival: edit → undo → edit leaves the first branch reachable and
   byte-exact (the §2 headline, as an explicit unit case).
4. All 6 merge conditions and all 7 boundary events have a test proving they
   do and do not merge.
5. Exactly one node per insert burst, erase burst, paste, cut, and external
   transaction — asserted by node count. Deferred MULTI/FILTER/REPLACE/
   MACRO/LSP reasons retain the hard-error gates in item 11 until their
   owning sprints replace those gates with the corresponding node-count
   cases.
6. 100 000 undo/redo cycles leave `tb->add.len` unchanged and RSS flat
   (± 1 MiB) — the §3 pitfall, gated.
7. After 10 000 random edits at a 4 KiB budget: every live node's parent is
   live, P is intact, `yew_undo_to` works for every live id.
8. Serialization round-trip green for ≥ 500 nodes with binary payloads;
   CRC/truncation/version cases drop cleanly and never partially apply;
   `.sagu` write is atomic via s08's primitive.
9. Cursor restore correct for 1- and 8-cursor sets including §6's
   window-gone row; `MarkRepair` restores collapsed marks exactly and the
   1 000-mark oracle test is green.
10. Grep gates: `yew_textbuf_insert\|yew_textbuf_delete` under `src/` hits
    only `text/{piece,edit,undo}.c`; `CLOCK_REALTIME` in `undo.c` hits only
    the `t_wall` capture site.
11. Deferred reasons hard-error naming their sprint (17, 19, 21, 34, 47).
12. ≥ 140 assertions across the five unit files.
