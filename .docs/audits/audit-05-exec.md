# F05 EXEC — E mode, shell jobs, filters

Status: closed
Baseline: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`
Opened: 2026-09-06
Closed: 2026-09-07
Scope: `src/ui/cmdline*.c`, `src/edit/job.c`, `src/edit/shell.c`
Owners read: Sprint 18, Sprint 19

The product-code baseline remained immutable. Audit-control changes raise
the lifecycle and shell-quote samples, enforce the interpolation ban, add
failed-filter journal and live typeahead proofs, and extend the exact job
environment dump.

## Q1 — one editor implementation

probed, nothing found

- `CmdLine` owns a `TextBuf *buf` and `Cursor cur`. Prompt replacement runs
  through `EditCtx`, `yew_undo_begin/end/abort`, and
  `yew_edit_delete/insert`; movement and drawing use the shared Unicode
  coordinate layer.
- The `src/ui/` scan found no character-array plus caret-index editor. The
  fixed arrays it did find are formatting scratch, completion punctuation,
  history hex digits, the diagnostic hint, and the message line; none owns
  editable prompt text or a cursor.

## Q2 — descriptor, child, and family accounting

probed, nothing found

| Family | Fresh audit control | Result |
|---|---|---|
| generic `YewJob` | 500 complete spawn/reap/release cycles | fd delta 0; every child `waitpid` returned `ECHILD`; 3,007 assertions |
| LSP | 200 fake-server ready/stop/drain sessions | fd delta 0; 3,401 assertions |
| Git | 100 blob requests through one persistent child | all 100 callbacks completed and the job table drained; 415 assertions |
| clipboard | 100 non-exiting helper writes | all owned children reaped, helper descendants gone; 606 assertions |
| AI curl lifecycle | 500 cancel/detach/reap cycles | job table and retired-job table empty every cycle; 2,000 assertions |

- Every configured LSP server reaches the single `spawn_server` job seam.
  All 40 Git verb descriptors reach the common Git spawn path; the blob
  batch and synchronous FUSS editor handover also use the central job core.
  All four AI subprocess construction sites use it as well.
- Clipboard remains the intentional s12 subprocess implementation rather
  than a `YewJob`, so it was included explicitly rather than being hidden
  behind the generic count. Its pipes are close-on-exec/nonblocking and its
  child ownership has a separate reap proof.
- The focused `job_` suite passed 37 tests and 6,329 assertions.

## Q3 — argv construction and shell boundary

probed, nothing found

- The exact `bytebuf_printf.*cmdline|sprintf.*shell` scan found no source
  matches. `scripts/bans.sh` now makes that pattern a repository gate and
  proves the gate with a seeded `bytebuf_printf(&cmdline, ...)` negative
  control. Its existing `system`/`popen` bans also remained green.
- All 14 `YewJobSpec` construction sites were read. LSP copies the configured
  command and arguments into an argv vector; Git uses structural argv for
  every verb, batch, and editor handover; AI uses fixed curl argv or the
  configured key-command argv; the workspace indexer and package Git helper
  are structural too.
- `spec.cmdline` occurs only in `src/edit/shell.c` for shell text supplied by
  the user or by a capability-authorized Fletch caller. No filename, root,
  ref, URL, key, selection, or protocol value is interpolated into it.

## Q4 — load-bearing filter I/O interleave

probed, nothing found

- `45_filter_streaming.fl` filters a region larger than 1 MiB through a child
  that writes 1 MiB before reading stdin. The production implementation
  passed with byte-exact length, first-line, last-line, and undo assertions.
- In an isolated temporary worktree, `yew_job_collect_fds` was changed to
  register only stdin while any input remained. With no product change on
  the audit branch, the same control stalled until Yew's five-second filter
  watchdog and failed after 5.7 seconds. Restoring simultaneous `POLLIN` and
  `POLLOUT` made it pass. The interleave is therefore load-bearing.

## Q5 — shell quoting through real `/bin/sh`

probed, nothing found

- `shell_quote_roundtrips_random_bytes` now executes 100,000 independently
  framed cases through real `/bin/sh`, batched 256 cases per shell to keep
  the audit fast. Every shell result is compared byte for byte with its
  source.
- Generation covers byte values 1 through 255, including quotes, dollar and
  backtick bytes, backslashes, embedded newlines, and invalid UTF-8. NUL is
  the sole exclusion because POSIX argv cannot carry it. Empty and targeted
  hard cases remain separate controls.
- All three shell-quote tests passed 1,262 assertions in 2.9 seconds.

## Q6 — failed-filter rollback, undo, and journal

probed, nothing found

- The new file-backed control filters `keep 0xff bytes` through a command
  that emits replacement stdout and exits 9. It observes the original bytes,
  text generation, undo generation/current node/depth, node and op counts,
  and null journal handle unchanged. `yew_journal_probe` also confirms that
  no crash-journal file was created. The test passed 20 assertions.
- Source ordering agrees with the dynamic proof: the job is fully collected
  and classified before an `EditCtx` is acquired. Only `YEW_FILT_OK` enters
  the one `YEW_TXN_FILTER` delete-plus-insert transaction, so a nonzero exit
  has no partial edit to undo or journal.
- Scripts 44 through 46 passed all 32 assertions: nonzero, exec failure,
  timeout, empty output, partial replacement, invalid UTF-8, pipe pressure,
  EPIPE, 4 MiB output, and the 10,000-line single-undo case.

## Q7 — foreground-filter typeahead

probed, nothing found

- The new real-PTY control starts `%!sleep 1; cat`, then sends the complete
  modal input burst `i`, `QUEUED`, Escape while the child is live. After a
  100 ms pump the word is absent from the grid, proving no mid-filter
  dispatch.
- Once the filter completion message appears, the same bytes replay in order
  and the golden contains `keep me` followed by `QUEUED`, with the editor
  back in line mode. Two independent focused executions matched the golden.

## Q8 — exact environment for every job type

probed, nothing found

| Row | Observed child value |
|---|---|
| `YEW_FILE` | empty for the unnamed audit buffer |
| `YEW_LINE` | `1` |
| `YEW_COL` | `3` after ASCII `a` plus the 25-byte ZWJ family grapheme |
| `YEW_WORKSPACE` | exact `yew_ws_root()` path |
| `YEW_JOB` | `1` |
| `PAGER`, `GIT_PAGER` | `cat`, `cat` |
| `COLUMNS`, `LINES` | absent, absent |

- All nine parent variables were first set to hostile values. The spawned
  shell observed exactly the table above, while the parent retained every
  hostile value both immediately after spawn and after reap. The control
  passed 41 assertions.
- LSP and all AI subprocess sites provide no environment overrides, so the
  central `job_build_env` table applies without variation. Git is the only
  audited family that adds relevant overrides; its actual fake-Git `env`
  dump now checks the complete Yew table too. `make test-git-script` passed
  15,372 assertions, including hostile-parent preservation.

## Unverified observations

None.

## Count

Raw 0 · deduped 0 · critical 0 · high 0 · medium 0 · low 0 · unverified 0.
