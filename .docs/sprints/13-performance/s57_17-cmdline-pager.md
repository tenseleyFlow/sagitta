# Sprint 57.17: Command-Line Pager — Unambiguous Execute and a Navigable Preview

## Prerequisites

- Sprint 18/18.5 — `CmdLine` (`src/ui/cmdline.h:29`), `yew_cmdline_cmd_accept`
  (`cmdline.c:1455`) and its §6 rule that Enter accepts a chosen row rather
  than executing; the ranked `Menu` (`src/ui/menu.h:35`) with selection held by
  IDENTITY (`menu.c:75`), `yew_menu_move` / `_select` / `_scroll` / `_rows`;
  `MenuSpec{.max_rows = 5}` set at `cmdline.c:477`; the bottom-aligned draw at
  `menu.c:296`; `YEW_REGION_MENU_ROW` per row.
- Sprint 26 — `src/ws/fuzzy.c`: `yew_fz_score` / `yew_fz_rank`, `FzMatch.pos`
  byte offsets for highlighting, the basename tier, stable-sort tie-breaking.
- `src/ui/cmdcomp.[ch]` — `CompSource` by kind, `yew_comp_filter_run` with the
  1500 µs live budget, the sliced `DirListing` cache and its idle resume.
- `src/ui/cmdparse.[ch]` — `resolve_name` (`cmdparse.c:638`) whose "exactly one"
  rule is unique PREFIX, not fuzzy.
- Binding: invariants 4, 5, 9.

## Goals

Two gaps between what the command line shows and what it will do.

**An unambiguous fuzzy match cannot be executed.** Typing `:fwq` narrows the
live menu to one row, but Enter runs `yew_cmd_parse`, whose `resolve_name`
does unique-PREFIX resolution and fails. The user can see exactly one answer
and cannot take it without pressing Tab first.

**The preview cannot be entered.** `<up>`/`<down>` in E mode are command-line
HISTORY (`runtime/init.fl:261-262`). The only keyboard routes into the list are
Tab / Shift-Tab / C-n / C-p, and every one of them immediately writes the
selected candidate into the prompt — moving the selection is not a pure
preview move. Arrowing up into the list to look before choosing is impossible.

The list is also fixed at five rows with no way to see past them except
paging, which loses your place.

Deferred, named: completion inside `:!` (Sprint 57.18 builds on this pager);
multi-column layout; fuzzy matching for command ARGUMENTS; persisting the
pager across prompts.

## Deliverables

### 1. Unambiguous fuzzy execute — `src/ui/cmdline.c`, `src/ui/cmdparse.c`

When Enter is pressed with no explicitly chosen row, and the live menu holds
**exactly one** candidate of kind `YEW_COMP_CMD`, execute that command rather
than prefix-resolving the typed text.

- The rule is "the live filter left exactly one row", a concept that does not
  exist today — the survey found "exactly one" only in `complete()`'s
  `items.len == 1U` and `resolve_name`'s `nmatch == 1U`. Add it as one
  predicate and use it in both places rather than a third copy.
- It applies ONLY when the caret is in token 0 and the typed text failed
  ordinary resolution. A typed name that resolves by prefix keeps winning —
  this never changes the meaning of a command that already worked.
- Arguments typed after the name are preserved and re-parsed against the
  resolved command, so `:fwq somefile` behaves as the full name would.
- The resolved name is what enters history, not the fuzzy stem.
- When the filter leaves more than one row, Enter behaves exactly as today:
  parse error, prompt stays open, offending token underlined.

**Pitfall:** `yew_cmdline_cmd_accept` snapshots the prompt generation because
the command it runs may replace the prompt. Resolving through the menu must
happen before invocation and must not read `line->menu` afterwards.

### 2. Arrowing into the preview — `src/ui/cmdline.c`, `runtime/init.fl`

`<up>` enters the pager when it is open and non-empty; otherwise it is history,
exactly as today. `<down>` moves within the pager when the pager has focus,
otherwise history. This is the fish behaviour the request names.

- New internal commands `ed.cmdline.menu.prev` / `.next` that move the
  selection WITHOUT writing into the prompt — the difference from the existing
  `complete_next` / `complete_prev`, which insert on every move. Register them
  `YEW_CMD_INTERNAL` like the other menu commands.
- Rebind `<up>` / `<down>` in E mode to new dispatchers that choose pager
  versus history by whether the pager is open and focused. History must remain
  reachable with the pager open: leaving the pager (Escape, or moving above the
  first row) returns focus to the prompt and the next `<up>` is history again.
- Entering the pager marks the selection explicit, so the existing §6 Enter
  rule already makes Enter accept the highlighted row. Verify rather than
  duplicate that logic.
- `ed.cmdline.ghost.accept` on `<right>` and the mouse paths must keep working
  unchanged.

The E-mode keymap is mirrored literally in `tests/unit/test_runtime_defaults.c`
and its binding count asserted; update the mirror and recount by running it.

### 3. A scrolling pager with an honest tail — `src/ui/menu.c`

The list scrolls rather than being capped at what fits.

- Keep `max_rows` as the VISIBLE height. `yew_menu_scroll` already moves `top`
  without touching `sel`; selection movement must now scroll `top` to keep the
  selection visible, which `yew_menu_move` partially does — extend it rather
  than adding a second scroll rule.
- The last visible row becomes an honest tail when more rows exist below:
  `… and N more`. Moving down onto it scrolls by one and keeps the tail; it is
  never itself selectable as a candidate.
- The existing right-aligned footer already renders `"%u/%u"` and `"%u+ of %u"`
  (`menu.c:344`). Reconcile the two rather than showing both counts: the tail
  row is the fish-like affordance, the footer is the precise count. State the
  rule in the header so a later change cannot make them disagree.
- Determinism: what is drawn is a pure function of `items`, `sel`, `top` and
  the area. No clock.

## Testing Strategy

- **Unit (`test_cmdline.c`)**: one fuzzy match executes and closes; two matches
  keep today's parse error; a prefix-resolvable name is unaffected; arguments
  survive fuzzy resolution; history records the resolved name; `<up>` with no
  pager is history; `<up>` with a pager enters it and does NOT alter the prompt
  text; leaving the pager restores history to `<up>`; Enter on a pager row
  accepts without executing (the existing
  `test_cmdline_enter_accepts_a_chosen_row_without_executing` must stay green,
  as must `test_cmdline_enter_executes_while_filtering`).
- **Unit (`test_menu.c`)**: scrolling keeps the selection visible; the tail row
  appears only when rows remain; moving onto the tail scrolls; the tail is
  never returned by `yew_menu_selected`; all seven existing tests stay green,
  especially selection-by-identity across a refilter.
- **PTY**: regenerate the goldens whose row content or footer changes and
  inspect each — the survey lists fourteen cmdline cases, of which
  `cmdline_completion_menu`, `cmdline_menu_scrolled`,
  `cmdline_menu_enter_not_execute` and the four `chrome_cmdline*` are the ones
  most likely to move. New: `s57_17_fuzzy_one_executes`,
  `s57_17_pager_arrow_up`, `s57_17_pager_tail_row`.
- **Perf**: `tests/perf/perf_cmdcomp.c` must stay green — the 1500 µs live
  budget and the opendir-count assertion are unaffected by pager changes, and
  proving that is the point.

## Definition of Done

1. A fuzzy stem matching exactly one command executes on Enter, with arguments
   preserved; two or more matches behave exactly as today.
2. `<up>` enters an open pager and is history when none is open; leaving the
   pager restores history.
3. Moving the selection in the pager does not modify the prompt text.
4. The pager scrolls, shows `… and N more` when rows remain, and that row is
   never selectable as a candidate.
5. Every existing cmdline, menu, fuzzy and cmdcomp test stays green, or is
   changed with a stated reason.
6. gcc and clang warning-free; unit, ASan/UBSan, PTY, perf green;
   `MODULES=""` builds.
