# Closeout: F01 — Unicode correctness

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s02

## DoD items (every numbered item from every sprint above)

- [ ] s02.1 `make` green with gcc and clang, zero warnings under `-std=c11 -pedantic -Wall -Wextra -Werror -Wvla`, including generated `tables.c` and `scripts/gen-unicode-tables.c`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s02.2 Table budget: the `_Static_assert` of §7 is present, the reported total is ≤ 64 KiB, and stage-2 dedup keeps ≥ 55% of blocks (generator prints both into `tables.c`'s trailing comment).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s02.3 `make unicode-tables && git diff --exit-code src/unicode/tables.c` is clean from a fresh clone with no network; running it twice produces byte-identical output.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s02.4 Round-trip law: all 256 one-byte and all 65,536 two-byte inputs re-encode byte-identically in `make test`; all 16,777,216 three-byte inputs pass in the fuzz lane; zero failures.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s02.5 Exhaustive scalars: 1,112,064 codepoints round-trip; `yew_utf8_encode` returns 0 for all 1,920 non-escape surrogates and for `0x110000`; the 128 escapes each encode to exactly one byte, `cp - 0xDC00`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s02.6 Conformance: `graphemebreaktest: N/N cases pass`, zero failures, N equal to the non-comment line count of the checked-in UCD 16.0.0 `GraphemeBreakTest.txt`, N ≥ 600. The count is asserted, not merely printed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s02.7 Golden corpus ≥ 40 cases, width goldens ≥ 60 rows, all green in both ambiguous-width settings; every category named in Testing Strategy has at least one case.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s02.8 `make fuzz FUZZ_ITERS=200000 FUZZ_SEED=1` exits 0 for both fuzzers, plain and under ASan/UBSan; all nine harness invariants checked per input; same seed twice ⇒ identical mutation-sequence hash; `tests/fuzz/crashes/` empty. `make fuzz` no longer hard-errors.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s02.9 `grep -rn 'wcwidth\|wcswidth\|mbrtowc\|wchar\.h\|setlocale\|iconv' src/` returns nothing; `scripts/bans.sh` exits 0 with the three new rules active, each proven to fire by a seeded violation.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s02.10 `utf8.c` contains no range comparison against `0xD800`/`0xDFFF`/ `0x10FFFF` outside comments — rejection lives only in the transition table, and the file header says so.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s02.11 Perf: segment + width over 1 MiB of ASCII in ≤ 5 ms on the dev machine (fast path proven live); CJK and emoji baselines committed under `tests/perf/baselines/`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s02.12 Deferred-surface lifecycle is explicit: Sprint 16 owns `yew_wb_*` and its word-break coverage, Sprint 20 owns the regex simple-fold entry point and its agreement corpus, and no normalization entry point exists; `grapheme.h`/`width.h` name the owning sprints and permanent non-goals.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s02.13 `ucd/MANIFEST` lists all six inputs with sha256; `ucd/LICENSE` holds the Unicode license; `README.md` gains a paragraph on why `src/unicode/tables.c` is generated *and* committed (redeeming Sprint 0's promise).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s02.14 ≥ 45 registered unit tests and ≥ 400 assertions across `test_utf8.c`, `test_grapheme.c`, `test_width.c`, `test_gbtest.c`; valgrind lane reports zero definite leaks; ASan/UBSan clean.
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

0 open confirmed findings.

## Deferrals recorded against this front

none

## Verdict

NOT READY — Confirmed findings are closed; independent evidence review remains.
