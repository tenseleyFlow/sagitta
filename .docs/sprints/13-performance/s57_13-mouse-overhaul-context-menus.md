# Sprint 57.13: Mouse Overhaul — Context Menus Everywhere

## Prerequisites

- Sprint 4 — SGR mouse decoding (`decode_mouse`, `src/term/input.c`), the
  1002/1006 enable/disable blobs in `src/term/tty.c`, and the pinned fact
  that motion with no button held is currently rejected.
- Sprint 22 — the region registry (`src/ui/region.h`), last-added-wins,
  `YEW_REGION_BLOCK`; the layout/hit-test identity law.
- Sprint 27 §5 — `src/ui/ctxmenu.[ch]` (the pop-up widget: capture-at-open,
  clamp-never-flip, greyed-never-hidden, region freeze during invocation),
  `src/ui/mouse.[ch]` (the router, its phase machine, `menu_allowed`,
  `apply_menu_action`, `invoke_named`), and the invariant-9 audit table.
- Sprint 52 / 57.5 / 57.7 — FUSS rows (`YEW_REGION_FUSS_ROW`, payload =
  interned path id), path-addressed `ed.git.*` commands taking `sarg`, the
  off-canvas drawer geometry (`yew_fuss_drawer_rect`, `yew_fuss_backdrop_rect`).
- Sprint 26 / 45 — pickers (`yew_picker_*`, accept modes HERE/VSPLIT/HSPLIT),
  LSP panels (`src/ui/panel.[ch]`, transient), completion menu, rename
  confirm panel, group picker (`yew_gp_show_edit` is the edit-group modal).
- Sprint 57.10 — positional tab numbering, `ed.tab.goto_bar`.
- Binding: `00-decisions.md` Mouse row and **invariant 9** ("every feature
  keyboard-reachable; mouse is an accelerator, never a requirement"),
  invariant 5 (deterministic render), invariant 6 (terminal always
  restored), invariant 4 (5 ms input budget — `tests/perf/mouse.c`).
- Interaction reference (read, never vendored):
  `~/GithubOrgs/FortranGoingOnForty/facsimile/src/ui/context_menu_module.f90`
  (bordered box at the pointer, label + accel + enabled, separators, hover,
  keyboard navigation, opaque action ids).

This sprint **supersedes** Sprint 27 §9's "document context menu → post-1.0"
deferral. `mouse.h`'s deferral comment and the two tests that pin it
(`ctxmenu_right_click_in_a_pane_opens_nothing`,
`mouse_right_click_in_a_pane_does_nothing`) are rewritten to the new contract.

## Goals

One context-menu system. A right-click, or a ctrl+left-click, opens a menu
**at the pointer, anywhere on the screen** — document text, gutter, pane
border, tab or group entry, strip tail, footer, FUSS file or directory row,
FUSS header or blank drawer, picker row, completion row, LSP panel, group
picker row, or the bare backdrop. The rows depend on what is under the
pointer and on live state (selection present, buffer dirty or read-only,
grouped or not, staged or not, LSP attached or not). Rows highlight as the
pointer moves over them, disabled rows are greyed, and when the screen is
short the menu sheds its lowest-priority rows rather than refusing.

Everything reachable from a menu row is a registry command with a keyboard
route, and `ed.ui.context_menu` (`t m`) opens the same menu for whatever the
keyboard focus is on. The router stays allocation-free, the region table stays
frozen during invocation, and the terminal is restored from any-motion
tracking on close, on suspend, and on exit.

FUSS also gets the two feel fixes the survey found missing: single click
selects a row, and the wheel scrolls the tree.

Deferred, named so nobody invents them: submenus; a scrolling menu (shedding
replaces it); mnemonic letters (post-1.0 unless the row set stabilises);
ctrl+wheel (stays unbound, Sprint 27); drag-and-drop from a menu.

## Deliverables

### 1. Any-motion tracking while a menu is open — `src/term/input.c`, `src/term/tty.c`, `src/term/input.h`

Decode SGR motion with no button held: `cb` base 35 (plus modifier bits 4/8/16)
becomes `Key{kind: YEW_EV_MOUSE, button: YEW_MB_NONE, ev: YEW_KEY_REPEAT,
col, row, mods}`. X10 never reports it; nothing else changes in the decoder.

Mode 1003 is enabled **only while a context menu is open**:

```c
/* src/term/tty.h */
void yew_tty_mouse_motion(bool on);      /* writes CSI ? 1003 h / l */
bool yew_tty_mouse_motion_active(void);  /* test seam */
```

The router calls it from the menu open/close paths (§3). Teardown
(`yew_tty_restore` / the atexit + signal-safe restore blob) always emits
`1003l` when the flag is set — invariant 6. Suspend (`ed.suspend`) and
`ed.mouse.disable` clear it too. **Pitfall:** the restore blob is written from
a signal context; extend the existing sig-safe restore sequence, do not add a
new write site (`scripts/check-sigsafe.sh` gates this).

`test_mouse_hover_without_a_button_is_not_an_event` is rewritten:
no-button motion is decoded, and the router drops it unless a menu is open.
`tests/unit/test_input.c` gains vectors for base 35 with each modifier;
`tests/unit/test_tty.c` pins that `1003l` is in the restore blob whenever the
flag was set and absent otherwise.

### 2. The menu widget — `src/ui/ctxmenu.[ch]`

Keep the module editor-ignorant (no `edit/ed.h`). Changes:

| Aspect | Contract |
|---|---|
| Box | Bordered, facsimile-style: one-cell border on all sides, no title. `box.h = rows + 2`, `box.w = widest + 2·pad + 2`. |
| Rows | `YEW_CTX_MAX_ROWS` = 32. `yew_ctx_item` gains a `u8 priority` (0 = never shed … 3 = shed first). A separator inherits the priority of the row above it and is dropped when it would lead or trail or double up. |
| Shedding | `yew_ctx_show` computes the box; if `allowed.h` cannot hold it, shed priority 3 rows, then 2, then 1; refuse only if priority-0 rows alone do not fit. Width: clamp to `allowed.w`, clip labels with `yew_str_clip`, drop accels first. Shedding is deterministic (same input → same rows) — invariant 5. |
| Placement | Clamp, never flip (unchanged). Anchor = the click cell; the box's top-left is the cell **below-right** of the pointer so the row under the pointer at open is the border, not a row (no accidental activation on release). |
| Hover | `yew_ctx_hover(i32 row)` unchanged semantics; a new `bool yew_ctx_hover_at(u16 x, u16 y)` maps a cell to a row via the box geometry (no region lookup — the table may be mid-frame) and returns whether the highlight moved, so the router repaints only on change. |
| Keys | Up/Down/Enter/Esc unchanged; add Home/End; everything else still swallowed. |
| Theme | `yew_ctx_draw(Grid *, const CtxStyle *)` — the caller supplies a `CtxStyle{ ThemeEnt surface, row, hover, disabled, accel, sep; }` resolved by the router from the roles in §6. No hardcoded colours remain. |
| Regions | `YEW_REGION_BLOCK` over the whole box (border included) then `YEW_REGION_CTX_ROW` per drawn row. Unchanged law. |
| Target | `yew_ctx_target(u32 id, const char *path)` unchanged; add `yew_ctx_target_rect(Rect)` + getter for pane-relative targets (which leaf, which cell) so the document menu can place the cursor where the click was. |

Test seams stay; add `yew_ctx_shed_count(void)` (rows shed at the last show)
and `yew_ctx_priority(u32 row)`.

### 3. Router: context resolution, opening, hover, dispatch — `src/ui/mouse.[ch]`

**Context resolution is one pure function**, unit-tested directly:

```c
typedef enum CtxKind {
    YEW_CTX_KIND_NONE = 0,
    YEW_CTX_KIND_TAB, YEW_CTX_KIND_GROUP,          /* unchanged */
    YEW_CTX_KIND_STRIP,        /* strip blank tail, chevrons, the + */
    YEW_CTX_KIND_DOC,          /* pane text or gutter */
    YEW_CTX_KIND_BORDER,       /* pane border */
    YEW_CTX_KIND_FOOTER,       /* statusline / message row */
    YEW_CTX_KIND_FUSS_FILE, YEW_CTX_KIND_FUSS_DIR,
    YEW_CTX_KIND_FUSS_BLANK,   /* header, blank drawer, backdrop */
    YEW_CTX_KIND_PICK_ROW, YEW_CTX_KIND_PICKER,
    YEW_CTX_KIND_COMPL_ROW,
    YEW_CTX_KIND_PANEL,        /* hover / signature / rename-confirm */
    YEW_CTX_KIND_GP_ROW, YEW_CTX_KIND_GP,
    YEW_CTX_KIND_EDITOR        /* nothing under the pointer */
} CtxKind;

typedef struct CtxContext {
    CtxKind kind;
    u32 id;            /* tab_id, gid, interned path id, leaf index, payload */
    i32 payload;       /* raw region payload for row kinds */
    Rect rect;         /* the region rect, or the pane rect for DOC */
} CtxContext;

CtxContext yew_mouse_context_at(const Ed *ed, u16 x, u16 y);
```

Resolution order: region hit first (`YEW_REGION_*` → kind by the table
below); a `NONE` hit falls through to geometry: inside `yew_fuss_drawer_rect`
or `yew_fuss_backdrop_rect` → `FUSS_BLANK`; inside `ed->footer_rect` →
`FOOTER`; inside `ed->tab_strip_rect` → `STRIP`; else `EDITOR`.

| Region | Kind |
|---|---|
| `PANE` | `DOC` (id = leaf index; rect = pane rect) |
| `PANE_BORDER` | `BORDER` |
| `TAB` payload ≥ 0 / < 0 | `TAB` / `GROUP` |
| `TAB_SCROLL`, `TAB_NEW` | `STRIP` |
| `FUSS_ROW` | `FUSS_FILE` or `FUSS_DIR` (ask the tree by path) |
| `PICK_ROW` | `PICK_ROW`; `BLOCK` while a picker is active → `PICKER` |
| `COMPL_ROW` | `COMPL_ROW` |
| `BLOCK` while `win->panel.open` and inside `panel.rect` | `PANEL` |
| `GP_ROW` / `GP_NAME` | `GP_ROW` / `GP`; `BLOCK` while `yew_gp_active()` → `GP` |
| `CTX_ROW` / `BLOCK` of the menu | the menu itself (close-and-reopen rule below) |

**Opening.** `mouse_press`: right button, or left button with `YEW_MOD_CTRL`,
opens the menu for `yew_mouse_context_at(ed, col, row)` via a builder (§4),
after closing any open menu. A right/ctrl press on the open menu's own row
invokes nothing — it closes it (Sprint 27). Ctrl+left never reaches the phase
machine, so it never arms a drag or a selection. Opening enables 1003 (§1)
and sets `full_damage`. Left-press outside the box closes it and is then
handled normally (unchanged). Wheel while open: closes the menu, then the
wheel is routed as usual.

**Hover.** `mouse_motion` with `button == YEW_MB_NONE`: if a menu is open,
`yew_ctx_hover_at(col, row)`; when it returns true mark a menu-only repaint
(`ed->overlay_dirty = true`, a new flag drawn by `draw_overlays` without a
full pane repaint). Otherwise drop the event. No allocation, no render in the
common path — `tests/perf/mouse.c` stays green with the menu closed, and a
new perf case pins ≤ 1 render per hovered-row change with it open.

**Dispatch is table-driven.** Replace the `CTXA_*` switch with:

```c
typedef enum CtxTarget {
    CTX_TGT_NONE,        /* invoke as-is */
    CTX_TGT_TAB,         /* yew_tab_switch to the captured tab_id first */
    CTX_TGT_GROUP,       /* yew_group_enter the captured gid first */
    CTX_TGT_PANE,        /* focus the captured leaf, place the cursor at the click cell */
    CTX_TGT_PATH,        /* cx.sarg = captured path (FUSS) */
    CTX_TGT_PICK,        /* yew_picker_select_payload + accept with iarg mode */
    CTX_TGT_COMPL        /* yew_compl_select(payload) first */
} CtxTarget;

typedef struct CtxActionDesc {
    const char *cmd;     /* registry name; NULL = handled inline (close overlay) */
    CtxTarget target;
    i64 iarg;
} CtxActionDesc;

static const CtxActionDesc ctx_actions[CTXA__N];   /* indexed by CtxAction */
```

`apply_menu_action` becomes: take the action, freeze regions, apply the
target, `invoke_named` with `cx.source = YEW_SRC_MOUSE` (fix the Sprint 27
inconsistency), unfreeze, mark damage. A `cmd == NULL` action closes the
relevant overlay (panel, picker, completion, group picker) inline. **The
freeze must be balanced on every path** — `fuzz_mouse` asserts it before
each event.

**FUSS feel fixes** (same router): left press on `YEW_REGION_FUSS_ROW` selects
that row (`yew_fuss_select_path(ed, path_id)` — new, in `fussmode.c`, no-op
under the shim); wheel over the drawer scrolls the tree by `YEW_WHEEL_ROWS`
(`yew_fuss_scroll(ed, rows)` — new). Double-click open is unchanged.

### 4. Row builders — `src/ui/ctxrows.c` (new), `src/ui/ctxrows.h`

One file, one builder per kind, `void yew_ctx_build(Ed *ed, const CtxContext *c)`.
Builders call `yew_ctx_begin/item/sep/target`. Priority in brackets;
`…` means the row opens a prompt; enable conditions in parentheses. Labels
are exact — tests and goldens pin them.

**DOC** (target: leaf + click cell):
`Cut` [0] (selection, !readonly) · `Copy` [0] (selection) · `Paste` [0]
(!readonly, register `"` non-empty) · `Delete` [1] (selection, !readonly) ·
`Select All` [1] · — · `Undo` [1] (undo available) · `Redo` [1] (redo
available) · — · `Split Right` [2] · `Split Below` [2] · `Close Pane` [2]
(more than one leaf) · — · `Save` [1] (dirty) · `Save As…` [2] · `Reload` [3]
(has path) · — · *LSP section, only when a server is attached to this
buffer:* `Go to Definition` [2] · `Find References…` [2] · `Rename…` [2] ·
`Hover` [3] · — · *Git section, only when FUSS is compiled in and the file is
in a repo:* `Toggle Blame` [3] · `Diff` [3] · — · `Command Palette…` [0] ·
`Find File…` [1] · `Go to Line…` [2] · `Toggle Wrap` [3].

**TAB** (unchanged rows plus): `Close Tab` [0] · `Close Other Tabs` [1] ·
— · `Copy Path` [1] · `Remove from Group` [2] (grouped) · — · `New Tab` [2]
· `Open in Split Right` [3] · `Open in Split Below` [3].

**GROUP**: `Edit Group…` [0] · `Rename Group…` [1] · — · `Close Group` [1] ·
`Dissolve Group` [1].

**STRIP** / **EDITOR**: `New Tab` [0] · `Open File…` [0] · `New Group…` [1]
· — · `Command Palette…` [0] · `Find Buffer…` [2].

**BORDER**: `Close Pane` [0] · `Grow` [1] · `Shrink` [1] · — · `Focus Next`
[2].

**FOOTER**: `Command Palette…` [0] · `Go to Line…` [1] · — · `Toggle Wrap`
[2] · `Line Numbers: <style>` [2] (cycles none→abs→rel→hybrid) · — ·
`Disable Mouse` [3].

**FUSS_FILE** (target: path): `Open` [0] · `Open in Split Right` [1] · `Open
in Split Below` [1] · `Preview` [2] · — · `Stage` [0] (unstaged or untracked)
· `Unstage` [0] (staged) · `Discard…` [1] (dirty) · — · `Diff` [2] · `Blame`
[2] · — · `Rename…` [3] · `Delete…` [3] · `Copy Path` [3].

**FUSS_DIR** (target: path): `Open as Group…` [0] · `Expand` / `Collapse`
[0] (by state) · — · `Stage All Below` [1] · `Unstage All Below` [1] · — ·
`Copy Path` [3].

**FUSS_BLANK**: `Refresh` [0] · `Commit…` [0] · — · `Push` [1] · `Pull` [1]
· `Fetch` [2] · — · `Status` [2] · `History` [2] · — · `Leave FUSS` [0].

**PICK_ROW**: `Open` [0] · `Open in Split Right` [1] · `Open in Split Below`
[1] · — · `Close` [0]. **PICKER** (box, no row): `Close` [0].

**COMPL_ROW**: `Accept` [0] · `Toggle Docs` [1] · — · `Cancel` [0].

**PANEL**: `Close` [0]; for the rename-confirm panel additionally `Apply`
[0] · `Show Diff` [1] · `Cancel` [0].

**GP_ROW**: `Toggle` [0] · — · `Confirm` [0] · `Cancel` [0]. **GP**:
`Confirm` [0] · `Cancel` [0].

Section-omission rule (an amendment to Sprint 27's greyed-never-hidden law,
recorded here): a *section* whose feature is unavailable in this build or
for this buffer (no LSP server, FUSS stripped) is omitted whole; *rows* within
an available section are greyed, never hidden. Shape stability holds per
(kind, availability).

**New commands** so every row is a registry command (`src/edit/edit_cmds.c`,
`src/edit/sel_actions.c`, `src/edit/pane_cmds.c`, registered in `cmd.c`, verb
list extended, E-mode abbrevs where natural, bound in `runtime/init.fl` and
added to `tests/unit/test_runtime_defaults.c`'s frozen table):

| Command | Does | Binding |
|---|---|---|
| `ed.sel.cut` | yank to `"` (and `+`) then delete the selection | H: `x` |
| `ed.edit.paste` | insert register `"` at the cursor (charwise/linewise by register kind) | L: `p` |
| `ed.sel.all` | select the whole buffer (enters H, char kind) | L: `g a` |
| `ed.tab.open_split_h` / `ed.tab.open_split_v` | open the active tab's buffer in a new split | — |
| `ed.view.number_cycle` | cycle number style | — |
| `ed.ui.context_menu` (extended) | opens the menu for the keyboard focus: FUSS row when FUSS is active, picker row when a picker is up, else the document at the cursor cell; `t m` stays | `t m` |

`ed.git.open_split_*`, `ed.git.view`, `ed.git.stage/unstage/discard/diff/
blame/file.rename/file.delete`, `ed.group.from_dir`, `ed.git.tree.*`,
`ed.git.commit/push/pull/fetch/status/history`, `ed.git.mode.leave`,
`ed.find.*`, `ed.pane.*`, `ed.lsp.*`, `ed.compl.*`, `ed.group.*`,
`ed.tab.*` already exist and are used as-is.

### 5. Keyboard route — `ed.ui.context_menu`

Anchors: document → the cursor cell; FUSS → the selected row; picker → the
selected row; tab strip focus (no window? never — there is always a window)
is reached by the existing `t m` path when the caller passes `iarg 1`
("strip"). `ed.ui.context_menu` gains `YEW_ARITY_OPT_INT`: 0/absent = focus
context, 1 = tab strip (the Sprint 27 behaviour). Invariant-9 audit table
(Sprint 27 §8) gains one row per new kind; `tests/unit/test_invariant9.c`
gains `invariant9_document_menu_rows_are_keyboard_reachable` and
`invariant9_fuss_menu_rows_are_keyboard_reachable`.

### 6. Theme roles — `runtime/themes/quiver-dark.fl`, `quiver-light.fl`, `src/ui/mouse.c`

| Role | Purpose | quiver-dark |
|---|---|---|
| `menu.surface` | box background / border colour | `bg: "#1b202a", fg: "#4b5263", mono: "plain"` |
| `menu.row` | enabled row text | `fg: "@fg", mono: "plain"` |
| `menu.hover` | highlighted row | `bg: "#2c313a", fg: "@fg", bold: true, mono: "reverse"` |
| `menu.disabled` | greyed row | `fg: "@grey", dim: true, mono: "dim"` |
| `menu.accel` | accelerator text | `fg: "@grey", mono: "dim"` |
| `menu.sep` | separator rule | `fg: "#4b5263", mono: "dim"` |

There is deliberately **no `menu.border` role** and no `border` field on
`CtxStyle`. `menu.surface` already carries both halves of the frame — its
`bg` fills the box and its `fg` draws the border — and `menu.row` owns the
text; a seventh role would be a second definition of one surface, and two
definitions of one surface are how a themed box comes to have a frame that
does not match the ground it is drawn on.

The router resolves them once per draw with `yew_theme_ui_tab` and the same
overlay-merge idiom as `tab_role_style`. `degrade_*` tests cover the 16-colour
and NO_COLOR renditions of the menu.

### 7. Docs and comments

`mouse.h` deferral comment rewritten; `.docs/sprints/05-ui-workspace/s27-mouse-and-feel.md`
§9 gets a one-line "superseded by Sprint 57.13" note; `README.md` mouse
section (if present) lists right-click / ctrl+click.

### 8. Defer

Submenus, scrolling menus, mnemonic letters, ctrl+wheel, menu drag-and-drop,
modifyOtherKeys → later. Each must hard-error naming this sprint if a caller
asks (`YEW_BUG`-free: refuse with a message).

## Testing Strategy

- **Unit (input/tty):** base-35 motion vectors × modifiers; 1003 blob
  present in restore only when armed; `yew_tty_mouse_motion` idempotent.
- **Unit (ctxmenu):** shedding order and determinism; refuse only when
  priority-0 rows do not fit; width clamp clips labels and drops accels first;
  bordered geometry; `yew_ctx_hover_at` mapping incl. border cells and
  separators; Home/End; existing 13 tests kept (rewrite
  `ctxmenu_right_click_in_a_pane_opens_nothing` → `…opens_the_document_menu`).
- **Unit (router):** `yew_mouse_context_at` for every kind incl. geometry
  fall-throughs (footer, drawer blank, backdrop, strip tail); ctrl+left opens
  and never arms; right press on the open menu closes; wheel closes then
  scrolls; hover repaint only on row change; no-button motion dropped with the
  menu closed; freeze balanced on every action path (extend `fuzz_mouse` with
  35-motion and ctrl+left events); FUSS single-click select and wheel scroll.
- **Unit (rows):** each builder's exact label list and enable flags against
  fixtures (selection / no selection, readonly, dirty, grouped, staged,
  untracked, LSP attached / absent, `MODULES=""`); every row's command
  resolves in the registry (`invariant9_every_new_command_is_registered`
  extended); new commands' behaviour (cut/paste/select-all, split-open,
  number cycle).
- **PTY:** regenerate `chrome_ctxmenu*` and `s27_group_menu_over_scrolled_strip`
  (bordered, themed); new cases: `s57_13_doc_menu` (right-click in text,
  hover one row via a 35-motion report, snapshot), `s57_13_ctrl_click_menu`,
  `s57_13_fuss_file_menu`, `s57_13_fuss_dir_menu`, `s57_13_footer_menu`,
  `s57_13_menu_sheds_rows` (10-row terminal), plus `_nocolor` / `_ascii`
  variants for the doc menu. `check-render.sh` and the determinism lane run
  them twice.
- **Perf:** `tests/perf/mouse.c` unchanged gate (no allocation tokens in
  `mouse.c`; ≤ 5 ms per 1000 motions; 0 renders with the menu closed) plus a
  new case: 1000 no-button motions across an open 20-row menu → ≤ 20 renders.
- **Fuzz:** `fuzz_mouse` event mix gains 35-motion (10 %) and ctrl-modified
  left presses; assertions unchanged plus "1003 flag is false whenever no menu
  is open".
- **Script:** `check-input.sh`, `check-sigsafe.sh`, `check-cmd-dispatch.sh`,
  `bans.sh` green; `MODULES=""` build has every kind except FUSS/LSP sections.

## Definition of Done

1. Right-click and ctrl+left-click open a context menu at the pointer on
   every surface listed in §3; a bare backdrop or footer opens the
   EDITOR/FOOTER menu rather than nothing.
2. Moving the pointer over an open menu highlights the row under it; motion
   with no menu open produces no render and no allocation.
3. Every row in §4 invokes a registry command with a keyboard binding or a
   documented keyboard route; `t m` reaches the focus-context menu.
4. A 10-row terminal still opens the document menu with its priority-0 rows;
   shedding is deterministic and covered by a golden.
5. The menu is themed through the §6 roles; NO_COLOR, 16-colour and ASCII
   renditions stay legible (degrade tests).
6. Mode 1003 is never left on: unit tests for restore, suspend, disable,
   and the fuzz assertion.
7. FUSS: single click selects, wheel scrolls, file and directory menus work
   with the drawer in overlay and fullscreen layouts.
8. gcc and clang warning-free; full unit, ASan/UBSan (tabs/mouse/ctx/fuss/
   input filters), PTY, fletch, script, fuzz-mouse, perf-mouse lanes green;
   `MODULES=""` unit lane green.
