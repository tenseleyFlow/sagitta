# Sprint 27: Mouse Routing and the Feel Pass

## Prerequisites

- Sprint 4 — SGR mouse decoding (1006 over 1002), the `Cb` table with its
  modifier bits, 0-based coordinates leaving `input.c`, the X10 fallback,
  and the pinned pitfall *"wheel events have no release — a drag state
  machine keyed on press-without-release hangs on the first scroll;
  Sprint 27's router must treat wheels as impulses."* This sprint is that
  router. s04 also pinned that mouse events are produced and **discarded**
  until now.
- Sprint 22 — the region registry (`yew_region_frame_begin` / `_add` /
  `_hit`, last-added-wins, `YEW_REGION_BLOCK`), `Rect`, the pane tree,
  `yew_pane_leaf_at`, click-to-focus, and the minimal border-drag resize
  state machine that this sprint re-hosts.
- Sprint 23 — `Tabs`, stable `tab_id`, `yew_tab_reorder` (insertion, not
  swap), `shifted_index`, the strip layout engine and its `StripSpan`s,
  `YEW_REGION_TAB` / `_TAB_SCROLL` and the payload sign convention.
- Sprint 24 — groups, the row-2 member strip renderer and the
  hover-preview path *"called by S27"*, `tabs.group_hover_preview`,
  `yew_group_add_member` / `_remove_member` / `_set_ordinal` /
  `yew_group_reorder_block`, `yew_group_dissolve`, the group picker.
- Sprint 26 — `YEW_REGION_PICK_ROW` and the list picker.
- Sprint 16 — the unit-engine vtable: `yew_unit_word`, `yew_unit_line`,
  their `span(u, p, alt)` and the contract every engine obeys.
  Sprint 17 — H mode, selection rendering, the selection→multi-cursor
  lift, and *"H borrows the previous unit mode's engine"*.
- Sprint 15 — `yew_vp_scroll`, `yew_vp_clamp`, the statusline/message
  line and its field-priority list; Sprint 12 — registers and OSC 52
  (Copy Path); Sprint 13 — keymap layers and the registry; Sprint 5 —
  `yew_render_tier`, `yew_rgb_to_256`, the 16-colour ladder; Sprint 2 —
  `yew_cluster_width`, `yew_str_clip`.
- Binding: `00-decisions.md` Mouse row (*"Supported … via a
  clickable-region registry populated by render passes … Keyboard never
  requires it"*) and **invariant 9**; invariant 5 (deterministic
  rendering) which every golden in §7 rests on.
  Source of truth for the interaction design:
  `~/GithubOrgs/FortranGoingOnForty/facsimile/src/ui/context_menu_module.f90`,
  `src/ui/tab_drag_module.f90`, `src/commands/command_handler_module.f90`
  (`open_tab_context_menu`, `tab_drag_motion`, `dwell_over_group`).

## Goals

Land `src/ui/mouse.c` — the one router every mouse event passes through —
and close the sprint 22/23/24 mouse deferrals: click-to-focus, border-drag
resize, tab and group drag-reorder with dwell-over-group opening the member
strip as a drop target, per-region wheel scrolling under the
scroll-what-the-pointer-is-over rule, and minimal context menus for tabs
and groups. Double- and triple-click selection routes through the Sprint 16
unit engines so the mouse and the keyboard agree on what a word is. Then
the feel pass: a pty-golden review of every chrome element in the program,
one glyph vocabulary table, and a `NO_COLOR` / 16-colour / no-mouse
degradation audit whose centrepiece is the invariant-9 table proving every
mouse action has a keyboard path. Deferred: a document context menu,
middle-click paste, F-mode tree mouse (52), LSP hover-on-pointer (47),
dragging a tab into a pane (post-1.0).

## Deliverables

### 1. The router — `src/ui/mouse.h`, `src/ui/mouse.c`

```c
typedef enum { YEW_MP_IDLE = 0, YEW_MP_ARMED,
               YEW_MP_DRAG_BORDER, YEW_MP_DRAG_TAB,
               YEW_MP_DRAG_GROUP,  YEW_MP_DRAG_SEL } MousePhase;

typedef struct MouseState {
    MousePhase phase;
    u8     held;                  /* button bitmask                      */
    u16    press_x, press_y;
    Region press_rgn;             /* CAPTURED at press — see the law     */
    /* identity of what is being dragged; never an index                 */
    u32    drag_tab_id, drag_gid;
    char  *drag_path;             /* resolved again at commit            */
    u32    tab_count_at_press;    /* a change cancels the drag           */
    /* multi-click                                                       */
    i64    last_click_ms; u16 last_click_x, last_click_y; u8 click_n;
    /* dwell over a group while dragging                                 */
    i64    dwell_since_ms; u32 dwell_gid;
    /* selection drag                                                    */
    const UnitOps *sel_unit; Span sel_anchor_span;
} MouseState;

void yew_mouse_event(Ed *ed, const Key *k);   /* THE entry point         */
```

**Law — the press captures its region; motion and release never
re-resolve identity.** `press_rgn` is stored at press and consulted for
the rest of the gesture. The tab strip scrolls, an async job (s19) can
close a tab, and every index above a closed tab renumbers — so a region
re-queried at release can name a different thing than the one the user
aimed at. Identity is carried as `tab_id` / `gid` / path, never as an
index (s23's law, in the input path).

**Arming, not dragging.** A press sets `YEW_MP_ARMED`; the drag begins
only when the pointer leaves the pressed **cell**. A click that never
moves must stay a click, and nothing is drawn or targeted until then.

**Wheels are impulses.** A wheel event is dispatched and returns without
touching `phase` — s04's pinned pitfall. It also never sets `held`.

- `yew_mouse_event` is called from the dispatch path with the normalized
  `Key` s04 produces; `input.c` still owns decoding and this file never
  sees a byte.
- Esc cancels any drag, restoring the entry state (border ratio, tab
  order, selection anchor). Focus-out (s04's `FOCUS_OUT`) also cancels —
  a drag whose release lands in another window would otherwise hang.

### 2. Region → action table

The sprint's central reference. Rows not listed are ignored, never
guessed at.

| Region | press (left) | drag | release | wheel | press (right) |
|---|---|---|---|---|---|
| `YEW_REGION_PANE` | focus leaf, place cursor (s22) | extend selection by `sel_unit` (§6) | commit selection | scroll **that** pane, 3 rows (§3) | — (unbound; §9) |
| `YEW_REGION_PANE_BORDER` | begin resize | move boundary in whole cells | ratio computed once, at release | ignored | — |
| `YEW_REGION_TAB`, payload > 0 | arm tab drag | reorder preview; dwell over a group (§4) | `yew_tab_reorder` or join group | scroll strip 1 entry | tab menu (§5) |
| `YEW_REGION_TAB`, payload < 0 | arm group drag | reorder-block preview | `yew_group_reorder_block` | scroll strip 1 entry | group menu (§5) |
| `YEW_REGION_TAB_SCROLL` | scroll 1 entry | held → auto-scroll (§4) | — | scroll 1 entry | — |
| `YEW_REGION_PICK_ROW` | select row | — | accept if release is in the same row | scroll list 3 rows | — |
| `YEW_REGION_GP_ROW` / `_GP_NAME` | focus row / tick (s24 rules) | — | — | scroll list | — |
| `YEW_REGION_CTX_ROW` | highlight row | — | invoke the row's action | — | — |
| `YEW_REGION_BLOCK` | swallowed | swallowed | swallowed | swallowed | swallowed |
| none | ignored | — | — | nothing | close an open menu |

- Border-drag resize moves here from `layout.c` unchanged in behavior:
  cells → ratio conversion happens **once at release** (s22's pitfall —
  per-motion round-trips accumulate float error and the border stutters).
- `YEW_REGION_BLOCK` swallowing is what makes a dialog modal to the
  mouse without any dialog knowing about the router.

### 3. Wheel: scroll the region under the pointer, not the focused one

```c
#define YEW_WHEEL_ROWS 3
```

- Wheel over a pane scrolls **that pane's** viewport (`yew_vp_scroll` then
  `yew_vp_clamp`) **without changing focus and without moving the
  cursor**. Why: two panes side by side exist to be compared; if the wheel
  followed focus you would have to click the other pane first, and
  clicking moves the cursor — so reading a second file would mutate your
  editing position. A scroll must never change document state.
- **Pitfall — never call `yew_vp_follow` after a wheel scroll.** Follow
  drags the viewport back to satisfy scrolloff around the cursor, which
  is exactly where it was: the wheel would appear to do nothing on an
  unfocused pane and would snap back on the focused one. `clamp`, not
  `follow`. Follow resumes at the next cursor motion, as always.
- Shift+wheel scrolls horizontally by 6 cells when `wrap` is off; it is a
  no-op when `wrap` is on (there is nowhere to go), with no message —
  a wheel is not a command that can fail.
- Ctrl+wheel is **unbound**, deliberately: it is the terminal emulator's
  font-size gesture, and stealing it would fight the host application.
- No acceleration, no momentum, no fractional accumulation: a wheel notch
  is exactly 3 rows, forever. Deterministic (invariant 5) and testable.
- Wheel over row 1 scrolls the tab strip; over row 2 scrolls the member
  strip (s24's ±2 scroll-region payload distinguishes the rows).

### 4. Tab and group drag-reorder, and dwell-into-group

```c
#define YEW_DRAG_DWELL_MS  400
#define YEW_DRAG_SCROLL_MS 120
```

**Nothing in `Tabs.v` changes until the drop.** The drag carries a target;
the renderer draws the strip as if the held entry were already at the
target; release applies it once. Facsimile's reasoning, kept verbatim
because each clause is a bug avoided: swapping live would look identical
and be far worse underneath — snapping back on cancel would mean undoing
an arbitrary number of moves, a drag that wandered off the bar would leave
the array half-shuffled, and a future drop-into-a-pane must not have
quietly reordered the strip on the way.

A consequence worth naming: because the array is frozen for the drag's
lifetime, a tab **index** is stable for that lifetime, which is why the
held entry can be matched by payload while it is being drawn. The commit
still resolves by `tab_id`, and a change in tab count mid-drag
(`tab_count_at_press`) cancels the drag outright.

- **Dwell over a group.** Resting a held tab on a *different* group's
  row-1 entry for `YEW_DRAG_DWELL_MS` opens that group's member strip on
  row 2 — reusing s24's hover-preview renderer, so nothing new is drawn
  and the strip already treats itself as still-hovered, which is exactly
  the "move down into it to drop" behavior needed. On a dwell and not
  immediately: dragging *past* a group on the way somewhere else must not
  make its members flash open.
- **Pitfall — the dwell target is not `yew_region_hit`.** The region table
  describes the *previewed* strip, where the held entry has been moved
  under the pointer, so hit-testing would always answer "you are hovering
  the thing you are holding". The dwell reads the **pre-drag** entry list
  (`yew_strip_pre_payload(slot)`, added to `tabs.c` this sprint) instead.
  A tab also never dwells into the group it is already a member of.
- **Drop targets.**

  | Drop on | Result |
  |---|---|
  | row 1, another entry | `yew_tab_reorder(from, to)` — insertion, not swap (s23) |
  | row 1, blank tail | move to the end; **the only way to carry a tab out of a group when the group is the sole row-1 entry** — there is nothing else to aim at |
  | row 2 (pinned or dwell-opened) | join that group at that ordinal (below) |
  | anywhere else / Esc / focus-out | cancel, nothing changes |

- Joining a group is s24's exact sequence, called and never re-derived —
  the ordinal off-by-one is s24's pitfall and must not be reinvented:

```c
if (tab->group_id) yew_group_remove_member(ed, tab_idx);  /* FIRST      */
yew_group_add_member(ed, gid, tab_idx);
yew_group_set_ordinal(ed, tab_idx, pos);   /* pos counts in the FINAL list */
yew_group_reorder_block(ed, gid, block_start);  /* keep members contiguous */
```

- Dragging a **group** entry moves the whole block via
  `yew_group_reorder_block` (s24 computes the destination order as
  `tab_id`s first, then places position by position).
- Held over a `TAB_SCROLL` chevron: auto-scroll one entry every
  `YEW_DRAG_SCROLL_MS`, **on a timer, not per motion event** — a fast
  pointer emits far more motion reports than a slow one and the strip
  would fly past the target.

### 5. Context menus — `src/ui/ctxmenu.h`, `src/ui/ctxmenu.c`

```c
#define YEW_CTX_MAX_ROWS  24
#define YEW_CTX_MIN_WIDTH 18

void yew_ctx_begin(u32 kind);
void yew_ctx_item (const char *label, const char *accel, u32 action,
                   bool enabled);
void yew_ctx_sep  (void);
bool yew_ctx_show (u16 anchor_x, u16 anchor_y, Rect allowed);
bool yew_ctx_key  (Ed *ed, const Key *k);     /* the menu's keymap layer */
void yew_ctx_draw (Ed *ed);
u32  yew_ctx_take (void);                 /* 0 = nothing chosen yet      */
```

This module owns geometry, drawing, hit-testing and key navigation and
nothing else. It does **not** include `ed.h`: the caller decides which
rows exist, whether each is enabled, and what each one does, and gets an
opaque `action` back. That keeps it low in the dependency graph — the
renderer uses it, so it cannot use the renderer — and unit-testable
without an editor.

| Kind | Rows |
|---|---|
| tab (`YEW_REGION_TAB` payload > 0, right-click) | `Close Tab` `C-w` · `Close Other Tabs` (disabled when there are none) · — · `Copy Path` · `Remove from Group` (disabled when ungrouped) |
| group (payload < 0, right-click) | `Edit Group…` · `Rename Group…` · — · `Dissolve Group` |

- Disabled rows are **greyed, never hidden**, so the menu keeps the same
  shape and a row does not move under the pointer between one right-click
  and the next.
- **THE PITFALL, and the reason this deliverable is written the way it
  is: the menu captures its target at open time and never re-resolves
  it.** The tab menu stores the `tab_id` *and* the canonical path; the
  group menu stores the `gid`. Every row action re-finds the target from
  that identity when it is invoked. It must not read the region payload
  at invoke time, because the tab strip can scroll under an open menu,
  tabs can close, and every index renumbers when one does — so the entry
  at those coordinates may be a different file by the time the row is
  clicked. A row handler that reaches for a payload is `YEW_BUG`.
- **Placement clamps, never flips.** Sliding the box back inside the
  allowed rectangle keeps the row the user aimed at under the pointer;
  flipping the menu above the anchor puts a *different* row there. A menu
  opened from the tab strip hangs directly under the clicked row rather
  than below the whole bar — inside a group the bar is two rows, and the
  general rule would leave two rows of dead space between the pointer and
  the thing it is travelling to.
- Regions: `YEW_REGION_BLOCK` over the box (so the document beneath is
  inert) plus `YEW_REGION_CTX_ROW` per drawn row, from the same `Rect`
  the row was drawn with. Last-added-wins (s22) makes the menu shadow
  everything under it with no z-order machinery.
- Keyboard path (invariant 9): the menu is a keymap layer. `↑`/`↓` skip
  separators and disabled rows, `Enter` invokes, `Esc` closes, and every
  row's action also exists as a registry command reachable without a
  mouse — `ed.tab.close_others`, `ed.tab.copy_path`, `ed.group.rename`,
  `ed.group.dissolve` (the last two wrap s24's existing surface), plus
  `ed.ui.context_menu`, which opens the menu for the *focused* tab or
  group at the keyboard cursor.
- `Copy Path` writes the canonical realpath to register `+` and to the
  system clipboard via s12's OSC 52 path.

### 6. Double and triple click — through the unit engines

```c
#define YEW_CLICK_MULTI_MS 400
```

| Clicks | Unit | Effect |
|---|---|---|
| 1 | — | place the cursor at the clicked grapheme (s22's `CCol → GCol` conversion) |
| 2 | `yew_unit_word` | select `span(p, alt)` → enter H mode with the word engine borrowed |
| 3 | `yew_unit_line` | select `span(p, alt)` → enter H mode with the line engine borrowed |
| 4 | — | wraps to 1; never a surprise paragraph selection |

- The counter resets when a click lands on a **different cell** (a cell,
  not a pixel radius — cells are the unit of everything here) or on
  focus-out, and when `YEW_CLICK_MULTI_MS` elapses.
- `alt` comes from the Alt modifier in `Cb`: Alt+double-click selects the
  WORD (whitespace-delimited), Alt+triple-click the whole display line —
  the same `alt` the keyboard's `A-←`/`A-→` pass (s16 §1).
- **Why the engines and not a local scan.** So the mouse and the keyboard
  agree on what a word is. A double-click that selects `foo` where
  `W`+`H`+`→` selects `foo.bar` is a bug users cannot name and always
  feel. Routing through `yew_unit_word.span` also makes double-click
  correct for CJK (per-ideograph, WB999), ZWJ emoji, `don't`, `1,000.50`
  and `foo_bar` **for free**, because s16 already fought those battles and
  has the conformance corpus.
- The result enters **H mode with the corresponding unit borrowed** —
  exactly the state `H` plus the unit-mode key would produce. Consequence:
  every H-mode key works on a mouse selection, the selection→multi-cursor
  lift (s17) works, and the recorder (s35) sees ordinary commands rather
  than a mouse-shaped side door.
- **Drag after a multi-click extends by whole units.** The anchor stays
  at the initial unit's span; the head snaps to the boundary of the unit
  under the pointer. Pitfall: extending by *characters* after a word
  double-click is what a naive implementation does, and it feels broken
  in a way people describe as "the selection is fighting me".
- Middle-click paste is **off by default** (`mouse.middle_paste`, default
  false) and, when enabled, pastes register `"` at the click position as
  one undo transaction. X11 primary-selection semantics cannot be
  implemented correctly from a terminal, and OSC 52 is write-only in most
  terminals — shipping the half-thing silently would be worse than the
  option.

### 7. The feel pass: glyph vocabulary and the chrome golden review

`src/ui/glyphs.h` — one table, every chrome glyph in the program:

| Glyph | Meaning | ASCII |
|---|---|---|
| `»` | collapsed node / expands rightward | `>` |
| `«` | expanded node / collapses leftward | `<` |
| `⇕` | draggable separator grip (pane border midpoint) | `\|` / `-` |
| `*` | modified (derived from the undo tree, s23) | `*` |
| `•` | ticked row with unsaved changes (s24 picker) | `+` |
| `▼` | expanded disclosure | `v` |
| `▶` | collapsed disclosure | `>` |
| `<` / `>N` | strip overflow, left / N more right (s23) | same |
| `[✓]` / `[ ]` | ticked / unticked (s24) | `[x]` / `[ ]` |
| `│ ─ ┼ ├ ┤ ┬ ┴` | pane borders and joints (s22) | `\| - + + + + +` |
| `↑ ✗ ↓` | git status glyphs — **reserved** for Sprint 52 | `^ x v` |

**The `»` / `«` rule: the arrow points the way the thing will move, not
the way it currently is.** Pinned because half of all terminal trees get
this inconsistent between two widgets and nobody ever files a bug about
it; they just find the UI slightly untrustworthy. `▶`/`▼` follow the same
rule for vertical disclosure.

- ASCII fallbacks are selected by `ui.ascii`, default *auto*: on when
  neither `LC_ALL` nor `LC_CTYPE` nor `LANG` contains `UTF-8`/`utf8`,
  off otherwise. `YEW_ASCII=1` forces on, `YEW_ASCII=0` forces off.
- Every glyph is measured with `yew_cluster_width` at its call site. A
  two-cell glyph in a one-cell slot is a layout bug that the goldens
  catch — `⇕` and `✓` are exactly the risky ones and both have a
  dedicated width assertion.

The review — every chrome element gets a golden, and this checklist is
the deliverable:

| Element | Golden |
|---|---|
| tab strip row 1 (ungrouped, grouped, overflow) | `chrome_tabs.golden` |
| row-2 member strip (pinned and hover-preview) | `chrome_group_strip.golden` |
| pane borders, all joint shapes, dim-inactive | `chrome_panes.golden` |
| statusline, every field, and its degradation order | `chrome_status.golden` |
| message line (info / warn / error / prompt) | `chrome_msg.golden` |
| cmdline + completion menu (s18) | `chrome_cmdline.golden` |
| list picker + match highlight + footer (s26) | `chrome_picker.golden` |
| group picker, New and Edit (s24) | `chrome_gp.golden` |
| context menus, both kinds, with a disabled row | `chrome_ctxmenu.golden` |
| search overlay + `[3/17]` (s21) | `chrome_search.golden` |
| gutter + wrap indicators (s15) | `chrome_gutter.golden` |
| drag in progress: ghost entry + drop target | `chrome_drag.golden` |

### 8. Degradation audit

| Condition | Detection | Behavior |
|---|---|---|
| `NO_COLOR` set non-empty | env, read once at startup (s00's single-decision rule) | **zero colour SGR emitted**; identity carried by attributes only — reverse for active/selected, dim for inactive, bold for headers, underline for the current match |
| 16-colour tier | s05's `yew_render_tier` | theme colours map through `yew_rgb_to_256` → nearest ANSI; active/selected use **reverse**, not colour, because at 16 colours accent and dim are not reliably distinguishable |
| 256-colour tier | s05 | cube/ramp mapping, unchanged |
| no mouse (terminal never reports, or `YEW_MOUSE=0`) | 1002/1006 not enabled | every action has a keyboard path — the table below is the proof |
| `ui.ascii` | §7 | glyph fallbacks; no chrome element loses meaning |
| tiny terminal (down to 20×5) | SIGWINCH | chrome sheds in a pinned order: row-2 strip → tab strip → gutter → statusline fields (s15's priority list) → message line last. Never a crash, never a zero- or negative-sized `Rect` |

**Invariant 9 audit — every mouse action and its keyboard twin.** This
table is a DoD item, not documentation:

| Mouse | Keyboard |
|---|---|
| click a pane | `ed.pane.focus_left/right/up/down`, `ed.pane.focus_next` |
| drag a border | `ed.pane.grow` / `ed.pane.shrink` |
| click a tab | `ed.tab.goto N`, `ed.tab.next` / `.prev` |
| drag a tab | `ed.tab.move N` |
| drop a tab into a group | `ed.group.add_tab` (new this sprint) |
| click a group entry | `ed.group.enter` |
| wheel | `ed.view.scroll.up` / `.down`, `ed.view.page.*` |
| double-click | `H` + `W` unit, `ed.sel.unit.expand` |
| triple-click | `H` + `L` unit |
| right-click menu | `ed.ui.context_menu` + each row's command |
| menu row | `↑`/`↓` + `Enter` in the menu layer |
| picker row click | `↑`/`↓` + `Enter` (s26) |
| tab-strip chevrons | `ed.tab.next` / `.prev` (scrolling follows the active tab) |

New registry commands landing here to close the audit:
`ed.group.add_tab`, `ed.tab.copy_path`, `ed.tab.close_others`,
`ed.group.rename`, `ed.ui.context_menu`.

### 9. Defer

- **Document context menu** (right-click inside a pane) → post-1.0
  (**superseded by Sprint 57.11**, which ships context menus on every surface);
  right-click on `YEW_REGION_PANE` is unbound and does nothing, rather
  than shipping a stub menu. Named here so nobody invents one.
- Dragging a tab into a *pane* to open it there → post-1.0; the drag
  module's target already has room for it and that is the whole
  preparation (s24/facsimile's reasoning for not mutating during a drag
  was written with this feature in mind).
- F-mode tree mouse (click to expand, drag to stage) → Sprint 52, routed
  through this same router and registry.
- LSP hover-on-pointer and diagnostics tooltips → Sprint 47.
- Pixel-precise / kitty-protocol mouse extensions → **never**: the cell
  is the unit of everything in this program, and sub-cell coordinates
  would break the layout/hit-test identity that s22 made a law.
- `ed.mouse.enable` / `.disable` land here (runtime toggle); the option
  model that persists them is Sprint 36.

## Testing Strategy

- **Unit** (`tests/unit/test_mouse.c`): every row of §2 driven with a
  synthetic `Key`; the phase machine (press → motion-in-cell stays ARMED
  → motion-out enters DRAG → release commits; Esc and focus-out cancel);
  wheel never mutates `phase` or `held`; captured-region law (mutate the
  region table between press and release, assert the commit used the
  captured identity); `tab_count_at_press` change cancelling a drag.
- **Unit** (`tests/unit/test_mouse_click.c`): the multi-click counter with
  an injected clock at 399/401 ms; reset on a different cell; quad-click
  wrapping to 1; double-click spans equal `yew_unit_word.span` on a table
  of ≥ 12 texts including CJK, a ZWJ family, `don't`, `foo_bar`,
  `1,000.50`, and an invalid-byte run (the s16 corpus, reused — the
  agreement *is* the test); Alt variants using `alt = true`; word-wise
  drag extension.
- **Unit** (`tests/unit/test_ctxmenu.c`): geometry clamping at all four
  edges and refusal when the space cannot hold the box; never flips;
  disabled and separator rows skipped by `↑`/`↓`; **target capture** —
  open a tab menu, scroll the strip so a different tab occupies those
  cells, invoke a row, assert the original `tab_id` was acted on; a row
  handler reading a payload trips `YEW_BUG` (proved with a deliberately
  wrong handler in the test file).
- **Unit** (`tests/unit/test_drag.c`): dwell timer at 399/401 ms with an
  injected clock; dwell reads the pre-drag payload list, not
  `yew_region_hit` (fixture where the two disagree); dwell never opens
  the group the held tab already belongs to; drop-on-blank-tail moving a
  tab out of a sole group; the join sequence producing the same state as
  the equivalent keyboard commands (`ed.group.add_tab`), asserted by
  comparing full membership/ordinal snapshots; auto-scroll throttled to
  one entry per 120 ms under a 1 000-event motion burst.
- **Pty** (`tests/pty/chrome_*.golden`): the §7 checklist, each captured
  at the truecolor tier, then re-captured under `NO_COLOR=1`,
  `YEW_COLORS=16`, and `YEW_ASCII=1` — 4 variants per element.
  Interaction goldens: click-to-focus with a CJK filename in the strip;
  wheel over an unfocused pane (assert the *other* pane scrolled, focus
  and cursor unchanged); drag-reorder with the ghost entry; dwell opening
  a member strip and dropping into it; both context menus over a scrolled
  strip.
- **Fuzz** (`tests/fuzz/fuzz_mouse.c`): random SGR mouse byte streams
  (including releases without presses, presses without releases, wheel
  bursts, coordinates far outside the grid, and X10-encoded events),
  100 000 events × 4 seeds — never a stuck `phase`, never a mutation
  without a matching release, never a `Rect` with negative dimensions,
  never a click resolved against a freed tab. ASan/UBSan.
- **Perf**: a 1 000-event motion burst produces **≤ 1 render** (the event
  loop's typeahead coalescing, s14) and no allocation in the router.
- **Determinism**: the full pty suite run twice, byte-compared.

## Definition of Done

1. `make test` green on gcc and clang, zero warnings at `-std=c11
   -pedantic -Wall -Wextra -Werror`; ASan/UBSan/valgrind/determinism
   lanes green.
2. **Every mouse event enters through `yew_mouse_event`**: `grep -rn
   'YEW_EV_MOUSE\|mouse' src/ --include=*.c | grep -v 'src/term/input.c\|
   src/ui/mouse.c'` shows no other dispatch site.
3. Every row of the §2 region table has a passing unit assertion; rows
   marked ignored are proven inert (the event changes no state).
4. Captured-identity law proven twice: a drag committed after the region
   table changed under it, and a context-menu row invoked after the strip
   scrolled — both act on the originally targeted `tab_id`.
5. Wheel over an unfocused pane scrolls that pane, leaves focus and the
   cursor untouched, and `grep -n 'yew_vp_follow' src/ui/mouse.c` →
   empty.
6. Double-click selection equals `yew_unit_word.span` and triple-click
   equals `yew_unit_line.span` for all ≥ 12 corpus texts; the resulting
   state is H mode with the corresponding unit borrowed (asserted via the
   mode chip in a pty golden, not just internally).
7. Dwell: 400 ms opens the member strip, 399 ms does not; a drag passing
   over three groups in 300 ms opens none of them.
8. Drag never mutates `Tabs.v` before release: a unit test snapshots the
   array at every motion event of a 50-step drag and asserts equality
   with the pre-press snapshot.
9. Context menus: geometry clamps at all four edges, never flips; a
   disabled row is drawn greyed and is unreachable by pointer and by
   `↑`/`↓`; menus cannot both be open.
10. **Invariant 9**: every row of §8's audit table has a working keyboard
    binding, verified by a script test that performs each action with
    keys only under `YEW_MOUSE=0` and reaches the same state as the mouse
    path.
11. `NO_COLOR=1` goldens contain **zero** colour SGR parameters
    (`grep -c '38;2\|38;5\|\[3[0-7]m\|\[9[0-7]m'` → 0) while remaining
    legible (active/selected still distinguishable — asserted by an
    attribute check, not by eye); the 16-colour and `YEW_ASCII=1` variants
    exist for every element in §7's checklist.
12. Chrome review complete: all 12 goldens in §7 exist, ×4 variants, and
    two consecutive full pty runs are byte-identical (invariant 5).
13. A 1 000-event motion burst produces ≤ 1 render and zero allocations
    in `mouse.c`; fuzz run (100 000 events × 4 seeds) leaves no stuck
    drag state.
14. Deferred surfaces hard-error or are provably inert naming their
    sprint: document context menu (post-1.0, right-click on a pane does
    nothing), F-mode mouse (52), LSP hover (47), tab-into-pane (post-1.0).
15. ≥ 140 assertions across the four unit files.
