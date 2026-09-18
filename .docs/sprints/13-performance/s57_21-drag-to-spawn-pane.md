# Sprint 57.21 — Drag a tab to an edge to spawn a pane

## The gesture

Drag a tab (row 1 or row 2) and release it over the **edge zone** of a
pane. The pane splits and the new leaf opens the DRAGGED tab's buffer.

| Release zone | Split | New pane lands |
|---|---|---|
| right edge  | `SPLIT_V` | right |
| left edge   | `SPLIT_V` | left  |
| bottom edge | `SPLIT_H` | below |

No top edge: the strip is there, and a tab released upward is already
the row-1 reorder gesture.

Release anywhere else over a pane still CANCELS, exactly as today.

## The decision this sprint makes, and why

**The dragged tab STAYS in the strip.** VSCode moves the tab out of its
group because its model is groups-on-top-containing-tabs. yew's model is
inverted — `Tab` owns `Pane *root`, so every tab has its own pane tree —
and there is no "group" for a tab to leave. Removing the tab would be a
destructive, un-undoable side effect of a drag (invariant 1), and it is
not what the gesture needs: showing one buffer in two windows is already
a first-class shape here, because `yew_pane_split` clones a Win onto the
SAME buffer. So the split shows the dragged tab's buffer and the strip is
untouched.

Concretely: active tab A, drag tab B to the right edge → A's pane tree
splits, the right leaf shows B's buffer, B is still a tab. Closing the
pane restores the previous layout and loses nothing.

## §1 Which leaf splits

The leaf **under the pointer at release**. At the right edge of a
single-pane tab that is the only leaf; in a multi-pane tab it is the leaf
whose own edge you dropped on, which is what "split this editor space"
means once there is more than one. Resolve it through the region
registry's `YEW_REGION_PANE` payload and `yew_pane_leaf_by_index` — never
by walking the tree from coordinates a second time.

## §2 Edge-zone geometry

Deterministic and stated once, in one function, used by BOTH the hit test
and the affordance render. Two readers of one rule, never two rules.

    depth = clamp(dimension / 5, 3, 12)   /* cells, integer division */

`dimension` is the leaf's content width for the left/right zones and its
content height for the bottom zone. A zone EXISTS only when
`yew_pane_split` would succeed for it — the leaf cap
(`YEW_PANE_MAX_LEAVES`) and `split_fits` are asked BEFORE the zone is
offered, so a zone the user can see is always a zone that works. A pane
too small to split shows no zone and the drop cancels as it does today.

Left and right zones cannot overlap: if `2 * depth >= width` the leaf is
too narrow and neither vertical zone is offered.

## §3 The split placement primitive

`yew_pane_split` always puts the clone in child `b`. Left and top
placement needs the new leaf in `a`. Add

    Pane *yew_pane_split_side(Ed *ed, Pane *leaf, SplitDir dir,
                              bool new_first);

and make `yew_pane_split(ed, leaf, dir)` delegate with `new_first =
false`, so no existing caller changes and no existing behaviour moves.

`state_token` must follow the OLD window, whichever child it lands in —
the retained record belongs to the window that was already there. Getting
this backwards silently hands a restored pane the wrong scroll position.

## §4 The affordance

While a drag is in flight and the pointer is in a live edge zone, draw
the region the new pane would occupy as a highlight. Drawn, never
registered — the same law the drag float already follows
(`yew_strip_float_rect`).

Invariant 5: same state plus the same `now_ms` renders byte-identically.
No animation, no pulse, no time-varying alpha.

## §5 Keyboard reach (invariant 9)

The gesture is mouse-only by nature, so the CAPABILITY ships with a
keyboard path or it does not ship:

- Commands `ed.tab.split_left`, `ed.tab.split_right`,
  `ed.tab.split_down`, each opening a chosen tab's buffer in a new pane
  on that side. `YEW_CMD_RECORDABLE`; unique motion words; `gen_cmds[]`
  entries or named `D(...)` exclusions.
- Rows on the TAB context menu ("Open in split right/left/below") through
  `ui/ctxrows.h`'s table — not a switch in `mouse.c`.
- Verify the context menu is genuinely keyboard-navigable. If it is not,
  default bindings are required instead; say which you found.

## §6 What must not regress

- `test_mouse_tab_dropped_on_a_pane_cancels` is NARROWED, not deleted:
  a release over a pane's INTERIOR still cancels. Keep it passing and add
  the edge cases beside it.
- `mouse.h`'s "DRAGGING A TAB INTO A PANE → post-1.0" note is now partly
  false. Rewrite it to say what shipped and what did not (dropping onto a
  pane's interior to open there is still post-1.0).
- Nothing in `Tabs.v` mutates during a drag. The split happens at
  RELEASE, after the drag state is torn down, and a refused split leaves
  the layout exactly as it was.
- A drag cancelled by Esc, by a tab-count change, or by focus-out spawns
  nothing.

## Definition of done

- Unit: zone geometry at both boundaries and off-by-one either side; each
  of the three sides placing the new leaf on the correct side with the
  correct buffer; `state_token` following the old window; refusal at the
  leaf cap; refusal when too narrow/short; interior release still
  cancelling; Esc mid-drag spawning nothing; a multi-pane tab splitting
  the leaf under the pointer.
- PTY: a golden for the affordance mid-drag and one for the resulting
  layout after each of the three drops.
- Lanes: unit, clang, `SAN=1` filtered, `MODULES=""`, `test-pty` full,
  `test-fletch`, `test-script`, roundtrip + coverage gate, `test-audit`,
  `scripts/check-cmd-dispatch.sh`, `bans.sh`, `check-input.sh`.
- `perf-*` unaffected: the zone test runs only while a drag is live.
