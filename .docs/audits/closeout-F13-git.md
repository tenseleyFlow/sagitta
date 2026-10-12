# Closeout: F13 — Git and FUSS

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s51, s52, s53

## DoD items (every numbered item from every sprint above)

- [ ] s51.1 `make test` green on gcc and clang at `-std=c11 -pedantic -Wall -Wextra -Werror`; ASan/UBSan/valgrind/determinism lanes green; the `MODULES=""` build compiles and every `ed.git.*` command reports `this build has no fuss module; rebuild with 'make MODULES="… fuss"'` verbatim.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s51.2 `grep -rn '/tmp/\|mkstemp\|tmpfile\|system(\|popen(' src/mod/git/` → empty. `grep -rn 'clock(\|cpu_time\|\btime(' src/mod/git/` → empty. `grep -rn 'sprintf\|bytebuf_printf' src/mod/git/ | grep -v porcelain.c` → empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s51.3 Every record kind in §5 (`#` ×5, `1`, `2`, `u`, `?`, `!`) has a passing parse assertion, and every `XY` pair in §5.2/§5.4 drives the §5.6 mapping table.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s51.4 The rename desync test passes and **fails** when `pv2_record` is patched to consume one NUL (the test's comment pins this).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s51.5 All six awkward filenames (space, `"`, `$`, newline, `-n`, CJK) survive porcelain → parse → argv → `execve` byte-identical, proven by the argv hook.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s51.6 Truncation fuzz: cutting the fixture buffer at every one of its N byte offsets never reads out of bounds (ASan) and never returns `YEW_GIT_OK` with a partial entry.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s51.7 TTL: the zero-CPU/10-s-wall-clock test expires the cache; the fake-clock rows at 499/500/501 ms are exact.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s51.8 Every `YEW_GV_MUTATE`/`YEW_GV_NET` verb invalidates the cache without its call site saying so (asserted by a table-driven test that walks the whole verb table and checks `gen` advanced).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s51.9 A refresh failing mid-parse leaves the published snapshot byte-identical (memcmp of the serialized snapshot) and `gen` unchanged.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s51.10 All 18 `GitStatusCode` values are reachable in tests; the five mid-* states are produced by filesystem probes only — `grep -rn 'strstr' src/mod/git/git.c` shows hits **only** inside the §12 auth-message table.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s51.11 The environment test asserts all 13 rows of §3 and that `environ` is unmutated.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s51.12 Perf: 20 000-entry parse ≤ 8 ms; a 500 ms-TTL refresh loop over the fixture repo keeps keypress p99 ≤ 5 ms under scripted typing.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s51.13 ≥ 140 assertions across the two unit files; the fixture repo produces identical object hashes on two different machines (CI asserts a pinned hash list).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s51.14 `ed.mode.enter F` and every Sprint 52/53 command hard-error naming their sprint.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.1 `make test` green on gcc and clang, zero warnings; ASan/UBSan/valgrind/ determinism/`MODULES=""` lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.2 Every row of the §3 glyph table appears in pty evidence, with Unicode and `git.ascii_glyphs` tree variants; all status combinations realizable by real Git are covered there. Marker stacking order is independently asserted for all 15 abstract non-empty subsets in the unit table.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.3 `grep -rn 'strlen\|wcwidth' src/mod/git/fussmode.c src/mod/git/fusstree.c` → empty; all width math routes through `src/unicode/`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.4 Depth navigation matches the hand-computed table for all 12 fixture items × 4 directions, wraparound included.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.5 **Selection survives refresh by path**: the delete-a-row-above test passes and fails against an index-based implementation.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.6 Collapsed state survives 100 consecutive refreshes with zero drift (the set harvested equals the set restored, every time).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.7 The §5 rule holds: with the jump disarmed, all 30 verb keys dispatch their command; with it armed, none of them do, and the window self-clears under the fake clock.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.8 Every §6 command exists in the registry with a non-empty `help`, is `YEW_CMD_RECORDABLE`, and is reachable from E mode; `yew_keymap_build` accepts the `mode:F` table with no duplicate sequence.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.9 No message, path, or ref reaches `execve` through formatting: the argv hook proves the `"`/`$(id)`/newline/`-n` fixtures byte-identical, and `grep -rn 'sprintf' src/mod/git/fuss*.c` → empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.10 Empty commit message spawns **zero** git processes and reports `aborting commit due to empty commit message`; comment stripping is exact for `#` and for a `core.commentChar=auto` repo.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.11 Both footer rows carry the `mode.git` background across the full width in the goldens; entering and leaving F mode restores the previous pane layout byte-identically (invariant 5).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.12 `q` and `<esc>` both return to L; neither exits yew; `ed.quit` is the only exit (pty golden).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.13 The rebase handover leaves the terminal restored on **all** exit paths including a synthetic SIGTERM mid-rebase (torture lane asserts termios equality before and after).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.14 `ed.group.from_dir` hard-errors naming Sprint 53; every Sprint 53 command still hard-errors naming it.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.15 Perf: 20 000-entry build+flatten ≤ 12 ms; keypress p99 ≤ 5 ms with a 500 ms refresh loop running; baselines committed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s52.16 ≥ 150 assertions across the four unit files; ≥ 12 pty goldens, all byte-stable across two runs.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.1 `make test` green on gcc and clang, zero warnings; ASan/UBSan/valgrind/ determinism/`MODULES=""` lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.2 The Myers differ matches the brute-force oracle's **edit-script length** on 5 000 random pairs and produces zero hunks (and zero allocations) for identical inputs.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.3 All 20 generated patches are accepted by a real `git apply --check --cached`, including all four no-final-newline combinations and a CRLF fixture.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.4 Staging a hunk from a **dirty** buffer changes `git diff --cached` to the golden and leaves the worktree file byte-identical; the one-time `staged from an unsaved buffer` message fires exactly once per session.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.5 Discarding a hunk is **one** undo transaction: undo depth is exactly 1 and the buffer is byte-identical to its pre-discard state; no `git checkout` appears anywhere — `grep -rn 'checkout' src/mod/git/gutter.c` → empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.6 The diff base is the index blob: a test where the worktree file, the index, and the buffer all differ asserts the signs describe buffer-vs-index; the conflicted fixture falls back to `HEAD:` and sets `base_is_head`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.7 `index_oid`-keyed caching: editing a buffer 100 times with an unchanged index spawns **zero** `cat-file` requests after the first (counted).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.8 Every size guard produces its stated visible outcome; no guard produces a silently partial hunk list.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.9 Blame: metadata reuse across groups is asserted against a fixture where only the first group carries it; `--contents -` blames unsaved lines as the zero sha rendered `(uncommitted)`; the cache invalidates on edit and the stale attr shows meanwhile.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.10 Side-by-side alignment holds after an unbalanced hunk (pty golden), and the sync-scroll feedback loop is proven absent (a test that would recurse without the `syncing` flag, with the comment saying so).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.11 All ten statusline rows of §7 appear in goldens; the segment never spawns a process — `grep -rn 'yew_git_spawn\|yew_job_spawn' src/ui/statusline.c` → empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.12 `ed.group.from_dir` is live: Sprint 24 DoD 11's hard-error is gone, the 40-file group costs one file read, ordinals are deterministic across runs, and F mode's `g` opens a group.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.13 No inotify anywhere: `grep -rn 'inotify\|kqueue\|fanotify' src/` → empty; focus-gain and user/external job completion both invalidate (asserted by a test that changes the repo behind yew's back and then delivers each trigger). Internal read workers do not self-invalidate; internal mutate/network callbacks invalidate structurally.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.14 Perf gates green: 100 kLOC/500-hunk re-diff ≤ 25 ms off the keystroke path, per-frame sign lookup ≤ 50 µs, 200 keystrokes ⇒ one re-diff, keypress p99 ≤ 5 ms; baselines committed under `tests/perf/baselines/`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.15 Conflict resolution commands hard-error with the permanent-non-goal message; sub-hunk staging is named as post-1.0, not deferred.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s53.16 ≥ 150 assertions across the three unit files; ≥ 14 pty goldens, all byte-stable across two runs.
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

none

## Deferrals recorded against this front

none

## Verdict

NOT READY — Independent evidence review remains pending.
