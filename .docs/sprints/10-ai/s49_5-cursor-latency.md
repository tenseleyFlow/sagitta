# Sprint 49.5: Cursor Latency Regression Gate

**COMPLETE 2026-08-20.** Stable hybrid-number cursor motion now redraws the
gutter and footer without revisiting document text.  The exact nine-line Wolf
case is committed as a 2,000-arrow PTY p99 gate (32.7 us observed against the
5 ms limit), its injected-delay self-test fires, the structural repaint unit
test passes, the complete PTY corpus is green, and `make perf-syn` passes
without changing the persistent editor log.

## Why this subsprint exists

Sprint 50 does not begin while ordinary navigation has an unexplained latency
regression.  The reported reproduction is:

```sh
./build/yew ~/scratch/wolf/starting/hello.lu
```

The file is nine lines and 138 bytes.  A traced PTY run puts yew's arrow
read-to-frame-write path below 0.3 ms, but the default hybrid line-number mode
still turns every vertical cursor move into `yew_ed_damage_document()`.  On a
larger Makefile that path is roughly twelve times slower than absolute line
numbers.  It also violates `00-decisions.md`: damage-tracked rendering is
O(damage), never a full document repaint per keystroke.

## Goals

1. Keep the distance-to-cursor hybrid line numbers as the default.
2. Repaint only the gutter cells whose relative values change when the cursor
   moves without scrolling; do not redraw document text or re-query syntax.
3. Put idle up/down arrows on a tiny native-Wolf file under the committed
   keypress-to-paint p99 gate.
4. Keep syntax performance runs from appending intentional degradation probes
   to the user's persistent editor log.

## Deliverables

- Split viewport changes from cursor-line-only changes in `yew_ed_render`.
  A viewport change may damage document rows.  A relative/hybrid cursor-line
  change with a stable viewport draws the focused window's gutter only and
  marks the footer dirty.
- Extend `tests/perf/latency.c` with a nine-line `hello.lu` fixture and an
  alternating CSI up/down measurement.  Record and gate
  `cursor_arrow_to_paint_p99` at the locked 5 ms limit.
- Make `perf-syn` discard routine benchmark logging locally.  It must not grow
  `$XDG_STATE_HOME/yew/log`; production logging behavior is unchanged.
- Preserve deterministic grids, hybrid-number semantics, selection overlays,
  scrolling, wrapping, panes, and shadow rendering.

## Definition of Done

1. The exact tiny-Wolf arrow interaction has a committed PTY p99 gate at
   `<= 5,000,000 ns` and the gate's injected-delay self-test fails as intended.
2. Stable-viewport relative/hybrid cursor motion does not call
   `yew_ed_damage_document`; it updates the gutter and footer only.
3. Absolute and disabled line-number modes retain their existing damage paths.
4. Viewport movement, wrapped views, selections, multiple panes, and a live
   shadow remain correct under unit/PTY coverage.
5. `make perf-syn` leaves the normal persistent yew log untouched.
6. Focused tests and the short CI lane are green under the repository's strict
   C11 warning policy.
