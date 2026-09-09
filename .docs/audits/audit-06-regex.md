# F06 RE — regex engine, search, replace

Status: closed
Baseline: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`
Opened: 2026-09-08
Closed: 2026-09-08
Scope: `src/search/`
Owners read: Sprint 20, Sprint 21

The product-code baseline remained immutable. Audit-control changes make the
differential oracle's exclusions explicit, add the stated deep empty-width
case and the combined zero-width replacement edge, and retain the
multi-cursor replacement exit as a hard-XPASS reproducer for Sprint 59.

## Q1 — no materialization and bounded input

probed, nothing found

- `rg -n 'textbuf_to_bytes|materialize|backtrack|yew_re_window_fill'
  src/search/` found only explanatory comments in `parse.c` and `pike.c`.
  There is no `textbuf_to_bytes`, no `yew_re_window_fill`, and no copied
  unbounded match window to inspect.
- `pike.c` consumes `TextIter` through `ReCursor`; `search.c` reads its
  literal fast path through chunk-aware access with a fixed 64-byte carry;
  its DFA and Pike paths consume the same input abstraction. The retained
  span window cap is 64 KiB in `regex_internal.h`.
- `re_empty_width_deep_subject` now drives the `TextIter` path over 100,000
  bytes and completes with the exact whole-input span.

## Q2 — empty-width `addthread` guard

probed, nothing found

- The production `list_has(l, pc)` guard remains inside the `addthread`
  work loop in `src/search/pike.c`, before any SPLIT/JMP successor is pushed.
  `re_empty_width_loop_terminates` passed its nested empty-width corpus.
- The new ordinary regression `re_empty_width_deep_subject` passed over
  `(a*)*` and 100,000 `a` bytes through `TextIter`, producing span
  `[0,100000]`.
- In an isolated baseline copy, moving the guard out of the loop made both
  the existing corpus and the 100,000-byte probe fail through the separate
  addthread stack cap: `regex: addthread stack overflow` (exit 4 in about
  0.18 s). The mutation is therefore detected, but not by the literal hang
  stated in the audit question; that wording is recorded below as an
  observation rather than mistaking the defensive stack cap for a product
  failure.

## Q3 — differential fuzz at 25 million pairs × four seeds

probed, nothing found

Each `fuzz_re_diff` iteration executes 16 comparisons, so
`--iters=1562500` is exactly 25,000,000 pairs. The four independent runs
were:

| Seed | Compared | Oracle-budget skipped | Output hash |
|---|---:|---:|---|
| `1` | 24,999,985 | 15 | `9704373e2805248e` |
| `0x243f6a8885a308d3` | 24,999,996 | 4 | `712706551a56494a` |
| `0x9e3779b97f4a7c15` | 24,999,994 | 6 | `8a98405f22d77e9c` |
| `0xd1b54a32d192ed03` | 24,999,988 | 12 | `9cf70b07cb681e82` |

Aggregate: 99,999,963 comparisons and 37 oracle-budget skips out of exactly
100,000,000 pairs: **0.000037%**, far below the 2% ceiling.

- The audit control splits the former ambiguous `UNKNOWN` outcome into
  `YEW_REF_BUDGET` and `YEW_REF_OUTSIDE`. A pattern outside the shared oracle
  subset, or a shared-subset pattern rejected by yew, now fails the fuzzer;
  only a bounded oracle execution can increment the reported skip count.
- Sampled skips were short nested-quantifier patterns, consistent with the
  intentionally bounded backtracking oracle rather than a growing grammar
  exclusion. No product compile rejection or out-of-subset outcome occurred.

## Q4 — pathological-pattern scaling

probed, nothing found

`build/perf_re_pathological --baseline tests/perf/component-limits.txt`
passed its policy and all four scaling ratios:

| Pattern | 10,000 bytes | 100,000 bytes | Ratio |
|---|---:|---:|---:|
| `a(a*)*b` | 17,000 ns | 161,000 ns | 9.47 |
| `(a|a)*b` | 16,000 ns | 158,000 ns | 9.88 |
| `(a+)+b` | 17,000 ns | 157,000 ns | 9.24 |
| `(a*)*b` | 17,000 ns | 160,000 ns | 9.41 |

All ratios are below the required 15. The additional hostile patterns
`a?{25}a{25}` and `.*.*.*.*=.*` completed in 30,000 ns and 17,000 ns.

## Q5 — smartcase semantics and consumers

probed, nothing found

- `search_smartcase` passed all five focused tests (65 assertions), including
  the required non-triggers `\\W`, `\\x41`, `[[:upper:]]`, and `\\A`.
- Uppercase detection is recorded only as the parser consumes a literal code
  point; `searchui.c` applies the result to the documented option table.
- The consumer scan found no product `strpbrk`, `isupper`, `iswupper`, or
  `toupper` shortcut in search, LSP document highlighting, or plugin search.

## Q6 — replacement transaction, ordering, and multi-cursor path

finding `YEW-F-005`

- Existing controls passed the normal one-undo cases: a confirm run ended by
  `q`, a 10,000-match replace-all, and back-to-front length-changing edits.
- A valid Fletch `edit {}` block with a live two-cursor set and a
  buffer-range replacement exits 4 at `src/edit/multicursor.c` with
  `multi-cursor edits require a MULTI transaction`.
- `tests/audit/yew_f_005.c` is the hard-XPASS reproduction. It mirrors the
  Fletch enlistment boundary: a MACRO transaction is opened with the
  multi-cursor snapshot withheld, then the real `yew_search_cmd_replace`
  handler receives the live two-cursor `EditCtx`. The replacement plan sees
  nonzero undo depth, preserves the MACRO reason, and the first text edit
  reaches the multi-cursor guard. Its child process observes the exact
  baseline exit without stopping the rest of `make test-audit`.
- This is High under the Sprint 58 rubric: valid user input reaches
  `yew_bug()`/exit 4. `YEW-F-005` remains open for Sprint 59 remediation;
  no product source changed in this audit front.

## Q7 — zero-width replacement at both file edges

probed, nothing found

`replace_zero_width_empty_first_and_unterminated_last` combines the two edge
conditions in one ordinary regression: input `"\\nlast"`, `:%s/^/> /g`, and
exact undo. It observed two replacements, `"> \\n> last"`, and byte-exact undo.
The existing zero-width progress path advances by one grapheme after a
zero-width match, so the empty first line and the unterminated final line both
fire exactly once.

## Q8 — literal ampersand and manual cross-check

probed, nothing found in implemented surfaces

- `yew_repl_expand` treats only backslash as replacement syntax; literal `&`
  has three explicit row-table controls, all passing.
- The command help documents the literal behavior. `docs/manual/` does not
  yet exist because Sprint 58 explicitly assigns the user manual to Sprint
  59, so the requested manual cross-check cannot yet be performed.

## Unverified observations

- Q2's destructive mutation is detected through the independent stack-depth
  guard, not a timeout/hang. The audit question and older test comment were
  more specific than the implementation's actual safety response; the
  ordinary test comment now states the mechanism without promising a hang.
- The manual-side literal-`&` check is pending Sprint 59's `docs/manual/`;
  this is release sequencing, not an implementation mismatch.

## Count

Raw 1 · deduped 1 · critical 0 · high 1 · medium 0 · low 0 · unverified 2.
