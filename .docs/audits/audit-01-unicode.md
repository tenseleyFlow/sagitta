# F01 UNI — Unicode substrate

Status: closed
Baseline: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`
Opened: 2026-09-03
Scope: `src/unicode/`, `ucd/`, `scripts/gen-unicode-tables.c`
Owners read: Sprint 2, Sprint 16, Sprint 46

## Q1 — round-trip law at every consumer

probed, nothing found

- `tests/audit/f01_unicode.c` isolated every three-byte input with a newline
  record boundary, then passed all 16,777,216 records through register
  capture/storage, Fletch `FlStr`, JSON write/parse with
  `YEW_JSF_RAW_BYTE`, and UTF-16 byte/column conversion.
- Harness result: `corpus=53 three-byte=16777216
  consumers=register,fletch,json,u16 ok` in 9.75 seconds on the arm64 audit
  machine.
- The same four consumer paths passed all 53 Sprint 2 golden corpus rows,
  including NUL, CRLF, truncated sequences, surrogate encodings, ZWJ
  families, combining stacks, and regional indicators.
- The pre-existing focused baselines remain green: UTF-8 24 tests /
  4,776,060 assertions; U16 6 / 11,653; Fletch corpus 1 / 338; JSON raw
  byte 1 / 7.

The harness first applies the core decoder/re-encoder to each independent
three-byte input, so consumer batching cannot conceal a cross-record decode.

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

probed, nothing found

Static enumeration found eleven implementation/declaration sites for
`yew_utf8_encode`. A temporary `YEW_BUG` in its escape branch at the exact
baseline produced these focused results:

- register: 43 tests / 699 assertions, no trip;
- JSON raw byte: 1 / 7, no trip;
- U16: 6 / 11,653, no trip;
- invalid-byte file load/save: 2 / 62, no trip;
- Fletch invalid-byte case mapping: trip at U+DC80;
- selected-text invalid-byte case mapping: trip at U+DCFF.

The two trips are deliberate decode → transform → re-encode paths. Sprint 2
§3 explicitly pins escape encoding to the original byte, and both owning
tests assert the result remains byte-exact. Save itself still copies raw
`TextBuf` bytes with no decode/re-encode path. The tripwire was removed and
the detached baseline worktree is clean.

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

Finding: `YEW-F-003`.

The first 10,000-cluster model/renderer/VT run reached a renderer `YEW_BUG`
on the ASCII-base keycap `1` + VS16 + U+20E3. The reduced reproducer proves
the grid stores the complete cluster with a one-cell head although the width
model returns two.

With that debt isolated through the complete-cluster `yew_grid_put` API, the
broad differential completed under both ambiguous-width settings:

```text
f01-vt-width: clusters=10000 narrow_cells=18000 wide_cells=19000 frame_bytes=70045/70045 ok
```

Both runs produced zero VT errors, the model and terminal cursor advances
matched, and every grid/VT cell compared byte-, width-, attribute-, and
color-exact.

## Q7 — locale ban and subprocess containment

probed, nothing found

- The question's exact banned-API grep exited 1 with no matches.
- Static enumeration found four `strtod` calls: LSP JSON, Fletch lexing,
  Fletch shortest-float formatting, and `str.float`. All rely on the same
  process-wide C locale, which cannot change because `setlocale` is banned.
- All 14 `YewJobSpec` construction sites were classified. Parsed Git jobs and
  the FUSS rebase handover force `LC_ALL=C` as Sprint 51 §3 requires. Plugin
  package operations force C for parsed output and intentionally preserve the
  user's locale for unparsed clone/checkout progress. User shell jobs preserve
  the pinned Sprint 19 §7 environment. LSP and AI/curl jobs exchange framed or
  structured protocols, key-command output is opaque secret text, and symbol
  discovery consumes a NUL-delimited path list; none parses localized numbers
  or control text.
- The two clipboard `execvp` sites move opaque bytes and the TTY guardian's
  remaining fork does not exec. No other process-launch path exists in `src/`.

## Unverified observations

- The Sprint 58 wording says `LC_ALL=C` is forced on every subprocess, while
  Sprint 19's pinned user-job environment deliberately does not include it.
  This is contract wording to reconcile, not a code finding.
- Q3 calls any escape re-encoding on a storage-bound path corruption, while
  Sprint 2 §3 explicitly defines escape re-encoding as the byte-exact inverse
  of decoding. Case transforms rely on that inverse and have byte-exact tests;
  the audit wording is broader than its invariant-2 rationale.
- Q7 says Sprint 45's JSON `strtod` is the only locale-sensitive call, but
  Sprints 29 and 32 intentionally added three Fletch calls under the same
  banned-`setlocale` rationale. The implementation is deterministic; the audit
  wording predates those consumers.

## Count

Raw 3 · deduped 3 · critical 0 · high 1 · medium 2 · low 0 · unverified 3.
