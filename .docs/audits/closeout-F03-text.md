# Closeout: F03 — Text engine and persistence

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s07, s08, s09, s10, s11, s12

## DoD items (every numbered item from every sprint above)

- [ ] s07.1 `make test` green under gcc and clang, `-std=c11 -pedantic -Wall -Wextra -Werror`, zero warnings.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s07.2 `sizeof(PieceNode) <= 96` enforced by `_Static_assert`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s07.3 Insert/delete/line-lookup all O(log n): demonstrated by the unit perf canary — 1 000 000 random 1–8 byte inserts into an initially 1 MiB buffer completes in < 2 s on the CI runner (crude ceiling; real gates land Sprint 11).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s07.4 Coalescing: the 10 000-keystroke unit case ends with ≤ 3 pieces.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s07.5 `line_start(line_of(off)) <= off` and round-trips exactly at line heads, for every byte offset of a 64 KiB mixed-EOL fixture.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s07.6 Snapshot semantics: post-snapshot edits never alter snapshot reads; both release orders valgrind-clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s07.7 `yew_textbuf_check` passes after every op in every unit test; at least one deliberately-corrupted-tree test proves the checker fires.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s07.8 Differential smoke: 10 000 ops × 4 seeds green in CI.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s07.9 ASan, UBSan, determinism lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s07.10 ≥ 60 assertions across `test_piece.c` + `test_piece_diff.c`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s07.11 No file I/O anywhere in `src/text/piece.c` (`grep -n 'open\|fopen\|read(' src/text/piece.c` → nothing) — load/save is Sprint 8's contract.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s08.1 `load → save → cmp` byte-identical for all ≥ 12 load fixtures, including BOM, CRLF, mixed, binary-with-NUL, no-final-newline.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s08.2 Atomic path issues exactly: write(s), fsync(file), rename, fsync(dir) — order proven by the shim's intercept log in a unit test.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s08.3 All 6 decision-table rows reachable and unit-tested; EXDEV and chown-failure rows exercised via the shim.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s08.4 Torture sweep green: every `YEW_FAULT_AT` value from 0 to run-length, 4 seeds, plus 500 external-SIGKILL iterations — checker invariant holds in 100% of iterations. `make torture` wired into CI.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s08.5 Journal replay recovers the scripted edit list exactly in the torture checker whenever dst holds pre-save content.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s08.6 Truncated-tail journal fuzz: replay at every truncation length of the 3-record fixture — zero crashes, valid-prefix semantics.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s08.7 Stale journal is renamed `.sagj.stale`, never applied, never deleted.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s08.8 `yew_file_save` returns `YEW_SAVE_CHANGED_ON_DISK` when mtime/size moved under us; dst untouched in that case.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s08.9 No mmap anywhere: `grep -rn 'mmap' src/` → nothing.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s08.10 ASan/UBSan/valgrind lanes green including the torture binaries.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s08.11 Exit code 3 on I/O failure paths from the headless torture binary.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s08.12 ≥ 80 assertions across the three unit files.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s09.1 All conversion functions live in `src/unicode/`; grep gate: `grep -rn 'yew_off_to_ccol\|width' src/text/ src/edit/` shows call sites only, zero width arithmetic outside `src/unicode/`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s09.2 Golden-case table: all ≥ 9 rows pass for boundaries, widths, and left/right motion round-trip.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s09.3 `yew_cursor_*` never produces a non-boundary `pos`: fuzz smoke (10k ops × 4 seeds) with the boundary assertion — zero violations.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s09.4 CRLF: motion treats `\r\n` as one step in both directions; `line_end` sits before `\r`; verified on a CRLF fixture from s08.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s09.5 All 12 mark bias cells unit-tested; 10k-mark oracle test green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s09.6 Merge rules: all table rows tested including primary survival and the two-permutation determinism case.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s09.7 Sprint 17-delivered multi-cursor editing preserves the sorted/deduplicated `CursorSet`, primary-survival rules, and one `YEW_TXN_MULTI` transaction; the original Sprint 17 hard error was a staging guard (S59-A11).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s09.8 `yew_gcol_to_off` on the second cell of a wide glyph returns the glyph start (explicit unit case).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s09.9 ASan/UBSan/determinism lanes green; valgrind clean on all four new unit binaries.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s09.10 ≥ 100 assertions across the four unit files.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s10.1 `make test` green under gcc and clang at `-std=c11 -pedantic -Wall -Wextra -Werror`, zero warnings; ASan/UBSan/valgrind/determinism green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s10.2 `sizeof(UndoOp) <= 32` by `_Static_assert`; no per-node malloc.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s10.3 Branch survival: edit → undo → edit leaves the first branch reachable and byte-exact (the §2 headline, as an explicit unit case).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s10.4 All 6 merge conditions and all 7 boundary events have a test proving they do and do not merge.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s10.5 Exactly one node per insert burst, erase burst, paste, cut, external, MULTI, FILTER, REPLACE, MACRO, and LSP transaction — asserted by current node-count cases; later owning sprints replace their historical hard errors with those cases (S59-A11).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s10.6 100 000 undo/redo cycles leave `tb->add.len` unchanged and RSS flat (± 1 MiB) — the §3 pitfall, gated.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s10.7 After 10 000 random edits at a 4 KiB budget: every live node's parent is live, P is intact, `yew_undo_to` works for every live id.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s10.8 Serialization round-trip green for ≥ 500 nodes with binary payloads; CRC/truncation/version cases drop cleanly and never partially apply; `.sagu` write is atomic via s08's primitive.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s10.9 Cursor restore correct for 1- and 8-cursor sets including §6's window-gone row; `MarkRepair` restores collapsed marks exactly and the 1 000-mark oracle test is green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s10.10 Grep gates: `yew_textbuf_insert\|yew_textbuf_delete` under `src/` hits only `text/{piece,edit,undo}.c`; `CLOCK_REALTIME` in `undo.c` hits only the `t_wall` capture site.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s10.11 The Sprint 17/19/21/34/47-owned callers are live and route through the reason codes established here; each user action creates exactly one undo node, while genuinely unknown reasons still fail explicitly (S59-A11).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s10.12 ≥ 140 assertions across the five unit files.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.1 `make test`, `make fuzz`, `make perf` green under gcc and clang, zero warnings at `-std=c11 -pedantic -Wall -Wextra -Werror`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.2 Differential campaign: 4 seeds × 200 000 ops × 3 mixes with zero divergences; the every-op tier (`len`, `line_count`, `yew_textbuf_check`, piece bound) runs on all 200 000.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.3 All 12 properties implemented and green at `YEW_PROP_N=500` × 4 seeds; P6 (undo–redo identity) and P8 (branch preservation) additionally green at `YEW_PROP_N=20 000` in the nightly lane.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.4 Invalid UTF-8, NUL, CRLF, sequence-splitting, and huge-line content classes are each provably exercised (per-class op counters printed at the end of a run and asserted non-zero) and round-trip byte-exactly through P11 — invariant 2 held under fuzz.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.5 A deliberately introduced bug in `piece.c` (an off-by-one at the `at == lbytes` seam, applied in a scratch commit) is caught by the campaign within 5 000 ops on all four seeds — the suite is proven to have teeth, not just to be green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.6 Shrinker self-test reduces a 5 000-op trace to ≤ 5 ops; the shrinker's output replays to the same failure.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.7 `gen-bigfile` output is byte-identical across gcc/clang lanes and two runs for all 6 profiles; `make fixtures-verify` green; `git status` is clean after `make fixtures` (nothing generated outside `build/`).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.8 Every §7 gated metric is measured and within budget on the designated runner: 100 MiB open ≤ 150 ms, 1 GiB open ≤ 2.0 s, RSS ≤ 1.6× on both code fixtures, head/tail insert p99 ≤ 50 µs on 1 GiB, `line_start` p99 ≤ 5 µs on `1g-code` **and** `1g-noline`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.9 Edit-at-both-ends on 1 GiB: 10 000 alternating inserts in ≤ 1.0 s with ≤ 8 MiB RSS growth.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.10 `100m-allnl` RSS is recorded in the baseline as informational with the s07 exemption cited in a comment; it does not fail the build.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.11 Baselines committed for the designated runner; the gate fires on a deliberately slowed build (scratch commit adding a sleep) and does not fire across 5 consecutive unmodified runs (no flapping); `perf-baseline-guard` rejects a mixed `src/` + baseline commit.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.12 Grep gate: `grep -rn 'yew_textbuf_\|piece\.h' tests/fuzz/oracle.c` → nothing; `grep -rn '\brand\b\|srand\|time(NULL)' tests/fuzz scripts/` → nothing.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.13 ASan/UBSan/valgrind/determinism lanes green including the fuzz and property binaries.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s11.14 ≥ 150 assertions across `test_oracle.c`, `test_textbuf_props.c`, `test_shrink.c`, and `test_piece_regress.c`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.1 `make test` green under gcc and clang, `-std=c11 -pedantic -Wall -Wextra -Werror`, zero warnings; ASan/UBSan/valgrind/determinism green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.2 All six RFC 4648 vectors pass; base64 round-trip fuzz green on 10 000 binary buffers; encoder emits no newline (`grep`-asserted in the test).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.3 Every row of the §2 routing table has a test; the pinned `1`-vs-`-` shift rule is tested at both sides of its boundary.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.4 All six §3 append rows tested, including the refused BLOCKWISE case.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.5 All §4 linewise rows and all §5 blockwise rows tested, including the short-line pad, the tab round-left pad, the CJK column, and buffer extension past the last line.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.6 A paste of any type is exactly **one** undo node; undo restores content and cursor byte/offset-exactly; 5 ring cycles produce exactly one node.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.7 OSC 52 goldens byte-exact for plain, tmux (ESC doubled), and screen (768-byte chunking, 3 chunks); over-`YEW_OSC52_MAX` payloads are never truncated and never emitted.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.8 Pty: OSC 52 bytes never appear between BSU and ESU; an unsolicited OSC 52 reply on input reaches the buffer zero times.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.9 Detection order correct for all 12 env/PATH combinations incl. the `$SSH_TTY` rule; every `YEW_CLIPBOARD` value honored.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.10 Non-exiting clipboard tool: 100 writes complete, none blocking > 2 ms, zero zombies, write reported successful.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.11 `clipboard.sync` default is `yank`; a delete under the default does not write the system clipboard (asserted via the fake tool's log).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.12 Grep gates: `grep -rn 'popen\|system(' src/` → nothing; `grep -rn '52;c;?\|52;.*?' src/` → nothing; `grep -rn 'yew_reg_set' src/` hits only `register.c`; `grep -rn 'width\|ccol' src/text/register.c` shows call sites into `src/unicode/` only, zero width arithmetic.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.13 Sprint 17 blockwise production and the Sprint 21/18/23 writers for `/`, `:`, and `#` route through the register choke points established here; absent or unknown producers remain explicit rather than lying (S59-A11).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s12.14 ≥ 130 assertions across the six unit files plus the two pty tests.
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

1 open confirmed finding: `YEW-F-100`.

## Deferrals recorded against this front

none

## Verdict

NOT READY — `YEW-F-100` and independent evidence review remain pending.
