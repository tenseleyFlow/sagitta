# Sprint 57.12: Selection, Clipboard Aliases, and Safe Job Return

## Prerequisites

- Sprint 4 — normalized modifier-bearing arrow and control-key events.
- Sprint 12 — internal registers, the `+` system register, subprocess/OSC 52
  clipboard backends, and `clipboard.sync`.
- Sprint 13/17 — layered keymaps and the single Highlight-mode selection
  model.
- Sprint 19 — no-range `:!` output in `YEW_BUF_NOUNDO` job buffers, ranged
  filters, and stable `origin_buf_id` ownership.
- Sprint 36 — `runtime/init.fl` is the shipped default keymap.
- Binding plans and invariants 1–6, 8–10. Selection remains byte-exact,
  clipboard work may not block the event loop on writes, and a transient
  output view may never hide dirty file bytes from the quit guard.

## Goals

Make Shift+Arrow the familiar character-precise route into Highlight mode
from L, W, B, and I, preserving the cursor as the anchor and extending the
same selection on repeats. Add Ctrl+C, Ctrl+X, and Ctrl+V as explicit system
clipboard aliases without replacing Yew's modal `h` then `y` route or
creating another selection/register implementation. Repair focused `:!`
output so an unforced `:q` dismisses the transient job view and returns to
the originating file; `:q!` remains the explicit whole-editor force quit.

No-range `:!` continues to write only to a no-undo scratch buffer. Ranged
forms such as `:%!sort` remain intentional, one-transaction file filters.
Job output remains reachable through `:jobs` after it is hidden and is
released only by the established clear-finished action.

## Deliverables

### 1. Shift+Arrow selection entry — `src/edit/edit_cmds.c|h`,
`src/edit/keys_highlight.c`, `runtime/init.fl`

Register four no-argument commands:

```c
CmdStatus yew_edit_cmd_sel_extend_left (CmdCtx *cx);
CmdStatus yew_edit_cmd_sel_extend_right(CmdCtx *cx);
CmdStatus yew_edit_cmd_sel_extend_up   (CmdCtx *cx);
CmdStatus yew_edit_cmd_sel_extend_down (CmdCtx *cx);
```

The first invocation outside H calls
`yew_mode_enter_highlight(ed, YEW_MODE_I, false)`, establishing the current
cursor position as the character-selection anchor, then performs exactly one
grapheme Left/Right or goal-column-preserving logical/display-row Up/Down
motion. Repeats in H preserve the original anchor. The commands operate on
the existing `Cursor`, `HState`, selection renderer, and damage path only.

Bind `S-<left/right/up/down>` in L, W, B, and I through `runtime/init.fl` and
in H's adaptive built-in table. Unshifted arrows and explicit `h` retain
their unit-sensitive L/W/B/I contracts. E and F retain ownership of their
own input models and do not receive these aliases.

**Pitfall:** `yew_cursor_left/right` collapse an initially empty selection by
moving both ends. The wrapper must save and restore the anchor in H, including
the first Shift+Arrow, rather than manufacturing a second selection span.

### 2. Explicit clipboard commands — `src/edit/sel_actions.c|h`,
`src/edit/cmd.c`, `src/edit/ed.c`

Register:

```c
CmdStatus yew_sel_cmd_clip_copy (CmdCtx *cx);
CmdStatus yew_sel_cmd_clip_cut  (CmdCtx *cx);
CmdStatus yew_sel_cmd_clip_paste(CmdCtx *cx);
```

Ctrl+C is H-only and captures the existing char/line/rect selection through
the same aggregation used by `ed.sel.yank`, routing it through
`yew_reg_yank(..., '+', ...)`. It keeps H active, matching ordinary yank.
Ctrl+X is H-only and uses the existing delete transaction and selection
collapse, but routes through `yew_reg_delete(..., '+', ...)` so it writes the
system clipboard regardless of `clipboard.sync`.

Ctrl+V is bound in L/W/B/H/I. It reads `+` once through `yew_clip_read`, then
inserts the returned bytes at every collapsed cursor or replaces every active
selection through the established edit choke point. A selection replacement
does not overwrite the clipboard with the displaced bytes. The operation is
one `YEW_TXN_PASTE` undo node; in I it first closes any typing transaction and
remains in I, while replacement from H collapses to L. Empty or failed reads
leave text and selection unchanged and produce a user-visible diagnostic.

The existing canonical route remains: `h`, extend, `y`. With the shipped
`clipboard.sync = "yank"`, `y` writes both Yew's unnamed/`0` registers and the
system clipboard. Ctrl+C/X/V are explicit `+` aliases and therefore remain
system-clipboard operations if a user sets clipboard sync to `off`.

### 3. Safe focused-job dismissal — `src/edit/shell.c|h`,
`src/edit/file_cmds.c`

Add:

```c
bool yew_shell_dismiss_output(Ed *ed);
```

It recognizes only a focused buffer owned by a public
`YEW_SINK_BUFFER` job, resolves `origin_buf_id`, and restores that buffer
through `yew_ed_show_buffer`. A missing origin falls back to the active tab's
stable `buffer_id`, then the primary document buffer. The job and its output
buffer remain alive and listed; a running job keeps streaming while hidden.

`yew_file_cmd_quit()` calls this helper before the whole-editor quit path only
when `bang == false`. A successful dismissal returns `YEW_CMD_OK` and reports
that `:jobs` can reopen the output. `:q!` continues to call
`yew_ed_request_quit(..., true)`. Once the file is restored, a later `:q`
sees its real dirty state and prompts normally.

**Pitfall:** never classify by scratch-buffer flags alone. Diagnostics,
pickers, macro editors, and job tables also use scratch buffers and retain
their own close contracts.

### 4. Scope and deferrals

This sprint does not implement the still-missing general `"x` register-prefix
key state or modal `p`/`P` bindings; those need a separate complete binding
contract rather than being smuggled into GUI aliases. It does not turn job
output into a terminal emulator, clear output merely because it was hidden,
or change ranged-filter semantics. Preserving the complete pre-command
cursor/viewport when the Sprint 19 scratch buffer temporarily occupies a
window is follow-up view-model work; this sprint guarantees restoration to
the correct buffer and quit safety.

## Testing Strategy

- Unit H-mode matrix: first and repeated Shift+Arrow from L/W/B/I, reverse
  direction across the anchor, goal-column vertical motion, UTF-8 grapheme
  boundaries, and unchanged unshifted unit motions.
- Unit selection actions: Ctrl+C keeps H and writes the `+`, unnamed, and `0`
  register values; Ctrl+X writes `+`, deletes once, and is one CUT undo node;
  Ctrl+V insert/replacement, empty/read failure atomicity, I-mode transaction
  separation, and multi-selection aggregation.
- Unit default-keymap freeze: all new bindings resolve to the named commands
  in their intended modes and remain absent in E/F.
- Unit jobs: focused running and finished output returns to its origin without
  release; unforced quit from output does not set `ed.quit`; force quit does;
  dirty origin is visible to the next unforced quit.
- PTY: real Shift+Arrow paints H selection; Ctrl+X/V round-trip through a
  deterministic custom clipboard reader; `:!printf` output followed by `:q`
  restores the original file and a second `:q` exercises its normal guard.
- Regression: existing H-mode, clipboard, register, job, cmdline, tab/group,
  and ranged-filter suites remain byte-identical. Focused ASan/UBSan and
  warning-clean default/core-only Clang and GCC builds are required.

## Definition of Done

1. Shift+Arrow from L/W/B/I enters H with the pre-motion cursor as anchor and
   moves exactly one grapheme or vertical row; repeats never re-anchor.
2. Existing unshifted H and unit-mode arrows are unchanged.
3. Ctrl+C copies the exact active selection to `+` and leaves it highlighted;
   Ctrl+X copies then removes it in one CUT undo node.
4. Ctrl+V reads `+`, inserts/replaces byte-exactly in one PASTE undo node, and
   remains independently undoable from adjacent I-mode typing.
5. Clipboard failure or an empty read changes no text and drops no selection.
6. `h` + motion + `y` remains the canonical modal path and the shipped yank
   sync continues to write the system clipboard.
7. Plain no-range `:!` never mutates or saves the originating file; ranged
   `:!` keeps its existing explicit filter behavior.
8. Unforced `:q` from focused job output restores the originating file without
   releasing the job; `:q!` still force-quits the whole editor.
9. A dirty originating file cannot be hidden from a later quit prompt by a job
   output view.
10. Focused unit/PTY, ASan/UBSan, existing regressions, and strict default and
    `MODULES=""` GCC/Clang builds are green on the committed SHA.
