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
