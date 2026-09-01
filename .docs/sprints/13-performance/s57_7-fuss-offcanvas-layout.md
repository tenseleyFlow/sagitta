# Sprint 57.7: FUSS Off-Canvas Layout

## Prerequisites

- Sprint 22 — cell-based pane layout, `Rect`, pane-tree geometry, and the
  rule that drawing and hit-testing consume one computed layout.
- Sprints 23 and 24 — one/two-row tab strips and tab/group regions.
- Sprint 56.5 — FUSS as transient mode state that never owns or serializes a
  pane tree, plus explicit tab/split opens and drawer-local interaction.
- Sprint 57.5 — compact remembered FUSS trees, natural-width/full-screen
  selection, the edge column, and drawer performance gates. Its overlay
  placement below the tab strip is superseded here; its tree, width, memory,
  fallback, and keyboard contracts remain.
- Sprint 57.6 — direct visible-tree type-to-jump and binary/source-service
  isolation. This sprint changes no binding or binary behavior.
- Binding plans and invariants 1–5, 7–9. In particular, shifting the visible
  editor may not mutate the pane tree, stored split ratios, view state, or
  workspace serialization.

## Goals

Turn the ordinary-width FUSS drawer into a real left off-canvas surface. FUSS
starts at terminal row zero. The tab strip, pane tree, document viewport, and
FUSS footer shift to the drawer's right instead of being painted underneath
it, so the editor remains visible at its true available width. The drawer and
its separator own the full terminal height, including rows alongside the tab
strip and footer. A dedicated neutral-gray surface distinguishes FUSS from the
dimmed editor. Smart width and the existing full-screen fallback remain
deterministic.

This is the final pre-audit layout correction. Sprint 58 remains audit-only
and must establish its baseline from the post-57.7 tree.

## Deliverables

### 1. Layout reserves one left off-canvas span

`src/ui/layout.c` consumes the existing module boundary:

```c
bool yew_fuss_active(const Ed *ed);
Rect yew_fuss_drawer_rect(const Ed *ed);
```

`yew_fuss_drawer_rect` becomes independent of tab/footer geometry and returns
the full terminal-owned drawer:

```text
overlay:    { x: 0, y: 0, w: smart_width, h: grid.rows }
fullscreen: { x: 0, y: 0, w: grid.cols,  h: grid.rows }
inactive:   callers ignore the rectangle
```

For an overlay, `yew_layout` uses `drawer.w` as the left origin and
`grid.cols - drawer.w` as the width for all live editor chrome:

| Surface | Rectangle rule |
|---|---|
| tab strip | `{drawer.w, 0, editor_w, strip_rows}` |
| pane tree | `{drawer.w, strip_rows, editor_w, pane_rows}` |
| F footer | `{drawer.w, grid.rows - 2, editor_w, 2}` |

The ordinary non-FUSS layout remains byte-identical. The full-screen fallback
assigns zero width to the hidden tab/pane side and retains the existing
full-width two-row F footer. No pane node, split ratio, `Win`, viewport,
cursor, tab, group, or persisted field changes when F mode enters or leaves.

**Pitfall:** subtract only after clamping `drawer.w <= grid.cols`. Terminal
dimensions and natural widths are `u16`; no intermediate addition may wrap.

### 2. FUSS owns row zero and the full height

`src/mod/git/fussmode.c` draws the workspace header at `drawer.y == 0`, on the
same screen row as the shifted tab strip. Overlay trees may use every drawer
row after the header, including the two rows alongside the right-hand F
footer. Full-screen trees stop before the full-width footer because those
cells are not available to the tree.

The separator is drawn after rows and fills every drawer row:

```c
fuss_edge(ed, drawer, layout, drawer.h);
```

Opening/loading, empty, short, and scrolled trees obey the same rule; edge
height never depends on item count or `drawn_rows`. Row hit regions still stop
at `tree_width` and never claim the edge or footer.

`yew_fuss_backdrop_rect` now describes only the visible right-hand editor body
for overlays. The existing dim attribute is applied there after the shifted
pane tree draws. In full-screen mode it describes the footer-excluding content
area used by centered previews.

**Pitfall:** drawing a full-height edge while leaving the footer anchored at
column zero lets the later footer pass erase the final two edge cells. Layout
and draw order must agree; repainting the edge twice is not the fix.

### 3. A dedicated neutral drawer surface

Add `git.drawer` to both shipped themes and use it for the drawer fill, header,
rows, unused space, and edge-background fallback:

| Theme | Drawer background | Edge |
|---|---|---|
| quiver-dark | `#1b202a` | existing bright white |
| quiver-light | `#eef1f4` | `#57606a` |

Missing custom-theme roles fall back to the normal editor foreground and
background. Mono/dumb renditions preserve separation with the existing bold
edge; they do not invent terminal colors. Marker foregrounds and selected-row
reverse video are unchanged.

Use a drawer-specific helper that consumes the role's foreground,
background, and attributes. Do not change `fuss_role_style`, whose
foreground-only merge is required by marker roles with no background.

### 4. Tabs, panels, mouse, and damage consume the shifted rectangles

- `yew_tab_strip_draw`, tab/group regions, drag slots, context-menu anchors,
  pane borders, gutter, document drawing, and cursor drawing consume the
  rectangles produced by `yew_layout`; none recomputes the inset.
- Overlay previews use the shifted editor rectangle and cannot overlap the
  drawer. Full-screen previews remain centered in the footer-excluding area.
- Enter, split-open, Escape, and `C-g q` preserve the exact pre-F pane/tab/view
  state. Leaving F invalidates the full grid and lays the editor back out from
  column zero.
- Selection and type-jump continue to damage only old/new FUSS rows. Enter,
  leave, terminal resize, natural-width change, and overlay/full-screen
  transitions remain full-layout/full-damage events.
- The `MODULES=""` shim returns empty FUSS rectangles and leaves core layout
  unchanged.

### 5. Defer unrelated FUSS changes

Manual resizing, horizontal tree scrolling, restart-persistent expansion,
marker redesign, additional Git verbs, keybinding changes, and pane-tree
serialization remain out of scope. No dependency or workspace-state field is
added.

## Testing Strategy

- **Unit/layout** (`tests/unit/test_fussdrawer.c`): exact 80×24 overlay
  rectangles for drawer, tab strip, pane tree, backdrop, and footer; a one-row
  and two-row tab strip; the 40×24 full-screen fallback; enter/leave restoration;
  zero-row/zero-column and `UINT16_MAX` dimensions.
- **Unit/draw/theme**: header appears on row zero; the edge glyph and style
  occupy rows 0 and `grid.rows - 1` for short, empty, and loading trees; the
  bottom-left drawer cell has `git.drawer` background; the right-hand tab and
  footer begin after the edge; shipped truecolor/256/16/mono theme roles compile.
- **Unit/mouse/panel**: tab regions and FUSS rows do not overlap; the edge is
  inert at top/middle/bottom; overlay preview rectangles start at or after the
  editor origin; full-screen preview placement remains bounded.
- **PTY**: deterministic 40/64/80/120-column goldens prove row-zero FUSS,
  shifted tabs/documents, full-height surface/edge, both themes' surface
  contrast, ASCII edge parity, full-screen fallback, and exact Escape restore.
- **Performance/fuzz**: existing 20,000-node layout/drawer budgets and FUSS
  geometry fuzz run unchanged. Layout remains O(panes), with O(1) drawer-width
  lookup and no per-frame heap allocation.
- **Lanes**: default and FUSS-only warning-clean builds, focused ASan/UBSan,
  full native `make test`, deterministic FUSS PTYs/fuzz, and `make size`.

## Definition of Done

1. At 80×24 with a 24-cell drawer, the drawer is `{0,0,24,24}`; the edge
   occupies column 23 on all 24 rows; tab, pane, and footer rectangles begin at
   column 24.
2. The FUSS header is on row zero beside—not beneath—the tab strip. No tab,
   pane, gutter, cursor, footer, panel, or clickable region enters the drawer.
3. The document is laid out at the remaining width, not merely repainted or
   clipped under an overlay. Wrap, gutter, pane borders, and mouse hits agree
   with that width.
4. Empty/loading/one-item trees retain the surface and separator through the
   bottom row. The final edge cell cannot be overwritten by footer drawing.
5. Dark and light shipped themes expose `git.drawer`; its background differs
   from `bg`, while marker and selection semantics remain unchanged.
6. Full-screen fallback, preview, footer, Escape, and open-destination behavior
   remain correct at 40 columns and smaller terminal edge cases.
7. Entering and leaving F 100 times preserves pane pointers, ratios, tabs,
   buffers, cursors, viewport positions, and workspace serialization.
8. Selection/type-jump movement remains drawer-local; geometry transitions
   restore every newly exposed editor cell without stale FUSS bytes.
9. Default and `MODULES="fuss"` shipping builds are warning-clean; the
   `MODULES=""` product surface remains unchanged and smoke-clean.
10. Focused unit/PTY/sanitizer/fuzz/performance gates, full `make test`, and
    all applicable native size gates pass. Sprint 58 names a post-57.7 green
    commit as its baseline.

## Closeout — REPOSITORY COMPLETE 2026-09-01

FUSS is now a true left off-canvas surface at ordinary widths. The drawer
owns row zero and the full terminal height; its separator reaches the bottom
row even for loading, empty, and one-item trees. The tab strip, pane tree,
document viewport, and F footer consume the remaining right-hand width rather
than drawing beneath the drawer. The dark and light shipped themes expose a
dedicated neutral `git.drawer` surface, while the existing full-screen fallback
keeps its full-width footer.

Entering F does not rewrite the pane tree or persist a temporary narrow
viewport. The layout skips viewport following while the drawer is active, so
100 enter/leave cycles preserve pane pointers, ratios, tabs, buffers, cursors,
goals, and viewport positions exactly. Preview, mouse, tab, damage, edge, and
zero/maximum-dimension tests consume the same computed rectangles.

The contract landed in `5ae5dc99`; implementation and unit coverage landed in
`31680d61`; the FUSS PTY matrix landed in `bf58be5b`; and the directory-startup
PTY contract follow-up landed in `23159853`. Commit `23159853` is the local
post-57.7 green candidate baseline for Sprint 58.

Local closeout evidence on Darwin arm64:

- the complete supported default-module `make test` suite is green: every PTY,
  Fletch 38/38, scripts 93 tests / 919 assertions, package integration 51/51,
  2,000 round-trip seeds, fuzz corpora, 432 syntax assets, policy/smoke/torture
  gates, and 2,414 unit tests / 71,034,643 assertions pass;
- focused drawer coverage passes 18 tests / 1,876 assertions and shipped-theme
  coverage passes 1 test / 102 assertions under plain, ASan/UBSan, and the
  Darwin arm64 alignment/UBSan profile;
- all FUSS-filtered PTYs plus the directory-startup path pass from two
  deterministic executions, covering 40/64/80/120/240 columns, both shipped
  themes, ASCII, full-screen fallback, shifted tabs/documents/footer, and exact
  Escape restoration;
- default, `MODULES="fuss"`, and `MODULES=""` shipping builds are
  warning-clean; the FUSS-only and core-only product smoke suites pass;
- deterministic FUSS fuzz passes 20,000 iterations at seed 1;
- all FUSS performance gates pass: 1.516 ms build+flatten, 0.001 ms navigation
  p99, 0.014 ms toggle+measure p99, 1.763 ms drawer entry, 0.002 ms
  input-to-damage p99, and 1.077 ms open resolution;
- native `make size` passes all six shipping profiles: 1,452,640 bytes
  minimal, 1,891,504 full, 1,555,088 LSP-only, 1,588,496 AI-only, 1,586,752
  FUSS-only, and 1,536,096 plugins-only.

This host has Apple clang only; `/usr/bin/gcc` is its clang compatibility
driver. True GNU GCC, Linux/x86_64, Valgrind/musl, designated-runner, and
hosted-CI evidence are not inferred. Sprint 58 may use `23159853` as a local
candidate only; no audit front opens until the exact post-57.7 baseline has the
hosted commit-of-record lanes required by the Sprint 58 contract.
