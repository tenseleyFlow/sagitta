# Sprint 57.14: Tab Drag Feel

## Prerequisites

- Sprint 22 — cell-based layout and the clickable-region registry. Draw and
  hit-test geometry are one fact, produced in the same render pass. A float
  that is drawn but never registered is the deliberate exception, and it is
  the only one: it is decoration the pointer is already carrying, so there is
  nothing under it to click.
- Sprints 23 and 24 — stable tab ids, the one/two-row strip, the row-1 entry
  list (`yew_tab_row1_entries`), group membership, and `yew_group_remove_member`
  with its auto-dissolve.
- Sprint 27 §4 — the drag machinery this sprint extends: `apply_drag_preview`,
  the pre-drag slot table (`yew_strip_slot_at` / `yew_strip_pre_payload` /
  `yew_strip_tail_x`), `drag_strip_motion`, `drag_strip_drop`,
  `drop_into_group`, `drop_target_row2`, `drag_dwell`, `yew_mouse_tick` and
  `yew_mouse_deadline`.
- Sprint 57.8 — the modern strip and the tail ` + ` control, which is drawn
  ONLY in the `else if (draw_new && …)` arm of `strip_render`. That arm is
  unreachable when `more_right` is true, which is the lockout Deliverable 1
  exists to fix.
- Sprint 57.13 — the router is the one place a mouse event becomes an action,
  and `src/ui/mouse.c` may not allocate (`tests/perf/mouse.c` reads its source).
- Binding plans and invariants 4, 5 and 9. In particular: the flash is a pure
  function of `(ed->now_ms − dwell_since_ms)` so that the same state renders
  byte-identically (invariant 5); the loop is woken at each phase edge rather
  than spun (invariant 4); and nothing here is the only way to do anything —
  every gesture has its Sprint 24 keyboard command (invariant 9).

## Goals

Make a tab drag feel like carrying something. The held entry stops being a dim
ghost sitting in the strip and becomes a float drawn at the pointer, with the
strip holding its place open as a gap that slides as the target changes. Row 1
becomes a uniform group EXIT — dropping a member on any row-1 slot takes it out
of its group and lands it there — which repairs a real lockout: row 1's blank
tail was the only exit, and an overflowing row 1 has no tail (the ` + ` / tail
arm only draws when `more_right` is false). And the dwell drops from 400 ms to
250 ms with a two-flash cue on the hovered group's entry, so a pause that is
about to open a group announces itself instead of appearing to hang.

Joining a group stays row-2's business (the dwell, then a drop on the member
strip) — unchanged. Dragging a tab into a PANE stays post-1.0, as Sprint 27 §9
filed it; a release over a pane still cancels.

## Deliverables

### 1. Row 1 is the group exit — `src/ui/mouse.c`

`drag_strip_drop` currently removes a held tab from its group only on the blank
tail. It must do so for EVERY row-1 slot target: dropping a member anywhere on
row 1 means "leave the group and live here".

```c
/* `to` is already resolved; this is the removal and the move. */
static void drop_out_of_group(Ed *ed, int to);
```

The order is the whole deliverable:

1. Resolve the target slot to a TAB INDEX with `slot_to_tab_index` — against
   the pre-drag slot table, while the group still exists.
2. `yew_group_remove_member(ed, from)` when the held tab is grouped.
3. RE-DERIVE `from` from `drag_tab_id` and clamp the destination to
   `[0, yew_tab_count − 1]`, then `yew_tab_reorder`.

Step 3 is not defensive noise. `yew_group_remove_member` DISSOLVES a group whose
last member just left, which deletes a row-1 entry and renumbers every slot to
its right — so a slot number resolved after the removal names a different thing,
and the only value that survives is the tab index resolved in step 1. Tab
indices themselves do not move (dissolve rewrites `group_id` and edits the
groups vector, never `Tabs.v`'s order), which is exactly why resolving to an
index is what makes step 1 safe.

Membership changed by a drop sets `ed->layout_dirty`, not merely
`ed->full_damage`: the active tab leaving a group — or its group dissolving —
takes row 2 away, and that is a strip-row count change the layout owns.

Unchanged: a drop on row 2 still JOINS (`drop_target_row2` → `drop_into_group`);
a group drag still moves its block; the blank tail still works where it exists.

### 2. The float and the gap — `src/ui/tabs.c`, `src/ui/mouse.c`

```c
/* mouse.h — the held entry, and where the pointer has it. */
bool yew_mouse_drag_float(const Ed *ed, i32 *payload, u16 *x, u16 *y,
                          u16 *grab_dx);
/* tabs.h — the cells the last render's float covered; w == 0 when none. */
Rect yew_strip_float_rect(void);
```

- The insertion shift is `apply_drag_preview`, kept as it is: it permutes the
  entry list so the entries the held one passes keep their relative order. What
  changes is that the permuted-in entry is no longer DRAWN. `strip_render`
  takes a `held_idx` and, for that one span, skips `yew_grid_puts` and skips
  `yew_region_add` while still recording the pre-drag slot cells. The row was
  already blanked, so the span reads as a gap of exactly the held entry's width,
  sitting where the drop would land.
- Row 2 hides the held member the same way, by payload, with no permutation:
  a member dragged off row 2 must not be in two places at once either.
- The float is drawn LAST in `yew_tab_strip_draw`, after both rows, so it is
  never overwritten by the row the pointer happens to be on. Its label is built
  from the payload — ` basename♦ ` for a tab, ` group ` for a group — without
  the row number, because a float is off-list and a number would name a position
  it is not in.
- `grab_dx` is `press_x − press_rgn.rect.x`: the float keeps the grip the press
  established instead of snapping its left edge to the pointer. Clamped into the
  grid on both axes.
- THE FLOAT REGISTERS NO REGION. It is drawn from `yew_grid_puts` and nothing
  else; `yew_strip_float_rect` exists so a test can hit-test every cell it
  covers and prove the registry never answers for it.
- A drag repaints when the pointer changes CELL, and on release. Not per motion
  report: a terminal emits as many of those per cell as it likes, and one
  repaint each is the slideshow §3's hover rule already refuses. The release
  repaints even when the drop changed nothing — putting the button down is what
  takes the float off the screen.

The drag's TARGETING is unaffected: it reads the pre-drag slot table, which is
recorded for the held slot exactly as before.

### 3. A 250 ms dwell with a two-flash cue — `src/ui/mouse.c`, `src/ui/tabs.c`

```c
enum {
    YEW_DRAG_DWELL_MS = 250,
    /* Derived, so the two can never disagree: on, off, on, then open. */
    YEW_DRAG_FLASH_MS = YEW_DRAG_DWELL_MS / 4
};

/* The group whose row-1 entry is flashed AT ed->now_ms; 0 when none. */
u32 yew_mouse_dwell_flash(const Ed *ed);
```

- Phase is `(ed->now_ms − m->dwell_since_ms) / YEW_DRAG_FLASH_MS`, and the cue
  is ON for phases 0 and 2. Never a per-render counter: the same state plus the
  same `now_ms` must render byte-identically (invariant 5), and a counter makes
  the picture depend on how many times the screen happened to be painted.
- The renderer toggles `YEW_ATTR_REVERSE` on the flashed entry rather than
  substituting a style, so the cue is visible whether or not the group is the
  active entry, and no new theme role is needed.
- `yew_mouse_deadline` returns the NEXT phase edge — `dwell_since + k·FLASH`
  for k in 1..3, then `dwell_since + DWELL` — so the loop sleeps to each edge
  instead of spinning. Edges that would change nothing are not scheduled.
- `yew_mouse_tick` marks `full_damage` when the phase it computes differs from
  the last one it saw (`m->flash_phase`). That field is a DAMAGE hint only; the
  picture is still computed from the clock, so a missed wake-up costs a late
  repaint, never a different frame.
- The open still happens at `>= YEW_DRAG_DWELL_MS` in `yew_mouse_tick`, and a
  drag that merely passes over a group still opens nothing.

### 4. Defer

- Dragging a tab INTO A PANE stays post-1.0 (Sprint 27 §9). A release over a
  pane cancels, as `test_mouse_tab_dropped_on_a_pane_cancels` asserts.
- Dropping a group onto another group (nesting) does not exist and is not
  invented here.
- Chevron hover reveal, and making an explicit strip scroll survive the
  follow-the-active clamp, are Sprint 57.15's. This sprint does not touch
  `yew_strip_layout` or mode 1003.

## Testing Strategy

Unit (`tests/unit/test_drag.c`, registered in `tests/unit/registry.c`):

- `drag_row1_slot_extracts_a_member_from_its_group` — a member dropped on a
  row-1 slot is ungrouped and sits at that position.
- `drag_extracting_a_sole_member_onto_a_slot_to_its_right` — the group
  DISSOLVES under the drop, a row-1 entry disappears, every slot to the right
  renumbers, and the tab still lands where the pre-resolved index said.
- `drag_row1_exit_works_when_the_strip_overflows` — the lockout: a narrow grid
  with `more_right` true, so no tail exists, and the exit still happens.
- `drag_float_is_drawn_and_registers_no_region` — the float's cells carry its
  label in the grid and `yew_region_hit` answers nothing over them.
- `drag_held_entry_leaves_a_gap_in_the_strip` — the held entry is absent from
  the strip and no TAB region carries its payload, while the pre-drag slot table
  still does.
- `drag_dwell_opens_a_group_at_250ms_and_not_at_249` (renamed from the 400 ms
  row) and `drag_dwell_flashes_twice_before_opening` — the phase table below,
  driven by `ed->now_ms`, never by a sleep.
- `drag_dwell_flash_marks_damage_only_at_an_edge` — one tick per millisecond
  across the whole dwell marks damage exactly three times.
- `drag_reports_a_deadline_at_every_flash_edge` — the deadline sequence
  62/124/186/250 and −1 once opened.

| elapsed (ms) | flash | why |
|---|---|---|
| 0 … 61 | on | the cue starts immediately |
| 62 … 123 | off | first gap |
| 124 … 185 | on | second flash |
| 186 … 249 | off | settle before the open |
| 250 | — | the member strip opens; the cue is done |

Regression: the whole `drag`, `mouse`, `tabs`, `strip`, `groupnav` and
`degrade` filters, plus `test_invariant9` (every gesture still has its keyboard
twin) and `tests/unit/test_theme.c` (no new role, so the list is unchanged).

PTY: `chrome_drag` and its `nocolor` / `colors_16` / `ascii` siblings are
re-recorded — they snapshot a drag mid-gesture, which is now a gap plus a
float — and so is `s27_dwell_opens_member_strip`, whose row 1 loses the held
entry the same way. No golden may capture a dwell IN FLIGHT: the flash is a
function of the clock, and a golden of it would be a timing race. That case is
safe because it settles 500 ms, well past the open, and the cue stops when the
member strip it was announcing appears.

Fuzz: `make fuzz-mouse` — the float and the exit path take no new allocation
and must survive the existing corpus.

Perf: `make perf-mouse` — the 1000-event burst budget is unchanged, and the
router's source still contains no allocation token.

## Definition of Done

1. A member dropped on ANY row-1 slot leaves its group; with the strip
   overflowing and no tail drawn, the exit still works.
2. The destination is resolved to a tab index before the removal, and a
   dissolving group cannot misplace the drop — proved by the sole-member test.
3. The held entry is drawn exactly once, at the pointer, and the strip holds a
   gap of its width at the target slot.
4. The float registers zero regions; `yew_region_hit` over its cells is
   unchanged by it.
5. `YEW_DRAG_DWELL_MS` is 250 and the cue flashes exactly twice, computed from
   `ed->now_ms − dwell_since_ms`.
6. `yew_mouse_deadline` wakes the loop at every phase edge; no polling and no
   sleep-driven test.
7. gcc and clang build warning-free under `-std=c11 -pedantic -Wall -Wextra
   -Werror`; ASan/UBSan, `MODULES=""`, perf-mouse, fuzz-mouse and the full PTY
   suite pass.

## Closeout — REPOSITORY COMPLETE 2026-09-12

All three deliverables shipped on branch `tab-drag-feel`.

- Row 1 is the exit: `drop_out_of_group` resolves the destination to a tab
  index from the pre-drag slot table BEFORE the removal, then re-derives `from`
  from the tab id and clamps. The sole-member test drops onto a slot to the
  group's right, the group dissolves under the drop, and the tab still lands
  where it was aimed.
- The float is `strip_draw_float`, drawn last in `yew_tab_strip_draw` from
  `yew_grid_puts` and nothing else; `yew_strip_float_rect` lets the test
  hit-test every cell it covers and find `YEW_REGION_NONE`.
- The dwell is 250 ms with quarters at 62/124/186; the cue is a pure function
  of `ed->now_ms − dwell_since_ms`, and `yew_mouse_deadline` returns each edge.
  `m->flash_phase` marks damage only, three times across the whole dwell.

Lanes: gcc and clang `-Werror` clean; `unit 2538/0`; `MODULES="" 2050/0`;
`SAN=1 2520/0`; `pty 468 cases, 0 mismatches`; `perf-mouse` burst 0.007 ms and
`router_allocations=none`; `fuzz-mouse` four seeds; `fletch 38/38`;
`script 93/0`; check-input, bans, check-cmd-dispatch, check-sigsafe ok.

Goldens re-recorded: `chrome_drag`, `chrome_drag_nocolor`,
`chrome_drag_colors_16`, `chrome_drag_ascii`, `s27_dwell_opens_member_strip` —
each loses the ghost entry from row 1 and gains the float at the pointer.

## Field repair — 2026-09-12

Two defects found dogfooding the shipped branch against a real workspace
(`1 untitled`, the directory groups `ch4/` and `ch5/`, and a loose `.lu`
tab). Both were reproduced by a failing test before either was touched.

### The dwell preview stuck to the first group it opened

`preview_gid` was assigned in exactly one place — `yew_mouse_tick`'s open
— and nothing ever put it back to 0 short of the gesture ending. So the
pointer could walk off a group and row 2 kept listing the group it had
left; and because both `yew_mouse_dwell_flash` and the open stand down
while `preview_gid == dwell_gid`, coming BACK to that group announced
nothing and re-opened nothing, which is the "I don't see the double
flash" half of the report.

`drag_dwell` now retracts the preview when the pointer is on ROW 1 and
that row's target is not the previewed group — `layout_dirty`, because a
strip-row count change is the layout's. Only from row 1: row 2 is the
preview's own surface and the place the join is aimed at, so the pointer
arriving there must not close the strip it was sent to use.

- `drag_dwell_preview_retracts_when_the_pointer_leaves`
- `drag_dwell_reopens_a_group_the_pointer_returns_to`
- `drag_dwell_preview_survives_the_pointer_on_row_2`
- `drag_dwell_flash_is_visible_on_the_active_group_entry` — the cue was
  running all along and the XOR does not cancel on the active entry; the
  invisibility was the stuck preview suppressing it.

### The neighbours did not get out of the carried tab's way

The float is drawn at `press_x − press_rgn.rect.x` BEHIND the pointer
(§2's grip), but `drag_strip_motion` resolved its target from the bare
pointer cell. Grab a wide tab near its right edge and the entry being
carried sits squarely on top of its neighbour while the strip insists
nothing has happened — the neighbour only moves once the POINTER has
crossed, a whole tab-width later, and then the row jumps.

`drag_target_slot` answers from the cells the carried entry covers:
its leading edge is `col − grab_dx`, clamped into the strip, and it
changes places with a neighbour once it has travelled half that
neighbour's width over it. That threshold is also what makes the answer
stable — the swap lands the carried entry exactly on the cells that
justified it, so the reverse test cannot fire at the same pointer
position and the preview cannot oscillate. Every cell of row 1 now
resolves an insertion point, including the cells left of the first entry
that answered nothing before; the blank tail is checked first and keeps
its own meaning.

`yew_strip_slot_cells` (tabs.h) exposes a slot's cell range, returning
false for a slot the layout scrolled off, which is what stops the search
at the edge of what is drawn.

- `drag_neighbours_slide_out_of_the_carried_tabs_way` — the reproducer:
  the pointer is still inside the tab's own slot and the carried entry
  is already over the second group past its midpoint.
- `drag_every_previewed_gap_is_where_the_drop_lands` and
  `drag_the_dogfood_strip_reorders_at_every_step` — the reported strip
  swept cell by cell, right to left, each position on its own fixture:
  the target only ever moves left, the gap is where the release lands,
  and the entries the carried one passed keep their order.

Unchanged: `drag_to_valid` is still set only from row 1, so a release
over a pane still cancels; the drop still resolves through
`slot_to_tab_index` against the pre-drag table.

Lanes: gcc and clang `-Werror` clean; `unit 2545/0`; `MODULES="" 2056/0`;
`SAN=1 2526/0`; full PTY green with no golden re-recorded; `perf-mouse`
burst 0.008 ms and `router_allocations=none`; `fuzz-mouse` four seeds;
check-input, bans, check-cmd-dispatch, check-sigsafe ok.

## Field repair — 2026-09-12 (second round)

Four more defects from dogfooding the merged `tab-drag-feel` +
`strip-scrolling` branches. Each was reproduced by a failing test
before anything was touched.

### The dwell was too eager — 250 ms → 500 ms

250 ms opened a group under a pointer that was only travelling past
it: the strip grew a row, the layout gave one away, and the picture
moved under a gesture that had not asked for anything.
`YEW_DRAG_DWELL_MS` is 500 and `YEW_DRAG_FLASH_MS` is still the
derived quarter, so the two flashes SPREAD across the longer window
instead of finishing early. Quarters are 125/250/375.

- `drag_dwell_opens_a_group_at_500ms_and_not_at_499` — renamed from
  the 250 ms row, and it now `_Static_assert`s the numbers its name
  promises, so a future retune fails to compile rather than lying.
- `dwell_quarter`'s clamp is unreachable at 500 (4·FLASH is exactly
  the dwell) and stays: 250 rolled into a fifth, lit quarter at 248,
  and the next tuning pass must not have to rediscover that.
- The `s27_dwell_opens_member_strip` PTY case settles 900 ms rather
  than 500 — a golden recorded on the dwell's own edge is a race.

### A group entry fought the pointer trying to rest on it

"Tab groups should have some space for allowing hover over without
shifting... currently the tab group wants to move left/right of where
I am hovering because it is assuming I want to shift left right, not
hover."

The half-width swap and the dwell wanted opposite things from the same
cells. `swap_threshold` (src/ui/mouse.c) keeps the half-width rule for
a plain tab and gives a GROUP entry a central dead band: its middle
half shifts nothing, its outer quarters are ordinary swap triggers.

The split is the outer quarter each side. The reported workspace's
group labels run 8 to 18 cells, so a quarter is two to four cells —
wide enough that a resting hand stays inside it, narrow enough that a
drag aiming past the group crosses it without a detour. The band is
CENTRED so the two directions are mirror images rather than two rules,
and below four cells there is no room to divide and the half rule
stands.

- `drag_a_group_entry_holds_still_inside_its_hover_band`
- `drag_a_group_entry_holds_still_coming_from_the_right`
- `drag_a_plain_neighbour_still_swaps_at_half_its_width` — the promise
  that nothing else moved.

Only WHERE the target changes moved; what a target means did not, so
`drag_every_previewed_gap_is_where_the_drop_lands` still holds.

**What the band broke, and the second half of the fix.** `drag_dwell`
read the REORDER TARGET's pre-drag payload, so "resting on a group"
and "the target is the group" had been the same fact. The band makes
them different on purpose — and then the dwell stopped seeing the
group the user had come to rest on, which would have made the one
place a tab can join a group the one place the pointer is not allowed
to linger.

`drag_dwell_slot` answers the other question: the slot the carried
entry OVERLAPS MOST. Overlap rather than a single cell — a midpoint or
a leading edge — because the carried entry and the entry under it are
both a dozen cells wide and either edge can be over a neighbour while
the bulk of the entry is not. Once the target HAS moved it gives
today's answer, since the gap is then drawn on the entry's old cells
and overlaps them entirely. Ties keep the leftmost, so the answer is a
function of state and not of loop order (invariant 5). `carried_span`
now holds the grip arithmetic the two readers share.

- `drag_resting_in_a_group_band_still_opens_it` — the carried tab
  parked at the band's far edge: nothing shifted AND the dwell is
  counting, then the strip opens at 500 ms. Both halves, because
  either alone is the bug.

### A row-2 chevron scrolled row 1

"If I hover a chevron in a tab group (row 2 chevron) expecting tabs
hidden right to appear in the row 2 space... what actually happens is
row 1 slides left."

The HOVER reveal was innocent — `chevron_at` reads the payload's
magnitude for the row and its sign for the direction, and a hover on
row 2's `>N` scrolls row 2. The DRAG autoscroll had its own copy of
that read (`drag_over_chevron`, src/ui/mouse.c:2213) which kept the
sign and dropped the magnitude, then named the row itself:
`strip_scroll(ed, false, delta)` at src/ui/mouse.c:2344. Every chevron
in the editor autoscrolled row 1, row 2's included.

`drag_over_chevron` is gone; `chevron_at` is the one reader of the
±1/±2 convention for both clocks. `strip_scroll`'s row-2 limit also
came from `yew_active_group_id`, while row 2 may be showing the
DWELL PREVIEW group — `row2_group` now states that rule once, and
`drop_target_row2` reads it too.

- `mouse_drag_autoscroll_moves_the_row_under_the_pointer` — the
  reproducer, asserted as a PAIR (row 2 moved, row 1 did not): before
  the fix `tabs.scroll` went to 1 and `member_scroll` stayed at 0.
- `mouse_row2_chevron_hover_scrolls_row_2_and_not_row_1` — the hover
  half, which passed all along and pins it.

### Row 2 accepted a drop and showed nothing about it

"When hovering drag in a tab group, the row 2 tabs don't shift like we
just spent time getting the row 1 tabs shifting left/right."

`drag_target_slot` resolved only against row 1's slot table, so row 2
had a drop target and no preview — the picture and the outcome
disagreed. Row 2 now gets row 1's whole treatment:

- `strip_row2` (src/ui/tabs.c), a second slot table filled by the same
  `strip_render`, which takes a `StripSlots *` instead of a
  `record_slots` flag. Cells only: row 2 never un-permutes a payload
  the way the dwell does on row 1.
- `drag_target_member` resolves the target from the carried tab's
  LEADING edge by the same half-width rule — no dead band, because row
  2 holds members and a member is a plain tab. Seeded from where the
  carried entry already sits (`group_ordinal − 1` on first arrival,
  since `yew_group_members` orders by ordinal), or by a plain insertion
  scan when the tab is joining from outside.
- `yew_tab_member_strip_draw` permutes the entry for a member being
  reordered and INSERTS one for a joiner — the list it lands in is one
  longer than the one on screen — then hides it, which is the gap.
  Labels are built after the permutation so the digits count what row
  2 shows.
- `drop_target_row2` reads that target rather than hit-testing the
  release column: the drawn row is permuted by the preview, so a fresh
  hit-test would answer "you are over the thing you are holding".

WHICH ROW OWNS THE PREVIEW: the row the pointer is on. Row 1 while it
is on row 1, row 2 while it is on row 2, never both — leaving row 2
clears it in the same motion, so the members close back up.

- `drag_row2_opens_a_gap_where_the_member_will_land`
- `drag_moving_between_rows_hands_the_preview_over` — both directions.
- `drag_row2_previewed_gap_is_the_ordinal_the_drop_commits` — every
  column of the row on its own fixture, asserting the gap the frame
  drew equals the ordinal the release committed, and that all three
  positions were actually reached.
