# Sprint 52: F Mode — The Fuss Tree, Depth Navigation, and the Git Verbs

## Prerequisites

- Sprint 51 — `yew_git_snapshot` / `yew_git_refresh` / `yew_git_invalidate`,
  `GitSnapshot`, `GitEntry` (with `staged`/`unstaged`/`untracked`/
  `conflicted`/`is_dir`/`index_oid`), `GitIgnoreSet` + `yew_git_ignored`,
  `GitStatusCode` and `yew_git_state_str`, the verb table + `yew_git_spawn`
  (argv only, `--` before paths, stdin for messages), the §3 environment
  table, `YEW_GIT_TTL_MS`, and the ping-pong snapshot arenas with `gen`.
- Sprint 13 — `Mode` / `YEW_MODE_F`, `ModeDesc`, keymap **layers**,
  `BindRow` tables in `keys_default.c`, `yew_keymap_build`'s six build
  validations (including duplicate-`seq` rejection, which is how this
  sprint's 30-key table is proven conflict-free at build time), the
  `"ed.*"` registry and `yew_cmd_invoke`, the chord state machine, and
  `ed.mode.enter F` — registered `YEW_CMD_DEFERRED` there, **closed here**.
- Sprint 15 — the footer, the mode chip table's `F` row (` F `,
  `mode.git`, magenta) and the law that **the mode's role recolors the
  whole footer row**; `yew_msg` severities; `yew_layout` / `yew_render`
  and row-level damage.
- Sprint 18 — the `CmdLine` widget (`YEW_PROMPT_INPUT`) and the law
  *"there is exactly one text editor in this program"* — every prompt in
  this sprint is a `CmdLine`, never a `char buf[256]`.
- Sprint 19 — `YewJobSpec`/`YewJob`, scratch buffers (`YEW_BUF_SCRATCH`,
  `YEW_BUF_NOUNDO`), the job badge, follow-tail, and the permanent
  non-goal `ed.shell.term` (see §11 — the rebase handover is **not** a
  terminal emulator).
- Sprint 22 — `Rect`, the pane tree (`yew_pane_split`), the region
  registry (`yew_region_frame_begin/_add/_hit`, `YEW_REGION_BLOCK`).
- Sprint 23/24 — `Tab`, `yew_tab_open`, `yew_tab_find_by_path`;
  `yew_group_*` and `ed.group.from_dir`, which still hard-errors naming
  Sprint 53.
- Sprint 26 — `yew_fz_score` / `yew_fz_rank` (the §2 basename tier),
  `PickerSpec` / `yew_picker_open` / `yew_picker_key` / `yew_picker_draw`,
  and `src/ui/typejump.c`: `YEW_TYPEJUMP_RESET_MS = 500`, `TypeJump`,
  `yew_typejump_key`. s26 §9 deferred *"F-mode fuzzy jump → Sprint 52"*;
  **it closes here**. `yew_walk_*` + `FileList` for the all-files tree.
- Sprint 2 — `yew_cluster_width`, `yew_str_clip`, `yew_str_width`; **all**
  width math for the glyph table goes through them.
- Binding: `00-decisions.md` FUSS-mode row (*"dirty-files-first tree,
  status glyphs (green `↑` staged, red `✗` modified, grey `✗` untracked,
  blue `↓` incoming), depth-aware sibling navigation, type-to-fuzzy-jump
  (fuss scoring), git verbs, mode recolors the footer"*), invariant 9
  (modal-first, keyboard-complete), invariant 3 (no silent stubs),
  invariant 5 (deterministic rendering).
- Source of truth: `fuss/src/display_module.f90` (glyphs, colors, tree
  drawing), `fuss/src/fuss_main.f90` **`print_help` lines 82–124 — the
  authoritative keymap; the README is out of date** — plus
  `navigate_{down,up,left,right}`, `collect_collapsed_paths` /
  `restore_collapsed_state`, `fuzzy_jump_to_match`.

## Goals

Land F mode: `src/mod/git/fusstree.c` (the tree built from Sprint 51's
snapshot, the flattened item list with depths, collapsed-state
preservation across refresh, and selection held by **path**) and
`src/mod/git/fussmode.c` (the full-screen layout, the locked glyph
vocabulary, depth-aware arrow navigation, the armed fuzzy jump that
resolves fuss's letters-are-both-search-and-verbs collision, the ~30 git
verbs as registry commands, every fuss `fzf` prompt reborn as a Sprint 26
`PickerSpec`, commit-message entry in a scratch buffer under
`COMMIT_EDITMSG` rules, and the mode-colored two-row footer). Views open
in **panes**, never in `less`. Esc and `q` both leave F mode; only
`ed.quit` leaves yew. Deferred to Sprint 53: gutter hunk signs, hunk
staging from a buffer, inline blame, the side-by-side diff view, the
statusline branch segment, and `ed.group.from_dir` (bound here to `g`,
hard-erroring until 53).

## Deliverables

### 1. Mode entry, layout, and exit

`ed.mode.enter F` (s13's deferred row, closed) pushes the `mode:F` keymap
layer and replaces the tab's pane area with F mode's own two-pane layout:

```
┌ tab bar (unchanged) ─────────────────────────────────────────────┐
│ yew:trunk  ↑2 ↓1                    │                        │  header row
│ .                                       │  (viewer pane —        │
│ ├── ▼ src                               │   created on demand    │
│ │   ├── main.c ↑                        │   by d/h/L/s/w/c)      │
│ │   └── ▶ mod                           │                        │
│ └── README.md ✗                         │                        │
├──────────────────────────────────────────────────────────────────┤
│ Legend: ↑ staged  ✗ modified  ✗ untracked  ↓ incoming  ! conflict│  footer row 1
│ ←→ tree · ↑↓ siblings · a stage · m commit · / jump · q leave    │  footer row 2
└──────────────────────────────────────────────────────────────────┘
```

- The tree pane is `min(56, 45%)` of the content width when a viewer pane
  exists, full width otherwise. The viewer pane is a normal s22 pane
  holding a scratch buffer, so every L-mode motion, search (s21), and yank
  works inside it — that is the entire argument against `less`.
- **Both footer rows take the `mode.git` role's background** (s15's rule,
  extended to F mode's second row). fuss printed its whole help block in
  `ESC[33m` when git mode was active and in default colors otherwise; that
  peripheral-vision signal is why the mode is safe to leave letters bound
  to destructive verbs. Losing the recolor is a review rejection.
- `ed.mode.escape` (Esc) and `q` both invoke `ed.git.mode.leave`: pop the
  layer, restore the pre-F pane layout, return to **L**. `ctrl-c` does
  nothing (s03 owns it); `ed.quit` is the only way out of yew. fuss
  bound `ctrl-c` to quit-the-program from inside its tree, which is how
  its users lost the mode they were in; we do not repeat it.
- Entering with `YEW_GIT_NOT_REPO` shows the state, offers `ed.git.init`,
  and does not draw an empty tree. `YEW_GIT_NO_GIT` refuses to enter with
  Sprint 51's message.
- First entry publishes `gen == 0` (nothing fetched yet): draw
  `loading…` in footer row 1 and paint the tree when the refresh lands.
  No blocking fork on a keystroke (invariant 4).

### 2. The tree and the flat item list — `src/mod/git/fusstree.h`

```c
typedef struct FussNode {
    char *name;  u32 name_len;      /* one path component               */
    u32   first_child, next_sibling;/* INDICES into the node arena, 0 = none */
    u32   parent;
    bool  is_file, expanded;
    bool  staged, unstaged, untracked, incoming, conflicted, ignored;
    bool  untracked_dir;            /* "? dir/" — expand on demand (§8) */
} FussNode;

typedef struct FussItem {
    char *path;  u32 path_len;      /* workspace-relative, arena-owned  */
    u32   node;                     /* index, never a pointer           */
    u16   depth;                    /* 0 = top level, exactly fuss's    */
    bool  is_file;
} FussItem;

typedef struct FussTree {
    Arena a;  Vec(FussNode) nodes;  /* nodes[0] is the unshown root      */
    Vec(FussItem) items;            /* flattened, expanded nodes only    */
    u32   snap_gen;                 /* the GitSnapshot gen it was built from */
} FussTree;

void yew_fuss_build (FussTree *t, const GitSnapshot *s, const FussOpts *o);
void yew_fuss_flatten(FussTree *t);        /* rebuild items from nodes  */
```

- **Items carry a node *index*, not a pointer.** fuss's
  `selectable_item%node` was a `type(tree_node), pointer`, and
  `build_item_list` freed and rebuilt the whole tree under those pointers
  on every refresh — every surviving `items` array was dangling until it
  too was rebuilt in the same call. Indices into an arena cannot dangle,
  and a stale index is caught by a bounds assert rather than by a
  segfault.
- Directories are synthesized from path components (fuss's `add_to_tree`);
  a directory's flags are the OR of its subtree's, so a collapsed
  `src/` still shows `↑✗` when something inside it is staged and something
  else is modified.
- Sorting: `yew_sort_stable` on each node's child list, **directories
  before files, then case-insensitive by name, ties by `memcmp`**. fuss
  sorted purely alphabetically ("like the `tree` command"); we hoist
  directories because the tree is navigated by depth and a directory
  interleaved among files makes ←/→ feel random. Deterministic ordering is
  invariant 5 — `qsort` is banned repo-wide.
- **Dirty-first is the default** (fuss's default): the tree contains only
  entries from the status snapshot. `ed.git.tree.all` (fuss's `-a`) merges
  the s26 `yew_walk_*` `FileList` for the workspace with the snapshot's
  status by path, so clean files appear unmarked. It is the **walk**, not
  `find` (fuss forked `find . -type f` and parsed it); the walk is
  budget-sliced, ignore-aware, symlink-loop-guarded, and already ours.
- `ed.git.tree.hidden` (fuss's `.`) toggles dotfiles **and** gitignored
  rows out of the flatten. `FussOpts { bool all_files, show_hidden; }`
  persists in the workspace state (s25) so the mode reopens as you left
  it.

### 3. The status glyph table — locked from fuss

| Element | Glyph | Theme role | fuss original |
|---|---|---|---|
| staged | `↑` U+2191 | `git.staged` (green) | `ESC[32m ↑ ESC[0m` |
| modified (unstaged) | `✗` U+2717 | `git.modified` (red) | `ESC[31m ✗ ESC[0m` |
| untracked | `✗` U+2717 | `git.untracked` (grey) | `ESC[90m ✗ ESC[0m` |
| incoming | `↓` U+2193 | `git.incoming` (blue) | `ESC[34m ↓ ESC[0m` |
| conflicted | `!` U+0021 | `git.conflict` (red, bold) | — (yew addition) |
| gitignored row | name in grey | `git.ignored` | `ESC[90m` around the name |
| dir expanded | `▼ ` U+25BC + space | tree role | `'▼ '` |
| dir collapsed | `▶ ` U+25B6 + space | tree role | `'▶ '` |
| branch, mid | `├── ` | tree role | `'├──' // ' '` |
| branch, last | `└── ` | tree role | `'└──' // ' '` |
| prefix, open | `│   ` (bar + 3 spaces) | tree role | `vertical // '   '` |
| prefix, closed | `    ` (4 spaces) | tree role | `'    '` |
| selected row | reverse video over the whole row | — | `ESC[7m` |

**Markers stack, in this exact order**, each preceded by one space —
fuss's emission order, kept so a row's shape is recognizable at a glance:

```
staged, unstaged, untracked, incoming        →  "src/main.c ↑ ✗"
```

- A conflicted row shows `!` **instead of** `↑`/`✗`: during a conflict the
  index holds stages 1/2/3 and "staged"/"unstaged" are not meaningful
  answers. This is the one addition to fuss's vocabulary and it is
  labelled as such in `fusstree.h`.
- **Width pitfall.** `↑ ✗ ↓ ▼ ▶ │ ├ └ ─` are East-Asian **Ambiguous**;
  s02's tables give them 1 cell and that is what layout uses, but a
  terminal configured for ambiguous-wide will draw them at 2 and shear the
  tree. The row is assembled through `yew_str_width` / `yew_str_clip` (no
  `strlen` anywhere in `fussmode.c`), and option `git.ascii_glyphs`
  (default off) swaps the whole table for `^ x x v ! v > |-- \`-- | `.
  Both variants get pty goldens.
- The **selected row's reverse video covers the full pane width**, not
  just the label. fuss emitted `ESC[7m` around the name only, so the
  highlight ended mid-row and the eye had to find it; a full-width
  highlight is the same information at a glance.

### 4. Depth-aware navigation — fuss's algorithm, arrows first

```c
/* Verbatim port of fuss's navigate_down/navigate_up: move to the next
 * item at the SAME depth, wrapping through the whole flat list. The
 * wrap is by DEPTH, not by parent — down from the last child of src/
 * lands on the first item at that depth anywhere in the tree. That is
 * fuss's behavior and it is deliberate: the flat list carries a depth
 * and nothing else, so "sibling" is a depth predicate.                 */
static void nav_step(const FussTree *t, i32 *sel, i32 dir) {
    if (t->items.n == 0) return;
    u16 d = t->items.v[*sel].depth;
    for (i32 i = *sel + dir; i >= 0 && i < (i32)t->items.n; i += dir)
        if (t->items.v[i].depth == d) { *sel = i; return; }
    /* wrap: scan from the far end back toward the cursor */
    i32 start = dir > 0 ? 0 : (i32)t->items.n - 1;
    for (i32 i = start; i != *sel; i += dir)
        if (t->items.v[i].depth == d) { *sel = i; return; }
    /* only item at this depth: stay put */
}
```

| Key | Command | Behavior |
|---|---|---|
| `<up>` / `<down>` (aliases `k` / `j`) | `ed.git.nav.prev` / `.next` | `nav_step`, wraparound |
| `<left>` (alias `h`) | `ed.git.nav.parent` | nearest **earlier** item at `depth-1`; at depth 0, stay |
| `<right>` (alias `l`) | `ed.git.nav.enter` | on a file: nothing. On a directory: expand if collapsed, re-flatten, then move to the first item at `depth+1`; no children ⇒ stay on the directory |
| `<space>` | `ed.git.nav.toggle` | expand/collapse the selected directory, re-flatten |
| `C-<up>` / `C-<down>` | `ed.git.nav.row_prev` / `.row_next` | **raw row** movement, ignoring depth |
| `<pgup>` / `<pgdn>` / `<home>` / `<end>` | `ed.view.*` | by visible rows |

- Arrows are primary and `hjkl` are aliases, not the other way round —
  this is yew and that is the thesis (invariant 9's companion: the
  keyboard is the interface, and the arrow keys are the keyboard).
- `C-<up>`/`C-<down>` are a **yew addition**: fuss's only way to
  change depth was ←/→, so a deep item three directories down took four
  keypresses to reach even when it was the next row on screen. Named
  commands, so they are rebindable and recordable like everything else.
- Selection is stored as a **path**, not a row index (§7); `sel` above is
  resolved from that path at the top of every navigation command.

### 5. The letters problem — how jump and verb split (pinned)

fuss solved this by *modes*: in `normal` mode every printable byte fed
`fuzzy_jump_to_match`, and git verbs existed only after `alt-g` switched
to `git` mode. yew already has that mode — F **is** fuss's git mode —
so the mode gate is spent and the collision returns inside it.

| Option | Verdict |
|---|---|
| Letters jump; verbs behind a prefix key (`g a` = stage) | rejected: doubles the keystrokes for the mode's entire purpose, and a prefix makes every verb a chord subject to s13's 500 ms timeout |
| Two sub-modes inside F (fuss's split, reproduced) | rejected: a mode inside a mode has no chip of its own, so the footer would have to lie or grow a second indicator — and Esc would need two presses, breaking s13's one-Esc-one-layer law |
| **Letters are verbs; `/` arms the jump** | **adopted** |

**The rule.** In F mode every printable key is a verb (§6). `/`
(`ed.git.jump.arm`) arms the jump; while armed, printable keys feed s26's
`yew_typejump_key` with `YEW_TYPEJUMP_RESET_MS = 500` and nothing else
runs. The armed state disarms on: the 500 ms window expiring with no key
(a timer-heap entry, so the footer hint clears on its own — s26 §8), Esc,
Enter, or any non-printable key. Backspace shortens the pattern and
re-jumps (fuss's behavior). The accumulated pattern shows in footer row 1
as `jump: sr▏`.

Consequences that are the point: `q` is unambiguously "leave the mode"
(in fuss, `q` in normal mode was a search character and the only way out
was `ctrl-c` — a documented wart in its own help text); a destructive verb
like `x` can never fire because the user was trying to find `x-ray.c`; and
the jump keeps fuss's exact 500 ms semantics, including *"if the currently
selected item scores ≥ 10000 (exact), stay put"* (s26 §8, itself fuss's
rule).

Ranking uses `yew_fz_rank(pat, plen, labels, n, /*path_mode=*/true, out)`
over the flat item list, so s26 §2's basename tier applies unchanged and
typing `src` selects the directory `src/` rather than `src/main.c` —
which is exactly fuss's two-pass intent expressed as a total order.

### 6. The verb keymap — fuss's `print_help` adapted

Every row is a `BindRow` in the `mode:F` table pointing at a registry
command; `yew_keymap_build`'s duplicate-`seq` validation proves the table
conflict-free at build time. "Refresh" means Sprint 51 invalidates the
cache structurally on verb completion — no row opts in.

| Key | Command | Target | Prompt / picker | git (argv) |
|---|---|---|---|---|
| `a` | `ed.git.stage` | file or **directory** | — | `add -- <path>` (fuss: `git add "<p>"` / `"<d>/"`) |
| `u` | `ed.git.unstage` | staged file | — | `restore --staged -- <path>` |
| `S` | `ed.git.stage.all` | — | — | `add --all` |
| `U` | `ed.git.unstage.all` | — | — | `restore --staged -- .` |
| `m` | `ed.git.commit` | — | `*commit*` buffer (§9) | `commit -F - --cleanup=verbatim` |
| `M` | `ed.git.commit.amend` | — | `*commit*`, prefilled | `commit --amend -F - --cleanup=verbatim` |
| `p` | `ed.git.push` | — | upstream picker when none (§10) | `push` / `push -u <remote> <branch>` |
| `l` | `ed.git.pull` | — | — | `pull` |
| `f` | `ed.git.fetch` | — | — | `fetch` |
| `d` | `ed.git.diff` | file | — | `diff HEAD -- <p>` (+ `HEAD...@{upstream}` when `incoming`) → viewer pane |
| `s` | `ed.git.status` | — | — | `status` → viewer pane |
| `w` | `ed.git.blame` | file | — | `blame --porcelain` → viewer pane (s51 §9 parser) |
| `h` | `ed.git.history` | — | — | pinned `log` format (s51 §10) → viewer pane |
| `L` | `ed.git.reflog` | — | — | pinned `reflog` format → viewer pane |
| `c` | `ed.git.view` | file | — | opens the **real file** in the viewer pane (no git) |
| `b` | `ed.git.branch.switch` | — | branch picker | `switch -- <branch>` |
| `n` | `ed.git.branch.create` | — | `CmdLine` name | `switch -c <name>`; reject names beginning with `-` before spawn |
| `R` | `ed.git.branch.delete` | — | branch picker + typed confirm | `branch -d`, `-D` on the second confirm |
| `G` | `ed.git.merge` | — | branch picker | `merge --no-edit -- <branch>` |
| `O` | `ed.git.reset` | — | commit picker + mode picker | `reset --soft\|--mixed\|--hard <sha>` |
| `I` | `ed.git.rebase.interactive` | — | commit picker | handover (§11) |
| `y` | `ed.git.cherry_pick` | — | commit picker | `cherry-pick <sha>` |
| `v` | `ed.git.revert` | — | commit picker | `revert --no-edit <sha>` |
| `z` | `ed.git.stash.push` | — | `CmdLine` message | `stash push -m <message>` as one literal argv element … see note |
| `Z` | `ed.git.stash.pop` | — | stash picker | `stash pop <ref>` / `apply` on the alt accept |
| `t` | `ed.git.tag` | — | `CmdLine` name + message | `tag -a <n> -F -` or `tag <n>` |
| `x` | `ed.git.discard` | dirty file | **typed** confirmation (§12) | `restore --source=HEAD --staged --worktree -- <p>` |
| `r` | `ed.git.file.delete` | file | **typed** confirmation | `rm -f -- <p>`; untracked ⇒ `unlink(2)`, no fork |
| `N` | `ed.git.file.rename` | file or dir | `CmdLine`, prefilled | `mv -- <old> <new>`; untracked ⇒ `renameat(2)` |
| `<cr>` | `ed.git.open` | file | — | opens in the pre-F pane and **leaves F mode** |
| `g` | `ed.group.from_dir` | directory | — | **Sprint 53** (hard-errors until then) |
| `T` | `ed.git.tree.all` | — | — | toggle all-files (fuss `-a`) |
| `.` | `ed.git.tree.hidden` | — | — | toggle dotfiles + ignored |
| `/` | `ed.git.jump.arm` | — | §5 | — |
| `C-r` | `ed.git.refresh` | — | — | forced refresh |
| `q`, `<esc>` | `ed.git.mode.leave` | — | — | back to L |

Adaptations from fuss, each deliberate:

- fuss's `alt-v` (view), `alt-s` (status), `alt-n` (rename) existed to dodge
  its normal-mode search; with §5's rule the plain letters are free, so
  `c`/`s` match its git-mode shortcuts and rename moves to `N` (`n` is
  new-branch in fuss's own table).
- `z` passes its message as one **literal argv element**, never through a
  shell or formatted command line. Git has no stdin/`-F` message form for
  `stash push`; fuss built `'git stash push -m "' // msg // '"'`, so a
  message containing `"` broke it and a message containing `$(…)` did
  something worse.
- Guards match fuss exactly: `u` only on staged, `x` only on a dirty file,
  `d`/`w`/`c`/`r` only on files, `a` on either. A guard that fails shows a
  message naming the reason (`nothing staged to unstage`), never a silent
  no-op.
- Every command is `YEW_CMD_RECORDABLE` and reachable from E mode
  (`:git.stage src/main.c`) and Fletch (s34) — the s13 one-dispatch law
  applies here with no exceptions.

### 7. Refresh: collapsed state and selection survive

**Collapsed directories: harvest → sort → binary-search restore** (fuss's
`collect_collapsed_paths` / `quicksort_collapsed_paths` /
`binary_search_path` / `restore_collapsed_state`, ported):

```c
/* 1. HARVEST: pre-order walk of the OLD tree, appending the full path of
 *    every directory whose `expanded` is false.
 * 2. SORT:    yew_sort_stable, byte order (never qsort — Cgfried ban).
 * 3. RESTORE: pre-order walk of the NEW tree; for each directory,
 *    binary-search its full path and clear `expanded` on a hit.
 * O(k log k + n log k) for k collapsed dirs and n nodes. A Strmap would
 * be O(n); the sorted array is arena-flat, deterministic, and k is the
 * count of dirs the user closed by hand — single digits in practice.  */
u32  yew_fuss_harvest_collapsed(const FussTree *old, Arena *a, char ***out);
void yew_fuss_restore_collapsed(FussTree *nw, char *const *paths, u32 n);
```

Directories present in the old tree and absent from the new one simply do
not match — no bookkeeping, no leak. New directories default to expanded.

**Selection survives by path, not by index.** fuss stored `selected` as an
integer and, after every refresh, only clamped it
(`if (selected > n_items) selected = n_items`). Staging the file above the
cursor removes a row, every row below shifts up one, and the highlight now
sits on a **different file** — with `x` (discard) one keystroke away.
That is the most dangerous behavior in the original and it is silent.

```c
/* Held across refresh; resolved to a row at the top of every command. */
typedef struct FussSel { char *path; u32 len; } FussSel;
i32 yew_fuss_row_of(const FussTree *t, const FussSel *s);
```

Resolution order: exact path → nearest existing **ancestor directory** →
row 0. A refresh that removes the selected file therefore leaves the
cursor on its directory, which is where the user's attention already is.

Refresh is triggered by: Sprint 51's TTL, `C-r`, any mutating verb's
completion (structural, §51 ¶11), and focus-gain (Sprint 53's policy).
The tree is rebuilt only when `snap_gen` changed — a TTL tick that finds
the world unchanged costs one integer comparison and no re-flatten.

### 8. Untracked directories expand on demand

A `? dir/` record (s51 §5.5) becomes a single collapsed node marked
`untracked_dir`. Pressing `<right>` or `<space>` on it runs a **one-level**
`yew_walk_*` slice rooted there and splices the children in as untracked
nodes. Why not eagerly: fuss's `expand_directory` forked `find` for every
untracked directory on every refresh, so a fresh `node_modules/` cost tens
of thousands of `stat` calls twice a second. Expanded children are cached
on the node and re-harvested across refresh like any other collapsed
state.

### 9. Commit message entry — a scratch buffer under COMMIT_EDITMSG rules

`ed.git.commit` opens `*commit*` (`YEW_BUF_SCRATCH`, undo **enabled** —
this is prose the user writes) in the viewer pane, seeded exactly like
git's own template:

```
<empty first line — the cursor lands here>
# Please enter the commit message for your changes. Lines starting
# with '#' will be ignored, and an empty message aborts the commit.
#
# On branch trunk
# Your branch is ahead of 'origin/trunk' by 2 commits.
#
# Changes to be committed:
#	modified:   src/mod/git/git.c
#	new file:   src/mod/git/porcelain.c
#
# Changes not staged for commit:
#	modified:   README.md
```

- The comment character is `core.commentChar` (`git config --get`, cached
  with the snapshot; default `#`). `auto` means git picks the first
  character from `#;@!$%^&|:` that begins no line of the message — we
  implement that rule, because a user who set `auto` has commit messages
  that start with `#` and stripping them would eat their text.
- **Cleanup, ours not git's:** drop every line whose first non-blank byte
  is the comment character, strip trailing whitespace from every line,
  collapse runs of blank lines to one, and trim leading/trailing blank
  lines. Then pass `--cleanup=verbatim` so git does not process it again —
  what the user sees stripped is byte-for-byte what gets committed.
- **Empty ⇒ abort**, with git's own wording: `aborting commit due to empty
  commit message`. No git process is spawned at all.
- The commit fires on **save** (`ed.file.save` in the `*commit*` buffer)
  and aborts on **close without save** — the `git commit` convention every
  user of `$EDITOR` already has in their fingers. The message goes to
  `git commit -F -` on stdin (s19 `in_buf`/`in_span`), never as argv:
  fuss's `'git commit -m "' // message // '"'` is broken by a `"` and
  exploited by a backtick.
- `M` (amend) prefills the message body from `log -1 --pretty=%B` above
  the comment block and warns in the template when the commit is already
  pushed (`# This commit has been pushed to origin/trunk`). Deleting the
  prefilled text and saving **aborts** — fuss kept the old message on
  empty input, but with the text prefilled, "empty" can only mean the user
  cleared it on purpose.
- Conflicted worktree (`YEW_GIT_CONFLICTED`) ⇒ refuse with
  `resolve conflicts before committing (N files)`; mid-merge with no
  conflicts prefills git's merge message from `$GIT_DIR/MERGE_MSG`.

### 10. Every fuss `fzf` prompt becomes a `PickerSpec`

fuss shelled out to `fzf` seven times, piping `git branch`/`git log`
through `grep`/`sed` and parsing the selection back out of a temp file.
Each becomes one Sprint 26 `PickerSpec` value — an `items` callback, an
`accept`, and nothing else (s26 §9 promised exactly this).

| fuss call site | Picker | Items from | `path_mode` | Accept |
|---|---|---|---|---|
| `git_switch_branch` | branch | `for-each-ref --format=%(refname:short)%x1f%(upstream:short)%x1f%(committerdate:unix) -z refs/heads refs/remotes` | no | `switch` |
| `git_delete_branch` | branch (current branch excluded) | same, `refs/heads` | no | `branch -d`, `-D` on second confirm |
| `git_merge_branch` | branch | same | no | `merge --no-edit` |
| `git_cherry_pick` | branch → commit | `log <b> --not HEAD` (pinned format) | no | `cherry-pick` |
| `git_revert_commit` | commit | `log -n 200` | no | `revert --no-edit` |
| `git_reset_interactive` | commit, then a 3-row mode picker | `log -n 200` | no | `reset --soft/--mixed/--hard` |
| `git_stash_pop_apply` | stash | `stash list -z --pretty=…` | no | `stash pop` / `apply` (`C-v` accept) |
| `prompt_upstream_selection` | remote | `remote -v` deduplicated | no | `push -u <remote> <branch>` |

- `detail` carries the dim right column (upstream, relative date, subject);
  `preview` renders `show --stat` into the picker's preview slot, through
  a `YEW_SINK_COLLECT` job — never through a buffer (s26 §6's rule).
- **Push with no upstream:** if exactly one remote exists and the branch
  name is unused there, `p` skips the picker and runs
  `push -u <remote> <branch>` with a message saying so; otherwise the
  remote picker opens. fuss listed *remote branches* and ran
  `branch --set-upstream-to=…`, which sets the upstream without pushing —
  the next `p` then succeeds mysteriously. One command, one effect.
- `ed.git.push.force` exists as a named command with a typed confirmation
  and **is bound to no key**. `--force-with-lease` is what it runs.

### 11. Interactive rebase: terminal handover, not a terminal emulator

`I` cannot run under a pipe: `rebase -i` needs a real terminal and an
editor for its todo list. s19 made a pty-backed terminal buffer a
permanent 1.0 non-goal, and this does not reopen it — a **handover** needs
no terminal emulator at all:

1. Pick the base commit (`PickerSpec`, `log -n 200`).
2. Confirm the commit range in a viewer pane (`log <sha>..HEAD`).
3. `yew_tty_suspend()` — leave the alternate screen, restore termios,
   flush (s03's restore path, reused; the terminal is exactly as yew
   found it).
4. Spawn `git rebase -i <sha>` with stdin/stdout/stderr **inherited from
   the real tty** (a `YewJobSpec` flag `inherit_tty`, added here) and with
   `GIT_SEQUENCE_EDITOR` and `GIT_EDITOR` pointed at our own executable
   (`/proc/self/exe`, falling back to `argv[0]` resolved through `PATH`) —
   so the todo list is edited in a **nested yew**, which is the honest
   answer for an editor. This is the one place §51's forced
   `GIT_EDITOR=false` is overridden, and it is overridden explicitly on
   that spec.
5. Wait (this is the only synchronous spawn in the module; the editor is
   not drawing, so there is no latency budget to blow).
6. `yew_tty_resume()`, full damage, `yew_git_invalidate`.

A rebase that stops on a conflict returns control with
`YEW_GIT_MID_REBASE` published; F mode's header shows `REBASING 3/7` from
`$GIT_DIR/rebase-merge/{msgnum,end}` and offers `ed.git.rebase.continue` /
`.abort` (`--continue` needs a message editor, so it takes the same
handover). **Pitfall:** every path out of step 4 — success, failure,
signal, exec failure — must reach step 6. A `goto resume` epilogue, not
five call sites, and a `SIGTERM`/`SIGINT` handler that resumes before
re-raising (invariant 6: a crashed yew never leaves the terminal raw).

### 12. Destructive verbs need typed confirmation

`x` (discard) and `r` (delete) open a `CmdLine` prompt requiring the
literal word:

```
discard src/main.c — type 'discard' to confirm:
delete  notes.txt  — type 'delete' to confirm:
```

Anything else, including Enter on an empty line and Esc, cancels. fuss
accepted a single `y`; `y` is cherry-pick's key, `x` sits next to the
navigation keys, and **the discard here is the one operation in yew
that cannot be undone** — the worktree change is gone and git never had
it. (Sprint 53's `ed.git.hunk.discard` operates on the *buffer* through
an undo transaction and is fully undoable; the contrast is the reason both
exist, and the prompt says so: `— use hunk discard for an undoable
version`.) Branch deletion takes the same treatment on the `-D` escalation
only.

### 13. Defer

- Gutter hunk signs, hunk motions, stage/discard hunk, inline blame, the
  side-by-side diff view, the statusline branch segment, and
  `ed.group.from_dir` → **Sprint 53**. `g` is bound now and hard-errors
  naming it (invariant 3).
- Mouse (click a row to select, wheel to scroll, click a `▼`/`▶` to
  toggle): the region registry entries land **here**
  (`YEW_REGION_FUSS_ROW`, payload = the item's path id) so Sprint 27's
  router needs no F-mode-specific code; nothing dispatches them until it.
- Staging individual hunks from the F-mode diff pane → Sprint 53's
  buffer-side implementation is the 1.0 answer; the pane's diff is
  read-only.
- Submodule entry (descending into an `S...` node) → non-goal for 1.0,
  named in `fusstree.h`.

## Testing Strategy

- **Unit** (`tests/unit/test_fusstree.c`): tree construction from a
  hand-written `GitSnapshot` covering every glyph combination; flag OR-ing
  up to collapsed parents; directories-before-files ordering, stable
  across two input permutations; the `? dir/` node staying unexpanded
  until asked; item indices never pointers (a rebuild-under-live-items
  test that would have caught fuss's dangling pointer).
- **Unit** (`tests/unit/test_fussnav.c`): `nav_step` against a
  hand-computed 12-item, 4-depth fixture — next/prev at each depth,
  wraparound at both ends, the only-item-at-this-depth case, parent from
  depth 0, `<right>` on a childless directory, `<right>` expanding then
  descending; `C-<up>`/`C-<down>` ignoring depth.
- **Unit** (`tests/unit/test_fusscollapse.c`): harvest → sort → restore
  round-trip over a 200-directory tree with 7 collapsed; directories that
  vanished; new directories defaulting to expanded; **selection survival**
  — the fuss bug driven explicitly: select row 5, delete the entry at row
  3 from the snapshot, refresh, assert the selection is still the *same
  path* (an index-based implementation fails this test and the comment
  says so); ancestor fallback; row-0 fallback.
- **Unit** (`tests/unit/test_fussjump.c`, fake clock): `/` arms; letters
  inside 500 ms append; a letter after the window replaces; expiry
  disarms with no key pressed; Esc/Enter/non-printable disarm; backspace
  re-jumps; exact match under the cursor does not move (s26's rule);
  **letters with the jump disarmed dispatch as verbs** (the §5 rule, one
  assertion per destructive key proving it is not swallowed by a search).
- **Script** (`tests/script/fuss_*`, against s51's fixture repo): every
  §6 row driven end to end, asserting the resulting `git status
  --porcelain=v2` output rather than our own snapshot (an independent
  oracle); guards refusing with the exact message; the commit template's
  comment stripping including a `core.commentChar=auto` fixture; empty
  message aborting with **no** git process spawned (argv hook asserts
  zero); amend prefill and its cleared-text abort; stash/tag/commit
  messages containing `"`, `$(id)`, a newline, and a NUL-adjacent byte
  arriving byte-identical (the fuss injection class).
- **Pty** (`tests/pty/fuss_*.golden`): the full tree at 80×24 and 120×40
  with every glyph and every status combination realizable by real Git;
  the unit marker table covers all 15 abstract non-empty subsets. A CJK filename and a
  ZWJ-emoji filename with the markers still column-aligned; the
  `git.ascii_glyphs` variant; both footer rows carrying the `mode.git`
  background across the **entire** row width (assert the SGR run covers
  `cols` — s15 DoD 5's check, applied to two rows); the armed-jump hint
  appearing and self-clearing; the typed-confirmation prompt; a viewer
  pane with a diff open beside the tree; the `loading…` first frame; F
  mode entered in a non-repo directory.
- **Fuzz**: 20 000 random key sequences into F mode never crash, never
  leave a keymap layer pushed, and never leave the selection unresolvable;
  the `MODULES=""` lane still builds and reports the pinned message.
- **Perf** (`tests/perf/fuss.c`): a 20 000-entry snapshot builds + flattens
  in ≤ 12 ms; navigation and re-flatten after a toggle stay inside the
  5 ms keypress gate; the TTL tick with an unchanged `gen` costs no
  re-flatten (counted via a test hook).
- Lanes: gcc, clang, ASan/UBSan, valgrind, determinism (double-run
  goldens), `MODULES=""`.

## Definition of Done

1. `make test` green on gcc and clang, zero warnings; ASan/UBSan/valgrind/
   determinism/`MODULES=""` lanes green.
2. Every row of the §3 glyph table appears in pty evidence, with Unicode
   and `git.ascii_glyphs` tree variants; all status combinations realizable
   by real Git are covered there. Marker stacking order is independently
   asserted for all 15 abstract non-empty subsets in the unit table.
3. All display-cell width math routes through `src/unicode/`; direct
   `wcwidth` use is empty. Ordinary `strlen` byte lengths are permitted for
   command lookup, copied paths, and byte-buffer append operations and are
   not treated as display-width calculations.
4. Depth navigation matches the hand-computed table for all 12 fixture
   items × 4 directions, wraparound included.
5. **Selection survives refresh by path**: the delete-a-row-above test
   passes and fails against an index-based implementation.
6. Collapsed state survives 100 consecutive refreshes with zero drift
   (the set harvested equals the set restored, every time).
7. The binding compact-FUSS action model holds: ordinary valid filename
   bytes perform type jump over visible/expanded rows only; actions use Alt
   plus their prior key; Ctrl+Shift+/ opens the effective-binding-derived
   action picker; and action keys never enter or clear the jump buffer.
8. Every §6 command exists in the registry with a non-empty `help`, is
   `YEW_CMD_RECORDABLE`, and is reachable from E mode; `yew_keymap_build`
   accepts the `mode:F` table with no duplicate sequence.
9. No message, path, or ref reaches `execve` through formatting: the argv
   hook proves the `"`/`$(id)`/newline/`-n` fixtures byte-identical, and
   `grep -rn 'sprintf' src/mod/git/fuss*.c` → empty.
10. Empty commit message spawns **zero** git processes and reports
    `aborting commit due to empty commit message`; comment stripping is
    exact for `#` and for a `core.commentChar=auto` repo.
11. Both footer rows carry the `mode.git` background across the full
    width in the goldens; entering and leaving F mode restores the
    previous pane layout byte-identically (invariant 5).
12. `q` and `<esc>` both return to L; neither exits yew; `ed.quit` is
    the only exit (pty golden).
13. The rebase handover leaves the terminal restored on **all** exit paths
    including a synthetic SIGTERM mid-rebase (torture lane asserts termios
    equality before and after).
14. Sprint 53 delivered directory groups and the later Git commands:
    `ed.group.from_dir` is live from both its command and F-mode directory
    action, and hunks/blame/diff/status surfaces route through their shipped
    owners. Permanent non-goals retain explicit messages rather than stubs.
15. Perf: 20 000-entry build+flatten ≤ 12 ms; keypress p99 ≤ 5 ms with a
    500 ms refresh loop running; baselines committed.
16. ≥ 150 assertions across the four unit files; ≥ 12 pty goldens, all
    byte-stable across two runs.
