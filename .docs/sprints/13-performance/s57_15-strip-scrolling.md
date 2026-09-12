# Sprint 57.15: Strip Scrolling — Chevron Correctness and Hover Reveal

## Prerequisites

- Sprint 22/23 — `yew_strip_layout` (`src/ui/strip.c`), the one placement
  engine for both rows; `StripSpan`; the layout/hit-test identity law.
- Sprint 24 — row 2 and its independent `member_scroll`.
- Sprint 27 — `YEW_REGION_TAB_SCROLL` with its ±1/±2 payload convention,
  `strip_scroll` in `src/ui/mouse.c`, the drag autoscroll.
- Sprint 57.13 — mode 1003 arming (`yew_tty_mouse_motion`), the no-button
  motion path in `mouse_motion`, `overlay_dirty` and the overlay-only repaint.
- Binding: invariants 4 (5 ms input budget), 5 (deterministic render), 9.

## Goals

Two defects in one subsystem.

**The chevron does not scroll.** `yew_strip_layout` keeps the active entry
visible by walking the first visible entry until the active one fits, then
writes that value back through its `int *scroll` parameter
(`src/ui/strip.c:122`). An explicit scroll is therefore overwritten on the
very next render, and the strip snaps back. It appears to work only when the
active entry is already adjacent to the chevron, which is exactly the
reported symptom. The follow-the-active behaviour is correct when the ACTIVE
TAB CHANGES; it is wrong as a permanent clamp.

**There is no hover reveal.** Pointing at a chevron does nothing. Hovering
should slide hidden entries into view, on row 1 and on row 2.

Deferred, named: smooth/animated scrolling, scrollbar chrome, wheel
acceleration, click-and-hold repeat beyond the existing drag autoscroll.

## Deliverables

### 1. Explicit scroll survives the render — `src/ui/strip.c`, `src/ui/tabs.h`

Separate "follow the active entry" from "the user chose this offset".

`yew_strip_layout` gains a parameter distinguishing the two, or the caller
stops passing `active` when the offset is user-owned — pick one and state
why in the header. The rule to implement:

- When the ACTIVE ENTRY CHANGES (switch, close, open, group enter/leave),
  the strip scrolls minimally to reveal it — today's behaviour, unchanged.
- When the user scrolls explicitly (chevron click, wheel, hover reveal), the
  offset is theirs and the layout MUST NOT walk it back, even when the active
  entry is off-screen. An off-screen active tab is a legitimate view.
- Any event that changes the active entry clears the user-owned flag, so the
  strip resumes following.

The flag lives beside `Tabs.scroll` / `Tabs.member_scroll` (one per row —
the two rows scroll independently and s24 made that a law).

**The pitfall:** `*scroll` is written back by the layout, so every caller
sees the clamped value. Do not add a second clamp at the call sites; there is
one placement engine and it stays that way.

### 2. Chevron hover reveals — `src/ui/mouse.c`, `src/ui/tabs.c`

Hovering a chevron slides entries into view on that row.

- No-button motion over a `YEW_REGION_TAB_SCROLL` region scrolls that row by
  one entry every `YEW_HOVER_SCROLL_MS = 300`, in the payload's direction
  (±1 row 1, ±2 row 2), while the pointer stays on it.
- Driven by the TIMER HEAP through `yew_mouse_deadline` / `yew_mouse_tick`,
  exactly as the drag autoscroll is. Never a per-render counter: the render
  is a pure function of state and `now_ms` (invariant 5).
- Leaving the chevron stops it immediately and cancels the pending deadline.
- Reaching either end stops it; the chevron stops being drawn and the region
  disappears, which must not strand a scheduled tick.
- This sets the user-owned flag from §1, so the reveal is not walked back.

**Mode 1003 is the cost here, and it must be decided explicitly.** Today
motion reporting is armed ONLY while a context menu is open, so a chevron
hover produces no events at all. Arm it whenever a strip chevron is drawn
(`more_left || more_right` on either row), and disarm when neither row shows
one and no menu is open. Two consequences to honour:
  - Arming and disarming must be idempotent and must not fight the menu's own
    arming — route both through one owner so a menu closing does not disarm a
    strip that still wants motion, and terminal restore still always emits
    `1003l` (invariant 6).
  - `tests/perf/mouse.c` asserts 1000 motion events cost 0 renders with no
    menu open. That gate must keep passing with the strip armed: a motion that
    is not over a chevron, and a motion that does not cross a scroll edge,
    must not repaint.

### 3. Keyboard parity (invariant 9)

No new commands. `ed.tab.next` / `.prev` scroll the strip by moving the
active entry, which is the existing audit-table row for the chevrons.

## Testing Strategy

- **Unit (`tests/unit/test_strip.c`)**: an explicit offset survives a render
  whose active entry is off-screen; changing the active entry clears the flag
  and re-reveals; the minimal-adjustment walk is unchanged when following;
  both rows keep independent offsets.
- **Unit (`tests/unit/test_mouse.c`)**: a chevron click scrolls by one and
  STAYS (the regression this sprint exists for) with the active tab far away
  and at both ends; wheel over the strip likewise; hover scrolls one entry per
  300 ms and stops on leaving; the pending tick is cancelled when the chevron
  disappears; 1003 is armed when a chevron is drawn and disarmed when neither
  row has one and no menu is open; a menu closing does not disarm a strip that
  still needs motion.
- **PTY**: `s57_15_chevron_click_scrolls` (click with a distant active tab,
  snapshot proves it stayed), `s57_15_chevron_hover_reveals` (drive `now_ms`
  deterministically across two reveal steps — no wall-clock sleep).
- **Perf**: `tests/perf/mouse.c` — the existing 0-render/1000-motion gate must
  pass with the strip armed; add a case for 1000 motions parked ON a chevron
  asserting at most one render per reveal step.
- **Fuzz**: `fuzz_mouse` gains the assertion that 1003 is armed if and only if
  a chevron is drawn or a menu is open.

## Definition of Done

1. A chevron click scrolls the strip and the strip stays scrolled, regardless
   of which tab is active, on both rows.
2. The strip still follows the active entry when the active entry changes.
3. Hovering a chevron reveals hidden entries at a steady cadence and stops on
   leave and at the ends.
4. Mode 1003 is armed exactly when a chevron is drawn or a menu is open, and
   the terminal is always restored.
5. The perf gate holds: motion with nothing to reveal costs no render.
6. gcc and clang warning-free; unit, ASan/UBSan, PTY, fuzz-mouse, perf-mouse
   green; `MODULES=""` builds.
