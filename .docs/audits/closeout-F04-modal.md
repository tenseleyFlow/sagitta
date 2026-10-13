# Closeout: F04 — Modal semantics

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s13, s14, s15, s16, s17

## DoD items (every numbered item from every sprint above)

- [ ] s13.1 `make test` green on gcc and clang, `-std=c11 -pedantic -Wall -Wextra -Werror`, ASan/UBSan/valgrind clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s13.2 Key-sequence round-trip holds for ≥ 80 atoms and for every row of every shipped table.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s13.3 All six `yew_keymap_build` validations fire, including the `<esc>`-as-prefix rejection.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s13.4 Chord matrix green (four `q`-family outcomes, dead-sequence drop, Esc cancel, re-feed), driven entirely by a fake clock: `grep -rn 'clock_gettime\|time(' src/edit/keymap.c src/edit/dispatch.c` → empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s13.5 Layer ownership proven by a 3-layer test where merging and ownership differ; the reported owning-layer name is asserted.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s13.6 All seven §7 count rows tested; `YEW_COUNT_MAX` clamp asserted.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s13.7 Registry determinism: two `--help-cmds` runs byte-identical; ≥ 40 commands registered, every one with a non-empty `help`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s13.8 `scripts/check-cmd-dispatch.sh` passes on the tree and fails on a seeded second call site.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s13.9 Every DEFERRED command returns `YEW_CMD_ERR_DEFERRED` naming its sprint (no silent stubs, invariant 3).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s13.10 Keymap lookup cost: 10⁶ single-key dispatches through a 300-row table in < 150 ms on the CI runner (crude ceiling; the keypress→paint gate lands in Sprint 14).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s13.11 ≥ 140 assertions across the six new unit files.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s13.12 `src/edit/keys_default.c` holds data only — no function definitions besides the single `yew_keys_default_install`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s14.1 `yew tests/fixtures/hello.txt` opens, arrows move, `i` inserts, Esc returns to L, `s` writes, `q` exits 0 — proven by a pty golden chain, byte-identical across two runs.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s14.2 `load → edit → save → cmp` preserves everything the s08 contract promises: CRLF, BOM, no-final-newline, invalid bytes, on a 6-fixture matrix driven through the **live editor**, not the API.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s14.3 Typeahead coalescing: the burst test observes exactly 1 frame for 4096 keys and for a 4 KB paste; a per-key render regression fails it.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s14.4 Latency gate live: keypress→paint p99 ≤ 5 ms and cold start → first paint ≤ 20 ms on the CI reference runner, with committed baselines.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s14.5 kill -9 torture green against the live editor: 100 % of iterations satisfy the s08 checker across the shim sweep, 4 seeds, and 500 external-SIGKILL iterations. `make torture` includes the new lane.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s14.6 Terminal restore holds for the live editor: `kill -SEGV`, `kill -TERM`, `kill -9`, and `ed.suspend`+`fg` each leave a usable terminal (pty test asserts the restore blob on the wire, invariant 6).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s14.7 Every L and I table row from §4/§5 is exercised by a unit or pty test; `ed.mode.enter` for W/B/H/E/F messages its sprint number.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s14.8 Undo granularity: typing `hello`, Esc, `u` restores the pre-insert buffer in one step; typing `hel`, `<left>`, `lo`, Esc needs two.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s14.9 Dirty-quit and journal-recovery prompts: all 3 + 3 answers covered; `w` on a failing save aborts the quit and keeps the buffer dirty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s14.10 `grep -rn 'vp\.top\|vp->top' src/ | grep -v 'ui/draw.c\|ui/win' ` → empty (viewport internals stay behind their two consumers, so Sprint 15's replacement is local).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s14.11 No direct key→function wiring: `scripts/check-cmd-dispatch.sh` still passes with ≥ 35 new commands registered.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s14.12 ≥ 120 assertions across the three new unit files; ≥ 10 pty goldens; ASan/UBSan/valgrind clean including the pty and torture binaries.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s15.1 All seven follow-cursor rules and all six wrap rules have dedicated unit assertions with hand-computed expectations.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s15.2 `sum(yew_wrap_rows)` matches the naive oracle for 1 000 random lines × 6 widths, including width 1 with wide glyphs; zero infinite loops (test has a step budget).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s15.3 Resize round-trip 80×24 → 40×12 → 80×24 produces a byte-identical grid and identical `Viewport` fields.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s15.4 Scroll throughput gate green (≥ 120 fps equivalent full-viewport redraw, both wrap modes, both sizes), baselines committed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s15.5 Statusline: the mode chip's role color is applied to the whole footer row in the golden (assert the SGR run covers `cols`), and truncation follows the pinned priority order at ≥ 6 widths.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s15.6 Position reads `line:GCol` and is correct after a wide glyph and after a tab (pty golden with `漢字\tx`).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s15.7 `FileMeta` indicators all reachable: `crlf`, `mixed!`, `bom`, `bin`, `!utf8` each appear in a golden.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s15.8 Message severities: INFO/WARN expire on schedule under the fake clock; ERROR survives until a key; the not-buried rule tested.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s15.9 Gutter width change across the 999→1000 boundary triggers full damage (frame counter asserts one full redraw, not a partial shift).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s15.10 `grep -rn 'wcwidth\|cells\|width' src/ui/ | grep -v 'unicode/'` shows calls into `src/unicode/` only — zero width arithmetic in `src/ui/` (the s09 rule, re-gated).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s15.11 Degenerate 4×1 terminal renders without a crash or an out-of-bounds write (ASan lane).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s15.12 ≥ 130 assertions across the four new unit files; ≥ 14 pty goldens, all byte-stable across two runs.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s16.1 `WordBreakTest.txt` conformance: 100 % of the checked-in corpus passes; any skip is listed in the fixture with a written reason.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s16.2 Every row of the §3 boundary table and the subword table has an assertion, including both CJK rows, both emoji rows, and the invalid-byte row.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s16.3 The §7 conformance suite passes for all four engines × both `alt` values; adding a stub engine that violates rule 2 fails the suite (proved once by a deliberately broken engine in the test file).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s16.4 B mode on a plain `.txt` with zero brackets yields ≥ 3 distinct containment levels (paragraph → section-or-indent → buffer), asserted on a prose fixture — the "useful without a language" gate.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s16.5 The containment chain on a C fixture returns 4 hand-computed levels; the syntax provider is registered, has a NULL `enclosing`, and is skipped without a branch anywhere outside `yew_block_level`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s16.6 Indent blocks absorb interior blank lines and never end on a blank line (Python-shaped fixture, both asserted).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s16.7 Quote/comment heuristics: `don't` does not suppress brackets; `x = 'a[b'` does; a `//` comment does; each in a unit test.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s16.8 Scan budget honored: unbalanced 100 kLOC fixture falls through to indent/paragraph in < 1 ms and never scans the whole file.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s16.9 Fuzz: 100k ops × 4 seeds, zero contract violations, ASan/UBSan clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s16.10 `grep -rn 'wcwidth\|0x4E00\|0x1F3' src/edit/` → empty: no hard-coded codepoint ranges and no width math outside `src/unicode/`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s16.11 `yew_block_provider_syntax_install` hard-errors naming Sprint 40; multi-cursor expand hard-errors naming Sprint 17.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s16.12 ≥ 160 assertions across the four new unit files; perf checks inside the keystroke budget; determinism lane green on the new goldens.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s17.1 Mode-memory: entering H from L/W/B/I selects the matching engine and renders `H·L`/`H·W`/`H·B`/`H·C`; the pinned form `ed.mode.enter H W` overrides. All five cases asserted (unit + golden).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s17.2 Every §2 rectangular edge-case row has an assertion, including both wide-glyph rows, both tab rows, and the short-line pad/skip pair.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s17.3 The highlight and the edit agree: for every rect fixture, the set of cells rendered selected equals the set of cells removed by `ed.sel.delete` (mechanically compared, not eyeballed).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s17.4 Every §3 action is registered, recordable, and covered for all three selection kinds; `ß`→`SS` and the join punctuation rules asserted.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s17.5 One transaction: a 200-cursor edit is exactly **one** undo step and exactly **one** journal transaction (`yew_journal_sync` called once — asserted via the s08 fault shim's intercept log).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s17.6 Rollback: a per-cursor failure leaves the buffer byte-identical to its pre-command state (fixture + assertion).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s17.7 All six overlap/merge rows tested; `yew_cset_check` invariants hold before and after every transaction in the fuzz run (100k ops × 4 seeds, zero violations, ASan/UBSan clean).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s17.8 Multi-cursor vs oracle: differential fuzz green — the multi-cursor application of an op set equals sequential application with fixups.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s17.9 Rendering: secondary cursors visible in the 16-color fallback lane; primary is the hardware cursor and is never double-painted (grid and raw-frame assertions prove exactly one hardware-cursor target, the primary cell has no secondary style, and the mode shape is correct; paint-time CUP escapes are not hardware cursor instances).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s17.10 Perf gate: 1 000-cursor insert inside the 5 ms p99 keypress→paint budget; keyboard selection extension damages only the union of old and new bounds (frame counter asserts row counts, not full redraws; mouse drag remains Sprint 27).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s17.11 Deferred paths hard-error naming their sprint: regex lift (21), per-cursor completion (43), per-cursor LSP edits (47).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s17.12 ≥ 180 assertions across the four new unit files; ≥ 12 new pty goldens, byte-stable across two runs (invariant 5).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s17.13 **Milestone gate**: a single pty script drives L → W → B → H → I → Esc → save on one fixture, exercising every mode's arrows, and its golden chain is green — modal complete.
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

1 Medium: `YEW-F-102` — modal closeout requires superseded staging guards.

## Deferrals recorded against this front

none

## Verdict

NOT READY — YEW-F-102 is open and independent evidence review remains pending.
