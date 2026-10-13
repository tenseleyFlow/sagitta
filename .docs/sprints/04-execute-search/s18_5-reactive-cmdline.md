# Sprint 18.5: The Reactive Command Line

## Prerequisites

- Sprint 18 — `CmdLine`, `CompMenu`, `CompItem`, `CompSource`, `CmdHist`,
  `yew_cmd_parse`, `yew_cmd_parse_point`, `yew_comp_query`,
  `yew_comp_enumerate`, `yew_comp_lcp`, `yew_comp_quote`; the law *"there
  is exactly one text editor in this program"*; the per-command `argspec` /
  `range_policy` / `abbrev` metadata on `CmdEntry`. **This sprint
  supersedes s18 §4's cycling table** (§6 below) and **closes its
  `yew_comp_score` seam** (§2). Everything else s18 pinned still holds.
- Sprint 13 — the `"ed.*"` registry (`yew_cmd_register_entry`,
  `yew_cmd_count`, `yew_cmd_at`, `yew_cmd_entry`), keymap layers, the
  `YEW_MODE_E` layer, `YEW_CMD_INTERNAL` / `YEW_CMD_DEFERRED`.
- Sprint 15 — the message line, `ed->footer_dirty`, and the timer heap
  (`yew_timer_add`, `yew_timer_cancel`, `YEW_TIMER_NONE`).
- Sprint 21 — the search-as-you-type precedent this sprint generalizes:
  `yew_cmdline_edited()` already fires `yew_search_input()` on every prompt
  edit, and its doctrine — *a half-typed pattern is the normal state of a
  prompt, not an error; keep the last good result and never blank the
  screen mid-word* — is binding on §4 and §9 here.
- Sprint 22 — `Rect`, the clickable-region registry
  (`yew_region_frame_begin` / `yew_region_add` / `yew_region_hit`,
  last-added-wins, cleared at frame begin), and the layout/draw split law:
  a renderer registers its region in the same statement block that draws
  it, from the same `Rect`.
- Sprint 2 — `yew_str_width`, `yew_str_clip`, `yew_cluster_width`,
  `yew_off_to_ccol`, `yew_ccol_to_off`. All width math stays in
  `src/unicode/`; s18 DoD 2's grep extends to every file this sprint adds.
- Sprint 26, **partially pulled forward**: its §1 (`yew_fz_score`) and §2
  (`yew_fz_rank`, the basename tier) land here, at the paths s26 names, so
  s26 inherits them as landed APIs. Nothing else of s26 moves — the
  workspace walk, the `.gitignore` matcher, the list picker, its three
  instances, and type-to-jump all stay in Sprint 26.
- Binding: `00-decisions.md` — invariant 3 (no silent stubs), 4
  (keypress→paint p99 ≤ 5 ms is a gate), 5 (deterministic rendering), 9
  (modal first: the mouse is an accelerator, never a requirement), 7
  (bespoke first). `01-architecture.md` — `src/ui/`, `src/ws/finder.c
  (fuzzy)`.

Assumptions beyond the earlier index entries, per cross-sprint discipline:

1. **Mouse events are decoded but not routed.** Sprint 4 lands SGR mouse
   decoding into `Key` (`YEW_EV_MOUSE`, `button`, `row`, `col`), and
   Sprint 22 landed the region registry plus `yew_pane_cmd_click`, but
   `loop_dispatch_event` drops `YEW_EV_MOUSE` at its `default:` and no
   renderer calls `yew_region_add`. §8 lands the **narrow** routing path
   (loop → `yew_ed_handle_mouse` → region hit → command) and registers the
   menu's own rows. Sprint 27 *widens* that path to drag-resize, tab
   reorder, and document scroll; it does not invent it.
2. `src/ws/` does not exist yet. §1 creates it, per
   `01-architecture.md`'s rule that a directory is created by the sprint
   that first puts a file in it.

## Goals

Turn the `:` prompt from a pull-model completer into a reactive one: the
menu filters live on every keystroke instead of waiting for Tab, ranks
through the fuss-derived scorer pulled forward from Sprint 26 (which also
yields the matched byte positions that make match highlighting possible),
previews the highlighted candidate as inline ghost text instead of
rewriting the line under the user, and tells the user what the parser
already understands — the resolved command and the argument it is waiting
for — before Enter rather than as an error after it. Underneath, three
structural changes make it programmatic rather than a pile of special
cases: completion sources become a registration API instead of a static
array, the menu becomes a spec-driven widget that Sprint 26's list picker
will be an instance of rather than a second implementation of, and every
menu action becomes a named `ed.cmdline.menu.*` command so it is
rebindable, recordable, and reachable from Fletch. Deferred: the
full-screen command palette and matching against command *help* text
(Sprint 38); option and value completion sources stay empty providers
until the options model (Sprint 36); the workspace walk, `.gitignore`
matcher, and list picker (Sprint 26); mouse drag, reorder, and
document-area scroll (Sprint 27).

## Deliverables

### 1. The fuzzy scorer, pulled forward — `src/ws/finder.h`, `src/ws/fuzzy.c`

Sprint 26 §1 and §2, verbatim and complete, landed at the paths Sprint 26
names. Reproduced here because this file must be sufficient on its own;
when the two disagree, s26 §1/§2 is the authority and this is the copy.

```c
#define YEW_FZ_NO_MATCH  INT32_MIN
#define YEW_FZ_MAX_POS   64
#define YEW_FZ_BASENAME_TIER 100000

typedef struct { u16 n_pos; u16 pos[YEW_FZ_MAX_POS]; } FzMatch;  /* bytes */

i32 yew_fz_score(const char *pat, u32 plen,
                 const char *text, u32 tlen, FzMatch *m);

typedef struct { u32 idx; i32 score; FzMatch m; } FzRanked;

u32 yew_fz_rank(const char *pat, u32 plen, const char *const *text,
                u32 n, bool path_mode, FzRanked *out);
```

The scoring table is the contract; every row is a unit case:

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

Ranking folds s26's two-pass basename rule into one total order:

```
sb  = yew_fz_score(pat, basename(text))
sp  = yew_fz_score(pat, text)                    /* path_mode only      */
key = (sb >= 5000) ? sb + YEW_FZ_BASENAME_TIER   /* exact/prefix tier   */
                   : max(sb, sp)
```

so every exact-or-prefix basename match outranks every fuzzy path match,
globally — which is why `src` selects the directory `src/` and not
`src/file.c`. `path_mode == false` (command names, buffer labels) scores
the label only and never applies `basename()`. Non-matching candidates are
**excluded, not sorted last**, so a count means "matches" and not "rows".
Ties break by shorter text, then `memcmp`, through `yew_sort_stable` —
never `qsort`, whose tie order differs between glibc and musl and would
make the pty goldens libc-dependent (invariant 5). `basename()` is an
inline scan for the last `/`, not POSIX `basename(3)`.

Two rules the table leaves implicit, pinned here because the port has to
choose: an **exact or prefix match fills its match positions** (those two
rows return before the scan, and a prefix match is the commonest row in
the menu — leaving it unhighlighted would make §5 pointless), and an
**empty pattern preserves source order** rather than falling through to
the byte tie-break, which is what keeps a bare `:` menu grouped by
command domain. The path source is the one exception: it breaks even
empty-pattern ties by name, because `readdir` order is filesystem order
and would otherwise leak into every golden.

Three divergences from the Fortran original, all deliberate, all s26's:
`YEW_FZ_NO_MATCH` is `INT32_MIN` rather than `0` (fuss cannot distinguish
"no match" from "a long path whose penalties cancelled its bonuses", and
the real match vanishes); matched byte positions are captured because a
filtering list must highlight them, capped at 64 with the tail
unhighlighted and never rescored; case folding is ASCII-only, so a
non-ASCII filename is findable case-sensitively and matches itself
exactly — pinned and documented rather than discovered.

**Pitfall — this is a greedy first-match scan, not an optimal alignment.**
`abc` against `axbxc_abc` scores the early, worse occurrence. That is
fuss's behavior, it is O(n) rather than O(n·m), and §2's basename tier
covers the case users actually complain about. Do not "improve" it into a
dynamic program without re-pinning the table, the parity corpus, and every
golden that depends on the ordering.

**Pitfall — this file does no width math and no Unicode work.** It is a
byte scan with ASCII folding. `grep -n 'utf8_\|grapheme\|width\|wcwidth'
src/ws/fuzzy.c` must be empty (DoD 3): the moment scoring starts consulting
width tables it is doing display work in a ranking function, and the two
diverge under emoji.

### 2. The `yew_comp_score` seam closes — `src/ui/cmdcomp.c`

`yew_comp_score` is **deleted**, not deprecated; s18 §4 named it as the
one-function seam and this is the swap it described. Every call site moves
to `yew_fz_score` / `yew_fz_rank`.

Three consequences that must be handled rather than discovered:

- **The sentinel changes.** `yew_comp_score` returned `-1` for "not a
  subsequence"; `yew_fz_score` returns `INT32_MIN`. Every `score < 0` test
  becomes `score == YEW_FZ_NO_MATCH` — a genuine match may now score
  negative (the length penalty), and a `< 0` test would silently drop
  long-but-real candidates. This is the single most likely regression in
  the sprint.
- **The ordering changes.** s18 sorted prefix matches first (alphabetically
  by name) and everything else by score. §1's total order replaces both
  keys. `candidate_cmp` and `path_candidate_cmp` collapse into one
  comparator over `FzRanked`.
- **`CompItem` grows two fields**, and both are data the renderer needs
  rather than state it derives:

```c
typedef struct {
    const char *text;      /* replacement text, arena-owned              */
    const char *detail;    /* right-column hint, may be NULL             */
    u8    kind;
    bool  is_dir;          /* suppresses the trailing space              */
    bool  deferred;        /* YEW_CMD_DEFERRED: drawn dim, marked, and
                            * ranked last within its score band          */
    i32   score;
    FzMatch m;             /* matched byte positions, for highlighting   */
} CompItem;
```

`deferred` closes a real gap: today `:` offers `file.open`, `tab.new`, and
`group.next` in the menu and hard-errors on Enter naming their sprint.
Offering a command and then refusing it is a worse answer than either
hiding it or marking it, and invariant 3 says the refusal must name its
sprint — so the menu keeps the row, draws it dim with a `⋯` marker, and
puts the sprint in the `detail` column. The user learns the command exists
and when it arrives, in the place they asked.

The path enumerator's bounded top-`YEW_COMP_MAX` heap stays exactly as it
is; only its comparator changes. It is the reason a 10 000-entry directory
allocates 500 filenames instead of 10 000, and it becomes load-bearing in
§4 where that loop runs per keystroke rather than per Tab.

**Golden churn is expected and is not a regression.** Re-ranking changes
row order wherever a golden shows more than one row. Five move —
`cmdline_completion_menu`, `_next`, `_next_again`, `_prev_wraps`,
`_menu_enter_not_execute` — and four do not, because they show zero or
one row (`_zero`, `_one`, `_printable_closes`, `_escape_restores`). A
golden update that was not read is a test that no longer tests anything.

What changes in them: with a short stem every prefix match ties at one
score, so the shorter-then-memcmp tie-break decides and `file.new` sorts
ahead of `file.open` where the old comparator sorted alphabetically.

### 3. Completion sources become a registry — `src/ui/cmdcomp.h`

The static `sources[]` array becomes a registration API, so a subsystem —
and, from Sprint 34/36, a Fletch script — adds a source without editing
`cmdcomp.c`.

```c
typedef struct CompReq {
    YewCompKind kind;
    const char *stem;      /* decoded token text at the cursor           */
    Ed         *ed;
    Arena      *arena;     /* where the returned strings are allocated   */
    i64         budget_us; /* 0 = unbounded (Tab); >0 = live, may slice  */
} CompReq;

enum {
    YEW_COMP_SRC_CACHEABLE = 1U << 0, /* result reusable while `head`
                                       * is unchanged (§4)              */
    YEW_COMP_SRC_PATHMODE  = 1U << 1, /* rank with the basename tier     */
    YEW_COMP_SRC_SLOW      = 1U << 2  /* may exceed budget; §4 slices it */
};

typedef struct CompSource {
    YewCompKind kind;
    const char *name;      /* stable id, for logs and Fletch             */
    u32 (*enumerate)(const CompReq *req, Vec_CompItem *out);
    u32 flags;
} CompSource;

/* Registration is idempotent by kind: re-registering a kind REPLACES it,
 * which is how a plugin overrides the built-in path source without a
 * removal API and without leaving two sources fighting over one kind. */
void yew_comp_source_register(const CompSource *src);
const CompSource *yew_comp_source(YewCompKind kind);
u32  yew_comp_source_count(void);
```

The five built-ins register from one `yew_comp_init()` called out of
`yew_cmd_init()`'s neighbourhood, keeping the current behavior: commands,
paths, buffers, and the two empty providers for options and values. **An
empty provider is data, not a stub** (s18's phrasing, still binding) — it
returns zero items and Sprint 36 replaces the function, not the plumbing.

`enumerate` returning `u32` keeps s18's contract: the return is the
*total* number of matches before the `YEW_COMP_MAX` cap, which is what the
footer's `500+ of 1200` reports.

**Pitfall — `CompReq.budget_us` is advisory to the source and mandatory to
the caller.** A source that ignores it is not a correctness bug; §4 caps
the damage by slicing. A caller that assumes a budgeted enumerate returned
*everything* is a correctness bug, and it looks like a menu that silently
lost rows.

**Pitfall — the arena is in the request, not chosen by the source.** The
built-in sources originally picked between `ed->cmdline.comp_arena` and
`ed->arena` by testing `ed->cmdline.active`, while §4's filter resets the
arena *it* was handed. Those are two different arenas the moment a prompt
is open, and the disagreement is a use-after-free on the cached set, not
a leak. `yew_comp_path_head_len` is exported for the same class of
reason: §4 keys its cache on the directory split and the path source
performs it, and two split rules would let the cache answer for one
directory while the source read another.

### 4. Live filtering inside the keystroke budget — `src/ui/cmdcomp.c`

The menu refilters on every prompt edit. The hook already exists and has
exactly one caller: `yew_cmdline_edited()`, which Sprint 21 uses for
search-as-you-type. `:` joins it; no second edit path is created.

```c
typedef struct CompFilter {
    Vec_CompItem base;     /* the enumerated set for `key` below         */
    YewCompKind  key_kind;
    char        *key_head; /* PATH: the directory this set came from     */
    char        *pat;      /* the pattern `base` was last narrowed to    */
    u32          total;    /* pre-cap total, for the footer              */
    bool         scanning; /* a sliced enumerate is still running        */
    TimerId      slice;    /* YEW_TIMER_NONE when idle                   */
} CompFilter;
```

When a keystroke arrives, exactly one of four things happens:

| Condition | Action | Cost |
|---|---|---|
| same kind and `head`, set **not capped**, new pattern extends the pattern the set was **enumerated** with | re-rank the surviving items only | O(base), ≤ `YEW_COMP_MAX` |
| the new pattern stops extending it (backspace past it, mid-token edit) | re-enumerate, then rank | one `opendir` |
| the cached set was **capped** | re-enumerate, then rank | one `opendir` |
| `head` or kind changed | re-enumerate, then rank | one `opendir` |

Narrowing on append is exact rather than approximate: a subsequence match
can only shrink when a character is appended, so no candidate that would
have matched is lost. That is the property DoD 7 tests, and it is what
makes the optimization legal rather than merely fast.

Two corrections to the obvious version of that rule, both found by
building it:

- **The comparison is against the pattern the set was ENUMERATED with,
  not the previous keystroke.** The cached set is a superset of every
  extension of that pattern, so retreating inside it — backspacing from
  `alph` to `a` after enumerating at `a` — is free, and only leaving it
  costs an `opendir`. Keying on the last keystroke would re-enumerate on
  every backspace for no reason.
- **A capped set may not be narrowed at all.** It holds the
  `YEW_COMP_MAX` best matches for the *old* pattern, and an entry the cap
  cut can still be among the best for the new one. Narrowing over it
  would silently drop rows — the precise failure the `budget_us` note
  warns about — so the filter re-enumerates and pays the `opendir`. In
  practice the cap stops biting after one or two characters, which is
  where the win is anyway.

Numbers this is designed against: the enumerated set is capped at
`YEW_COMP_MAX` (500) by the existing top-k heap, and `yew_fz_score` is one
allocation-free pass over ~40 bytes — roughly 200 ns. Re-ranking 500
candidates is ~100 µs, forty times inside the 5 ms gate. **The cost that
matters is the `opendir`, not the ranking**, which is why the cache is
keyed on `head` and not on the whole stem: typing `src/ui/cmd` walks the
directory once and then only re-ranks, and the perf gate in Testing
measures precisely that.

**Amendment (implementation).** The two rules above collide in exactly the
case DoD 10 measures. A 10 000-entry directory caps immediately, and *a
capped set may not be narrowed*, so the filter re-enumerated on **every**
keystroke — nine `opendir`s over the DoD's twelve characters, not one.
The ranked cache cannot fix this by itself: it is keyed on the pattern,
and no pattern-keyed set that dropped rows can answer a longer pattern.

So the path source gained a second cache underneath the filter's, keyed
on the **directory** and holding every name in it (`YEW_COMP_LIST_MAX`,
50 000; past that nothing is cached and the scan streams as before).
Dot files are held and filtered at rank time, because whether they are
wanted depends on the pattern. `CompReq.allow_cache` separates the two
callers: the live filter may reuse, while a direct `yew_comp_enumerate`
is a fresh read *and* retires the listing — without that, a caller that
touched the filesystem gets a memoised answer.

Two costs the first cut added back, both caught by the gate and both
worth remembering because they outweighed the syscall they replaced:

- 10 000 individual `strdup`s cost more than the `readdir`. Names live in
  one blob addressed by offset.
- `PathCandidate` carried an `FzMatch`. Positions are 130 bytes and the
  top-k heap sifts by copying whole structs, so one re-rank moved ~40 MB
  through `path_candidate_swap`. Only the ≤ 500 survivors need positions;
  they are rescored at finish. The name length is carried for the same
  reason — the comparator ran ~85 000 times per keystroke calling
  `strlen` twice.

Measured after all three: `opendirs=1`, worst keystroke 3.5 ms (the cold
scan), every other path keystroke 0.7–1.9 ms, command-source keystrokes
under 0.1 ms.

- `YEW_COMP_DEBOUNCE_MS` (default 0) delays the first enumerate after a
  `head` change. It exists for the pty harness and for hostile
  filesystems; the default is 0 because a debounce the user can feel is
  worse than the work it avoids.
- Cache invalidation is `head`-equality only. **No `stat` of the
  directory, no mtime check, no inotify.** A directory that changes
  underneath an open prompt shows a stale list until the head changes or
  the prompt closes; that is a one-keystroke-old list, and the alternative
  is a syscall per keystroke to detect a case nobody hits.
- The filter state lives on `CmdLine` and dies with `yew_cmdline_close`,
  alongside `comp_arena`. **Pitfall:** `menu_discard()` currently frees the
  arena the `CompItem.text` pointers live in. Under live filtering,
  discard-and-rebuild happens per keystroke, so the arena must be reset
  (`arena_free_all` on the *filter's own* arena) rather than leaked, and
  `menu.replace` must be recomputed from the fresh parse rather than
  carried over — a stale `replace` span splices the completion into the
  wrong byte range, which is the ugliest failure mode available here.

### 5. The menu becomes a spec-driven widget — `src/ui/menu.h`, `src/ui/menu.c`

Extracted from `cmdline.c` so Sprint 26's list picker is an *instance* of
this widget rather than a second one. What moves is the drawing, the
scrolling, the selection model, and the region registration; what stays in
`cmdline.c` is what is specific to a command line.

```c
typedef struct MenuSpec {
    const char *title;   /* NULL = inline, drawn above the prompt row    */
    u16   max_rows;      /* visible rows; 0 = fit the space              */
    bool  ghost;         /* preview the selection as inline ghost text   */
    bool  wrap;          /* selection wraps at both ends                 */
    u16   detail_col;    /* column the detail text starts at, in cells   */
} MenuSpec;

typedef struct Menu {
    MenuSpec spec;
    Vec_CompItem items;
    i32   sel;           /* -1 = nothing selected; see §6's Enter rule   */
    bool  explicit_sel;  /* the user CHOSE this row (Tab/C-n/click)      */
    u32   top;           /* first visible row — the menu scrolls         */
    Span  replace;       /* byte range in the prompt buffer              */
    u32   total;         /* pre-cap match count, for the footer          */
    char *stem;          /* restored on Esc                              */
    bool  scanning;
} Menu;

void yew_menu_init (Menu *m, const MenuSpec *spec);
void yew_menu_free (Menu *m);
void yew_menu_reset(Menu *m, Vec_CompItem items, u32 total, Span replace);
/* Selection is held by IDENTITY across a refilter, never by row index. */
bool yew_menu_move (Menu *m, i32 delta, bool page);
void yew_menu_draw (Ed *ed, Menu *m, Rect area);
```

- **Selection survives refiltering by identity, never by row index.** One
  more character reorders the list, and a row-index selection then slides
  onto a different command a fraction of a second before Enter.
  `yew_menu_reset` re-finds the previously selected item by its `text`; if
  it is gone from the filtered set, selection falls back to "nothing
  selected" (`sel = -1`, `explicit_sel = false`) rather than to row 0 —
  falling to row 0 would leave an *unexplicit* selection that §6's Enter
  rule would then treat as a choice the user never made. This is the most
  common bug in hand-rolled completers and it is silent: the user runs the
  wrong command and blames themselves.
- The menu **scrolls**. Today it shows the first five items and a
  `500+ of 1200` footer with no way to reach item six. `top` follows `sel`
  with the standard keep-selection-visible rule; the footer becomes
  `sel+1/total` when something is selected.
- Matched bytes from `CompItem.m.pos` draw in the accent style. Positions
  are **byte** offsets into `text`; they must be converted to cells through
  `yew_off_to_ccol`-style arithmetic against the drawn string, never by
  assuming one byte is one cell. A CJK or emoji candidate otherwise
  highlights the wrong columns — and Testing pins exactly that case.
- Rows clip through `yew_str_clip`; the detail column is right-aligned and
  is dropped entirely under 40 columns.
- **Region registration happens in the same block as the row draw**, from
  the same `Rect`, per s22's law. A new `YEW_REGION_MENU_ROW` joins
  `region.h`'s enum, payload = the row's index within `items` (this widget
  has no stable ids to use, and the table is rebuilt every frame, so the
  index is valid for exactly the frame it was registered in — which is the
  only frame that can be clicked). A `YEW_REGION_BLOCK` covers the whole
  menu rectangle so a click on a gap does not fall through to the pane
  beneath.

### 6. The interaction contract — supersedes Sprint 18 §4's table

s18 §4 pinned a Tab-cycling table and made every row a pty golden. A live
menu changes what those keys mean, so the table is restated whole rather
than patched. Rows marked **(s18)** are unchanged.

| State | Key | Behavior |
|---|---|---|
| typing, matches exist | *(any printable)* | menu refilters and stays open; `sel = -1`; ghost shows the top candidate |
| typing, no matches | *(any printable)* | menu closes; **no message** (see below); line unchanged |
| menu open, `sel = -1` | `Tab` | if the LCP is longer than the stem, insert the LCP only and keep `sel = -1`; otherwise select row 0 and set `explicit_sel` |
| menu open, `sel ≥ 0` | `Tab` | select next row, `explicit_sel` stays set |
| menu open | `S-Tab` | select previous row; wraps at both ends **(s18)** |
| menu open | `C-n` / `C-p` | select next / previous — the same as Tab / S-Tab, for hands that already know it |
| menu open | `PgDn` / `PgUp` | move one visible page |
| menu open, `sel ≥ 0` | `Right` / `C-e` at end of line | accept the ghost: replace `menu.replace`, add the trailing space or `/`, close the menu |
| menu open, `explicit_sel` | `Enter` | accept the selected item and close the menu — **does not execute** **(s18)** |
| menu open, `sel = -1` | `Enter` | **execute the line** — see below |
| menu open | `Esc` | close the menu and restore the stem as it was **(s18)** |
| menu closed | `Esc` | close the prompt **(s18)** |
| no matches | `Tab` | message `no completions`; line unchanged **(s18)** |
| unknown command / free-string arg | `Tab` | insert a literal tab **(s18)** |

Two rows carry the whole design and both are worth defending in writing.

**Enter is governed by `explicit_sel`, not by "is the menu open".** s18's
rule — Enter with a menu open accepts instead of executing — exists to stop
`:w /etc/pas` from running when the user meant to pick `passwd`. Under a
live menu the menu is *always* open while a command name is being typed,
so that rule literally applied would mean the prompt can never be executed
with one Enter. The property s18 was protecting is preserved exactly by
keying on whether the user *chose* a row: a selection only becomes
explicit through `Tab`, `S-Tab`, `C-n`, `C-p`, `PgUp`, `PgDn`, or a click.
Filtering alone never selects. So the dangerous case — the user was
looking at a highlighted row and pressed Enter — still accepts, and the
common case — the user typed a complete command and pressed Enter — still
executes.

**A live filter that finds nothing says nothing.** Tab is a question and
deserves the `no completions` answer; a keystroke is not, and answering
every unmatched character with an error message makes the message line
flash through a word being typed. This is Sprint 21's doctrine — a
half-typed pattern is the normal state of a prompt — applied to the menu.

**`↑`/`↓` remain history, and do not move the selection.** This is the one
place where the obvious binding is the wrong one: because a live menu is
open for the whole time a command name is being typed, giving `↑` to the
menu would make history unreachable exactly when it is most wanted (you
reach for history *blind*, at the start of a line, which is precisely when
the menu is fullest). Menu movement lives on `Tab`/`S-Tab` and `C-n`/`C-p`,
both of which are already muscle memory from readline and from every shell
completer. **This is a reversible call:** it is keymap data in
`keys_default.c`, not code, so flipping it is a two-line change plus
goldens.

### 7. Ghost text — `src/ui/cmdline.c`

When a candidate is highlighted, the part of it that is not yet typed
renders **after the caret, dim, as a draw-time overlay**:

```
:file.wr█ite_quit
        ╰── ghost: dim, never in the TextBuf
```

- **The ghost is never inserted into `line->buf`.** It is a `const char *`
  into the filter arena plus a byte count, consumed by `yew_cmdline_draw`.
  Putting it in the buffer would poison the history draft, feed the parser
  text the user never typed, and make `yew_cmdline_text()` — which Sprint
  21's search reads on every keystroke — return a pattern with a
  suggestion glued to it. This is the sprint's sharpest pitfall and DoD 6
  greps for it.
- The ghost is only drawn when the caret is at end of line and the
  candidate has the current stem as a prefix. A fuzzy (non-prefix) match
  has no coherent "rest of it" to show, so it shows nothing and the menu
  row carries the highlighting instead.
- Accepting the ghost is the same code path as accepting a menu item; there
  is one `insert_completion`.
- The ghost is clipped like everything else, and it never scrolls the
  prompt: horizontal scroll follows the caret, and the caret is before the
  ghost. A ghost that pushed the view would move text the user is reading.

### 8. Mouse — narrow routing, wide enough for the menu

```c
/* Consumes the event; false = nobody claimed it. */
bool yew_ed_handle_mouse(Ed *ed, const Key *k);
```

- `loop_dispatch_event` stops dropping `YEW_EV_MOUSE` and calls it.
- The handler queries `yew_region_hit(k->col, k->row)` and dispatches on
  `Region.kind`: `YEW_REGION_MENU_ROW` → select that row (setting
  `explicit_sel`) on press, accept on a second press of the same row;
  `YEW_REGION_BLOCK` → consume and do nothing; wheel over the menu
  rectangle → scroll `top` by three rows without moving the selection;
  anything else → the existing `yew_pane_cmd_click` path, which Sprint 22
  already wrote and which has had no caller until now.
- **Every mouse action here has a key binding today** (invariant 9): row
  selection is `C-n`/`C-p`, acceptance is `Enter`, scrolling is
  `PgUp`/`PgDn`. The mouse is an accelerator; the sprint adds no
  mouse-only capability.
- **Scope line, stated so Sprint 27 is not surprised:** this lands the
  *routing spine* and the menu's regions. Drag-resize, tab reorder,
  selection-by-drag, and document-area scroll stay Sprint 27's, and
  `yew_ed_handle_mouse` is written as a dispatch table it extends.

### 9. Live parse feedback — `src/ui/cmdline.c`

The prompt reports what the parser already understands, on the same
per-edit hook as §4, using the tolerant `yew_cmd_parse_point` that Sprint
18 already built for completion.

```
:w                                  → w → file.write · <file>
:12,40 sort                         → unknown command 'sort' (try Tab)
:1,2d                               → d → edit.line.delete · 2 lines
```

- The hint is drawn on the message row in the **normal** footer style, not
  the error style. It is a hint, and Sprint 21's doctrine forbids styling
  the normal state of a half-typed line as a failure.
- A real `CmdErr` from Enter still wins the row and still draws in the
  error style with the offending token underlined; the hint is what is
  shown when there is no error.
- Argument names come from one table, so `argspec` has exactly one human
  rendering:

| `argspec` | Shown | Source |
|---|---|---|
| `f` | `<file>` | `YEW_COMP_PATH` |
| `b` | `<buffer>` | `YEW_COMP_BUFFER` |
| `o` | `<option>` | `YEW_COMP_OPTION` |
| `v` | `<value>` | `YEW_COMP_VALUE` |
| `s` | `<text>` | none |
| `*` suffix | `…` after the last name | repeats |

- Range validity is reported as a count (`2 lines`, `whole buffer`,
  `selection`) rather than as a bare `1,2` echo: the user typed the
  numbers, so repeating them says nothing; how many lines they resolve to
  is the thing they cannot see.
- **Pitfall — the hint must not run the parser twice per keystroke.** §4's
  filter already calls `yew_cmd_parse_point`; the hint consumes that same
  result. Two parses per edit is not a latency problem at prompt lengths,
  but two *independent* parses drift, and then the hint names one command
  while the menu completes another.

### 10. Menu actions as named commands — `src/edit/cmd.c`, `src/edit/keys_default.c`

Every new behavior is a registered `ed.*` command, so it is rebindable,
recordable, and callable from Fletch (Sprint 34) rather than being a
keystroke handled in a `switch`:

| Command | Bound to | Notes |
|---|---|---|
| `ed.cmdline.menu.next` | `Tab`, `C-n` | LCP-then-select per §6 |
| `ed.cmdline.menu.prev` | `S-Tab`, `C-p` | wraps |
| `ed.cmdline.menu.page_next` | `PgDn` | |
| `ed.cmdline.menu.page_prev` | `PgUp` | |
| `ed.cmdline.menu.accept` | *(none by default)* | mouse and Fletch reach it |
| `ed.cmdline.menu.dismiss` | *(Esc, via existing cancel)* | restores the stem |
| `ed.cmdline.ghost.accept` | `Right` at EOL, `C-e` | |

All carry `YEW_CMD_INTERNAL`, like the existing `ed.cmdline.*` entries, so
they cannot be typed as `:cmdline.menu.next` — they are keymap plumbing,
not user commands, and s18's completion source already filters
`YEW_CMD_INTERNAL` out of the menu.

**Pitfall — `command_name_valid()` will reject these on sight.**
`src/edit/cmd.c` validates every registered name against a hardcoded
allowlist of domains and terminal verbs and a 4-segment cap.
`ed.cmdline.menu.next` is four segments (fine) but `menu`, `page_next`,
`page_prev`, `dismiss`, and `ghost` are not in the `verbs[]`/`domains[]`
tables. They must be added in the same commit, or `yew_cmd_init()`
`YEW_BUG`s at startup and every single test dies at once — which reads as
a catastrophic break rather than as a missing table row.

Existing `ed.cmdline.complete_next` / `complete_prev` stay as the names
the keymap already uses and become thin aliases onto `menu.next` /
`menu.prev`, so no existing binding, golden, or test breaks on a rename.

### 11. Defer

- **The command palette** — a full-screen picker over `yew_cmd_at()`, and
  matching against command **help text** rather than names — stays Sprint
  38, as Sprint 26 §9 already assigned it. This sprint ranks command names
  and declared abbreviations only. `ed.find.command` hard-errors naming
  Sprint 38 (invariant 3).
- **Options and values**: the `YEW_COMP_OPTION` / `YEW_COMP_VALUE` sources
  are registered through §3 and return zero items until Sprint 36 lands the
  options model. An empty provider is data, not a stub.
- **Sprint 26 keeps** the workspace walk, the `.gitignore` matcher, the
  list picker and its three instances, and type-to-jump. Only §1/§2 moved.
- **Sprint 27 keeps** mouse drag-resize, tab reorder, drag-select, and
  document scroll; §8 lands only the routing spine and the menu's regions.
- **Frecency / usage-weighted ranking** is a non-goal for 1.0, for s26's
  reason: it makes ranking depend on history and therefore unreproducible
  in the pty goldens. No field is added speculatively.

## Testing Strategy

- **Unit** (`tests/unit/test_fuzzy.c`, new): every §1 table row driven
  positive and negative; a **fuss-parity corpus** of ≥ 30 (pattern, text,
  expected score) triples checked in, so a refactor cannot drift the
  numbers; the `YEW_FZ_NO_MATCH` divergence (a long path whose arithmetic
  reaches 0 is still a match); position capture and the 64-cap; ASCII-fold
  behavior on a Cyrillic and a CJK name; empty pattern; pattern longer than
  text. Ranking: the `src` → `src/` case explicitly; every prefix-basename
  match above every fuzzy path match; tie determinism across two input
  permutations; `path_mode == false` ignoring basenames.
- **Unit** (`tests/unit/test_cmdcomp.c`, extended): the sentinel change
  (a candidate scoring negative is kept, a `YEW_FZ_NO_MATCH` candidate is
  dropped); **narrowing equivalence** — for 10 000 random (pattern,
  appended-char) pairs over a 2 000-item set, the narrowed result equals a
  full re-rank exactly; cache keyed on `head` (typing three characters
  after `src/` performs one `opendir`, counted via the existing
  `yew_comp_test_*` hook style); source registration replacing a kind;
  `deferred` items ranked last within their band and never dropped.
- **Unit** (`tests/unit/test_menu.c`, new): selection held by identity
  across a refilter that reorders the list; selection lost from the
  filtered set falls to `sel = -1` and **not** to row 0; `explicit_sel`
  set only by the six explicit movers; scrolling keeps the selection
  visible at both ends; region registration count equals the drawn row
  count; refusal to draw under 40 columns drops the detail column rather
  than overlapping.
- **Unit** (`tests/unit/test_cmdline.c`, extended): the §6 Enter rule as a
  table — filter-only Enter executes, post-Tab Enter accepts; ghost text
  is never in the buffer (`yew_cmdline_text()` after a ghost is shown
  returns exactly what was typed); ghost accepted equals menu accepted,
  byte for byte; the hint table's six argspec rows; hint never sets
  `YEW_MSG_ERROR`.
- **Pty**: the nine goldens listed in §2 regenerated and reviewed; new
  goldens for the live menu at three prefix lengths, matched-character
  highlighting on an ASCII and on a CJK candidate (highlight cells aligned
  with glyphs), ghost text shown and accepted, the menu scrolled past row
  five, `scanning…` during a sliced enumerate, a mouse click selecting a
  row and a second click accepting it, and the live hint for `:w` and for
  `:1,2d`. Two consecutive full runs byte-identical.
- **Fuzz** (`tests/fuzz/fuzz_fuzzy.c`, new — pulled forward from s26):
  random patterns × random texts including invalid UTF-8, NULs, and 4 KiB
  paths; 100 000 cases × 4 seeds; never reads out of bounds, never returns
  a score colliding with the sentinel, always deterministic for identical
  input. `fuzz_cmdparse` extends with a live-filter mode that drives
  `yew_comp_query` + filter at every cursor position of every generated
  line, asserting no crash and a `replace` span always inside the buffer.
- **Perf** (`tests/perf/perf_cmdcomp.c`, retargeted): the existing gate
  measures one Tab in a 10 000-entry directory. It becomes a
  **per-keystroke** gate: typing a 12-character path into that directory,
  every keystroke completes in ≤ 5 ms and only the first performs an
  `opendir`. The command source (≈ 150 entries) is measured separately at
  ≤ 500 µs per keystroke. Baselines committed.

## Definition of Done

1. `make test` green on gcc and clang, zero warnings at `-std=c11
   -pedantic -Wall -Wextra -Werror`; ASan/UBSan/valgrind/determinism lanes
   green.
2. Every row of §1's scoring table has a passing assertion and the ≥ 30
   triple fuss-parity corpus matches exactly; `src` ranks `src/` above
   `src/file.c`.
3. `grep -rn 'yew_comp_score' src/ tests/` is empty, and
   `grep -n 'utf8_\|grapheme\|width\|wcwidth' src/ws/fuzzy.c` is empty.
4. s18 DoD 2 still holds with the new files:
   `grep -rn 'utf8_\|grapheme\|width\|wcwidth' src/ui/cmdline.c
   src/ui/cmdcomp.c src/ui/menu.c` returns only calls into
   `src/unicode/`.
5. s18 DoD 3 still holds: `grep -n '\[[a-z_]*caret\|caret++\|caret--'
   src/ui/cmdline.c` is empty, and the `YEW_MODE_E` keymap walk still finds
   every leaf a registered `ed.*` name.
6. **Ghost text is never in the buffer**: a unit test types a stem, shows a
   ghost, and asserts `yew_cmdline_text()` returns the typed bytes exactly;
   `grep -n 'ghost' src/ui/cmdline.c` shows no call to `yew_edit_insert`
   on a ghost path.
7. Narrowing equivalence: 10 000 random append cases produce results
   identical to a full re-rank.
8. Selection survives refiltering by identity — a pty test types a
   character that reorders the list and presses Enter, landing on the row
   that was highlighted — and a lost selection falls to `sel = -1`, not to
   row 0.
9. The §6 Enter rule: filter-only Enter executes; Enter after any of the
   six explicit movers accepts without executing. Both directions asserted
   at unit and pty level.
10. **Perf gate**: typing 12 characters into a 10 000-entry directory
    performs exactly one `opendir` and every keystroke completes in ≤ 5 ms;
    command-source keystrokes ≤ 500 µs; baselines committed.
11. Every mouse action in §8 has a working key binding, asserted by a test
    that drives the keyboard path and the mouse path to the same state.
12. Every new command registers cleanly (`command_name_valid` extended in
    the same commit), carries `YEW_CMD_INTERNAL`, and does not appear in
    the completion menu.
13. Deferred commands appear marked with their sprint and still hard-error
    on Enter naming it; `ed.find.command` names Sprint 38.
14. All nine regenerated goldens reviewed row by row in the commit that
    changes them, and two consecutive full pty runs are byte-identical.
15. ≥ 150 assertions across `test_fuzzy.c`, `test_menu.c`, and the
    extensions to `test_cmdcomp.c` and `test_cmdline.c`.
