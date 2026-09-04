# F03 TEXT — text engine, undo, registers

Status: closed
Baseline: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`
Opened: 2026-09-03
Scope: `src/text/`, `tests/fuzz/oracle.c`, `tests/torture/`
Owners read: Sprint 7, Sprint 8, Sprint 9, Sprint 10, Sprint 11,
Sprint 12

The product-code baseline remained immutable. Audit-control changes add
edge-specific save, register, paste, and LSP-rename coverage only.
All five new cases passed the default arm64 macOS build and ASan/UBSan;
the four module-independent cases also passed the `MODULES=""` build.
Hosted run `33836915903` then passed the complete 22-job push matrix at
close head `b9e604d4`, including the new controls on both compilers, both
hosted arm64 targets, musl, ASan/UBSan, determinism, and `MODULES=""`.

## Q1 — byte-preserving open, edit, and save

probed, nothing found in the exercised paths

- `file_edit_save_preserves_untouched_edge_bytes` opens, edits, saves, and
  compares an empty file, a file containing only U+200D sequences, a binary
  file with an embedded NUL, and a missing-final-newline file whose edit is
  at EOF. It passed 59 assertions on arm64 macOS.
- `file_save_rejects_changed_disk` replaces the destination inode after
  load and proves save returns `YEW_SAVE_CHANGED_ON_DISK` without changing
  the replacement. `save_hardlink_preserves_shared_inode` now uses exactly
  three names and proves both saves retain one inode and byte-identical
  content through every name.
- `save_symlink_into_read_only_directory_stays_in_place` composes the two
  decision-table conditions: a symlink remains a symlink, its target
  directory loses write permission after load, the fallback retains the
  target inode, and every target byte is correct. The existing independent
  symlink, dangling-symlink, and unwritable-directory cases remain green.
- The sparse-file boundary case at 2 GiB + 1 returns
  `YEW_LOAD_TOO_LARGE`. The size predicate is strictly `>` against
  2×1024³, so exactly 2 GiB is admitted rather than rejected one byte
  early. See the exact-boundary execution limitation under observations.

## Q2 — atomic syscall order and real-file truncation

probed, nothing found

- The fault-shim checker consumed its intercept logs as a four-state
  automaton: one or more `write` calls, `fsync-file`, `rename`, then
  `fsync-dir`. Metadata-only `fchown`/`close` entries are accepted between
  the file barrier and rename; any other entry or ordering is a failure.
- All four seeded atomic sweeps passed with terminal syscall counts 17, 11,
  10, and 10. The deterministic replay at seed 424242 also passed.
- `src/text/file.c` contains no `O_TRUNC`; the in-place path writes only
  after a durable backup exists. The atomic path writes a sibling temporary,
  never the real destination, before rename.

## Q3 — 10× save torture and journal recovery

probed, nothing found

- `make torture TORTURE_SIGKILL_ITERS=5000` used the repository's nightly
  10× setting. The API lane passed all fault boundaries for four atomic
  seeds, the hardlink lane, late-hardlink fallback, EXDEV fallback,
  ownership fallback, in-place backup/rotation/failed-write/metadata-fault
  lanes, EINTR retry, deterministic scheduling, and 5,000 externally timed
  `SIGKILL`s.
- The live-editor lane repeated the four atomic seeds and hardlink boundary
  sweep (terminal syscall counts 34, 28, 26, 29, and 38), EINTR and
  determinism checks, and another 5,000 external kills against the shipping
  editor path. Its checker accepted 100% of cases.
- The batch path added 200 externally timed kills: 24 retained the old
  image, 176 committed the new image, and 157 left recoverable journals;
  none produced a partial destination.
- After every kill the checker required the destination to equal the exact
  pre-save or post-save image. Whenever the pre-save image remained, journal
  replay had to reconstruct the post-edit buffer exactly; the in-place lane
  additionally required a restorable backup.

## Q4 — one transaction and mutation API

probed, nothing found

- `rg 'yew_txn_' src tests` returned no matches. The only public transaction
  spelling is `yew_undo_begin/end/abort`; user-visible mutation enters
  `yew_edit_insert/delete` through `EditCtx`.
- Every raw `yew_textbuf_insert/delete/insert_span` call outside
  `src/text/edit.c` was classified. `src/text/undo.c` is replay and
  validation; batch stdin and `symwalk` are initial hydration; AI/LSP logs,
  Git diff, rename preview, and trust views are no-undo/read-only scratch
  projections; macro edit is initial scratch population followed by a saved
  undo root. The multiline LSP test proves the actual apply path commits one
  `YEW_TXN_LSP` node.

## Q5 — undo replay storage, RSS, and compaction safety

probed, nothing found

- `make perf-undo` warmed 1,000 cycles, then completed 100,000 undo/redo
  cycles with `tb->add.len` fixed at 8 bytes. Peak RSS grew 16,384 bytes
  against the 1,048,576-byte ceiling.
- `undo_redo_cycles_do_not_grow_add_store` independently passed 100,000
  cycles and retained two nodes. The focused undo property suite also kept
  byte-exact state across roots and branches.
- `compact_history` is private and has one caller, `trim_tree`. Implicit
  transactions call it only when depth is zero; explicit transactions call
  it only after `yew_undo_end` decrements depth to zero. `UndoNodeInfo`
  contains values rather than arena pointers, and its sole product consumer
  copies those values into picker-owned storage before later edits.

## Q6 — generation-safe mark repair

probed, nothing found

- `undo_marks_stale_generation_skips_repair` deletes a mark, reuses its slab
  slot with a different generation, and undoes the recorded deletion. The
  replacement mark stays at byte zero rather than teleporting.
- The complete focused mark-repair suite passed 5 tests and 2,504,032
  assertions, including the randomized 1,000-mark oracle.

## Q7 — register routing at adversarial deletes

probed, nothing found

- The exact `yew_reg_set(` source grep reaches only `register.c`; there is no
  external caller bypassing `yew_reg_yank`/`yew_reg_delete`.
- `register_blockwise_short_delete_uses_small_delete` builds a ragged
  three-row block whose byte payload has no LF. It reaches `-`, leaves `1`
  empty, and preserves row geometry, matching the pinned payload/type rule.
- `lsp_rename_multiline_delete_does_not_touch_registers` applies one edit
  spanning two source lines. It commits exactly one LSP undo node while
  leaving unnamed, numbered, small-delete, and kill-ring state unchanged.

## Q8 — cross-EOL paste

probed, nothing found

- `paste_crlf_into_lf_preserves_both_files_bytes` loads a CRLF source and LF
  destination, captures a linewise register, pastes into the LF buffer, and
  saves both. The source remains `source\r\n`; the destination becomes
  exactly `destination\nsource\r\n`. It passed 44 assertions.
- The complete paste-focused suite passed 16 tests / 510 assertions,
  including missing-final-newline synthesis, CRLF destination synthesis,
  blockwise padding, CJK columns, ragged rows, and undo restoration.

## Differential evidence

- The twelve standard `fuzz_textbuf` runs cover four fixed seeds × typing,
  paste, and undo mixes at 200,000 operations each: 2,400,000 independently
  checked operations against `tests/fuzz/oracle.c`, with invariant checks,
  cursor/mark state, snapshots, branch state, binary/NUL/CRLF payloads, and
  deterministic trace hashes.
- Four dedicated line-heavy runs added 800,000 operations across the same
  fixed seeds. Their trace hashes were `2db9bc6382ff29a0`,
  `44d2278c221762f9`, `18c00a70d9139e2c`, and `5f92005806df28ba`;
  all matched the byte-array oracle. Total differential text coverage was
  3,200,000 operations.
- `fuzz_undo --iters=200000 --seed=58` independently completed 90,238 edits,
  39,749 undos, 9,538 redos, and 2,281 abandoned branches with oracle hash
  `1b63e33f5578e69f`.
- The twelve TextBuf property tests passed 2,470,123 assertions.

## Unverified observations

- An exact 2 GiB file was not read, edited, and written end-to-end on the
  audit machine: doing so would consume several GiB of resident and I/O
  capacity for a boundary already isolated by the strict predicate and the
  sparse 2 GiB + 1 rejection case. This is an execution-evidence gap, not a
  reproduced defect.
- The symlink test uses a directory changed to mode 0500 rather than a
  separately mounted read-only filesystem. It exercises the intended
  in-place decision and identity preservation but does not synthesize an
  `EROFS` result from the kernel.

## Count

Raw 0 · deduped 0 · critical 0 · high 0 · medium 0 · low 0 · unverified 2.
