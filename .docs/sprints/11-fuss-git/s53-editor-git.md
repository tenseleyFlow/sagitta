# Sprint 53: Git in the Editor — Hunks, Blame, Diff View, Groups

**COMPLETE 2026-08-22.** The index-batched, linear-space diff runtime, hunk
stage/discard, viewport blame, synchronized diff view, cached statusline,
refresh boundaries, and directory-group opening are implemented. Strict GCC,
Clang, ASan/UBSan, deterministic PTY/script, real-Git fixture, structural, and
performance gates are green locally; release remains conditional on the exact
pushed SHA clearing hosted CI and the explicit Valgrind dispatch.

## Prerequisites

- Sprint 51 — `yew_git_snapshot` (`branch`, `upstream`, `ahead`,
  `behind`, `detached`, `unborn`, `conflicted`, `gen`, `taken_ms`),
  `GitEntry.index_oid` (the porcelain v2 `hI` field — this sprint's diff
  cache key), `GitStatusCode` incl. `YEW_GIT_MID_*`, the verb table +
  `yew_git_spawn` (argv only, `--` before paths, stdin for payloads),
  `yew_git_parse_blame` and the `--incremental` record reader,
  `yew_git_invalidate`.
- Sprint 52 — F mode, its tree/item model (`FussItem.path`), the viewer
  pane, and the `g` binding that hard-errors naming **this** sprint.
- Sprint 24 — the facsimile group model: `TabGroup` (id, label, dir_path,
  last_active_member — **no member list**), `yew_group_create`,
  `yew_group_add_member`, `yew_group_remove_member`, `yew_group_members`,
  `yew_group_label`, `yew_tab_is_resident` / `yew_tab_hydrate` /
  `yew_tab_defer`, and s24 §8's deferral: *"F-mode 'open directory as a
  group': Sprint 53 — `ed.group.from_dir` hard-errors naming it."*
  **This sprint discharges that deferral.**
- Sprint 15 — the **2-cell sign column**, reserved and documented in
  `gutter.h` as *"for git hunk signs (Sprint 53) and LSP diagnostic signs
  (Sprint 46)"*; the statusline segment table with the zero-width
  `badges` row (`git branch (53)`, drop priority 5); the gutter-width
  full-damage rule.
- Sprint 22 — `Rect`, `yew_pane_split`, the pane tree, the region
  registry. Sprint 23 — `yew_tab_open`, `yew_tab_find_by_path`.
- Sprint 10 — `yew_undo_begin` / `yew_undo_end` / `yew_undo_abort` and the
  undo **tree**; the buffer's undo generation counter (this sprint's
  cache-invalidation key). Sprint 7/8/9 — `TextBuf`, `TextIter`,
  `FileMeta.eol` / `missing_final_nl`, `Mark` bias, `LineNo`.
- Sprint 4 — terminal **focus events** (`FocusIn`/`FocusOut`, normalized
  by `input.c`); Sprint 19 — job completion and `yew_job_reap`.
- Sprint 26 — `yew_walk_*` / `FileList` / `WalkOpts` (the group-open walk)
  and `yew_sort_stable`. Sprint 21 — the search overlay doctrine (a
  partial result for one frame beats a stalled keystroke).
- Binding: `00-decisions.md` — bespoke-first (invariant 7: **no libgit2,
  no libxdiff — the differ is ours**), single-threaded core (invariant 8),
  latency as a CI gate (invariant 4), no data loss (invariant 1),
  deterministic rendering (invariant 5).

## Goals

Bring git into the buffer: `src/mod/git/gutter.c` (the index-blob differ,
its size budget, the hunk list, sign-column rendering, hunk motions, and
stage/discard **from the buffer** with discard as one undo transaction),
`src/mod/git/blame.c` (lazy per-viewport inline blame as virtual text,
cached and invalidated on edit), `src/mod/git/diffview.c` (side-by-side
panes with row-aligned synchronized scrolling and intraline attributes),
the statusline branch/ahead/behind segment fed from Sprint 51's cache, and
`yew_group_from_dir` / `ed.group.from_dir` — the Sprint 24 deferral,
discharged, and F mode's `g` key with it. The `.git` watching policy is
pinned here: poll on focus-gain and after every job completes, **no
inotify in 1.0**. Conflict-marker navigation and three-way merge editing
are not in 1.0 and are named as such.

## Deliverables

### 1. The diff base is the index blob, not the file on disk

```c
/* Content of `path` at index stage 0 — what `git show :path` prints.
 * Returns false (and an empty base) for a path with no index entry.  */
bool yew_git_index_blob(Ed *ed, const char *path, u32 len, Bytebuf *out);
```

The comparison is `buffer contents` vs `git show :<path>`, and **not** the
worktree file, for three reasons that are each a bug if forgotten:

1. **The buffer is dirty in memory.** The point of gutter signs is to show
   changes as they are typed; diffing against the file on disk shows
   nothing until save.
2. **Staged-but-unsaved is the interesting state.** After
   `ed.git.hunk.stage` (§4) the index has content that exists on neither
   disk nor HEAD; only the index is a coherent base.
3. `:<path>` is `:0:<path>` — **stage 0 does not exist during a
   conflict**. When `yew_git_snapshot()->conflicted` and the file has a
   `u` record, fall back to `HEAD:<path>` and mark the hunk list
   `base_is_head` so the gutter can render conflict-state signs. Falling
   through to "no base" would erase every sign exactly when the user needs
   them most.

Pitfalls:

- `:<path>` is an **object name, not a pathspec**, so `--` does not
  protect it: a path beginning with `-` would be read as an option and one
  containing `:` as a rev separator. Pinned safe form: a long-lived
  `git cat-file --batch` job per workspace, fed `:<path>` **on stdin**,
  where the name is a line rather than an argument. It also amortizes the
  fork across every buffer.
- `--textconv` is **off**: a textconv filter would hand us a rendering of
  the blob (a PDF's text, say) that no diff against real bytes can align.
- No smudge/clean filter is applied either. A repo using `core.autocrlf`
  or a `.gitattributes` filter therefore sees whole-file differences; that
  is honest (the index really does differ from the buffer byte-wise) and
  is documented in the manual rather than papered over.
- **Cache key is `GitEntry.index_oid`** (porcelain v2's `hI`). If the
  index blob id is unchanged since the last fetch, the cached base is
  still valid and no process is spawned — which is why §51 kept `hI` in
  the parsed entry. An untracked file has no `hI`: base is empty and every
  line is an add.

### 2. The differ: Myers O(ND), and why not histogram

```c
typedef enum { YEW_HUNK_ADD, YEW_HUNK_DEL, YEW_HUNK_MOD } HunkKind;

typedef struct GitHunk {
    LineNo base_lo, base_n;   /* lines in the index blob                */
    LineNo buf_lo,  buf_n;    /* lines in the buffer                    */
    HunkKind kind;
} GitHunk;

typedef struct HunkList {
    Arena a; Vec(GitHunk) h;  /* sorted by buf_lo, non-overlapping      */
    u64  buf_gen;             /* buffer undo generation it was built at */
    char base_oid[65];
    bool base_is_head, truncated;
} HunkList;

/* Myers greedy forward/backward with linear-space refinement, over u64
 * line hashes. `budget_d` caps the edit distance D.                    */
bool yew_diff_lines(Arena *a, const u64 *ah, u32 an, const u64 *bh, u32 bn,
                    u32 budget_d, Vec(GitHunk) *out);
```

**Decision: Myers, not histogram.** Reasoning, pinned so it is not
relitigated:

- The sign column needs hunk **boundaries**, not a pretty alignment. The
  entire visible difference between Myers and histogram/patience output is
  *which* of several minimal-length scripts you get — and at two cells of
  gutter, that difference is invisible. Histogram's payoff is in code
  review of large refactors, which is `git diff`'s job, not ours.
- Myers is a **minimal** edit script by construction; histogram is a
  heuristic that falls back to Myers on its unhappy paths, so shipping
  histogram means shipping Myers plus a rare-element index anyway.
- One routine serves three callers: line-level hunks here, char-level
  intraline attrs in §6 (same algorithm, `u64` per grapheme cluster
  instead of per line), and the side-by-side row map. Two algorithms would
  mean two sets of edge cases.
- ~200 lines of C, O(ND) time and O(N) space with the linear refinement —
  it fits the "small enough to own completely" doctrine.

Elements are `u64` line hashes (FNV-1a over the raw bytes **including**
the line terminator, so a CRLF/LF change is a real difference — s08
preserves EOL and so must the differ). Hashing makes each comparison O(1);
collisions are resolved by a byte compare only when two hashes match
*and* the diff is about to emit an equality, so a collision costs
correctness nothing.

**Size budget (each produces a stated, visible outcome — never a silent
partial):**

| Guard | Value | Outcome |
|---|---|---|
| `YEW_DIFF_MAX_LINES` | 200 000 | no signs; statusline shows `git: file too large for signs` |
| `YEW_DIFF_MAX_BYTES` | 16 MiB | same |
| `YEW_DIFF_MAX_D` | 4 096 | abort; emit **one** `YEW_HUNK_MOD` covering both files whole, `truncated = true`, gutter renders `~` across the file |
| `YEW_DIFF_BUDGET_US` | 4 000 | one slice; unfinished diffs resume on the idle timer (s26 §3's slicing discipline) |

**Viewport scoping, stated precisely.** The *diff* is whole-file; the
*lookup* is viewport-scoped. Diffing only the visible lines is wrong —
a hunk's boundaries depend on the global alignment, so a window into the
middle of a file with an inserted line at the top would report every
visible line as modified. So: recompute the whole-file hunk list when
`buf_gen` changes, debounced 150 ms on the idle timer (typing does not
re-diff per keystroke), and per frame the gutter does one binary search
into the sorted `HunkList` for the viewport's line range — O(log n + k)
with k the hunks on screen. Line hashes are cached beside the buffer's
line index and recomputed only for the lines s07's op stream damaged.

**Sign column rendering** (s15 reserved exactly 2 cells):

| Kind | Cells | Role |
|---|---|---|
| added | `▎ ` | `git.sign.add` (green) |
| modified | `▎ ` | `git.sign.mod` (amber) |
| deleted above this line | `▔ ` | `git.sign.del` (red) |
| deleted at buffer end | `▁ ` | `git.sign.del` |
| conflict (base fell back to HEAD) | `▎ ` | `git.sign.conflict` (magenta) |
| diff truncated | `~ ` | `git.sign.unknown` (dim) |

A deletion has no line of its own; it is drawn on the line **after** the
removed text, which is where the cursor must go to restore it. LSP
diagnostic signs (s46) own the same 2 cells: diagnostics win when both
apply, and the git sign moves to the second cell. Pinned here so neither
sprint has to guess.

### 3. Hunk motions

| Command | Default binding | Behavior |
|---|---|---|
| `ed.git.hunk.next` | `A-<down>` | cursor to the first line of the next hunk; wraps with a message |
| `ed.git.hunk.prev` | `A-<up>` | previous hunk |
| `ed.git.hunk.first` / `.last` | — | ends |

Both are `YEW_CMD_REPEATABLE`, so `3 A-<down>` skips three hunks. The
bindings land in `keys_default.c`'s L/W/B tables; if `A-<down>` is already
taken there, `yew_keymap_build`'s duplicate-`seq` validation (s13 §5) hard
-fails the build — the conflict is caught at compile time, not by a user.

### 4. Stage and discard a hunk, from the buffer

```c
bool yew_git_hunk_stage  (Ed *ed, Win *w, const GitHunk *h);
bool yew_git_hunk_discard(Ed *ed, Win *w, const GitHunk *h);
```

**Stage** synthesizes a unified patch and feeds it to
`git apply --cached -` on stdin (s19 `in_buf`/`in_span`):

```
diff --git a/<path> b/<path>
--- a/<path>
+++ b/<path>
@@ -<base_lo>,<base_n> +<buf_lo>,<buf_n> @@
 context
-removed
+added
 context
```

Pitfalls, each a test:

- **Context is exactly 3 lines** on both sides (clipped at file bounds),
  so `--unidiff-zero` is *not* passed. Passing it with real context makes
  `git apply` misplace hunks; passing zero context without it is rejected.
  Pin the count.
- **`\ No newline at end of file`** must be emitted after the last line of
  either side when that side lacks a final newline (s08's
  `FileMeta.missing_final_nl`, and the base blob's own last byte). Omitting
  it silently *adds* a trailing newline to the staged content — a one-byte
  corruption that survives into the commit and that nobody notices until a
  diff shows a phantom change.
- **Bytes, not lines.** The patch carries the buffer's raw bytes including
  CRLF; a CRLF file whose patch was built from normalized lines applies to
  a different file than the one on screen.
- The header path is workspace-relative with `/` separators, and a path
  containing a space needs **no** quoting in the `diff --git` line as long
  as `--no-prefix` is not used and the path has no tab — but a path
  containing a `\n` cannot be expressed in a patch at all. Detect it and
  refuse with `cannot stage hunks in a file whose name contains a
  newline; use F mode to stage the whole file` rather than producing a
  patch that applies to the wrong file.
- Apply failure is surfaced verbatim (git's `error: patch failed:` lines)
  and changes nothing. `--cached` never touches the worktree, so a failed
  stage cannot corrupt anything (invariant 1).

**Staging from a dirty buffer is allowed**, and this is a decision, not an
oversight: it is the exact workflow the feature exists for — stage the
part of the edit that is finished while the rest is still in flight.
Consequence: the worktree file and the index now differ from each other
*and* from the buffer. The statusline shows `*` and `↑` together, and the
first time it happens in a session a message says `staged from an unsaved
buffer — the file on disk is still the old version`. Refusing instead
would make the feature useless.

**Discard reverses the hunk in the BUFFER**, as one undo transaction:

```c
yew_undo_begin(w->buf);                         /* s10 */
yew_textbuf_delete(tb, buffer_span_of(h));     /* the buffer's version   */
yew_textbuf_insert(tb, h_start, base_lines, n);/* the index's version    */
yew_undo_end(w->buf);                        /* ONE undo step          */
```

It never runs `git checkout -- <path>` and never touches the worktree.
This is the whole argument: `git checkout` destroys work irrecoverably,
which is why Sprint 52's file-level `x` demands a typed confirmation. A
hunk discard here is one `ed.edit.undo` away from being back — an
undo capability git itself cannot offer, on top of git's own data. The
message says so: `hunk discarded (undo restores it)`.

Multi-cursor and selections: with a selection active, `ed.git.hunk.stage`
/ `.discard` operate on **every hunk intersecting it**, still as one patch
and one transaction respectively.

### 5. Inline blame as virtual text

```c
void yew_git_blame_toggle (Ed *ed, Win *w);          /* ed.git.blame.toggle */
const BlameLine *yew_git_blame_at(Ed *ed, Win *w, LineNo l);
```

- Rendered at the **end of the line**, after the text, as virtual cells:
  `  ▏ jane, 3 weeks ago · fix the CRLF case`, in the `git.blame` (dim)
  role. Virtual text never shifts real text, is never selectable,
  clickable, or cursor-addressable, and is **omitted entirely** when fewer
  than 24 cells remain on that row (clipping it to `…` is noise). Wrap
  mode: drawn only on a logical line's final display row.
- Computed **lazily per viewport**: one
  `blame --incremental -L <top>,<bottom> --contents - -- <path>` job per
  viewport range, feeding the **dirty buffer** on stdin so unsaved edits
  blame correctly (s51 §9). Lines with the all-zero sha render as
  `(uncommitted)`, never as the author name git invents for them.
- Cache: `Strmap` of `(buf_id, range_lo/64)` → arena block, plus the
  commit-metadata map keyed by sha (s51 §9's parser already builds it —
  the second and later groups of a commit carry no metadata).
  **Invalidated on any edit** by comparing the buffer's undo generation.
  While a recomputation is in flight the previous text stays on screen in
  a *stale* attr (dimmer still) rather than vanishing — s21's overlay
  doctrine: a stale annotation for one frame is invisible, flicker is not.
- Relative times are formatted by us from `%at` epoch against a `now`
  passed in by the caller — never `%ar`, never `localtime` in the render
  path. That is what makes the pty goldens byte-stable (invariant 5) and
  it is the same injected-`now` discipline as s26's undo branch picker.
- Scrolling a 100 kLOC file must not spawn a job per row: requests are
  quantized to 64-line ranges, debounced 200 ms after scrolling stops, and
  capped at `YEW_BLAME_MAX_INFLIGHT = 2`.

### 6. The diff view — side-by-side, row-aligned, synchronized

`ed.git.diff.view` (F mode's `d` gains a `D` sibling; E mode `:git.diff`)
opens two scratch buffers in a vertical split (s22): base on the left,
buffer on the right.

```c
typedef struct DiffRowMap { Vec(i32) left, right; } DiffRowMap; /* -1 = filler */
```

- **Alignment is by row map, not by line number.** After the first
  unbalanced hunk the two files' line numbers diverge permanently; the map
  is built by the §2 differ and filler rows (`~`, `git.diff.filler`) pad
  the shorter side so equal content sits on the same screen row. Scrolling
  by raw line number is the classic side-by-side bug and it looks correct
  for exactly as long as the first hunk is balanced.
- **Synchronized scrolling**: windows carry a `u32 scroll_link` group id;
  any viewport change on a member maps its top row through `DiffRowMap`
  and sets the others. **Pitfall — the feedback loop:** A scrolls B, B's
  scroll handler scrolls A, forever. One module-static `syncing` flag,
  entered before propagating and cleared after, plus a
  `_Static_assert`-adjacent comment saying why. Cursor movement is *not*
  synchronized (following someone else's cursor is disorienting); only the
  viewport is.
- **Intraline attributes** for `YEW_HUNK_MOD` hunks whose two sides have
  equal line counts: run the §2 differ at grapheme-cluster granularity
  over each line pair, attr the differing runs `git.diff.intra.{add,del}`.
  Capped at `YEW_DIFF_INTRALINE_MAX = 512` bytes per line; beyond that the
  whole line takes the hunk attr. Cluster granularity, not byte: attring
  half a ZWJ family is invariant 2's failure mode.
- Both panes are `YEW_BUF_SCRATCH | YEW_BUF_NOUNDO` and **read-only**;
  editing happens in the real buffer, and hunk staging from the diff view
  is Sprint 52's F-mode surface plus §4's buffer commands, not a third
  path.

### 7. The statusline branch segment

s15 reserved the `badges` segment at width 0 and drop priority 5; it gets
its width here. Content comes **only** from `yew_git_snapshot()` — the
statusline never spawns anything and never blocks (invariant 4).

| State | Segment |
|---|---|
| normal | `⎇ trunk ↑2 ↓1` (arrows omitted when the count is 0; both omitted when `ahead < 0`, i.e. no `# branch.ab`) |
| no upstream | `⎇ trunk` |
| detached | `⎇ (a1b2c3d)` |
| unborn | `⎇ (no commits)` |
| mid-merge | `⎇ trunk\|MERGING` |
| mid-rebase | `⎇ trunk\|REBASE 3/7` (from `$GIT_DIR/rebase-merge/{msgnum,end}`) |
| cherry-pick / revert / bisect | `\|CHERRY-PICKING` / `\|REVERTING` / `\|BISECTING` |
| conflicted | the above plus `⚑N` in the `error` role, N = `u` records |
| not a repo / no git | segment absent (width 0), never an error chip |

git's own vocabulary is used deliberately: a user who has read
`git status` once already knows what `REBASE 3/7` means. The `⎇` glyph
falls back to `git:` under `git.ascii_glyphs` (s52 §3). While a refresh is
in flight the last known value stays — a segment that blinks empty twice a
second is worse than one that is 500 ms stale.

### 8. `ed.group.from_dir` — Sprint 24's deferral, discharged

```c
typedef struct GroupFromDirOpts {
    bool recursive;      /* default true                                 */
    bool respect_ignore; /* default true — s26 walk + s51 ignore set      */
    u32  max_members;    /* YEW_GROUP_MAX_MEMBERS = 200                   */
    const char *label;   /* NULL => basename(dir) + "/"                   */
} GroupFromDirOpts;

u32 yew_group_from_dir(Ed *ed, const char *dir, const GroupFromDirOpts *o);
```

Algorithm, and every step is a Sprint 24 rule being honored:

1. Walk `dir` with `yew_walk_*` (s26 §3): explicit stack, entries sorted
   with `yew_sort_stable` before descending (**deterministic membership
   order** — `readdir` order would make two identical checkouts produce
   different `group_ordinal`s), symlink-loop guard on, `.git` skipped,
   ignore rules from the s51 set when inside a repo and from s26's
   matcher otherwise.
2. **Over `max_members` ⇒ do not open 5 000 tabs.** Open s24's group
   picker (`yew_gp_show`) rooted at `dir` with the walk's paths
   preselected, and let the user cut it down. A group that silently
   truncated would be a lie about what "open this directory" did.
3. `yew_group_create(ed, dir, o->label)`.
4. For each path in walk order: `yew_tab_find_by_path` first — an
   already-open tab is **adopted, not duplicated** (s24 §4's confirm
   rule). An adopted tab that already belongs to another group goes
   through `yew_group_remove_member` **before** `yew_group_add_member`, or
   both groups' ordinals desync (s24 §2's pinned rule).
5. New tabs open **deferred** (`yew_tab_defer`): a 40-file directory costs
   exactly **one** file read, the one member that gets focused. s24 DoD 4
   already asserts this for groups; the same hook counts it here.
6. Focus the first member; record it as `last_active_member` (a path, per
   the model).

Surfaces: F mode's `g` on a directory row (Sprint 52 bound it and left it
hard-erroring), E mode `:group.from_dir <dir>`, and the file finder's
alternate accept on a directory row. **Pitfall:** the group's `dir_path`
is a *label of origin*, not a constraint — s24 says so explicitly, and
members added later may live anywhere. Nothing in this sprint may start
treating it as a membership filter.

### 9. The `.git` watching policy — poll, no inotify (pinned for 1.0)

Refresh triggers, and this is the complete list:

| Trigger | Action |
|---|---|
| Sprint 51's 500 ms TTL, on demand | normal path |
| **Focus-gain** (s04 `FocusIn`) | `yew_git_invalidate` then refresh — not merely "let the TTL expire" |
| **Every user/external job completion** (`yew_job_reap`, including ad-hoc git jobs) | invalidate; `:!make`, `:!git rebase --continue`, and an LSP formatter all change the worktree |

Yew's private read-only Git workers are not external completion events: their
result is the refresh itself, so invalidating on their completion would create
an endless status-refresh loop. Private mutate/network jobs already invalidate
structurally from the verb callback established by Sprint 51.
| Buffer save (s08's atomic-save completion) | invalidate |
| Explicit `C-r` / `ed.git.refresh` | forced |

**No inotify in 1.0.** The reasoning, pinned so it is a decision and not
an omission:

- It buys little. In a single-user editor the overwhelming majority of
  `.git` changes are caused *by yew itself*, and those are already
  covered structurally (§51's mutating-verb invalidation). The remaining
  case is a git command run in another terminal — and the user is not
  looking at yew while they do it. Focus-gain covers exactly that
  moment, which is when they look back.
- It costs a platform abstraction. `arm64-macos` is one of the four locked
  targets, so inotify means kqueue too, plus a fallback for neither —
  three code paths and a new class of fd-exhaustion bug, against a
  dependency-free doctrine (invariant 7) and a single-threaded loop
  (invariant 8).
- Watching `.git` well is not simple. Packfile churn, `index.lock`
  create/delete pairs, and `refs/` directory-vs-packed-refs storage make
  the event stream noisy enough that a correct implementation debounces
  down to roughly the 500 ms poll we already have.

Reopen condition, stated so a future maintainer knows the bar: a
reproducible report where a stale snapshot survives a focus-gain **and**
a job completion. Absent that, this stays closed.

### 10. Defer and non-goals

- **Conflict-marker navigation and three-way merge editing** (`ed.git.
  conflict.next`, take-ours/take-theirs) — **not in 1.0**. The commands
  are registered and hard-error with `conflict resolution is not a 1.0
  feature (F mode's diff and your editor are)`; a permanent scope
  statement, not a sprint deferral. F mode already shows conflicted files
  and this sprint's gutter shows their hunks against `HEAD`.
- Staging **selections** finer than a hunk (a sub-hunk, git's `e` in `add
  -p`) → post-1.0; §4's selection path covers whole hunks only.
- Blame in the gutter (an author column instead of virtual text) →
  post-1.0 config, not a new mechanism.
- `git.blame` / `git.sign.*` / `git.diff.*` theme roles are declared in
  the default themes here; theme authoring docs are Sprint 41's, unchanged.
- Persisting hunk lists or blame caches across restart: never. They are
  derived from the index and rebuilt in milliseconds; Sprint 25's schema
  says so.

## Testing Strategy

- **Unit** (`tests/unit/test_diff.c`): the Myers implementation against a
  brute-force minimal-edit-distance oracle on 5 000 random line-sequence
  pairs (n ≤ 40) — asserting **equal edit-script length**, not identical
  scripts; empty vs empty, empty vs N, identical inputs (zero hunks, and
  zero allocations); a 10 000-line file with one changed line completing
  in ≤ 2 ms; `YEW_DIFF_MAX_D` exceeded producing exactly one
  `truncated` hunk; hash-collision handling forced by a stubbed hash that
  collides everything (result must still be correct);
  CRLF-vs-LF counted as a difference; the intraline path at cluster
  granularity over a ZWJ family and a CJK run.
- **Unit** (`tests/unit/test_hunk.c`): patch generation golden-compared
  against `git diff` output for 20 fixture edits (add at BOF, add at EOF,
  delete at BOF/EOF, modify, adjacent hunks 1 line apart, hunks at file
  bounds where context clips); **the no-final-newline case in all four
  combinations** (base has/lacks, buffer has/lacks) — each generated patch
  is fed to a real `git apply --check --cached` and must be accepted;
  a newline-in-filename fixture refused with the exact message.
- **Unit** (`tests/unit/test_blamecache.c`, fake clock + injected `now`):
  metadata reuse across groups; cache hit/miss by undo generation;
  quantization to 64-line ranges; the in-flight stale-attr path;
  `YEW_BLAME_MAX_INFLIGHT`; relative-time formatting table (`just now`,
  `3 minutes ago`, `3 weeks ago`, `2 years ago`) with a fixed `now`.
- **Script** (`tests/script/git_hunk_*`, against s51's fixture repo):
  stage a hunk from a dirty buffer, then assert `git diff --cached`
  matches a golden **and** the worktree file is byte-unchanged; discard a
  hunk and assert the buffer matches the base, then `ed.edit.undo` and
  assert byte-identity with the pre-discard buffer and **undo depth
  exactly 1**; hunk stage over a selection spanning 3 hunks producing one
  patch; a conflicted file falling back to `HEAD:` base.
- **Script** (`tests/script/group_from_dir_*`): a 40-file directory opens
  a group with **1** file read (s24's file.c counting hook); ordinals
  follow the sorted walk order and are identical across two runs on a
  scrambled-creation-order fixture; an already-open tab is adopted, not
  duplicated; an adopted tab from another group leaves it first (both
  groups' ordinals stay contiguous); a 5 000-file directory opens the s24
  picker instead of tabs; an empty directory reports and creates no group.
- **Pty** (`tests/pty/git_editor_*.golden`): all six sign kinds in the
  gutter, including deletion-above and deletion-at-EOF; a sign next to a
  gutter-width change across 99→100 lines (one full redraw, s15 DoD 9);
  inline blame with a CJK line where the annotation is omitted for lack of
  room and where it fits; the stale-blame attr; side-by-side view with
  filler rows and intraline attrs; synchronized scroll (scroll left,
  assert right's grid moved to the aligned row); every statusline row of
  §7 including `REBASE 3/7` and `⚑2`.
- **Perf** (`tests/perf/git_gutter.c`): a 100 kLOC buffer with 500 hunks —
  full re-diff ≤ 25 ms off the keystroke path (idle timer, sliced at 4 ms)
  and **per-frame sign lookup ≤ 50 µs**; typing 200 characters triggers
  **one** re-diff (debounce proven by a counter hook); keypress p99 ≤ 5 ms
  throughout; scrolling the same file with blame on stays inside the
  scroll-throughput gate.
- **Torture**: kill -9 during a hunk stage leaves the index either fully
  patched or untouched, never half (invariant 1; `git apply` is atomic per
  invocation — the test asserts it).
- Lanes: gcc, clang, ASan/UBSan, valgrind, determinism, `MODULES=""`.

## Definition of Done

1. `make test` green on gcc and clang, zero warnings; ASan/UBSan/valgrind/
   determinism/`MODULES=""` lanes green.
2. The Myers differ matches the brute-force oracle's **edit-script length**
   on 5 000 random pairs and produces zero hunks (and zero allocations)
   for identical inputs.
3. All 20 generated patches are accepted by a real `git apply --check
   --cached`, including all four no-final-newline combinations and a CRLF
   fixture.
4. Staging a hunk from a **dirty** buffer changes `git diff --cached` to
   the golden and leaves the worktree file byte-identical; the one-time
   `staged from an unsaved buffer` message fires exactly once per session.
5. Discarding a hunk is **one** undo transaction: undo depth is exactly 1
   and the buffer is byte-identical to its pre-discard state; no
   `git checkout` appears anywhere —
   `grep -rn 'checkout' src/mod/git/gutter.c` → empty.
6. The diff base is the index blob: a test where the worktree file, the
   index, and the buffer all differ asserts the signs describe
   buffer-vs-index; the conflicted fixture falls back to `HEAD:` and sets
   `base_is_head`.
7. `index_oid`-keyed caching: editing a buffer 100 times with an unchanged
   index spawns **zero** `cat-file` requests after the first (counted).
8. Every size guard produces its stated visible outcome; no guard produces
   a silently partial hunk list.
9. Blame: metadata reuse across groups is asserted against a fixture where
   only the first group carries it; `--contents -` blames unsaved lines as
   the zero sha rendered `(uncommitted)`; the cache invalidates on edit and
   the stale attr shows meanwhile.
10. Side-by-side alignment holds after an unbalanced hunk (pty golden), and
    the sync-scroll feedback loop is proven absent (a test that would
    recurse without the `syncing` flag, with the comment saying so).
11. All ten statusline rows of §7 appear in goldens; the segment never
    spawns a process —
    `grep -rn 'yew_git_spawn\|yew_job_spawn' src/ui/statusline.c` → empty.
12. `ed.group.from_dir` is live: Sprint 24 DoD 11's hard-error is gone,
    the 40-file group costs one file read, ordinals are deterministic
    across runs, and F mode's `g` opens a group.
13. No inotify anywhere: `grep -rn 'inotify\|kqueue\|fanotify' src/` →
    empty; focus-gain and user/external job completion both invalidate
    (asserted by a test that changes the repo behind yew's back and then
    delivers each trigger). Internal read workers do not self-invalidate;
    internal mutate/network callbacks invalidate structurally.
14. Perf gates green: 100 kLOC/500-hunk re-diff ≤ 25 ms off the keystroke
    path, per-frame sign lookup ≤ 50 µs, 200 keystrokes ⇒ one re-diff,
    keypress p99 ≤ 5 ms; baselines committed under `tests/perf/baselines/`.
15. Conflict resolution commands hard-error with the permanent-non-goal
    message; sub-hunk staging is named as post-1.0, not deferred.
16. ≥ 150 assertions across the three unit files; ≥ 14 pty goldens, all
    byte-stable across two runs.
