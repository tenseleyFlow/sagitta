# Sprint 57.8: Modern Tab Strip

## Prerequisites

- Sprint 22 — cell-based layout and the clickable-region registry. Draw and
  hit-test geometry are one fact, produced in the same render pass.
- Sprints 23 and 24 — stable tab ids, one/two-row tab strips, overflow
  scrolling, modified markers, and the facsimile group/member model.
- Sprint 27 — press/release tab activation, drag/reorder, group dwell, and
  context menus. A normal tab remains draggable; the new-tab control is not.
- Sprint 41 — compiled dark/light UI themes and deterministic truecolor,
  256-color, 16-color, mono, and dumb renditions.
- Sprint 57.7 — FUSS shifts the tab rectangle off-canvas. Every strip cell and
  region in this sprint consumes that rectangle without recomputing its origin.
- Binding plans and invariants 1, 4, 5, 7, and 9. In particular, the visual
  refresh may not weaken tab identity, click correctness, deterministic output,
  latency, or keyboard reachability.

## Goals

Replace yew's bracketed, reverse-only tabs with the modern facsimile strip:
padded bracket-free entries, dedicated active/inactive theme roles, distinct
modified/orphan treatments, and a three-cell ` + ` action at the visible tail.
The strip is present for a single tab so a workspace always exposes the
new-untitled action. Clicking `+` invokes the existing `ed.tab.new` command;
`t t` and `:tabnew` remain the keyboard and E-mode paths. Existing tab click,
drag, group, overflow, FUSS-offset, and multibyte geometry must remain exact.

This is the final pre-audit chrome correction. Sprint 58 establishes its
baseline from the post-57.8 tree and remains gated on hosted evidence.

## Deliverables

### 1. Modern labels and row visibility — `src/ui/tabs.c`

Row-one labels follow facsimile's cell shape:

| Entry | Label |
|---|---|
| ungrouped tab | ` N basenameM ` |
| tab group | ` group-label ` |
| row-two member | ` basenameM ` |

`N` is the existing one-based goto number and `M` is the derived modified
glyph. No brackets or colon remain. Padding is part of the clickable span, so
adjacent tabs retain an easy target without inserting unowned gaps.

`yew_tab_strip_rows()` returns one row whenever row one has at least one entry,
including the ordinary one-tab workspace. An active group or drag preview
still reserves row two. Empty editor state returns zero rows.

**Pitfall:** labels remain capped and measured through `ui/strip.c` in cells.
No draw, hit-test, drag-slot, or add-control placement may use `strlen()` as a
screen width for arbitrary tab text.

### 2. Dedicated tab theme roles — shipped themes and `src/ui/tabs.c`

Both `runtime/themes/quiver-dark.fl` and `quiver-light.fl` add:

| Role | Purpose |
|---|---|
| `tab.bar` | full-row strip surface |
| `tab.active` | active tab/group pill |
| `tab.inactive` | ordinary inactive pill |
| `tab.modified` | inactive modified pill |
| `tab.orphan` | missing/outside-workspace pill |
| `tab.add` | ` + ` action |

Dark uses a neutral raised surface over `#12141a`; light uses a neutral
blue-gray surface over white. Active uses the selection surface plus bold text;
inactive is muted; modified uses the theme warning color; orphan is dim; add
uses the theme accent color. Mono/dumb keeps active reverse, inactive plain,
modified bold, orphan dim, and add bold.

The renderer fills the complete row with `tab.bar`, then paints each entry with
the state role selected in this order: active, orphan, modified, inactive.
Custom themes that omit the new roles fall back to `fg`/`bg`, with the same
mono attributes, rather than producing invisible cells or a null dereference.

**Pitfall:** `ThemeEnt` colors may be terminal-default. Overlay a role only
where it specifies a non-default foreground/background; otherwise retain the
base `fg`/`bg`. A foreground-only role must not erase the strip background.

### 3. Tail ` + ` control and exact overflow ownership

Add `YEW_REGION_TAB_NEW` to `src/ui/region.h`. Its payload is unused. Row-one
rendering owns a three-cell ` + ` control under this truth table:

| State | Right-edge owner |
|---|---|
| all entries visible and at least 3 cells remain | ` + ` |
| right overflow | existing `>N` region; no add control |
| fewer than 3 free cells | blank strip tail; no partial control |
| row two | never an add control |

Placement starts at the exact half-open tail of the last painted tab, adjusted
for the left-overflow cell by the same render pass. The three painted cells and
the registered region are the same `Rect`. The control uses `tab.add` and never
enters the row-one drag-slot table.

**Pitfall:** reserving three cells before layout would hide another tab merely
to show `+`. Match facsimile: lay out tabs first, then use otherwise-empty tail
space. Overflow information outranks the add action.

### 4. Mouse activation reuses the named command

`src/ui/mouse.c` treats `YEW_REGION_TAB_NEW` as a non-draggable armed click.
On left-button release, invoke `ed.tab.new` only if release still lands in the
same region. The command context uses `YEW_SRC_MOUSE`, the current `Ed`, and the
focused `Win`. Command failure (including the 512-tab cap) preserves the active
tab and uses the existing message path.

The action creates a new `path == NULL` tab through `yew_tab_cmd_new`, switches
to it, and leaves it in the current workspace's tab set. It does not create a
file, choose a path, join a group, or replace the active buffer. Keyboard
`t t`, E-mode `:tabnew`, Fletch, and replay retain the same registry command.

Right click, middle click, wheel, drag, and context-menu behavior over `+` are
inert. Existing `YEW_REGION_TAB` press/release and drag identity remain
unchanged.

### 5. Refresh deterministic contracts and audit handoff

Update affected unit expectations and PTY goldens. Add one focused PTY that
clicks `+`, proves a second active `Untitled` tab appears, and then uses the
keyboard tab command to prove both tabs are live. Cover dark/light, 16-color,
mono/no-color, and ASCII through the existing chrome matrix.

After local closeout, update `.docs/HANDOFF.md`, `.docs/sprints/index.md`, and
Sprint 58's prerequisite/baseline wording. The local baseline hash names the
post-57.8 code/golden commit, not a documentation-only closeout commit. Hosted
CI remains explicitly unverified until that exact hash is pushed and green.

### 6. Defer unrelated tab changes

Close buttons, pinned tabs, icons, per-tab accent selection, hover-only
styling, tab search, vertical tabs, additional group behavior, and new
workspace-state fields remain out of scope. No dependency is added. No core
editing, persistence, or module surface is removed for footprint reasons.

## Testing Strategy

- **Unit/model:** exact modern labels for ordinary, modified, CJK, untitled,
  group, and member entries; one-tab/empty/two-row strip heights; new tabs have
  `path == NULL` and stable fresh ids.
- **Unit/draw/theme:** full-row `tab.bar`; active/inactive/modified/orphan/add
  role selection; dark/light and all degradation tiers; custom-role fallback;
  three-cell add region equals the painted span.
- **Unit/mouse:** press/release on `+` invokes the existing command once;
  release outside cancels; no drag starts; cap refusal preserves identity;
  ordinary tab activation, CJK clicks, drag reorder, and group dwell remain.
- **Unit/overflow/layout:** `+` appears only with three spare cells, disappears
  under right overflow, coexists with left overflow when the right edge is
  clear, and honors nonzero FUSS tab-strip origins.
- **PTY:** modern three-tab and group strips; dark/light/16/mono/ASCII chrome;
  CJK click; narrow overflow; one-tab strip; clicked `+` creates and activates
  an untitled tab without corrupting the previous document.
- **Regression/performance:** focused tab/mouse/theme suites, full native
  `make test`, strict default/core-only builds, applicable sanitizer, FUSS PTY,
  performance, and native size gates. Rendering remains allocation-free and
  O(visible strip entries).

## Definition of Done

1. A one-tab workspace draws one modern padded tab and a clickable ` + ` when
   at least three tail cells exist; no bracket or colon chrome remains.
2. Active, inactive, modified, orphan, strip, and add roles compile in both
   shipped themes and degrade deterministically through every rendition.
3. Every tab, overflow, and add region is cell-exact for ASCII, CJK, a nonzero
   FUSS origin, and clipped labels; existing tab clicks still select the tab
   that was pointed at.
4. Clicking and releasing on `+` invokes `ed.tab.new` exactly once, creates a
   fresh untitled tab in the live workspace, and activates it. Releasing
   outside, dragging, or using another mouse button creates nothing.
5. Right overflow owns the edge and suppresses `+`; `+` never obscures `>N`,
   never appears partially, and never becomes a drag slot.
6. `t t`, `:tabnew`, Fletch dispatch, tab/group drag, context menus, member
   rows, modified derivation, and stable-id close behavior remain green.
7. Focused unit and PTY coverage passes under plain and sanitizer builds; the
   full supported native `make test` suite is green and deterministic.
8. Default and `MODULES=""` strict builds, FUSS smoke/PTYs, performance, and
   all applicable native size profiles remain within their existing gates.
9. Sprint 58 names the post-57.8 local candidate and stays gated until the
   exact pushed commit has the required hosted commit-of-record evidence.

## Closeout — REPOSITORY COMPLETE 2026-09-01

Yew now draws facsimile-style padded, bracket-free tabs for both one-tab and
multi-tab workspaces. Active, inactive, modified, orphan, strip, and add
surfaces are theme roles in both shipped themes and degrade through the
existing 16-color, mono/no-color, dumb, and ASCII paths. The one-tab strip is
therefore visible without inventing a second layout rule.

The otherwise-empty row-one tail owns a cell-exact ` + ` control only when
three cells remain and right overflow does not own the edge. Its painted span
and `YEW_REGION_TAB_NEW` rectangle are the same fact from the render pass, and
the control is excluded from drag slots. A same-region left press/release
invokes the existing `ed.tab.new` command with mouse provenance; leaving the
region, repainting it elsewhere, dragging, or using another button creates
nothing. Ordinary tab activation, CJK geometry, drag/reorder, groups, member
rows, overflow, and the Sprint 57.7 FUSS inset continue to use their existing
identities and rectangles.

The sprint contract landed in `e1fcb336`; implementation and focused unit
coverage landed in `92e72902`; the end-to-end click PTY landed in `16005b33`;
focused modern chrome goldens landed in `81391063`; the complete one-tab PTY
contract migration landed in `5c6217b0`; and the remaining fixed-row mouse
tests were retargeted to computed document geometry in `89eefa40`. Commit
`89eefa40` was the first local post-Sprint-57.8 candidate. Hosted remediation
landed in `e93e0ef7`: profiler dump control transport receives a 30-second
hosted ceiling while the actual latency gates remain unchanged, and the job
streaming fixture now kills its direct pipe-owning child on every POSIX host.
`e93e0ef7` is the fixed post-Sprint-57.8 code baseline for Sprint 58.

Local closeout evidence on Darwin arm64:

- the complete supported default-module `make test` suite is green: every PTY,
  Fletch 38/38, scripts 93 tests / 919 assertions, package integration 51/51,
  2,000 round-trip seeds, fuzz corpora, 432 syntax assets, policy/smoke/torture
  gates, and 2,419 unit tests / 71,034,731 assertions pass;
- three complete post-migration PTY executions are deterministic, including
  the FUSS Git fixtures, local-socket AI streams, dark/light/16/mono/ASCII
  chrome, CJK clicks, tab/group drag behavior, and `s57_8_click_new_tab`;
- focused tab, new-tab mouse, click-counter, group-navigation, and theme
  coverage passes 73 tests / 2,079 assertions under both plain and Darwin
  arm64 ASan/UBSan builds; the clicked-new-tab PTY also passes under
  ASan/UBSan;
- independent default and `MODULES=""` shipping builds are warning-clean, and
  the core-only product smoke suite is green;
- all FUSS performance rows pass, including 1.524 ms build+flatten, 0.001 ms
  navigation p99, 0.014 ms toggle+measure p99, 1.766 ms drawer entry, 0.002 ms
  input-to-damage p99, and 1.047 ms open resolution;
- native `make size` passes all six shipping profiles: 1,452,640 bytes
  minimal, 1,891,504 full, 1,571,600 LSP-only, 1,588,496 AI-only, 1,586,752
  FUSS-only, and 1,536,096 plugins-only.

Hosted closeout evidence:

- GitHub Actions run `33595465809` is green at exact code commit `e93e0ef7`:
  all 22 standard push jobs pass, including GNU GCC, Clang, macOS arm64,
  hosted arm64, musl, modules, size, performance, determinism, sanitizer,
  PTY, script, Fletch dispatch, LSP, embedded, allocation, and torture;
- the performance lane passes the exact Sprint 56 profiler cross-check that
  exposed the loaded-runner control timeout, and the macOS lane passes the
  complete unit suite containing `job_stream_bypasses_safe_prefix`;
- Valgrind, designated-runner performance, and nightly fuzz/performance/
  torture jobs were skipped by the push trigger as designed and remain
  separate evidence obligations rather than inferred successes.

The Sprint 58 hosted baseline gate is satisfied. Sprint 58 has not begun; its
audit fronts must preserve `e93e0ef7` as the fixed code baseline when opened.
