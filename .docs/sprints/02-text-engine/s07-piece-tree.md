# Sprint 7: Piece Tree

## Prerequisites

- Sprint 0 — util containers (`Arena`, `Vec(T)`, `Bytebuf`, `yew_sort_stable`),
  `yew_log`/`YEW_BUG`, exit-code contract, the one Makefile.
- Sprint 1 — bespoke unit harness (registry table), `make test`, CI lanes
  (gcc/clang/ASan/UBSan/determinism).
- Sprint 2 — UTF-8 decoder + invalid-byte policy exist; this sprint does NOT
  use them (the piece tree is byte-oriented by design) but the test corpus
  reuses s02 fixtures as realistic input.
- Binding: `.docs/plan/00-decisions.md` (Buffer structure row — locked:
  original buffer read fully, **no mmap**; append-only add buffer; balanced
  tree with byte *and* newline subtree counts), `01-architecture.md` (Text
  engine section, `src/text/` layout).
- Assumption beyond index entries: s00's `Bytebuf` grows geometrically and
  never shrinks in place (append-only usage here relies on stable growth,
  not stable addresses — see Deliverable 2 pitfall).

## Goals

Land `TextBuf`: the piece tree that every later sprint edits through. Two
immutable-ish backing stores (original file bytes, append-only add buffer),
a weight-balanced tree of pieces whose nodes carry subtree byte and newline
counts, O(log n) insert/delete/lookup, O(log n) line↔offset in both
directions, chunk and line iterators, piece coalescing so typing does not
mint one piece per keystroke, and O(1) copy-on-write snapshots (refcounted
nodes) that Sprint 10's undo and future async consumers stand on. File
load/save is Sprint 8 (this sprint constructs buffers from memory only);
cursors/marks are Sprint 9; undo recording is Sprint 10; the differential
fuzz campaign and 1 GB budgets are Sprint 11 (this sprint ships a smoke-
scale differential test only).

## Deliverables

### 1. Typed offsets — `src/text/coords.h`

```c
typedef struct { u64 v; } ByteOff;   /* byte offset into a TextBuf        */
typedef struct { u64 v; } LineNo;    /* 0-based internal; +1 at display   */
#define BYTEOFF(x) ((ByteOff){ (x) })
#define LINENO(x)  ((LineNo){ (x) })
```

Single-member structs so the compiler rejects mixed arithmetic (the Cgfried
`ValueId` trick). Sprint 9 adds `CharCol`/`GCol`/`CCol` beside these; width
math is banned outside `src/unicode/` from day one. `Span` (`{u64 lo, hi;}`,
half-open `[lo,hi)`) also lives here — it is the ledger's byte-range type
and is deliberately *untyped* (raw u64s) because it is what the backing
stores speak.

### 2. Backing stores — `src/text/piece.h`, `src/text/piece.c`

```c
typedef struct {
    u8      *bytes;    u64 len;      /* orig: malloc'd once; add: Bytebuf */
    Vec(u64) lfs;                    /* offsets of every '\n', ascending  */
} TextStore;

typedef struct TextBuf {
    TextStore  orig;                 /* immutable after construction      */
    TextStore  add;                  /* append-only, grows forever        */
    PieceNode *root;                 /* NULL = empty buffer               */
    u64        gen;                  /* bumped per edit; iterators check  */
} TextBuf;
```

- `orig` is the file read **fully into memory — no mmap** (locked: a file
  truncated under an mmap is a SIGBUS; no-corruption outranks cleverness).
- `add` only ever appends; existing `[lo,hi)` spans into it never dangle
  logically. **Pitfall:** `Bytebuf` growth reallocs, so piece spans store
  *offsets*, never pointers; iterators re-derive pointers per chunk.
- `lfs` is the newline index: `lfs[i]` = offset of the i-th `'\n'`. Built by
  one `memchr` sweep for `orig` at construction; extended incrementally as
  `add` appends. It makes "k-th newline inside a piece" an O(1) array read,
  which is what buys O(log n) line↔offset (Deliverable 6).
- **Why u64 entries:** 8 bytes per line. The s11 1 GiB fixture (~64 B/line)
  costs ~128 MiB ≈ 0.125× — inside the 1.6× memory gate. A degenerate
  all-newline file blows the ratio; documented and accepted, revisit in
  Sprint 56 only if a real corpus hits it.

### 3. Node structure and the balancing decision

```c
typedef enum { YEW_STORE_ORIG = 0, YEW_STORE_ADD = 1 } PieceSrc;

typedef struct PieceNode {
    struct PieceNode *left, *right;  /* NO parent pointer — see pitfall  */
    u32  refs;                       /* COW: >1 = shared with a snapshot */
    u8   src;                        /* PieceSrc                         */
    Span span;                       /* [lo,hi) into that store          */
    u64  lf_first;                   /* index into store lfs of first LF */
    u64  lf_count;                   /* LFs inside span                  */
    u64  sub_bytes;                  /* subtree bytes  incl. this piece  */
    u64  sub_lfs;                    /* subtree LFs    incl. this piece  */
    u32  sub_count;                  /* subtree node count = WBT weight  */
} PieceNode;
_Static_assert(sizeof(PieceNode) <= 96, "node bloat");
```

**Balancing: weight-balanced tree (WBT), Δ=3, Γ=2 — pinned.** Rationale:

- The balance metadata (`sub_count`) is maintained by the *same* one-line
  augmentation pass that maintains `sub_bytes`/`sub_lfs` (`node_fix()`
  below). Red-black adds a second bookkeeping dimension (color invariants)
  orthogonal to the augmentation we must carry anyway.
- Snapshots need path-copy persistence (Deliverable 8). WBT rebalancing is
  pure local rotations decided from child weights — no parent pointers, no
  fixup walk back to the root, so it composes cleanly with COW cloning.
  Red-black insert/delete fixups classically climb parent chains; doing
  that over a COW path stack is a known bug farm.
- Worst-case (not amortized) O(log n) height ≈ 2·log₂ n. Splay trees were
  rejected outright: amortized bounds spike individual keystrokes, and
  latency budgets are CI *gates* (invariant 4).
- Δ=3, Γ=2 is the integer-arithmetic parameter pair proven correct
  (Hirai & Yamamoto 2011); no floating point, no overflow at u32 weights.

```c
static u32 wt(const PieceNode *n) { return (n ? n->sub_count : 0) + 1; }

static void node_fix(PieceNode *n) {          /* the one augmentation site */
    n->sub_count = 1        + (n->left  ? n->left->sub_count : 0)
                            + (n->right ? n->right->sub_count : 0);
    n->sub_bytes = (n->span.hi - n->span.lo)
                            + (n->left  ? n->left->sub_bytes : 0)
                            + (n->right ? n->right->sub_bytes : 0);
    n->sub_lfs   = n->lf_count
                            + (n->left  ? n->left->sub_lfs : 0)
                            + (n->right ? n->right->sub_lfs : 0);
}

/* after any child change: if wt(r) > 3*wt(l) rotate left (double when
 * wt(r->left) >= 2*wt(r->right)); mirror for the right-heavy case.   */
static PieceNode *balance(TextBuf *tb, PieceNode *n);
```

**Pitfall — no parent pointers, ever:** COW sharing means a node can be
reachable from two roots; a parent pointer can only name one of them.
Iterators and rebalancing use an explicit descent stack instead
(`YEW_PIECE_MAX_DEPTH 128` — a 3:1 WBT over the full u32 node domain can
reach about 78 levels; 64 is not a safe bound).

### 4. Insert, split, and coalescing

```c
void yew_textbuf_insert(TextBuf *tb, ByteOff at, const u8 *bytes, u64 len);
```

Algorithm: append `bytes` to `add` (extending `add.lfs` with any newlines),
then splice a piece `{ADD, [old_len, old_len+len)}` into the tree at `at`.

**Coalescing fast path — mandatory, not an optimization.** Before splicing:
if the piece ending exactly at `at` has `src == YEW_STORE_ADD` and
`span.hi == old_add_len` (it was the most recent append), extend that
piece's `span.hi += len` in place (after COW-owning its path) and update
counts up the path. Without this, every keystroke mints a node and an hour
of typing is a million-piece tree. With it, a burst of N sequential
single-char inserts costs exactly the pieces the surrounding split cost —
typically 1.

The splice itself, persistent-recursive style:

```c
static PieceNode *node_own(TextBuf *tb, PieceNode *n) {
    if (n->refs == 1) return n;              /* sole owner: mutate in place */
    PieceNode *c = node_clone(tb, n);        /* shallow copy, refs = 1      */
    if (c->left)  c->left->refs++;           /* children now shared         */
    if (c->right) c->right->refs++;
    n->refs--;
    return c;
}

static PieceNode *ins_rec(TextBuf *tb, PieceNode *n, u64 at, Piece np) {
    if (!n) return node_new(tb, np);
    n = node_own(tb, n);
    u64 lbytes = n->left ? n->left->sub_bytes : 0;
    u64 plen   = n->span.hi - n->span.lo;
    if (at <= lbytes && !(at == lbytes /* boundary: prefer left edge */))
        n->left = ins_rec(tb, n->left, at, np);
    else if (at >= lbytes + plen)
        n->right = ins_rec(tb, n->right, at - lbytes - plen, np);
    else {                                   /* strictly inside: split      */
        u64 k = at - lbytes;                 /* split point within piece    */
        Piece rt = piece_tail(tb, node_piece(n), k);  /* [lo+k, hi)         */
        piece_truncate(tb, n, k);            /* n keeps [lo, lo+k)          */
        n->right = ins_rec(tb, n->right, 0, np);      /* new piece first    */
        n->right = ins_rec(tb, n->right, np.span.hi - np.span.lo, rt);
    }
    node_fix(n);
    return balance(tb, n);
}
```

`piece_tail`/`piece_truncate` recompute `lf_first`/`lf_count` for the two
halves by binary-searching the store's `lfs` for the split offset — O(log L),
never a byte scan. **Pitfall:** the boundary case `at == lbytes` must take
exactly one branch or inserts at piece seams double-insert; pin it with a
unit test at every seam of a 3-piece buffer.

### 5. Delete

```c
void yew_textbuf_delete(TextBuf *tb, Span range);   /* [lo,hi) in buffer */
```

Algorithm: split at `range.lo` and `range.hi` so both are piece seams
(reusing the §4 split machinery), then remove the whole pieces between the
seams. Removal is `del_rec`: descend by byte offset, COW-owning the path;
when a node's piece falls fully inside the range, replace it with
`join(left, right)` — WBT join = take the heavier side's edge node as the
new subtree root (standard WBT delete), then `balance` on the way up.
Deleting a k-piece range is O(k + log n); the fuzzer (s11) leans on ranges
that span many pieces. Deleted bytes are NOT freed — they remain in the
stores (that is what makes snapshots and Sprint 10 undo cheap). Store
growth is bounded by total bytes ever inserted; the s10 trim policy is the
pressure valve, not this sprint's problem.

### 6. Line ↔ offset, O(log n) both directions

```c
u64     yew_textbuf_len(const TextBuf *tb);
u64     yew_textbuf_line_count(const TextBuf *tb);   /* sub_lfs(root)+1  */
ByteOff yew_textbuf_line_start(const TextBuf *tb, LineNo l);
LineNo  yew_textbuf_line_of(const TextBuf *tb, ByteOff off);
Span    yew_textbuf_line_span(const TextBuf *tb, LineNo l); /* incl. EOL */
```

`line_start(l)`: descend with remaining count `r = l`; at each node, if
`r <= sub_lfs(left)` go left; else `r -= sub_lfs(left)`, and if
`r <= lf_count` the answer is inside this piece:
`bytes_before_piece + (store->lfs.data[n->lf_first + r - 1] + 1 - n->span.lo)`
— an O(1) array read, no scanning; else `r -= lf_count`, go right. `l == 0`
returns offset 0. Out-of-range `l` is a `YEW_BUG` (callers clamp; the
buffer never guesses).

`line_of(off)`: descend accumulating LFs of skipped left subtrees and
pieces; within the final piece, count LFs `< off` by binary search in the
store's `lfs` window `[lf_first, lf_first + lf_count)`. `off == len` maps
to the last line. Both directions: no loop ever touches text bytes.

### 7. Iterators — chunks and lines

```c
typedef struct {
    const PieceNode *stack[YEW_PIECE_MAX_DEPTH];
    u32 depth;
    u64 gen;                      /* copied from tb; checked every call  */
    u64 skip;                     /* bytes to skip inside current piece  */
} TextIter;

bool yew_textiter_begin(TextIter *it, const TextBuf *tb, ByteOff at);
bool yew_textiter_chunk(TextIter *it, const TextBuf *tb,
                        const u8 **p, u64 *n);       /* current run      */
bool yew_textiter_advance(TextIter *it, const TextBuf *tb); /* next piece */
```

Yields maximal contiguous byte runs — the substrate for s02's decoder,
Sprint 20's search, and Sprint 8's save path (save = iterate + write, zero
copies). A line iterator (`yew_lineiter_*`) wraps it, yielding `Span` per
line via §6. **Pitfall:** any edit bumps `tb->gen`; an iterator observing a
stale `gen` is a `YEW_BUG` — silent iteration over a mutated tree is how
editors corrupt saves. Iterating a *snapshot* (below) is the supported way
to read concurrently with edits.

### 8. Snapshots — COW + refcounts (undo's substrate)

```c
typedef struct { PieceNode *root; u64 len; u64 gen; } TextSnap;

TextSnap yew_textbuf_snap(TextBuf *tb);        /* O(1): root->refs++     */
void     yew_textsnap_release(TextBuf *tb, TextSnap *s);
bool     yew_textsnap_iter(TextIter *it, const TextSnap *s, ByteOff at);
```

Semantics: a snapshot pins the tree shape at a moment; both stores are
append-only/immutable so shared nodes stay valid forever. Every mutating
descent calls `node_own` (§4), so an edit after a snapshot copies O(log n)
nodes and shares the rest. `release` walks down decrementing refs, freeing
nodes that hit 0 (children of a freed node get `refs--` recursively —
iterative with the explicit stack, no recursion depth risk). Sprint 10
records op logs, not snapshots, for undo — but uses snapshots to capture
deleted bytes lazily and to hand stable views to future async consumers
(syntax, s39). **Pitfall:** `refs` is u32 and single-threaded (invariant
8) — no atomics, and adding them later without a decision update is a
review rejection.

### 9. Invariant checker

```c
void yew_textbuf_check(const TextBuf *tb);   /* debug builds + tests     */
```

Walks the tree asserting: WBT balance at every node; `sub_*` equal
recomputation; spans in-range for their store; `lf_first/lf_count` match a
binary search of the store `lfs`; no zero-length pieces (split/delete must
never leave one — they inflate the tree silently); refs ≥ 1. Called after
every op in unit tests and by the s11 fuzzer.

### 10. Defer

- File load/atomic save/journal → hard-error naming Sprint 8
  (`yew_file_load: Sprint 8`); this sprint builds buffers only via
  `yew_textbuf_new()` / `yew_textbuf_from_bytes(copy)`.
- Cursors, marks, typed grapheme/cell columns → Sprint 9.
- Undo recording → Sprint 10 (`piece.c` keeps no history yet).
- Differential fuzz campaign, 1 GB perf gates → Sprint 11.
- No Fletch surface, no rendering, no keybindings — Campaigns 03/06+.

## Testing Strategy

- **Unit** (`tests/unit/test_piece.c`): construction from bytes incl. empty
  and no-trailing-newline; insert at 0/mid/end/every seam of a 3-piece
  buffer; delete single-byte, cross-piece, whole-buffer, empty-range;
  `line_start`/`line_of` round-trip for every line of a fixture with LF at
  positions {0, adjacent, EOF, no-EOF-newline}; coalescing: 10 000
  sequential 1-byte inserts yield ≤ 3 pieces and identical content;
  interleaved (non-sequential) inserts do NOT coalesce; snapshot taken,
  100 edits applied, snapshot content byte-identical to before; release
  order (snap before buf, buf before snap) both leak-free;
  `yew_textbuf_check` after every mutation above.
- **Differential smoke** (`tests/unit/test_piece_diff.c`): 10 000 random
  ops (seeded, printed on failure) against a naive `Bytebuf` oracle,
  comparing full content + line count every 100 ops. The full campaign
  with the array-of-lines oracle is Sprint 11; this is the tripwire.
- **Determinism lane**: run the differential smoke twice with the same
  seed; identical op traces and identical final trees (compare via
  materialize + piece-count).
- **Sanitizers**: ASan/UBSan lanes green; valgrind spot-run of
  `test_piece` shows zero leaks (snapshot refcounting is the risk).

## Definition of Done

1. `make test` green under gcc and clang, `-std=c11 -pedantic -Wall
   -Wextra -Werror`, zero warnings.
2. `sizeof(PieceNode) <= 96` enforced by `_Static_assert`.
3. Insert/delete/line-lookup all O(log n): demonstrated by the unit perf
   canary — 1 000 000 random 1–8 byte inserts into an initially 1 MiB
   buffer completes in < 2 s on the CI runner (crude ceiling; real gates
   land Sprint 11).
4. Coalescing: the 10 000-keystroke unit case ends with ≤ 3 pieces.
5. `line_start(line_of(off))  <= off` and round-trips exactly at line
   heads, for every byte offset of a 64 KiB mixed-EOL fixture.
6. Snapshot semantics: post-snapshot edits never alter snapshot reads;
   both release orders valgrind-clean.
7. `yew_textbuf_check` passes after every op in every unit test; at least
   one deliberately-corrupted-tree test proves the checker fires.
8. Differential smoke: 10 000 ops × 4 seeds green in CI.
9. ASan, UBSan, determinism lanes green.
10. ≥ 60 assertions across `test_piece.c` + `test_piece_diff.c`.
11. No file I/O anywhere in `src/text/piece.c` (`grep -n 'open\|fopen\|read('
    src/text/piece.c` → nothing) — load/save is Sprint 8's contract.
