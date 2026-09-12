# F13 GIT — Git layer and FUSS mode

Status: closed
Baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`
Opened: 2026-09-12
Closed: 2026-09-12
Scope: `src/mod/git/`
Owners read: Sprints 51, 52, 53

The product-code baseline remained immutable. This front adds hostile-path,
clock-step, and subprocess-kill controls plus four hard-XFAIL reproducers; no
`src/` file changed.

## Q1 — static verb and argv boundary

findings `YEW-F-017`, `YEW-F-020`

- The 40-row `git_verbs` table remains the only inventory used by normal
  finite verbs. The callback path and persistent blob batch both resolve a
  descriptor and use argv-only job spawning. Source scans find no
  `yew_git_raw`, `system(`, `popen(`, temporary-file API, or
  `yew_shell_quote` under `src/mod/git/`.
- The hostile filename matrix observes each selected path after its own `--`
  and byte-identical in the captured argv. No path is interpolated into a
  formatted argv element.
- Interactive rebase is the exception: `fuss_rebase_sync` constructs a direct
  `YewJobSpec` and invokes `yew_job_run_sync` without a verb descriptor.
  `tests/audit/yew_f_017.c` records the bypass and its missing shared
  environment policy as a Medium hard-XFAIL.
- Sprint 51's literal `sprintf|bytebuf_printf` release gate has seven matches
  outside `porcelain.c`. They format owned status, patch, and picker-detail
  text rather than argv, but the gate cannot distinguish that safe use.
  `tests/audit/yew_f_020.c` pins the Medium control defect for Sprint 59.

## Q2 — porcelain-v2 rename consumption

finding `YEW-F-019`

- A manual mutation changed both record-counting and parse passes to advance
  only to the destination NUL. The named unit test still passed all 16
  assertions: the original path begins with an unknown kind byte, so the
  mutant skips it and resumes at the next ordinary entry.
- `tests/audit/yew_f_019.c` models the exact stream advance and proves the
  mutant still exposes all seven asserted entries. This Medium test-control
  finding remains for Sprint 59.
- The unmodified porcelain filter passes 8 tests and 1,757 assertions. Four
  independent 200,000-iteration fuzz streams pass with hashes
  `d67ee79f885b048f`, `27dcd0ac818c8422`, `44cfc3632804e25f`, and
  `e961e594b27f5e9c`.

## Q3 — byte-exact filename matrix

probed, nothing found

- One isolated repository carries `a b`, `a\nb`, `a"b`, UTF-8 alpha, and
  `a 0x80 b`. Every name traverses yew status parsing, the FUSS tree,
  stage, unstage, commit, `ls-tree -z`, diff, and blame with exact byte
  comparisons and a literal-path argv capture.
- Darwin's filesystem refuses the invalid UTF-8 directory entry. On that
  platform the control writes its blob and raw pathname directly into the Git
  index, so the parser, tree, verbs, commit, and tree-object path remain real
  Git operations without pretending APFS accepted a name it cannot represent.
  Filesystems that permit the name take the ordinary worktree path.
- `test-fuss-commands` passes 544 assertions; the broader Git fixture passes
  15,372 assertions.

## Q4 — injected monotonic TTL

finding `YEW-F-018`

- Cache age is computed from the injected `i64` millisecond clock. Exact
  499/500/501 ms controls pass, and a live cache stepped from 700,000 ms back
  to 100,000 ms schedules a refresh rather than treating the snapshot as
  immortal.
- The required module-wide clock scan nevertheless has five matches. Four are
  helper names ending in `clock`; the fifth is a real `time(NULL)` call in
  FUSS relative-time detail. `tests/audit/yew_f_018.c` records the
  uninjectable display edge and the overbroad gate as one Medium finding.

## Q5 — failed-refresh publication isolation

probed, nothing found

- A populated generation-2 snapshot is published, then a real fake-`git`
  subprocess is killed with `SIGKILL` at 200 deterministic varied read
  instants. After every completion the published pointer, full snapshot
  struct, entry struct, branch, object id, comment character, path bytes, and
  `gen` are identical to the pre-kill values.
- The focused stress case passes 3,348 assertions. The complete Git-cache
  filter passes 34 tests and 7,761 assertions.

## Q6 — lock and in-progress state probes

probed, nothing found

- `index.lock` suppresses an expired refresh with no status spawn and leaves
  the published cache standing.
- `MERGE_HEAD`, both rebase directories, `CHERRY_PICK_HEAD`, `REVERT_HEAD`,
  `BISECT_LOG`, and `index.lock` drive the enum taxonomy through one
  `access(F_OK)` helper. The only `strstr` in `git.c` is the documented
  authentication-message table.

## Q7 — forced child environment and parent isolation

finding `YEW-F-017`

- The ordinary spawn, callback, and blob-batch builders force
  `GIT_TERMINAL_PROMPT=0`, both Git editors to `false`, `GIT_FLUSH=1`, both
  pagers to `cat`, and `LC_ALL=C`; they remove the pinned trace and terminal
  geometry rows. Every descriptor reaches one of these shared builders.
- A fake Git under a hostile parent environment confirms every forced,
  removed, and passed-through row. The test also compares the parent
  environment after child completion; it is unchanged.
- The direct interactive-rebase path does not share that builder and omits
  prompt, flush, and trace policy. This is the second violated requirement
  captured by `YEW-F-017` rather than a duplicate finding.

## Q8 — FUSS expansion memory and group opening

probed, nothing found

- Expansion memory retains `remembered/` while a refresh removes that
  directory and reapplies it when the directory returns. Automatic ancestry
  for an open file remains distinct from remembered user expansion.
- The FUSS unit slice passes 83 tests and 3,793 assertions. The directory
  group script passes 5,620 assertions, including deterministic sorted walk,
  adopt-before-add ordinal repair, one resident read for a 40-file group,
  picker fallback over the cap, and inert empty-directory behavior.
- The reported action/type-jump conflict was already repaired before this
  front: bare filename bytes search only visible rows, actions use Alt
  sequences, `A-g` routes to the real directory-group command, and `C-S-/`
  opens the effective-binding-derived `FUSS actions` picker. The focused
  action-palette PTY matches its committed golden, so no new field finding is
  assigned.

## Unverified observations

None.

## Count

Raw 4 · deduped 4 · critical 0 · high 0 · medium 4 · low 0 · unverified 0.
