# Closeout: F09 — Recorder and replay

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s35, s38

## DoD items (every numbered item from every sprint above)

- [ ] s35.1 `make test` green at `-Werror`; gcc and clang builds, ASan/UBSan, valgrind, and determinism remain CI gates. `make test-roundtrip` is part of `make test` and completes in ≤ 60 s.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s35.2 **The law holds**: 2 000 generated seeds plus the whole committed corpus pass properties P1–P5, from six initial fixtures incl. CRLF, no-final- newline, and invalid-UTF-8 content.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s35.3 The shrinker works: `YEW_RT_SELFTEST=1` fails, shrinks a 96-event session to ≤ 3 events, names the divergent op and the first differing byte, and writes a replayable `.rec`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s35.4 Emitter fuzzer: 10 min in the ASan lane, zero unparseable emissions, zero crashes; corpus committed and replayed in `make test`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s35.5 Every row of §2's mapping table and all five folding rows have a unit test; the `TAKES_COUNT` no-fold rule has both a unit test and a round-trip fixture that fails without it.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s35.6 **One execution path**: `grep -c 'yew_cmd_invoke\|yew_edit_\| yew_textbuf_\|yew_reg_set' src/fl/record.c` → `0`; replay goes through the ordinary cached Fletch compiler plus `fl_call_chunk` only.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s35.7 `5@a` produces exactly 5 undo nodes; a macro whose 3rd command errors leaves the buffer byte-identical to the pre-replay state.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s35.8 Recording during replay records nothing (`YEW_SRC_REPLAY` drop asserted); `q` while recording stops rather than nesting.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s35.9 Generator coverage: every `YEW_CMD_RECORDABLE` command is either reached by the generator or on the denylist with a stated reason; `make test-roundtrip-coverage` fails on an unreached, unlisted command.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s35.10 Every `YEW_CMD_RECORDABLE` command emits a word that `yew_cmd_by_word` maps back to the same `CmdId` (bijection asserted over the whole registry — Sprint 34 enforces the field, this asserts the round trip).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s35.11 Perf: tap overhead ≤ 200 ns/dispatch, emit of 10 000 events ≤ 10 ms; baselines committed to the perf lane.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s35.12 Pty goldens for record/replay byte-stable across the determinism lane's double run.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s35.13 ≥ 120 recorder/replay unit assertions and ≥ 20 committed `.rec` corpus files. Macro-driven script tests are explicitly Sprint 37.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s35.14 `ed.macro.*` no longer `YEW_CMD_DEFERRED`; Sprint 38 surfaces (browser, edit flow, library) hard-error naming Sprint 38.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s38.1 `make test` green on gcc and clang at `-Werror`; ASan/UBSan/valgrind/ determinism lanes clean; `make test-roundtrip` still green with the record→edit→store no-op fixture added.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s38.2 Indicator: present while recording, names the register, never dropped or truncated below `●a` at 40 columns, damages the footer only on change (frame count unchanged across a 4 000-key burst).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s38.3 `5@a` produces exactly 5 undo nodes; one `u` undoes exactly one run.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s38.4 Browser: registers and library entries in one deterministic list; all six keys (`Enter e y d n /`) work; fuzzy filter searches name **and** source; byte-stable golden.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s38.5 **Edit flow**: `ed.macro.edit` opens an ordinary buffer (proven by a test that runs multi-cursor editing and undo inside it); `:w` compiles first and stores nothing on failure, with the caret at the right line; an unknown motion word is rejected at store time with a suggestion.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s38.6 `grep -rn 'macro_reg' src/` hits only three files — the scratch buffer is not a special editor.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s38.7 `:macro name` leaves the register intact, appends to the library, and the named function is callable immediately (no restart).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s38.8 Library: all eight naming/collision rows tested; a collision hard-errors naming both files; a broken file is skipped and the rest load; scan order bytewise and stable.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s38.9 Header: all five fields parsed; unknown schema version skipped; a `keymap:` mismatch demonstrably changes nothing about execution (a macro recorded under a rebound keymap replays byte-identically under the default one — the sprint's headline claim, as an executable test).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s38.10 `--batch`: replay, list, and library work headless; `ed.macro.record` refuses naming itself per Sprint 37's contract.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s38.11 Perf: 200-file library scan ≤ 5 ms; committed baseline; startup budget unchanged.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s38.12 Pty goldens for the indicator (2 widths), the edit flow, and the browser are byte-stable across the determinism lane's double run.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s38.13 ≥ 90 assertions across the two unit files; ≥ 15 script tests; the deferred surfaces hard-error naming Sprints 41 and 55.
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

none

## Deferrals recorded against this front

none

## Verdict

NOT READY — Independent evidence review remains pending.
