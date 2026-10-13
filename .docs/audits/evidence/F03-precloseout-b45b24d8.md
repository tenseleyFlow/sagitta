# F03 text-engine pre-closeout evidence

- Date: 2026-10-13
- Baseline: `b45b24d8be373f8f713a27b96859277023e9f98a`
- Last F03-owned executable/test-surface change: `541b3b93`
- Host: Darwin 25.4.0 arm64
- Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0

Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and required release/platform campaigns remain.**

This packet prepares F03 for closeout without signing it. Its author also
authored F03 remediation and therefore cannot be Sprint 59's independent
reviewer. A future reviewer must establish the baseline again, independently
inspect or rerun the cited witnesses, and decide every checkbox in
`.docs/audits/closeout-F03-text.md`.

## Scope and provenance

The original F03 audit is `.docs/audits/audit-03-text.md`. It reviewed
detached product baseline `41fef416`; hosted close run `33836915903` at
`b9e604d4` passed its complete 22-job matrix. That audit exercised the full
3.2-million-operation differential campaign, nightly properties, save
torture, large-file seams, and the then-current text/undo/register surface.
Those results remain useful historical evidence, but they are not being
represented as execution against this packet's baseline.

The current recheck began from the manifest at
`/private/tmp/yew-s59-f03-recheck-20261013.WBZA2P/manifest.md`. It first filed
and closed `YEW-F-100`, which aligned historical staging guards with the
later-shipped cursor, undo-reason, and register callers. While collecting the
closeout evidence it then filed `YEW-F-101`: Sprint 10's mutation-ownership
wording excluded audited initial-hydration and internal-projection calls, and
Sprint 12 named an obsolete public raw register setter. Decision S59-A12 and
commit `f16c5dcd` brought those contracts into agreement with the existing
audited implementation without changing product code.

At this baseline all 101 audit registry entries pass, both remediation
ledgers report zero active XFAILs, and F03 has zero open confirmed findings.
Long campaigns were deliberately omitted while the Sprint 58 tier-1 soak owns
four cores. The adjacent `lsp-wolf-default` worktree was not merged again;
that work is already present through `origin/trunk`.

## Current-baseline executable evidence

### Strict builds and owned units

Apple Clang and Homebrew GCC 16 each built the product and focused test
surfaces under the repository's locked `-std=c11 -pedantic -Wall -Wextra
-Werror -Wvla` policy with zero warnings. Every registered test defined by
the current/evolved F03-owned unit sources was then invoked by exact name
under both compilers:

| Sprint | Current/evolved owned sources | Tests | Assertions |
|---|---|---:|---:|
| s07 | `test_piece.c` | 27 | 149,325 |
| s08 | `test_file.c`, `test_journal.c`, `test_save_paths.c` | 41 | 2,458 |
| s09 | `test_coords.c`, `test_cursor.c`, `test_mark.c`, `test_multicursor.c` | 56 | 2,382,966 |
| s10 | `test_undo.c`, `test_undo_tree.c`, `test_undo_trim.c`, `test_undo_serial.c`, `test_undo_marks.c` | 67 | 2,712,128 |
| s11 | `test_oracle.c`, `test_textbuf_props.c`, `test_shrink.c`, `test_piece_regress.c` | 21 | 2,476,067 |
| s12 | `test_base64.c`, `test_register.c`, `test_register_append.c`, `test_paste.c`, `test_kill_ring.c`, `test_osc52.c`, `test_clipboard.c` | 57 | 2,127,433 |
| **Total** | **24 source files** | **269** | **9,850,377** |

`test_piece_diff.c` has been folded into `test_piece.c`; the historical
`test_ring.c` surface evolved into `test_kill_ring.c`; and append cases now
live in `test_register_append.c`. Those evolutions are made explicit rather
than hiding present coverage behind obsolete filenames.

The complete current audit suite also passed under both compilers:

```text
audit: 101 tests, 0 failures
governance: 0 active XFAILs; findings, debt, burndown, priority, and closeout checks green
```

### Sanitizer surface

A fresh Apple Clang ASan/UBSan tree (`build-f03-san`) built the product, unit
runner, `fuzz_textbuf`, `fuzz_undo`, and `fuzz_undo_serial`. Broad focused
filters all passed:

| Filter | Tests | Assertions |
|---|---:|---:|
| `piece_` | 36 | 151,194 |
| `file_` | 54 | 2,643 |
| `journal_` | 23 | 1,837 |
| `save_` | 67 | 21,583 |
| `coords_` | 10 | 729 |
| `cursor_` | 79 | 2,805,722 |
| `mark_` | 11 | 442,190 |
| `multicursor_` | 40 | 2,209,222 |
| `undo_` | 103 | 2,814,225 |
| `oracle_` | 4 | 2,506,443 |
| `textbuf_prop` | 12 | 2,470,036 |
| `shrink_` | 3 | 5,018 |
| `base64_` | 3 | 2,124,343 |
| `register_` | 23 | 211 |
| `paste_` | 20 | 707 |
| `kill_ring_` | 6 | 60 |
| `osc52_` | 8 | 2,261 |
| `clipboard_` | 17 | 974 |

These filters intentionally overlap and are not summed as unique coverage.
Darwin ASan supplies no LeakSanitizer, and Valgrind is not available on this
host; Linux leak/descriptor evidence therefore remains a release blocker.

### Deterministic fuzz samples

Seed 5903 produced the following plain-build results, byte-identically on a
second execution:

```text
typing trace=3e8b799fc15d482f final=670db60fbcdffcf3 bytes=66541 pieces=7 invalid=192 nul=96 crlf=192 split=60 huge=102 ok
paste  trace=e4d021e1d6eac798 final=44c834d473dfb7e4 bytes=0 pieces=0 invalid=192 nul=117 crlf=182 split=65 huge=83 ok
undo   trace=be09aac6d3edfb8e final=0b29362d51992148 bytes=18371 pieces=8 invalid=204 nul=107 crlf=199 split=72 huge=103 ok
lines  trace=5f57cf8b8d55e840 final=dea280e5ac638339 bytes=841 pieces=5 invalid=199 nul=88 crlf=204 split=65 huge=95 ok
fuzz_undo seed=5903 iters=10000 hash=2cf4315417fbaa6f edits=4483 undo=1987 redo=460 branches=107 ok
fuzz_undo_serial iters=5000 corpus=7 hash=8c7a9adb4bfae4af ok
```

The `fuzz_undo` driver enforces a 10,000-iteration floor, so a requested
5,000-iteration sample reported 10,000. Fresh ASan/UBSan executions reproduced
every hash and counter exactly. This is a deterministic current-baseline
sample, not a substitute for Sprint 11's full 3.2-million-operation campaign.

### Persistence torture and large-file evidence

`make torture-live-check` passed against the current source:

```text
clean child save+check ok
```

The original F03 audit separately records full API fault sweeps, 5,000
external kills, a further 5,000 live-editor kills, 200 batch kills, exact
journal replay, and destination old-or-new invariants. Its invariant-1
addendum records an exact 1.5 GiB APFS save and an x86_64 ext4 run at 99.9006%
full that retained the old hardlinked image and replayable journal on ENOSPC.
Those exhaustive runs were not repeated on `b45b24d8` and remain historical.

The exact 2 GiB accepted boundary still lacks an end-to-end read/edit/write
run, and the read-only-directory case uses mode 0500 rather than a genuinely
read-only mounted filesystem returning `EROFS`. Both are evidence gaps, not
newly reproduced defects.

### Performance

Current gating performance checks passed:

```text
piece-perf: seed=9e3779b97f4a7c15 ops=1000000 bytes=5547225 pieces=1755012 elapsed_ms=1065 budget_ms=3500 mode=GATING ok
undo-perf: warm=1000 cycles=100000 add_bytes=8 rss_growth=16384 rss_budget=1048576
perf-baseline-selftest: 16 checks ok
perf-baseline-guard: ok
```

The local piece result also meets Sprint 7's historical 2-second ceiling.
The huge-fixture and designated-runner Sprint 11 measurements were not rerun
locally: their generated inputs exceed 300 MiB and the full fixture is 1 GiB,
while the independent soak owns the machine's long-running capacity.

### PTY and clipboard boundary

The same executable source used here passed the full Apple Clang PTY suite
twice during the immediately preceding F02 packet: 557 cases in the default
environment and 557 with `COLORTERM=truecolor`. That included `osc52_frame`
and `osc52_reply`. The intervening commits changed only documentation,
audit-control metadata, and contract tests; no product or PTY source changed.
This is linked same-source evidence, not a newly executed F03 PTY run.

### Static ownership and policy

- `scripts/bans.sh` passed after decision S59-A12.
- `src/` contains no `mmap`, `popen`, or `system` call and no OSC 52 query
  spelling.
- `src/text/piece.c` contains no `open`, `fopen`, `read`, or `pread` call.
- `tests/fuzz/oracle.c` contains no `yew_textbuf_` or `piece.h` dependency;
  `tests/fuzz/` and `scripts/` contain no `rand`, `srand`, `random`,
  `srandom`, or `time(NULL)` use.
- `sizeof(PieceNode) <= 96` and `sizeof(UndoOp) <= 32` are compile-time
  assertions in `piece.h` and `undo.h`.
- The only `CLOCK_REALTIME` occurrence in `undo.c` is the `t_wall` capture.
- There is no public raw `yew_reg_set` symbol. `reg_set` is private to
  `register.c`; production append enters through `yew_reg_append` and other
  writers use typed producer routes.
- Register column handling calls `yew_ccol_max` and `yew_ccol_shortfall`;
  no duplicate character-width algorithm lives in `register.c`.
- Every raw text-buffer mutation outside piece/edit/undo was classified as
  initial hydration or an internal read-only/no-undo/temporary projection:
  trust prompt, symwalk, Git diff, AI and LSP logs, LSP rename preview, batch
  stdin, macro browse, and command editor. Editable-document operations still
  route through `EditCtx`.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-076` | unsaved undo-sidecar anchor canonicality fixed in `4c887676`; accepted files now preserve the canonical byte contract |
| `YEW-F-077` | rectangular yank short-row padding fixed in `792e32d8`; geometry reproducer passes |
| `YEW-F-078` | journal base identity strengthened in `f65eea0d`; same-size/same-mtime replacement-inode reproducer passes |
| `YEW-F-100` | S59-A11 and `34dd30ef` align historical staging guards with the later shipped callers and retained choke points |
| `YEW-F-101` | S59-A12 and `f16c5dcd` align mutation/register ownership wording with audited hydration/projection and private-setter behavior |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

| Row | Primary witnesses |
|---|---|
| s07.1 | strict Apple Clang and GCC 16 builds and exact owned-unit runs; hosted/Valgrind matrix still required |
| s07.2 | compile-time `PieceNode <= 96` assertion |
| s07.3 | current one-million-op `piece-perf` at 1.065 s |
| s07.4 | `test_piece.c` coalescing cases among 27 tests / 149,325 assertions |
| s07.5 | piece mixed-EOL offset/line property cases and oracle sample |
| s07.6 | snapshot units; sanitizer filters; Valgrind release evidence remains outstanding |
| s07.7 | checker-after-operation property/oracle coverage and corrupt-tree unit |
| s07.8 | deterministic current sample; historical four-seed differential campaign |
| s07.9 | fresh ASan/UBSan sample and duplicate hashes; hosted determinism lane still required |
| s07.10 | 27 current/evolved owned tests / 149,325 assertions |
| s07.11 | empty file-I/O ownership search in `piece.c` |
| s08.1 | 17 exact file tests plus edge-byte audit controls |
| s08.2 | save-path intercept-order units and historical exhaustive fault sweep |
| s08.3 | save decision-table units, including EXDEV and ownership fallback |
| s08.4 | current clean live check; historical four-seed/5,000-kill torture; exact-baseline sweep outstanding |
| s08.5 | journal replay units and historical kill checker |
| s08.6 | journal truncation-prefix cases among 14 journal tests |
| s08.7 | stale-journal tests and `.sagj.stale` behavior |
| s08.8 | changed-disk/replacement-inode units and `YEW-F-078` remediation |
| s08.9 | empty `mmap` search under `src/` |
| s08.10 | fresh ASan/UBSan focused filters; Linux Valgrind remains outstanding |
| s08.11 | torture/batch I/O-failure exit-contract tests |
| s08.12 | 41 owned tests / 2,458 assertions |
| s09.1 | Unicode ownership searches and register column-helper calls |
| s09.2 | exact coordinate golden/boundary units among 10 tests / 729 assertions |
| s09.3 | cursor property/oracle tests plus deterministic current sample |
| s09.4 | CRLF cursor and line-end cases in coordinate/cursor units |
| s09.5 | mark-bias table and 1,000-mark oracle; 5 mark tests / 2,504,032 assertions |
| s09.6 | multicursor merge/permutation cases |
| s09.7 | live multicursor units, one multi-transaction behavior, and S59-A11 |
| s09.8 | wide-glyph second-cell coordinate unit |
| s09.9 | fresh ASan/UBSan focused filters; Linux Valgrind remains outstanding |
| s09.10 | 56 owned tests / 2,382,966 assertions |
| s10.1 | strict Clang/GCC builds, fresh ASan/UBSan filters; hosted/Valgrind lanes outstanding |
| s10.2 | compile-time `UndoOp <= 32` assertion and undo storage tests |
| s10.3 | undo branch-survival units and current deterministic undo sample |
| s10.4 | merge/boundary-event undo cases among 31 `test_undo.c` tests |
| s10.5 | current transaction-reason node-count cases and S59-A11 |
| s10.6 | `perf-undo`: 100,000 cycles, add store fixed at 8 bytes, RSS +16 KiB |
| s10.7 | undo trimming/tree invariants and historical random-edit campaign |
| s10.8 | serialization/CRC/truncation/version units and `fuzz_undo_serial`; `YEW-F-076` remediation |
| s10.9 | cursor/mark restore units including 2,504,032 mark assertions |
| s10.10 | classified raw-mutation search, sole `CLOCK_REALTIME` capture, and S59-A12 |
| s10.11 | live later-sprint reason-code callers, node-count cases, and S59-A11 |
| s10.12 | 67 owned tests / 2,712,128 assertions |
| s11.1 | strict Clang/GCC focused builds; short current fuzz/perf samples; full `make fuzz/perf` matrix outstanding |
| s11.2 | historical 3.2-million-op campaign; deterministic current four-mix sample is not a replacement |
| s11.3 | 12 property tests / 2,470,036 assertions; historical nightly-depth cases |
| s11.4 | current nonzero invalid/NUL/CRLF/split/huge counters and byte-exact oracle hashes |
| s11.5 | historical scratch-mutation teeth drill documented by the original audit evidence |
| s11.6 | shrinker units, including replay-equivalent minimization |
| s11.7 | generator/fixture determinism historical evidence; exact-baseline regeneration outstanding |
| s11.8 | historical designated-runner measurements; current huge-fixture gate outstanding |
| s11.9 | historical 1 GiB both-ends measurement; exact-baseline gate outstanding |
| s11.10 | committed informational all-newline baseline and exemption policy |
| s11.11 | 16-check baseline selftest and green mixed-commit guard; designated-runner stability evidence remains historical |
| s11.12 | empty oracle coupling and banned-randomness searches |
| s11.13 | fresh ASan/UBSan short fuzz/property evidence; full sanitizer/Valgrind campaign outstanding |
| s11.14 | 21 owned tests / 2,476,067 assertions |
| s12.1 | strict Clang/GCC exact units and fresh ASan/UBSan focused filters; Valgrind outstanding |
| s12.2 | base64 vectors/fuzz/no-newline cases; 3 tests / 2,124,343 assertions |
| s12.3 | register routing-table and `1`/`-` boundary units |
| s12.4 | append table in seven `test_register_append.c` tests |
| s12.5 | linewise/blockwise paste matrix, including short-line/CJK/extension cases; `YEW-F-077` remediation |
| s12.6 | paste/ring single-node undo cases |
| s12.7 | OSC 52 unit vectors for plain/tmux/screen and maximum handling |
| s12.8 | same-source 557-case PTY runs, including `osc52_frame` and `osc52_reply` |
| s12.9 | clipboard environment/PATH detection table |
| s12.10 | clipboard nonblocking/zombie-reaping units |
| s12.11 | default yank-only clipboard synchronization unit |
| s12.12 | empty process/query/raw-public-setter searches, private setter, typed routes, Unicode column helpers, and S59-A12 |
| s12.13 | live blockwise/macro/command/search producer routes and S59-A11 |
| s12.14 | 57 owned units / 2,127,433 assertions plus same-source OSC 52 PTYs |

## Remaining blockers

1. No independent reviewer has signed the **current** baseline. The original
   audit predates every remediation it caused and decisions S59-A11/A12.
2. No hosted exact-baseline four-target/full matrix exists for `b45b24d8`;
   local `trunk` is ahead of `origin/trunk`. Sprint 59 requires that matrix at
   the commit a closeout signs.
3. Valgrind and LeakSanitizer evidence is unavailable on this Darwin host.
   Linux leak and descriptor checks remain outstanding.
4. Sprint 11's exact full fuzz/property campaigns, the complete save-torture
   sweep, designated performance runs, and huge-file fixtures were not rerun
   on this baseline while the independent soak owns four cores. Short focused
   samples and historical campaigns are not substitutes.
5. The exact 2 GiB accepted-file end-to-end case and a real read-only
   filesystem/`EROFS` save case remain original-audit evidence gaps.
6. Sprint 58's soak is frozen against product commit `4c99e134`. It is
   separate evidence and cannot substitute for Sprint 59's exact closeout
   matrix or F03-specific campaigns.

## Coverage limits

This was remediation reconciliation and evidence mapping, not a fresh broad
novel-defect hunt. It audited F03's current ownership contracts, replayed all
101 audit reproducers, ran every current/evolved owned unit under two
compilers, and exercised focused sanitizers, deterministic fuzz samples,
performance gates, and a live persistence check. It found and closed the
contract defect `YEW-F-101`; passing these local samples does not prove the
text engine and persistence front correct.
