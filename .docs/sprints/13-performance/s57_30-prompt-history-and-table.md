# Sprint 57.30: Prompt History and the Completion Table

## Prerequisites

- **Sprint 57.29** — prompt selection and its ONE collapse helper; history
  navigation collapses the selection.
- **Sprint 57.28** — the prompt readline set; `A-r` inserts a register
  (so `C-r` is free to move here); `Ed.invoke_seq`.
- **Sprint 57.26** — the history suggestion snapshot (yew's own bang
  entries, then fish/zsh/bash per `shell.suggest_history`, default
  `all`), loaded on the first idle turn after the prompt opens.
- **Sprint 57.17 §2** — today's rule, `yew_cmdline_cmd_up` /
  `yew_cmdline_cmd_down` (`src/ui/cmdline.c:1424–1440`): Up takes the
  pager whenever a list is open and non-empty; Down moves in the pager
  only while it has focus; `history_move` (`:1086`) walks
  `yew_hist_prev/next` and replaces the whole line.
  `Menu.explicit_sel` / `yew_menu_focused` mark "the user has entered the
  table". `C-p`/`C-n` are bound to completion next/prev.
- Binding: invariants 4, 5, 9.

## Goals

Dogfooding found 57.17's rule backwards for a shell prompt. The live menu
is open the whole time a token is typed, so Up lands in the table instead
of history. The user's design (2026-09-23, not to be relitigated):

1. **Up/Down are history** unless the user has ENTERED the table with Tab.
2. **Tab enters the table**; inside it Up/Down move between rows.
3. **Up on the table's top row** leaves the table AND goes to history: the
   line is replaced with the previous history entry and the table closes.
4. **Down on the bottom visible row** scrolls when rows are hidden below;
   on the true LAST row it leaves the table back to the line, KEEPING the
   highlighted candidate that navigation already wrote there. Up from the
   line then goes to history again.
5. Entering the table again restores row navigation.
6. **History is fish-style "smart"**: with text on the line, Up walks
   older entries that CONTAIN that text (substring), newest first, no
   duplicates, with the matched part highlighted. An empty line walks all
   history.
7. **`C-p`/`C-n` mirror Up/Down exactly**, edge rules included.
8. **`C-r`** searches history; **`A-<up>`/`A-<down>`** search history for
   the TOKEN under the caret.

## Deliverables

### 1. Focus and the arrows — `src/ui/cmdline.c`

Rewrite `yew_cmdline_cmd_up/down` and the 57.17 §2 comment block to state
the new ONE rule. "In the table" is `yew_menu_focused(&line->menu)`, which
only Tab (and `S-<tab>`) grant. The live, unfocused menu never takes an
arrow.

| State | Up | Down |
|---|---|---|
| not in table | history older (§2) | history newer (§2) |
| in table, not top row | row up | — |
| in table, top row | leave table, close it, history older from the ORIGINAL typed text (§2) | — |
| in table, not bottom visible row | — | row down |
| in table, bottom visible row, rows hidden below | — | scroll one row (selection moves down) |
| in table, true last row | — | leave table, keep the candidate in the line, table stays OPEN but unfocused |

`<pgup>`/`<pgdn>` keep paging inside the table.

**Pitfall — "the original typed text".** Row navigation writes each
candidate into the line. Up off the top must search history with what the
user TYPED (`line->menu_stem` / `menu_original`), not with the candidate
the preview last wrote, or history is filtered by a word the user never
typed.

### 2. Smart history — `src/ui/cmdline.c`, `src/ui/cmdhist.[ch]`

- **Source.** A bang line (`:!`, `:!!`, `:r !`, a range's `!`) walks the
  57.26 suggestion snapshot, so Up and the ghost always agree (yew's own
  entries first, then the shell files the option allows). Other prompts
  walk the prompt's own history, as today.
- **Term.** Frozen when a history walk BEGINS (the first Up/Down after an
  edit): the line's text then (for a bang line, its body). Walking does
  not re-derive it. Any edit ends the walk; the next Up starts a new one
  with the new text.
- **Match.** Case-sensitive substring of the entry (bang: the body). Empty
  term matches every entry. Duplicates skipped (an entry equal to one
  already visited in this walk). Newest first.
- **End.** Down past the newest match restores the text the walk began
  with (readline's draft). Up past the oldest stays put.
- **Highlight.** While a walked entry is on the line, its first
  occurrence of the term is drawn in the search-match style the document
  uses for `/` matches (reuse; no new theme key). State, not text: it
  never enters the line's bytes.

### 3. `C-p` / `C-n` — `runtime/init.fl`

Rebind to `ed.cmdline.up` / `ed.cmdline.down`. Completion next/prev stay
on Tab / `S-<tab>`.

### 4. `C-r` history search — new `ed.cmdline.hist_search`

Opens the existing pager (57.17's widget; no new widget) in a HISTORY
mode listing the entries that match the current line text (same source
and match rule as §2), newest first, with the matched part highlighted
per row. While it is open:

- typing edits the line and refilters the list live;
- Up/Down/`C-p`/`C-n` move between rows; `C-r` again moves to the next
  OLDER match; `<pgup>`/`<pgdn>` page;
- Enter puts the highlighted entry in the line and closes the list (it
  does NOT execute — a second Enter does);
- `<esc>` / `C-g` close the list and restore the line as it was when
  `C-r` was pressed.

The pager's footer names the mode (`history search`).

### 5. Token search — `A-<up>` / `A-<down>`, new `ed.cmdline.token_prev` / `token_next`

fish's `history-token-search`. The token is the shell word under the caret
(`yew_shctx_at` on a bang line; whitespace-delimited otherwise); its text
is the term. `A-<up>` replaces JUST that token with the next older
distinct token, from any history entry, that contains the term;
`A-<down>` walks back; walking past the newest restores the original
token. Tokens from a bang entry are its raw shell words (quotes as typed).
Consecutive presses continue the walk (`Ed.invoke_seq`); anything else
ends it.

### 6. Bindings — `runtime/init.fl`

`<up>`/`<down>` keep `ed.cmdline.up`/`down` (new semantics). `C-p`/`C-n`
→ same. `C-r` → `ed.cmdline.hist_search` (moving off insert-register,
which 57.28 already put on `A-r`). `A-<up>`/`A-<down>` → token search;
verify both are free in E mode. Mirror in
`tests/unit/test_runtime_defaults.c`; RECOUNT `yew_bind_active_count` by
running the test.

### 7. Existing tests that pin 57.17 §2

Tests and goldens asserting "Up takes the pager while a list is open"
encode the rule this sprint replaces. EDIT them to the new rule (do not
delete coverage) and name each one in the report. Inspect every golden
diff before regenerating.

### 8. Size

**Do not touch `tests/size/`**; the orchestrator runs the CI rebaseline.
Report an added-bytes estimate.

### 9. Defer

- fish's history pager with multi-select delete (`C-r` then `S-<del>`).
- Persistent shell session → 57.27.

## Testing Strategy

- **Unit, real keys through `yew_ed_handle_key` in E mode**
  (`tests/unit/test_prompt_keys.c`): every row of the §1 table, including
  Up off the top row searching with the TYPED text (not the previewed
  candidate), Down off the true last row keeping the candidate and leaving
  the table open but unfocused, then Up going to history, then Tab
  re-entering the table.
- **Unit, smart history**: substring walk, frozen term, duplicates
  skipped, Down past newest restores the draft, empty line walks all,
  bang lines walking the 57.26 snapshot (fixture HOME histories, as 57.26's
  tests do), non-bang lines walking the prompt history; the highlight never
  entering `yew_cmdline_text()`.
- **Unit, `C-r`**: filter as typed, `C-r` again goes older, Enter fills
  without executing, Esc restores.
- **Unit, token search**: replaces only the token; distinct tokens; walk
  back restores.
- **Unit, `C-p`/`C-n`** identical to Up/Down in every §1 state.
- **PTY**: `s57_30_up_is_history_with_menu_open`,
  `s57_30_table_top_to_history`, `s57_30_table_bottom_exit`,
  `s57_30_substring_history_highlight`, `s57_30_ctrl_r_search`.

## Definition of Done

1. Every §1 row, §2 rule, §4 and §5 behaviour proven by real-key tests.
2. With the live menu open, Up goes to history (the dogfooding bug).
3. `C-p`/`C-n` behave identically to Up/Down.
4. Tests that pinned 57.17 §2 are edited, not deleted, and listed.
5. `yew_bind_active_count` recounted by running the test.
6. The five `s57_30_*` goldens pass; full `make test-pty` green.
7. gcc AND clang warning-free: build the WHOLE tree with
   `make -k CC=gcc-16 BUILD=build-gcc FAULTSHIM_ARCH_FLAGS=`; always check
   `snprintf`'s result; never pass a variable by value in the same call
   that writes it through a pointer (argument order is unspecified; x86_64
   GCC and clang disagree, and this Mac cannot see it); `MODULES=""`
   builds and passes; ASan/UBSan filtered runs clean.
8. `test-fletch test-script test-roundtrip test-roundtrip-coverage
   test-audit` and the `scripts/check-*.sh` gates green.
9. `tests/size/` untouched; added-bytes estimate in the report.

## Implementation notes (divergences from this contract)

Where the contract and the code disagreed, or the contract was silent,
the implementation does what the contract intends:

1. **The page keys enter the table too.**  Tab and S-Tab grant focus
   (`yew_menu_focus` in `completion_cycle` and in `complete`'s
   enter-the-list branch), and so do `<pgup>`/`<pgdn>`: they choose a row
   and write it into the line exactly as Tab does, and "keep paging inside
   the table" needs them to be in it.  A click still selects without
   entering, so the arrows stay history after one.  The unbound Fletch
   preview commands `ed.cmdline.menu.next`/`prev` are unchanged.
2. **"The typed text" is the table's stem** (`menu_stem` put back over
   `menu.replace`) -- the text Esc restores.  When the first Tab extended
   the word by its common prefix (`fil` -> `file.`), that is the text the
   table was entered from, so Up off the top row searches with `file.`,
   never with the row the preview wrote (`file.new`).  With nothing older
   matching, the line goes back to that text with its live table,
   unfocused.
3. **While a walked entry is on the line there is no table and no
   ghost.**  `cmdline_refilter_as` still computes the hint, then closes
   the table; the next edit ends the walk and the live table returns.
   Tab also ends a walk.  Down past the newest restores the draft and its
   live table.
4. **The smart walk lives in `src/ui/cmdhist.[ch]`** (`YewHistView`,
   `YewHistWalk`, `yew_hist_find`, `yew_hist_walk_*`): a view is the
   prompt's `CmdHist` or the 57.26 snapshot, newest first.  `CmdLine.hist`
   (a `HistCur`) became `CmdLine.walk`; `yew_hist_prev/next` (prefix)
   remain for the Fletch REPL.  The unbound `ed.cmdline.hist_prev/next`
   drive the same walk as the arrows.
5. **Bang-line term and placement.**  The term is the body with its
   leading blanks dropped (the snapshot drops them too); an entry is
   placed after the draft's first `walk_prefix` bytes -- the `!`, `r !`,
   `%!` the user typed.
6. **Duplicates.**  Both sources already hold each text once (CmdHist on
   add, the snapshot on load); the walk still skips a repeat through its
   `seen` stack, pinned on a hand-built view.  C-r's rows rely on the
   sources' dedupe, keeping its refilter one pass per keystroke.
7. **The highlight** reuses the document's `/` match style through the
   new `yew_draw_search_style` (extracted from `draw_search_rows`: theme
   `search.match`, then the no-colour and 16-colour degradations);
   `yew_cmdline_hist_match` exposes the span as state.
8. **C-r details.**  Rows are capped at 512 (the footer then reads
   `512+ of N`); every refilter selects the newest row; C-r, Up and Down
   stop at the ends rather than wrap.  The footer's mode name rides
   57.32's `where` note (`history search 2/2`); with no match the hint
   reads `history search: no match`.  A click selects a row and a second
   click fills the line, like Enter; Tab leaves the search (keeping the
   line) for completion; generator arrivals and the idle directory scan
   never touch C-r's rows.
9. **Token search.**  Entries newest first, each entry's words right to
   left (fish's order); a token equal to the one the walk began on is
   skipped along with repeats.  Words split at unquoted blanks with
   quotes and escapes honoured on a bang line, at blanks otherwise, at
   most 256 per entry.  The caret's token on a bang line starts where
   `yew_shctx_at` says and runs to the end of that shell word.
   A-<down> with no walk in progress does nothing.
10. **The three new commands are internal**, like `ed.cmdline.last_arg`:
    not recordable, so no motion word and no `gen_cmds[]`/`D(...)` row;
    verbs `hist_search`, `token_prev`, `token_next` added to
    `command_name_valid`; all three in the invariant-9 list and in
    57.29's collapse table (with `ed.cmdline.up/down/hist_prev/hist_next`,
    all PSEL_COLLAPSE).  None is `YEW_CMD_PROMPTS` -- each acts only on
    an already-open prompt, as `complete_next` and `last_arg` do -- so
    none joins the batch refusal table.
11. **PTY.**  The runner allows one snapshot per case, so
    `s57_30_table_bottom_exit` pins the exit state (candidate kept, table
    open); the Up that follows is pinned by the unit tests.
    `s57_17_pager_arrow_up` now drives Tab, Tab, Up (the empty-history
    branch of Up off the top row) and `s57_17_pager_tail_row` enters the
    table with Tab.
12. **Bindings: 385** (383 + `A-<up>`, `A-<down>`; C-p, C-n and C-r were
    rebound in place).

### Tests edited from 57.17 §2's rule (§7)

- `test_cmdline_up_enters_the_pager_without_touching_the_prompt` ->
  `test_cmdline_up_is_history_with_the_live_menu_open`
- `test_cmdline_leaving_the_pager_restores_history_to_up` ->
  `test_cmdline_up_off_the_top_row_walks_history_from_the_typed_text`
- `test_cmdline_enter_on_a_pager_row_accepts_without_executing`,
  `test_cmdline_typing_blurs_the_pager_but_keeps_the_row`: enter by Tab
- `test_cmdline_bang_completions_use_the_same_pager`: Up is history
- `test_cmdline_up_is_history_when_no_pager_is_open`: comment only
- `test_cmdline_printable_edit_resets_history_walk_to_new_draft`,
  `test_cmdline_ghost_is_never_in_the_buffer`,
  `test_prompt_keys_selection_is_never_text`: the walk's draft/term
- `test_prompt_keys_alt_r_inserts_a_register`: C-r is history search
- `tests/unit/test_runtime_defaults.c`: the E mirror and the count
- PTY `s57_17_pager_arrow_up`, `s57_17_pager_tail_row` and their goldens
