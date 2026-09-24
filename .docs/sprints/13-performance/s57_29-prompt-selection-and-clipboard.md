# Sprint 57.29: Prompt Selection and the Clipboard

## Prerequisites

- **Sprint 57.28** — the prompt's readline set and the yank stack
  (`src/text/yankstack.[ch]`); `C-v` already bound to `ed.clip.paste` in
  E mode; prompt kills and yanks go through the Insert-mode
  `ed.edit.kill.*` commands on the prompt Win; `Ed.invoke_seq`.
- **The prompt is a Win.** `yew_cmdline_target(ed)`; `line->cur` mirrors
  its primary cursor (`src/ui/cmdline.c:162`) and `Cursor.anchor` exists
  (`src/text/cursor.h:62`) — but the prompt draws NO selection today, and
  most prompt paths reset `anchor = pos` (`cmdline.c:248`, `:350`).
- **Existing commands**: `ed.sel.extend.left|right|up|down`,
  `ed.clip.copy`, `ed.clip.cut`, `ed.clip.paste` (`src/edit/cmd.c:544–566`,
  `sel_actions.c`). `copy` and `cut` are bound in NO mode today.
- **Terminal**: `ISIG` is cleared (`src/term/tty.c:525`), so `C-c` arrives
  as a key. E mode binds `C-g` to `ed.cmdline.cancel` and leaves `C-c`
  and `C-x` unbound.
- Binding: invariants 2 (grapheme-correct selection edges), 5, 9.

## Goals

The user wants GUI-style selection in the `:` / `:!` prompt: Shift+motion
selects, typing replaces the selection, `C-c` copies, `C-x` cuts, `C-v`
pastes, all against the SYSTEM clipboard. Decisions made with the user
(2026-09-23), not to be relitigated:

- `C-c` copies when there is a selection; with none it CANCELS the prompt,
  exactly as `C-g` does, so the abort reflex still works.
- `C-x` cuts the selection to the clipboard; with none it does nothing.
- The clipboard is not the yank stack. Copy and cut never touch the yank
  stack (57.28), and kills never touch the clipboard.

Insert mode gets the same `C-c` / `C-x` (selection only) because it has
Shift-selection and `C-v` but no way to copy or cut from the keyboard.

Deferred: mouse drag-selection in the prompt (mouse is an accelerator,
invariant 9 is met by the keys) → named, not scheduled. The Up/Down/Tab
redesign and `C-r` → 57.30; fish extras → 57.31.

## Deliverables

### 1. Selection state in the prompt — `src/ui/cmdline.c`

The selection is `[min(anchor,pos), max(anchor,pos))` of the prompt Win's
primary cursor; empty when `anchor == pos`. Audit every place the prompt
resets `anchor = pos` and keep only the ones that SHOULD collapse (below).
Selection edges are always grapheme boundaries (extend by grapheme, word or
line units that already respect them).

**Collapse rules** — one table, one helper that every path calls:

| Event | Effect on the selection |
|---|---|
| Shift+motion | extends (anchor stays) |
| plain motion (`<left>`, `C-b`, `A-b`, `C-a`, `<home>`, …) | collapses; `<left>`/`<right>` collapse to the selection's START/END (the GUI convention), others move from the caret |
| printable key, paste, yank | REPLACES the selection |
| `<bs>`, `<del>`, `C-d`, `C-h` | DELETES the selection (only) |
| kill commands (`C-w`, `A-<bs>`, `A-d`, `C-u`, `C-k`) | collapse, then act from the caret as today; they never kill the selection — `C-x` is the cut |
| transpose / case (`C-t`, `A-t`, `A-u`, …) | collapse, then act |
| Tab / completion accept / ghost accept | collapse, then act |
| history (Up/Down, `C-p`/`C-n`), Enter, cancel | collapse; Enter submits the whole line |
| undo / redo | collapse |

**Pitfall — the selection is never text.** It must not leak into
`yew_cmdline_text()`, the history draft, the parse point or the ghost.

### 2. Keys — `runtime/init.fl`

**E mode:**

| Key | Command |
|---|---|
| `S-<left>` / `S-<right>` | `ed.sel.extend.left` / `right` (verify on the prompt Win) |
| `A-S-<left>` / `A-S-<right>` | extend by word (new `ed.sel.extend.word_prev` / `word_next` if none exists; word unit = the prompt's `A-b`/`A-f` unit) |
| `S-<home>` / `S-<end>`, `C-S-<left>` / `C-S-<right>` | extend to line start / end (new `ed.sel.extend.line_home` / `line_end` if none exists) |
| `C-c` | new `ed.cmdline.copy_or_cancel`: selection → `ed.clip.copy`, collapse, stay in the prompt; none → exactly `ed.cmdline.cancel` |
| `C-x` | `ed.clip.cut` when a selection exists; none → no-op, no message |
| `C-v` | `ed.clip.paste` (already bound): replaces a selection; newlines fold to a blank as 57.28 made yank and paste do |

`A-S-<right>` is `ed.shadow.accept_word_alt` in I/L/W/B modes; in E mode it
is free. Verify every chord is free in E before binding; report any
collision rather than overwriting.

**I mode:** `C-c` → `ed.clip.copy` and `C-x` → `ed.clip.cut`, each acting
only when a selection exists and otherwise a silent no-op (Insert has no
prompt to cancel). Verify they are free in I.

Mirror every binding in `tests/unit/test_runtime_defaults.c`; RECOUNT
`yew_bind_active_count` by running the test.

### 3. Drawing — `src/ui/cmdline.c`

Paint the selected cells with the SAME style the document uses for a
selection (find where `draw.c` paints a Win's `anchor..pos`; reuse that
style lookup, do not add a new theme key). Cover: horizontal scroll (the
selection may begin or end off-screen — `s18_cmdline_horizontal_scroll`
is the model), wide graphemes (a double-width cluster is selected whole or
not at all), the ghost (never selected, never styled as selection), and
`nocolor` / 16-colour / ASCII themes (the golden variants `chrome_cmdline`
already has). Invariant 5: the selection is state; identical state renders
identical bytes.

### 4. Clipboard behaviour

Reuse `ed.clip.copy` / `cut` / `paste` rather than writing prompt twins;
make them work on the prompt Win if they do not. The system clipboard path
(OSC 52 and the platform tool, with `fakeclip` in tests) is whatever those
commands already use. `cut` is one undo step in the prompt.

### 5. Size

This adds code to a size-gated binary. **Do not touch `tests/size/`**; the
orchestrator runs the CI rebaseline. Report your estimate of added bytes.

### 6. Defer

- Mouse drag-selection and double-click word selection in the prompt.
- Primary-selection (X11 middle-click) semantics.

## Testing Strategy

- **Unit, real keys through `yew_ed_handle_key` in E mode**
  (`tests/unit/test_prompt_keys.c`): every §2 row; every §1 collapse row;
  `C-c` with and without a selection (copy vs cancel, asserted through the
  fake clipboard and the prompt's `active` flag); `C-x` with and without;
  typing over a selection; `<bs>` over a selection; `C-v` over a
  selection; a selection over a CJK wide cluster and a combining sequence
  (edges on grapheme boundaries); the selection never appearing in
  `yew_cmdline_text()` or the history draft; copy/cut leaving the yank
  stack byte-identical and kills leaving the clipboard untouched.
- **Unit, Insert mode**: `C-c` / `C-x` with a selection hit the fake
  clipboard; without one they change nothing.
- **Unit, undo**: cut, typed replacement and paste-over-selection are each
  one undo step in the prompt.
- **PTY**: `s57_29_prompt_select_and_copy`, `s57_29_prompt_type_over`,
  `s57_29_prompt_select_scrolled` (selection across the horizontal scroll
  edge), `s57_29_prompt_select_nocolor`.

## Definition of Done

1. Every §2 key does what its row says in the prompt, proven by real-key
   unit tests; every §1 collapse row has a test.
2. `C-c` copies with a selection and cancels without; `C-x` cuts or does
   nothing; `C-v` replaces a selection.
3. Insert mode `C-c` / `C-x` work on a selection and are inert without.
4. The selection never reaches the prompt's text, history or parse.
5. Clipboard and yank stack stay separate (asserted both ways).
6. `yew_bind_active_count` recounted by running the test.
7. The four `s57_29_*` goldens pass; full `make test-pty` green.
8. gcc AND clang warning-free: build the WHOLE tree with
   `make -k CC=gcc-16 BUILD=build-gcc FAULTSHIM_ARCH_FLAGS=`; always check
   `snprintf`'s result; never pass a variable by value in the same call
   that writes it through a pointer (C leaves argument order unspecified —
   x86_64 GCC and clang disagree, and this Mac cannot see it);
   `MODULES=""` builds and passes; ASan/UBSan filtered runs clean.
9. `test-fletch test-script test-roundtrip test-roundtrip-coverage
   test-audit` and the `scripts/check-*.sh` gates green.
10. `tests/size/` untouched; added-bytes estimate in the report.

## Implementation notes (divergences from this contract)

Where the contract and the code disagreed, the implementation does what
the contract intends:

1. **The collapse table is one function the dispatcher calls.**
   `yew_cmdline_sel(ed, command, after)` in `src/ui/cmdline.c` holds the
   §1 table keyed by command name; `yew_ed_invoke` calls it around every
   command it runs on the prompt Win -- before the command, inside the
   undo transaction the dispatcher just opened (so a replacement or a
   deletion over a selection is one step), and after it, when everything
   but a Shift+motion leaves the selection collapsed.  That "after" rule
   also collapses what undo and redo restore.  The two existing
   `anchor = pos` resets (`set_error`, `replace_span`) are collapses the
   table wants and stay.  Needed because the cursor motions keep an
   anchor that differs from the caret (that is how H extends): without
   the table a plain motion in the prompt grew the selection.
2. **C-b and C-f collapse to the edge, like `<left>` and `<right>`.**
   They are the same commands (`ed.move.char.prev`,
   `ed.cmdline.ghost.accept`), and the table is per command.  Cocoa's
   emacs keys do the same.  Every other motion collapses at the caret and
   then moves.
3. **`<right>`/C-f with a selection collapse to the END and do not take
   the ghost** -- the GUI rule for `<right>` wins over "ghost accept:
   collapse, then act".  The next `<right>` takes it.  A-f/A-`<right>`
   and C-e/C-`<right>` collapse and then act, as the table says.  The
   ghost stays drawn during a selection (it is a function of the text
   and the caret) and is never styled as selected.
4. **Shift+motion in the prompt used to enter H.**  `ed.sel.extend.*`
   called `yew_mode_enter_highlight` unconditionally; on the prompt Win
   they now run the motion and put the anchor back, and E stays E.
   The new `ed.sel.extend.word_prev|word_next|line_home|line_end` use
   the same path (H in a document).  Motion words are at most 16
   characters, so they are `sel_word_prev`, `sel_word_next`,
   `sel_line_home`, `sel_line_end`.
5. **Insert mode never holds a selection.**  Insert's Shift+arrows enter
   H (Sprint 57.12), where C-c and C-x were already bound
   (`keys_highlight.c`), so Insert users could already copy and cut from
   the keyboard.  An anchor away from the caret outside H is not a
   selection (57.13's rule, which the context menu relies on), so the new
   I-mode C-c/C-x are always inert and return OK, which keeps the
   typing transaction open.  The Insert test drives the real path:
   Shift+arrows from Insert, then C-c reaches the fake clipboard.
6. **`ed.clip.copy`/`cut` choose where they act in one place**
   (`clip_scope` in `sel_actions.c`): the prompt's selection, H, inert in
   I, refused elsewhere as before.  The prompt's cut keeps the prompt open
   and in E (H's cut returns to L) and is one `YEW_TXN_CUT` step.  Copy
   collapses at the caret.
7. **A-., C-r/A-r and C-q replace a selection too**: A-. through its own
   single edit, and the register and literal inserts through the
   `ed.edit.insert.text` they run.  A bracketed paste replaces a selection
   as a typed key does.
8. **Two more goldens**: `s57_29_prompt_select_colors_16` and
   `s57_29_prompt_select_ascii` run the nocolor scene under the other two
   degradations `chrome_cmdline` has.  The harness hands `YEW_CLIPBOARD`
   to `s57_29_prompt_select_and_copy`, as it does to
   `s57_12_clipboard_*`.
9. **383 bindings** (371 + 12: ten in E, two in I).
