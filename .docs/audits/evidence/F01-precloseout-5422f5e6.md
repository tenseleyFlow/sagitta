# F01 Unicode pre-closeout evidence

- Date: 2026-10-13
- Baseline: `5422f5e6ca1982c664c04aef4b1aa2e830564bbc`
- Product-source commit: `8e9f1990` (the following commit changes ledgers and
  status documentation only)
- Host: Darwin 25.4.0 arm64
- Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0

Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and four-target release evidence remain.**

This packet prepares the F01 closeout without signing it. Its author also
authored remediation in this front and therefore cannot be the independent
Sprint 59 §2 reviewer. A future reviewer must establish the baseline again,
re-run or independently inspect the cited witnesses, and decide each
checkbox in `.docs/audits/closeout-F01-unicode.md`.

## Scope and provenance

The original F01 audit is `.docs/audits/audit-01-unicode.md`. It reviewed
detached baseline `41fef416`, filed `YEW-F-001` through `YEW-F-003`, and was
confirmed by hosted audit-control run `33815573832` at `3e0edb57` across GCC,
Clang, ASan/UBSan, Linux arm64, macOS arm64, musl, and `MODULES=""`.

The current recheck manifest is
`/private/tmp/yew-s59-f01-recheck-20261013.sXMZuo/manifest.md`. Contract
reconciliation filed `YEW-F-095`; source-evidence review filed
`YEW-F-098`. Both are now fixed. At this baseline all 98 audit registry
entries pass, both remediation ledgers report zero active XFAILs, and the F01
closeout reports zero open confirmed findings.

The adjacent `lsp-wolf-default` worktree was not merged again; its changes are
already present through `origin/trunk`. Long fuzzing was deliberately omitted
from this local recheck while the Sprint 58 tier-1 soak owns four cores.

## Current-baseline executable evidence

### Strict builds

Apple Clang built `build/yew`, `build/unit_tests`, and `build/audit_tests`
under the repository's locked C11 warning flags. Homebrew GCC 16 independently
built the same product and test surfaces with:

```text
make -j2 BUILD=build-f01-gcc CC=gcc-16 \
  build-f01-gcc/yew \
  build-f01-gcc/unit_tests \
  build-f01-gcc/audit_tests
```

Both compilers exited 0 with zero warnings under `-std=c11 -pedantic -Wall
-Wextra -Werror -Wvla`.

### Unit, conformance, and audit surfaces

The 45 test functions defined by the four Sprint 2-owned unit files were run
individually by their explicit registry names:

```text
owned-unicode: 45 tests, 4789439 assertions, 0 failures
```

Their distribution is 22 in `test_utf8.c`, 15 in `test_grapheme.c`, seven in
`test_width.c`, and one in `test_gbtest.c`. The broader focused filters passed
under both Apple Clang and GCC 16:

| Filter | Result |
|---|---|
| `utf8_` | 29 tests / 4,776,145 assertions |
| `grapheme_` | 23 / 1,473 |
| `width_` | 22 / 1,052 |
| `graphemebreaktest` | 1 / 11,606; 1,093/1,093 UCD cases |

The current F01 audit harnesses also passed:

```text
f01-unicode: corpus=53 three-byte=16777216 consumers=register,fletch,json,u16 ok
f01-vt-width: clusters=10000 narrow_cells=18000 wide_cells=19000 frame_bytes=70045/70045 ok
audit: 98 tests, 0 failures
```

The first harness applies the byte-preserving decoder/re-encoder to all
16,777,216 independent three-byte inputs before exercising four downstream
consumers. The second compares model and terminal placement under both
ambiguous-width settings.

### Sanitizer surface

The exact thirteen tests added for `YEW-F-098` passed individually under
Clang ASan/UBSan. They cover illegal UTF-8 leads, overlong/surrogate/above-max
boundary forms, non-escape surrogate encoding, earliest validation errors,
regional-indicator resets, emoji-ZWJ preconditions, Indic-linker
preconditions, Prepend/Control interaction, invalid-byte display width,
ambiguous-width reversibility, and atomic invalid-byte clipping.

Earlier F01 remediations carried focused sanitizer evidence, and the historical
hosted audit-control run covered the original three findings. This packet does
not promote those runs into an exact-baseline full sanitizer regression.

### Generated data and static policy

- `(cd ucd && shasum -a 256 -c MANIFEST)` verified all ten checked-in UCD
  16.0.0 inputs.
- Two consecutive `make unicode-tables` runs regenerated `tables.c`,
  `tables_wb.c`, `tables_case.c`, and `tables_cat.c` offline; both left all
  four files byte-identical to Git.
- The base table reports 27,594 bytes and 190/1,536 unique stage-2 blocks,
  or 87% deduplication, and retains its compile-time 64 KiB assertion.
- The checked-in fixtures contain 53 non-comment corpus rows, 74 width rows,
  and 1,093 GraphemeBreakTest rows.
- `scripts/bans.sh` exited 0. The direct locale/Unicode API search is empty,
  and `utf8.c` retains DFA-only structural rejection without post-hoc
  surrogate or Unicode-ceiling comparisons.
- The six original Sprint 2 inputs remain in `ucd/MANIFEST`; four later
  delivered Word_Break/case/category inputs expand the manifest to ten.
  `ucd/LICENSE` and the README's offline checked-in-table rationale remain
  present.

### Performance

`make perf-unicode` passed its policy self-test and the gating run:

```text
unicode-perf: ascii bytes=1048576 clusters=1048576 cells=1048576 best_us=1224 ok
unicode-perf: cjk bytes=1048578 clusters=349526 cells=699052 best_us=4408
unicode-perf: emoji bytes=1048600 clusters=41944 cells=83888 best_us=3408
```

The ASCII result is 1.224 ms against the Sprint 2 limit of 5 ms for 1 MiB.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-001` | centralized fixed-cell glyph fallback; audit reproducer passes under both ambiguous-width settings |
| `YEW-F-002` | completed regional-indicator clusters flush promptly; long-RI audit reproducer passes |
| `YEW-F-003` | ASCII-base keycap grid width matches the model; broad VT differential passes |
| `YEW-F-095` | closeout and owning contracts agree that delivered Word_Break and case-folding surfaces are not active deferrals |
| `YEW-F-098` | 45 explicitly registered owned tests; exact tests pass under Clang, GCC 16, and ASan/UBSan |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

| Row | Primary witnesses |
|---|---|
| s02.1 | strict Apple Clang and Homebrew GCC 16 product/unit/audit builds; generated table compilation |
| s02.2 | `tables.c` size assertion and trailing 27,594-byte / 87%-dedup report |
| s02.3 | two clean offline regenerations of all four currently generated tables |
| s02.4 | `f01-unicode` all-three-byte sweep; exhaustive short-string units; UTF-8/grapheme fuzz harnesses |
| s02.5 | `utf8_exhaustive_scalars`, `utf8_exhaustive_surrogates`, encoding-edge and new boundary-form tests |
| s02.6 | `graphemebreaktest: 1093/1093 cases pass` and checked-in UCD fixture count |
| s02.7 | 53-row corpus, 74-row width corpus, both ambiguous-width settings in units and VT differential |
| s02.8 | deterministic UTF-8/grapheme fuzz harnesses and historical hosted sanitizer evidence; exact-baseline campaign still required |
| s02.9 | `scripts/bans.sh`; empty direct banned-API search; seeded ban selftests |
| s02.10 | DFA-only validity header and absence of post-hoc range comparisons in `utf8.c` |
| s02.11 | `make perf-unicode`; ASCII 1 MiB at 1.224 ms plus recorded CJK/emoji results |
| s02.12 | ownership/non-goal comments in `grapheme.h`, `width.h`, and `utf8.h`; `YEW-F-095` |
| s02.13 | ten-entry verified manifest containing the original six inputs, Unicode license, README offline-generation rationale |
| s02.14 | 45 owned registered tests / 4,789,439 assertions; exact new tests under ASan/UBSan; Valgrind still required |

## Remaining blockers

1. No independent reviewer has signed the **current** baseline. The original
   independent audit predates the remediations it caused.
2. No hosted CI run exists for `5422f5e6`; local `trunk` is ahead of
   `origin/trunk`. Sprint 59 DoD 15 requires the complete four-target matrix at
   the commit each closeout signs.
3. Valgrind is not available on this Darwin host. Linux evidence must come
   from CI or a designated Linux runner.
4. The required exact-baseline full plain/sanitized fuzz and determinism
   campaigns have not been rerun here. The 14-target Sprint 58 soak is
   concurrently consuming four cores against its intentionally frozen product
   baseline; this recheck did not distort it with competing long campaigns.
5. The Sprint 58 soak does not substitute for Sprint 59's exact closeout
   matrix. It remains separate evidence against product commit `4c99e134`.

## Coverage limits

This was remediation reconciliation and evidence mapping, not a fresh broad
novel-defect hunt. It inspected the changed F01 paths and their contracts,
replayed every audit reproducer, and exercised focused units on both local
compilers. It found no new confirmed issue after `YEW-F-098`, but passing
local samples do not prove the whole Unicode front correct.
