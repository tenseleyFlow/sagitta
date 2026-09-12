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
