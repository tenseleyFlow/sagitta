# F01 UNI — Unicode substrate

Status: in progress
Baseline: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`
Opened: 2026-09-03
Scope: `src/unicode/`, `ucd/`, `scripts/gen-unicode-tables.c`
Owners read: Sprint 2, Sprint 16, Sprint 46

## Q1 — round-trip law at every consumer

In progress. Baseline evidence already green:

- `fuzz_utf8: exhaustive-three-byte=16777216 ok`
- UTF-8 focus: 24 tests, 4,776,060 assertions
- U16 focus: 6 tests, 11,653 assertions
- Fletch s02-corpus focus: 1 test, 338 assertions
- JSON raw-byte focus: 1 test, 7 assertions

The combined register/Fletch/JSON/U16 corpus harness remains to be added before
this question closes.

## Q2 — 64-codepoint backward restart bound

Finding: `YEW-F-002`.

The baseline's 65-RI unit case reaches the documented parity approximation.
Direct callers are `src/ui/panel.c`, `src/fl/stdstr.c`, and `src/edit/job.c`.
Panel whitespace trimming and Fletch boundary validation find a nearby safe
restart before the bound or retain their promised boundary property. The job
streaming caller consumes the approximate offset as an exact final-cluster
boundary: on an odd 65-RI run it holds eight bytes rather than four, delaying
one completed flag until the child writes again or exits.

## Q3 — escape codepoints reaching storage

In progress. Static enumeration found eleven implementation/declaration sites
for `yew_utf8_encode`; the seeded escape-branch tripwire remains to run against
each consumer workload.

## Q4 — shipped chrome glyph widths

Finding: `YEW-F-001`.

With `ambiguous_wide=false`, every central glyph occupies its pinned slot. With
`ambiguous_wide=true`, `YEW_GLYPH_DIRTY_TICK` widens from one cell to two; box
borders, disclosure arrows, Git arrows, and several other fixed-slot glyphs do
the same. The committed unit reproducer records the first divergence.

## Q5 — offline regeneration, manifest, and table budget

probed, nothing found

- `shasum -a 256 -c ucd/MANIFEST`: all ten inputs `OK`.
- `make unicode-tables`: regenerated `tables.c`, `tables_wb.c`,
  `tables_case.c`, and `tables_cat.c` offline.
- `git diff --exit-code` over those four outputs: clean.
- Both the base and Word_Break tables retain compile-time size assertions.

## Q6 — model width versus VT placement

Pending: the existing renderer/VT differential is green, but the required
10,000-cluster mixed corpus has not yet been run as one recorded probe.

## Q7 — locale ban and subprocess containment

In progress. The banned locale API grep is empty. Git jobs explicitly force
`LC_ALL=C`; the central job layer preserves the Sprint 19 environment contract
and does not force it for arbitrary user shell jobs. Each protocol subprocess
construction site still needs a recorded classification before this question
closes.

## Unverified observations

- The Sprint 58 wording says `LC_ALL=C` is forced on every subprocess, while
  Sprint 19's pinned user-job environment deliberately does not include it.
  This is contract wording to reconcile, not a code finding.

## Count

Raw 2 · deduped 2 · critical 0 · high 0 · medium 2 · low 0 · unverified 1.
