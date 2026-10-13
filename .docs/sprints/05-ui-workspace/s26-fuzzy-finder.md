# Sprint 26: The Fuzzy Finder and the One Picker

## Prerequisites

- Sprint 22 — `Rect`, the region registry (`yew_region_frame_begin` /
  `_add` / `_hit`, last-added-wins, `YEW_REGION_BLOCK`), and the law that
  layout computes cells once while drawing *and* hit-testing consume the
  same values.
- Sprint 23 — `Tab` with stable `tab_id`, `Tabs`, `yew_tab_index_of_id`,
  `yew_tab_switch`, `yew_tab_modified` (derived from undo, never cached).
- Sprint 24 — groups, `yew_group_label`, lazy hydration
  (`yew_tab_is_resident` / `yew_tab_hydrate`). The s24 **group picker** is
  a different widget with a different interaction (walk directories, tick
  a path set) and is **not** retrofitted onto this sprint's list picker;
  both draw chrome through the same helpers, which is the reuse that
  matters.
- Sprint 18 — the `CmdLine` widget and the law *"there is exactly one text
  editor in this program"*: the picker's filter line is a `CmdLine` of
  kind `YEW_PROMPT_INPUT`, not a `char buf[256]`.
- Sprint 13 — the `"ed.*"` registry (`yew_cmd_register`, `yew_cmd_count`,
  `yew_cmd_at`), keymap layers (the picker is a modal layer), the timer
  heap. Sprint 2 — `yew_str_clip`, `yew_str_width`, `yew_cluster_width`
  (all width math stays in `src/unicode/`). Sprint 0/1 — `Arena`,
  `Vec(T)`, `yew_sort_stable`, `yew_log`, the unit harness.
- Sprint 10 — `yew_undo_list`, `yew_undo_children`, `yew_undo_describe(ut,
  id, now, buf, n)`, `yew_undo_to`, `UndoNodeInfo`. **s10 §11 deferred the
  undo branch picker UI to this sprint**; it closes here.
- Binding: `00-decisions.md` — bespoke-first (invariant 7; POSIX
  `fnmatch` is available and rejected in §4 for stated reasons),
  keypress→paint p99 ≤ 5 ms as a CI gate (invariant 4), modal-first
  (invariant 9). `01-architecture.md` — `src/ws/finder.c (fuzzy)`.
  Source of truth for the scoring port:
  `~/GithubOrgs/FortranGoingOnForty/fuss/src/fuss_main.f90`
  (`fuzzy_match_score`, `fuzzy_jump_to_match`).

## Goals

Land the ranking engine and the one list picker every future list in the
program will be an instance of. Specifically: the fuss scoring function
ported and pinned as a table, with its **two-pass basename rule** expressed
as a total order so it ranks a list rather than picking one row; the
bespoke workspace file walk (explicit-stack traversal, symlink-loop guard
via a device+inode set, a bespoke `.gitignore` matcher whose supported and
unsupported syntax is spelled out honestly, and dir-prefix pruning); the
shared list-picker widget (filter line + ranked list + preview slot +
counts footer); three instances — file finder, buffer/tab switcher, and the
undo branch picker that closes Sprint 10's deferral; and the type-to-jump
plumbing with its 500 ms inter-keystroke reset for lists that cannot have a
filter line. Ranking a 100 000-file workspace stays inside the keystroke
budget via an incremental narrowing strategy over a cached candidate set.
Later owning sprints delivered all four picker surfaces: the LSP symbol
picker (47), plugin/package discovery UI (54–55), F-mode fuzzy jump (52),
and command palette (38). They reuse `PickerSpec` where filtering lists is
the final interaction and the type-to-jump engine for the visible F-mode
tree; none introduces a second picker event loop.

## Deliverables

### 1. The scoring function — `src/ws/finder.h`, `src/ws/fuzzy.c`

```c
#define YEW_FZ_NO_MATCH  INT32_MIN
#define YEW_FZ_MAX_POS   64

typedef struct { u16 n_pos; u16 pos[YEW_FZ_MAX_POS]; } FzMatch;  /* bytes */

/* Score `text` against `pat`. Returns YEW_FZ_NO_MATCH when pat is not a
 * subsequence of text. `m` may be NULL when positions are not wanted.  */
i32 yew_fz_score(const char *pat, u32 plen,
                 const char *text, u32 tlen, FzMatch *m);
```

The table is the contract — every row is a unit test:

| Rule | Value | Condition |
|---|---|---|
| empty pattern | `1` | `plen == 0`; everything matches, source order preserved |
| exact | `10000` | case-folded `pat` equals case-folded `text`; returns immediately |
| prefix | `5000` | case-folded `text` starts with case-folded `pat`; returns immediately |
| matched char | `+100` | each character matched by the greedy left-to-right scan |
| consecutive run | `+50 × k` | `k = 2, 3, …` is the position within the run; the **first** character of a run gets no run bonus |
| start of text | `+200` | the match landed at byte 0 |
| word boundary | `+150` | the preceding byte is one of `/` `_` `-` `.` |
| gap | `−1` | each unmatched byte **after** the first match |
| length penalty | `−tlen` | applied once, at the end |
| no match | `YEW_FZ_NO_MATCH` | pattern not exhausted |

```c
i32 yew_fz_score(const char *pat, u32 plen,
                 const char *text, u32 tlen, FzMatch *m)
{
    if (m) m->n_pos = 0;
    if (plen == 0) return 1;
    if (plen > tlen) return YEW_FZ_NO_MATCH;      /* cheap reject first  */
    if (fz_eq_fold(pat, plen, text, tlen))  return 10000;
    if (fz_pfx_fold(pat, plen, text, plen)) return 5000;

    i32 score = 0; u32 pi = 0, run = 0; bool started = false;
    for (u32 ti = 0; ti < tlen && pi < plen; ti++) {
        if (fz_fold(text[ti]) != fz_fold(pat[pi])) {
            run = 0;
            if (started) score -= 1;              /* gap penalty         */
            continue;
        }
        started = true;
        score += 100;
        run += 1;
        if (run >= 2) score += (i32)(50 * run);   /* 2nd char of a run
                                                   * is worth 100, 3rd
                                                   * 150, and so on      */
        if (ti == 0) score += 200;
        else if (text[ti-1] == '/' || text[ti-1] == '_' ||
                 text[ti-1] == '-' || text[ti-1] == '.') score += 150;
        if (m && m->n_pos < YEW_FZ_MAX_POS) m->pos[m->n_pos++] = (u16)ti;
        pi++;
    }
    if (pi < plen) return YEW_FZ_NO_MATCH;
    score -= (i32)tlen;
    return score <= YEW_FZ_NO_MATCH ? YEW_FZ_NO_MATCH + 1 : score;
}
```

Three deliberate divergences from the Fortran original:

- **`YEW_FZ_NO_MATCH` is `INT32_MIN`, not `0`.** fuss returns 0 both for
  "pattern not exhausted" and, arithmetically, for a long path whose gap
  and length penalties cancel its bonuses — a real match then vanishes
  from the list. The sentinel separates the two, and the clamp on the last
  line keeps a genuine score from colliding with it.
- **Matched byte positions are captured** (`FzMatch.pos`) so the picker
  can highlight them; fuss does not need them, a filtering list does.
  Capped at 64 — beyond that the tail is unhighlighted, never rescored.
- **Case folding is ASCII-only** (`fz_fold` = `'A'..'Z' | 0x20`, every
  other byte verbatim). Full Unicode folding of both strings on every
  keystroke across 100 000 paths is not free, and would need a
  normalization pass to be correct rather than merely slower. A non-ASCII
  filename is therefore findable case-sensitively and matches itself
  exactly — pinned, tested, and in the manual, rather than discovered.

**Pitfall — this is a greedy first-match scan, not an optimal alignment.**
fzf runs a dynamic program to find the best alignment; this takes the
first, so `abc` against `axbxc_abc` scores the early, worse one. That is
fuss's behavior, it is O(n) instead of O(n·m), and §2's basename rule
covers the case users actually complain about (a directory losing to a
file inside it). Do not "improve" it into a DP without re-pinning the
table and the goldens.

### 2. Ranking a list — the two-pass basename rule as a total order

```c
#define YEW_FZ_BASENAME_TIER 100000

typedef struct { u32 idx; i32 score; FzMatch m; } FzRanked;

/* Rank `n` candidates; returns the count that matched.  Output is
 * descending by score, ties broken deterministically (see below).      */
u32 yew_fz_rank(const char *pat, u32 plen, const char *const *text,
                u32 n, bool path_mode, FzRanked *out);
```

fuss's `fuzzy_jump_to_match` runs pass 1 over basenames, takes the answer
if it scored ≥ 5000, and only then runs pass 2 over full paths — which
selects one row. A filtering list needs an ordering of *all* rows, so the
same intent becomes one key:

```
sb = yew_fz_score(pat, basename(text))
sp = yew_fz_score(pat, text)                     /* path_mode only      */
key = (sb >= 5000) ? sb + YEW_FZ_BASENAME_TIER   /* exact/prefix tier   */
                   : max(sb, sp)
```

so **every exact-or-prefix basename match outranks every fuzzy path
match**, globally. That is exactly why `src` selects the directory `src/`
and not `src/file.c`: `src` is a prefix of the basename `src`
(5000 + 100000), while `src/file.c` only matches fuzzily on its path.

- `path_mode == false` (buffer lists, undo nodes, commands) scores the
  label only; `basename()` is not applied.
- A candidate with `YEW_FZ_NO_MATCH` on both keys is excluded, not sorted
  to the bottom — the footer's count must mean "matches", not "rows".
- Ties break by (shorter text, then `memcmp` of the bytes), through
  `yew_sort_stable`. Never `qsort`: glibc and musl disagree on tie order
  and the pty goldens would differ per-libc (Cgfried doctrine, invariant 5).
- `basename()` here is a byte scan for the last `/`, not POSIX `basename(3)`
  (which may mutate its argument and is not thread-consistent). One inline
  helper, one behavior.

### 3. The workspace walk — `src/ws/walk.h`, `src/ws/walk.c`

```c
typedef struct WalkOpts {
    bool follow_symlinks;    /* default false                           */
    bool hidden;             /* include dotfiles; default false         */
    u32  max_depth;          /* default 64                              */
    u64  max_entries;        /* default 200000                          */
} WalkOpts;

typedef struct FileList {
    Arena      a;
    Vec(char*) paths;        /* workspace-RELATIVE, arena-owned          */
    u64 n_dirs, n_files, n_ignored, n_loops;
    bool truncated;
} FileList;

typedef struct WalkState WalkState;
WalkState *yew_walk_begin(const char *root, const WalkOpts *o, FileList *out);
bool       yew_walk_step (WalkState *w, i64 budget_us);   /* false = done */
void       yew_walk_end  (WalkState *w);
```

- `opendir` / `readdir` / `fstatat(AT_SYMLINK_NOFOLLOW)` over an
  **explicit stack**, never `nftw`/`ftw`: they cannot be
  budget-interrupted, they carry callback state through globals, and their
  order is not ours to pin. Recursion is out too — a 400-deep tree is a
  stack hazard in a program that must never crash.
- **Determinism (invariant 5): each directory's entries are sorted with
  `yew_sort_stable` before descending.** `readdir` order is filesystem
  order; it differs between ext4 and xfs and between two identical
  checkouts, and would leak into tie-breaking and every pty golden.
  Cgfried's `find | sort` lesson, one layer down.
- **Symlink-loop guard**: an open-addressed `(st_dev, st_ino)` set of
  every directory *entered*; one already in the set is skipped and counted
  in `n_loops`. Maintained **unconditionally**, not only under
  `follow_symlinks` — bind mounts and some network filesystems produce
  cycles with no symlink in sight, and a guard that is optional by
  construction is off exactly when it matters. 16 B per directory.
- `.git` is skipped unconditionally, before any ignore matching.
- Budget: `yew_walk_step(budget_us)` returns after roughly that much work,
  so a UI-initiated walk runs in 2 ms slices on the idle timer and **never
  blocks a keystroke**. `max_entries` exhaustion sets `truncated` and
  emits one message (`showing the first 200000 files; narrow the
  workspace`) — never a silent short list.
- Paths are stored workspace-relative: that is what the user reads, what
  scoring sees, and what keeps 100 000 strings small. One arena, one
  `Vec(char*)`, no per-path malloc.

### 4. The `.gitignore` matcher — `src/ws/gitignore.c`, honestly scoped

```c
typedef struct GiSet GiSet;                  /* compiled, arena-owned    */
GiSet *yew_gi_load (Arena *a, const char *dir, const GiSet *parent);
bool   yew_gi_match(const GiSet *g, const char *rel, bool is_dir);
bool   yew_gi_prunable(const GiSet *g, const char *rel);   /* §4.1       */
```

| Syntax | Supported | Note |
|---|---|---|
| blank lines, `#` comments | **yes** | leading `\#` escapes a literal `#` |
| trailing whitespace stripped unless `\ ` | **yes** | |
| leading `/` — anchored to the ignore file's directory | **yes** | |
| trailing `/` — directory only | **yes** | |
| `*` — any run not crossing `/` | **yes** | |
| `?` — one byte not `/` | **yes** | |
| `[abc]`, `[a-z]`, `[!a-z]`, `[^a-z]` | **yes** | byte ranges |
| `[[:alpha:]]` POSIX classes | **no** | vanishingly rare in real ignore files; a literal `[[:alpha:]]` is treated as a bracket set of those bytes, which is what git does *not* do — divergence documented |
| `**/` prefix, `/**` suffix, `a/**/b` | **yes** | |
| `!` negation, last matching rule wins | **yes** | with git's own rule below |
| nested `.gitignore` in subdirectories | **yes** | a stack of `GiSet`s, deepest matched last |
| `.git/info/exclude` | **yes** | loaded once at the root |
| `core.excludesFile` (global ignores) | **no** | requires a `git config` subprocess per walk |
| `$GIT_DIR/info/sparse-checkout`, skip-worktree, assume-unchanged | **no** | index features |
| **index awareness** (a tracked file is never ignored) | **no** | we have no index reader; a tracked-but-ignored file appears in the finder. The most user-visible divergence — documented in the manual |
| `core.ignorecase` on case-insensitive filesystems | **no** | matching is always case-sensitive |

**Why bespoke and why a subset.** No dependency (invariant 7) and no
`git` subprocess: the finder must work in a directory that is not a
repository at all, and shelling out per walk would put process-spawn
latency inside the file list. POSIX `fnmatch(3)` is rejected separately —
no `**`, `FNM_PATHNAME` semantics that differ across libcs (a determinism
hazard), and negation/anchoring have to be built around it anyway. The
unsupported rows are exactly the ones needing a subprocess or an index
parser; everything a checked-in `.gitignore` actually contains is
supported. Patterns compile once at load into a small segment program
(literal runs, wildcards, bracket sets); matching never re-parses, which
is what makes 200 000 `yew_gi_match` calls cheap.

#### 4.1 Dir-prefix pruning

git's own rule — *"it is not possible to re-include a file if a parent
directory of that file is excluded"* — is what makes pruning legal. When a
directory matches an ignore rule and no in-scope negation could re-include
anything beneath it, the subtree is skipped without an `opendir`:
`node_modules/` costs one `fstatat` instead of 90 000 entries, which is
the difference between a usable and an unusable finder on a JavaScript
checkout.

`yew_gi_prunable` is deliberately **conservative**: a negation is "in
scope" if any active pattern begins with `!` and its literal prefix (up to
its first wildcard) lies inside the excluded directory. When in doubt,
descend — a slow correct walk beats a fast one that hides a file the user
can see in `git status`.

### 5. The one list picker — `src/ui/picker.h`, `src/ui/picker.c`

```c
typedef struct PickItem {
    const char *label;    /* what is ranked and drawn                    */
    const char *detail;   /* dim right column (dirname, timestamp, …)    */
    i32         payload;  /* IDENTITY, never a row index (s23 law)       */
    u8          flags;    /* YEW_PICK_MODIFIED | _DEFERRED | _ORPHAN     */
} PickItem;

typedef struct PickerSpec {
    const char *title;
    const PickItem *(*items)(void *ctx, u32 *n);    /* candidate source  */
    void (*preview)(Ed *ed, void *ctx, i32 payload, Rect r); /* or NULL  */
    bool (*accept )(Ed *ed, void *ctx, i32 payload, u8 how);
    bool  path_mode;      /* §2's two-pass basename rule                 */
    void *ctx;
} PickerSpec;

void yew_picker_open(Ed *ed, const PickerSpec *s);
bool yew_picker_key (Ed *ed, const Key *k);      /* false = not ours     */
void yew_picker_draw(Ed *ed, Rect area);
void yew_picker_close(Ed *ed, bool accepted);
```

Chrome — one design, and every future list is an instance of it:

```
┌ Find file ─────────────────────────────────────────────┐
│ > ui/ta▏                                               │   filter line
├────────────────────────────────────────────────────────┤
│ src/ui/tabs.c            src/ui                    *   │   ranked list
│ src/ui/tabs.h            src/ui                        │
│ src/ui/statusline.c      src/ui                        │
├────────────────────────────────────────────────────────┤
│ 3/1043   ↑↓ move · enter open · ^v split · esc cancel  │   counts footer
└────────────────────────────────────────────────────────┘
```

- Geometry: centered, `min(80, cols-4)` wide, `min(20, rows-4)` tall;
  under 24 columns it refuses to open with a message rather than drawing a
  6-cell box.
- **The filter line is an s18 `CmdLine`** of kind `YEW_PROMPT_INPUT`; its
  motions and edits are the same `ed.move.*` / `ed.ins.*` / `ed.del.*`
  commands L and I modes use. A second mini-editor here is the exact
  pitfall s18 forbids.
- Matched bytes from `FzRanked.m.pos` render in the accent style; labels
  clip via `yew_str_clip` (never `strlen`); the `detail` column is
  right-aligned in the remaining cells and dropped under 40 columns.
- The counts footer shows `shown/total`, plus ` scanning…` during an
  incremental rescan (§7); a truncated walk appends `+`.
- Regions: `YEW_REGION_BLOCK` over the box, plus a new
  `YEW_REGION_PICK_ROW` (added to `region.h`'s enum; payload = the item's
  `payload`, not the row index) per drawn row, from the same `Rect` the
  row was drawn with. Sprint 27 routes clicks and wheel here.
- Keys (invariant 9 — everything Sprint 27's mouse will do has a key
  today): `↑`/`↓` or `C-p`/`C-n` move, `PgUp`/`PgDn` page, `Home`/`End`
  jump, `Enter` accept (`how = 0`), `C-v`/`C-s` accept in a vertical or
  horizontal split (`how = 1/2`; the picker calls `yew_pane_split` then
  `accept`), `Esc` cancel.
- **Selection is held by payload across refilters, never by row index.**
  One more character reorders the list, and a row-index selection then
  slides onto a different file a fraction of a second before Enter. If the
  held payload leaves the filtered set, selection falls to row 0. This is
  the most common bug in hand-rolled pickers and it is silent — the user
  opens the wrong file and blames themselves.
- The picker is a keymap layer (s13), pushed on open and popped on close,
  so every other key is swallowed rather than leaking into the document.

### 6. The three instances

| Command | Items | `path_mode` | Payload | Accept |
|---|---|---|---|---|
| `ed.find.file` | walk result (§3) | yes | index into `FileList.paths` | `yew_tab_open` |
| `ed.find.buffer` | open tabs | yes | **`tab_id`** (s23 law) | `yew_tab_switch(yew_tab_index_of_id(id))` |
| `ed.undo.branches` | `yew_undo_list` | no | undo node id | `yew_undo_to` |

- **File finder.** `detail` = the dirname. Preview = the first 40 lines of
  the file, read lazily through a capped 64 KiB `pread` into a scratch
  buffer — **never through the buffer list**. A preview must not create a
  `Buffer`, allocate a `TextBuf`, or make a deferred tab look resident
  (s24 pitfall 1, restated: an allocation is what residency is asked of).
  Binary files (s08's NUL heuristic) preview as
  `binary file, 41232 bytes`.
- **Buffer / tab switcher.** `label` = basename, `detail` = dirname,
  flags carry `YEW_PICK_MODIFIED` (from `yew_tab_modified`, derived from
  undo — never a stored flag), `_DEFERRED`, `_ORPHAN` (s25). Deferred tabs
  are listed and marked; accepting one hydrates it. Group members show
  `yew_group_label`'s prefix in `detail`.
- **Undo branch picker** — closes Sprint 10's deferral. Rows come from
  `yew_undo_describe(ut, id, now, buf, n)` with `now` passed in, so the
  rendered text is deterministic and the determinism lane can pin it.
  Tree structure is shown by indenting `depth`; `on_current_path` rows are
  bright, others dim, `is_saved` carries `*`, `is_trimmed` renders
  `(trimmed)`. Preview = the node's `+ins −del` summary. `yew_undo_dump`
  stays as the headless/`--batch` surface.

### 7. Perf: incremental filtering inside the keystroke budget

```c
typedef struct FilterState {
    Vec(u32)  cand;        /* item indices currently matching            */
    Vec(i32)  score;       /* parallel to cand                           */
    char      pat[256]; u32 plen;
    u32       src_gen;     /* item-source generation                     */
    u32       scan_at;     /* resume cursor for a sliced full rescan     */
} FilterState;
```

The numbers this is designed against: 100 000 candidates, mean path 40
bytes. `yew_fz_score` is one allocation-free pass over the text — budget
~200 ns per candidate, so a **full** rescan is ~20 ms, four times over the
5 ms keypress→paint gate. Hence:

1. **Narrowing on append.** If the new pattern has the old as a strict
   prefix, rescore only the current `cand` set. A subsequence match can
   only shrink under an appended character, so this is exact rather than
   an approximation. After two characters the set is typically under
   2 000 candidates → under 0.5 ms, and every later keystroke completes
   inside the frame.
2. **Sliced full rescan** for backspace or a mid-line edit (the rare
   key): 2 ms slices on the idle timer, `scan_at` resuming where the last
   stopped, the list drawn from what is scored so far with ` scanning…`
   in the footer. A partial list for one frame is invisible; a stalled
   keystroke is not (s21's overlay doctrine).
3. **Cached candidate set.** The `FileList` is built once per picker open
   (itself sliced, §3) and reused for its lifetime; a background refresh
   runs at most once per 30 s and bumps `src_gen`, forcing a full rescan
   at the next keystroke. The walk is never redone per keystroke.

Pitfalls: no `strlen` in the inner loop (lengths are stored beside the
paths); no per-candidate allocation (both vectors reserved to `n` at
open); and **do not sort the candidate set** — at most 20 rows are shown,
so bounded insertion into a 20-slot top-k array beats a 100 000-element
sort by two orders of magnitude and only the visible window needs order.
The full `yew_fz_rank` stays for tests and for callers with small `n`.

### 8. Type-to-jump — `src/ui/typejump.c`

For lists that cannot carry a filter line: s24's group picker listing, and
(Sprint 52) the F-mode tree, which is where fuss's behavior comes from.

```c
#define YEW_TYPEJUMP_RESET_MS 500

typedef struct TypeJump { char pat[64]; u32 len; i64 deadline_ms; } TypeJump;

bool yew_typejump_key(TypeJump *tj, const Key *k, i64 now_ms,
                      const PickItem *items, u32 n, u32 *sel);
```

- A printable key **inside** the window appends to `pat`; the same key
  after the window has expired **replaces** it. The deadline is a timer
  heap entry, not a lazily-checked timestamp, so the status hint showing
  the accumulated pattern clears on its own with no further keys — the
  same discipline as s24's 500 ms digit-extension window.
- Jump target = `yew_fz_rank`'s best. **If the currently selected item
  scores ≥ 10000 (exact), stay put** — fuss's rule verbatim: an exact
  match under the cursor must never be moved off.
- Non-printable keys clear the state *first*, then dispatch normally. A
  letter typed much later must not read as a continuation.
- `yew_typejump_key` returns false when the key was not consumed, so a
  host list keeps its own bindings.

### 9. Later consumers

- Later owning sprints delivered all four picker surfaces: LSP symbol search
  in Sprint 47, plugin/package discovery in Sprints 54–55, F-mode fuzzy jump
  in Sprint 52, and the **command palette** over `yew_cmd_at()` in Sprint 38.
  Filtered lists use `PickerSpec`; the visible F-mode tree uses §8's
  type-to-jump engine. `ed.find.symbol` and `ed.find.command` are live registry
  commands rather than staging errors.
- Content search (grep-style, matching inside files) → not a picker
  feature; 1.0's answer is `:!rg` through Sprint 19's job buffer, as
  Sprint 21 already documented.
- Frecency / usage-weighted ranking → post-1.0, and named as a
  non-goal-for-now rather than reserved: it requires persisted usage
  counters, which make ranking depend on history and therefore
  unreproducible in the pty goldens. No field is added speculatively.
- Multi-select in the picker (the s24 group picker's tick model) → stays
  in the group picker; the list picker is single-select by design.

## Testing Strategy

- **Unit** (`tests/unit/test_fuzzy.c`): every §1 table row driven
  positive and negative; a **fuss-parity corpus** of ≥ 30
  (pattern, text, expected score) triples computed from the Fortran
  original and checked in, so a refactor cannot drift the numbers; the
  `YEW_FZ_NO_MATCH` divergence (a long path whose arithmetic reaches 0 is
  still a match); position capture and the 64-position cap; ASCII-fold
  behavior on a Cyrillic and a CJK filename; empty pattern; pattern
  longer than text.
- **Unit** (`tests/unit/test_rank.c`): the `src` → `src/` case explicitly;
  basename-tier ordering (every prefix-basename match above every fuzzy
  path match); tie-break determinism across two input permutations;
  `path_mode == false` ignoring basenames; matched-count equals the
  footer's number.
- **Unit** (`tests/unit/test_walk.c`): determinism — two walks of the same
  fixture tree produce identical lists (and a fixture whose `readdir`
  order is deliberately scrambled by creation order); the symlink-loop
  fixture (`a/b -> ../a`) terminating with `n_loops == 1`; `max_entries`
  truncation flagged; `max_depth`; hidden-file option; budget slicing
  (a walk resumed across 50 `yew_walk_step` calls equals a single-call
  walk, byte for byte).
- **Unit** (`tests/unit/test_gitignore.c`): one assertion per supported
  row of §4's table plus a negative for each unsupported row (asserting
  the *documented* behavior, not an aspiration); nested `.gitignore`
  precedence; negation ordering; `yew_gi_prunable` conservative case (a
  `!node_modules/keep.js` rule prevents pruning) and its aggressive case.
- **Unit** (`tests/unit/test_picker.c`): selection-by-payload across a
  refilter (the §5 bug, driven by a fixture where the row index would
  land on a different item); narrowing-equivalence property — for 10 000
  random (pattern, appended-char) pairs over a 5 000-item set, the
  narrowed result equals a full rescan exactly; the counts footer;
  refusal under 24 columns; layer push/pop leaving no stray keymap layer.
- **Pty** (`tests/pty/finder_*.golden`): full chrome at 80×24 and 120×40;
  match highlighting on a CJK path (highlight cells align with glyphs);
  the buffer switcher showing `*` for modified and the deferred marker;
  the undo branch picker over a 3-branch tree with a fixed injected
  `now`; ` scanning…` footer during a sliced rescan.
- **Fuzz** (`tests/fuzz/fuzz_fuzzy.c`): random patterns × random texts
  including invalid UTF-8, NULs, 4 KiB paths, 100 000 cases × 4 seeds —
  never reads out of bounds, never returns a score colliding with the
  sentinel, always deterministic for the same inputs. Plus a
  `.gitignore` pattern fuzzer (random pattern bytes never crash the
  compiler or matcher).
- **Perf** (`tests/perf/finder.c`), on a generated 100 000-file tree:
  full walk within the sliced budget with no slice over 3 ms; **first
  keystroke performs ≤ 5 ms of work in-frame** (sliced remainder on the
  idle timer); each subsequent narrowing keystroke completes fully in
  ≤ 5 ms; picker open → first paint ≤ 16 ms. Baselines committed under
  `tests/perf/baselines/`.

## Definition of Done

1. `make test` green on gcc and clang, zero warnings at `-std=c11
   -pedantic -Wall -Wextra -Werror`; ASan/UBSan/valgrind/determinism
   lanes green.
2. Every row of the §1 scoring table has a passing assertion, and the
   ≥ 30-triple fuss-parity corpus matches exactly.
3. `src` ranks the directory `src/` above `src/file.c` (explicit test),
   and no fuzzy path match ever outranks a prefix basename match.
4. Walk determinism: two walks of the scrambled-creation-order fixture
   produce byte-identical path lists; the symlink-loop fixture terminates
   with `n_loops == 1`; `grep -rn 'nftw\|ftw(\|scandir' src/ws/` → empty.
5. Every supported `.gitignore` row has an assertion; every unsupported
   row has an assertion pinning the documented divergence; a
   `node_modules/` fixture of 5 000 files is pruned with **one**
   `fstatat` and zero `opendir` calls (counted via a test hook).
6. **Perf gate**: on 100 000 files, the first filter keystroke does ≤ 5 ms
   of in-frame work and every narrowing keystroke completes in ≤ 5 ms;
   baselines committed.
7. Narrowing equivalence: 10 000 random append cases produce results
   identical to a full rescan (the property that makes the optimization
   legal).
8. Selection survives refiltering by payload — the pty test types a
   character that reorders the list and presses Enter, landing on the
   item that was highlighted.
9. The filter line is the s18 widget: `grep -n 'char .*\[\(64\|128\|256\)\]'
   src/ui/picker.c` shows no ad-hoc input buffer, and prompt edits
   dispatch through the registry.
10. Three picker instances live and keyboard-complete; Sprint 10's undo
    branch picker deferral is closed (its rows come from `yew_undo_list`
    / `yew_undo_describe` with an injected `now`).
11. Previewing a file creates no `Buffer` and no `TextBuf` (asserted via
    the s24 file-read test hook: previewing 20 files performs 20 `pread`s
    and 0 buffer allocations).
12. Type-to-jump: appends inside 500 ms, replaces after, exact match under
    the cursor does not move, deadline clears the hint with no keys
    (mocked clock).
13. The later symbol, plugin/package, F-mode, and command discovery surfaces
    are live through the shared picker or type-to-jump machinery delivered
    here; no second list event loop or ad-hoc filter editor exists.
14. Two consecutive full pty runs byte-identical (invariant 5); ≥ 150
    assertions across the five unit files.
