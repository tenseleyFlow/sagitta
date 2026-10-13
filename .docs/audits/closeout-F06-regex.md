# Closeout: F06 — Regex and search

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s20, s21

## DoD items (every numbered item from every sprint above)

- [ ] s20.1 `make test` green on both compilers, zero warnings; ASan/UBSan/valgrind/ determinism lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s20.2 No materialization: `grep -rn 'textbuf_to_bytes\|materialize' src/search/` shows only `yew_re_window_fill`'s bounded 64 KiB window with its documented comment; the 1 GB throughput test's peak RSS delta < 4 MiB.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s20.3 **No backtracking anywhere**: `grep -rn 'backtrack' src/search/` returns only comments explaining its absence; the pathological table is green with `t(10⁵)/t(10⁴) ≤ 15` on all four scaling patterns, and `a(a*)*b` × 100 000 a's completes in ≤ 50 ms against a committed baseline.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s20.4 Golden match table ≥ 150 rows green with captures compared element-wise; leftmost-first pinned by explicit rows.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s20.5 Engine agreement: Pike VM, DFA test, and forward+reverse DFA spans agree on 100% of golden rows and 100% of fuzz cases.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s20.6 Differential fuzz: ≥ 5 000 000 (pattern, input) pairs × 4 seeds, zero mismatches; oracle-budget skip rate reported and < 2%.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s20.7 Compile fuzz: 1 000 000 inputs, zero crashes, `err.off` always in range.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s20.8 Every §8 error row fires with the exact message and offset; lookaround/backref/atomic give the permanent-non-goal message, **not** a sprint deferral.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s20.9 Empty-width protection: the `(a*)*` unit case completes; moving the `sparse_has` check out of `addthread` makes it fail (documented in the test comment).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s20.10 Chunk-carry: a match spanning three pieces of an insertion-built piece tree is found, and mid-UTF-8 prefilter candidates are rejected.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s20.11 `_Static_assert(sizeof(ReInst) <= 16)`; all §5 limits enforced during emission, each with a firing test.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s20.12 DFA cache overflow flushes rather than grows, and a thrashing pattern falls back to the Pike VM after two flushes (test with a lowered cap).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s20.13 ≥ 300 assertions across the four unit files.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.1 `make test` green on gcc and clang, zero warnings; ASan/UBSan/valgrind/ determinism lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.2 The search prompt is the Sprint 18 widget: s18's grep gate extended to `src/search/searchui.c` shows shared `Cursor`/`TextBuf` use and no local caret/index arithmetic.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.3 Cancel restores exactly: the pty golden's pre-`/` and post-Esc grids are byte-identical, including viewport top line and `goal_col` (also asserted structurally).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.4 All 6 smartcase rows plus the 4 non-triggering escape cases pass; `\Wfoo` matches `Foo` under `ignorecase+smartcase`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.5 **No whole-file scan on a keystroke**: the overlay test proves the scanned range is bounded by viewport + look-ahead on a fixture whose out-of-scope matches a full scan would find; the 1 GB perf test holds keypress p99 ≤ 5 ms.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.6 Every replace-all and every confirm run (including one ended with `q`) is exactly **one** undo transaction — undo-tree depth asserted in ≥ 6 unit cases including the 10 000-match one.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.7 Zero-width handling: `:%s/^/> /` prefixes every line (with and without a final newline) and `:%s/x*/-/g` terminates; both are golden tests.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.8 All template rows implemented; `&` is literal (explicit test); `p`-flag case inference covers all three shapes.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.9 Overlay damage: the pty test asserts a one-character pattern change damages only the lines whose highlight set changed, against a computed expectation rather than a magic number.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.10 Jumplist: all push-rule rows and all five dedupe/coalesce rules tested, incl. first-back push, forward-tail truncation, wraparound at exactly 100, and dead-mark skipping; changelist coalescing verified at the 499/501 ms boundaries and same-line vs different-line.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.11 Persistence deferral is real: `grep -rn 'open(\|fopen\|write(' src/edit/jumplist.c` → empty; the serializer exists, round-trips in memory under test, and is called by nobody until Sprint 25.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.12 `:g` hard-errors naming Sprint 34; multi-file search documents `:!rg`; `'a` addresses now resolve (the Sprint 18 deferral is closed).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.13 Fuzz: 10 000 ops × 4 seeds with all overlay/mark/undo invariants green; `yew_re_quote` round-trip over 100 000 random literals.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s21.14 ≥ 160 assertions across the four unit files.
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

4 open: `YEW-F-082` (Critical), `YEW-F-083`, `YEW-F-084`, and
`YEW-F-085` (Medium).

## Deferrals recorded against this front

none

## Verdict

NOT READY — Independent review found four open regex/search defects.
