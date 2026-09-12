# F15 CI — build, tests, determinism, and gates

Status: closed
Baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`
Opened: 2026-09-12
Closed: 2026-09-12
Scope: `Makefile`, `scripts/`, `.github/workflows/`, `tests/`,
`.docs/audits/xfail-debt.md`
Owners read: Sprints 00, 01, 11, 33, 56, 57

Questions 1–7 and 9 kept the fixed product baseline immutable. Question 8 is
the explicit Sprint 37 §9 obligation assigned to this front: it added a
coverage-only VM/compiler path and runner/report tooling. Ordinary bytecode,
shipping execution, and the latency-profiled compiler path are unchanged
unless the audit runner requests coverage.

## Q1 — XFAIL debt and hard XPASS

findings `YEW-F-024`–`YEW-F-026`

| Surface | Debt linkage | Seeded XPASS verdict |
|---|---|---|
| audit/unit | every registered `YEW-F-###` names a findings-ledger row | hard XPASS: `build/audit_tests --xpass-probe` reports `XPASS YEW-F-000` and exits 1 |
| script | no `# XFAIL:` parser, ID linkage, XFAIL, or XPASS state | not representable; `YEW-F-025` |
| PTY | `PtyCase` has no `xfail_id`, and the runner has no XFAIL/XPASS path | not representable; `YEW-F-026` |
| Fletch conformance | directive and debt-file linkage exist | hard XPASS: the committed `xpass.fl` meta-probe reports XPASS and makes the meta check fail as intended |

- All four entries currently present in `xfail-debt.md` still reproduce as
  hard XFAILs and cite their corresponding findings. The historical Fletch
  conformance ledger is empty and has no orphan marker.
- The authoritative findings ledger contains 75 open IDs while the claimed
  cross-surface debt ledger contains only `YEW-F-001`–`004`: 71 findings are
  missing. `YEW-F-024` pins that ledger failure.
- Audit fixtures have no XFAIL whose ID is absent from `findings.md`, and no
  duplicate or malformed registry ID. That narrower bidirectional control
  passes; it does not repair the incomplete authoritative debt table.

## Q2 — ban honesty

findings `YEW-F-027`–`YEW-F-071`

The unmodified repository passes `scripts/bans.sh`. Each row below then puts
one intent-forbidden spelling in an isolated otherwise-clean repository and
runs that same script. Every seed is accepted. The shared reproducer is
`tests/audit/f15_ban_misses.c`; `tests/audit/f15_ban_miss.sh` constructs each
fixture.

| ID | Rule defeated by the accepted seed |
|---|---|
| `YEW-F-027` | macro-forwarded nonliteral Fletch format |
| `YEW-F-028` | macro-forwarded Fletch `abort` |
| `YEW-F-029` | macro-forwarded `qsort` |
| `YEW-F-030` | token-pasted `__attribute__` |
| `YEW-F-031` | token-pasted constructor |
| `YEW-F-032` | token-pasted `pthread_create` |
| `YEW-F-033` | omitted `__TIMESTAMP__` reproducibility poison |
| `YEW-F-034` | macro-forwarded `mmap` |
| `YEW-F-035` | macro-forwarded libc allocation |
| `YEW-F-036` | variable-NULL `getcwd` allocation |
| `YEW-F-037` | variable-NULL `realpath` allocation |
| `YEW-F-038` | locale-dependent `mbtowc` |
| `YEW-F-039` | native `dlvsym` loading |
| `YEW-F-040` | macro-forwarded `strerror_r` |
| `YEW-F-041` | glibc `backtrace_symbols_fd` |
| `YEW-F-042` | GNU `getopt_long_only` |
| `YEW-F-043` | continued-line `long double` |
| `YEW-F-044` | parenthesized disabled-shim success |
| `YEW-F-045` | decimal local Unicode width table |
| `YEW-F-046` | decimal packed syntax color |
| `YEW-F-047` | local syntax width arithmetic |
| `YEW-F-048` | direct `posix_openpt` outside the harness |
| `YEW-F-049` | split-name golden update in CI |
| `YEW-F-050` | piece-tree `pread` |
| `YEW-F-051` | manual destructive shadow-row fill |
| `YEW-F-052` | indirect FUSS pane-root replacement |
| `YEW-F-053` | libc `random()` in deterministic fuzzing |
| `YEW-F-054` | clipboard shell through direct `execl` |
| `YEW-F-055` | job data appended into shell text |
| `YEW-F-056` | split-literal OSC 52 query |
| `YEW-F-057` | direct `tcflush` outside `tty.c` |
| `YEW-F-058` | register wrapper hidden in the allowed file |
| `YEW-F-059` | option wrapper hidden in the allowed file |
| `YEW-F-060` | package-Git wrapper used on the startup path |
| `YEW-F-061` | local register-width lookup table |
| `YEW-F-062` | renamed register-column arithmetic |
| `YEW-F-063` | helper names present only in comments |
| `YEW-F-064` | copied and renamed piece-model oracle |
| `YEW-F-065` | hand-edited generated Unicode table retaining its marker |
| `YEW-F-066` | process termination through `_Exit` |
| `YEW-F-067` | AI body logging under a renamed variable |
| `YEW-F-068` | unregistered `static void` unit test |
| `YEW-F-069` | missing PTY registry |
| `YEW-F-070` | computed name for a missing PTY golden |
| `YEW-F-071` | orphan golden hidden by a dead `#if 0` registry row |

These are findings against claimed gate coverage, not claims that the seeded
violations exist in product code. Sprint 59 should replace or narrow each
control according to what it can prove; retaining a grep while broadening its
claim would not close the finding.

## Q3 — performance-gate honesty

finding `YEW-F-072`

The requested 30-run recomputation cannot be performed on either designated
runner from the committed tree:

| Designated input | State |
|---|---|
| `tests/perf/calib-reference.txt` | absent; only `calib-reference-x86_64.template` exists |
| x86 baseline calibration vector | `scale_permille=0 c1=0 c2=0 c3=0` |
| `tests/perf/calib-reference-arm64.txt` | absent; only an arm64 template exists |
| `tests/perf/baselines/perf-arm64-linux.txt` | absent; only an arm64 template exists |

Both designated workflow jobs are variable-gated self-hosted jobs. Hosted
standard lanes run the harnesses in advisory mode and retain hard sanity
ceilings, but those results cannot establish a designated-runner noise floor.
`YEW-F-072` records the unusable evidence chain rather than inventing a
performance verdict.

## Q4 — baseline drift

finding `YEW-F-073`

`perf-baseline-guard.sh` reads only changed path names. It rejects a commit
that mixes `src/` and baseline changes, but never reads the commit message or
the numeric diff. The isolated reproducer doubles a baseline in a commit
named `Refresh numbers`; the real guard accepts it.

History contains 29 commits that modify an existing file below
`tests/perf/baselines/`. Only three commit messages carry a literal old→new
record. This classification asks only whether the required record exists; it
does not retroactively claim that every unexplained move loosened a budget.

| Commit | Subject | old→new recorded |
|---|---|---|
| `a9bbf35a` | Make hosted Fletch timing advisory | no |
| `804b7850` | Record midline shadow baseline | no |
| `54e933ea` | Enforce calibrated baseline v2 | no |
| `66c18c55` | Gate example plugin latency | no |
| `b88c34af` | Gate Wolf insert-mode backspace latency | no |
| `ba159fc3` | Pace workspace indexing between input frames | no |
| `768aa072` | Stabilize Git diff scheduling gates | no |
| `0e0c5637` | Refresh Git gutter baseline observations | no |
| `0cd5fb3e` | Recalibrate comment edit syntax baseline | no |
| `88103669` | Add cursor latency baseline | no |
| `0ccda889` | Complete Sprint 47 LSP features | no |
| `9e86eb29` | Exercise live LSP performance paths | no |
| `564db9df` | Record symbol index performance baseline | no |
| `7b602f31` | Finish no-LSP symbol completion | no |
| `3d54627f` | Gate native syntax pack performance | no |
| `5e6939d3` | Meet embedded syntax budgets | no |
| `1878d8dd` | Calibrate langpack syntax baselines | no |
| `701ef10a` | Gate Sprint 41 syntax performance | no |
| `666e542e` | Rename editor to yew | no |
| `f6777f50` | Benchmark 200-file macro scans | no |
| `b0efeae7` | Gate Sprint 36 config performance | no |
| `59969789` | Complete the Fletch editor API | no |
| `180bd855` | Fix an ASan global-buffer-overflow in the new pty case | no |
| `7b60e4d5` | Gate search latency; bound the overlay's engine window | no |
| `720f3e7a` | Gate search throughput; fix a DFA cache bug and job output truncation | yes |
| `bb5275e7` | Gate regex linearity and make the engine actually linear | yes |
| `b46c4b3d` | Gate Sprint 19 streaming latency and job torture | no |
| `daa62af9` | perf: rebaseline 1 GiB line lookup | yes |
| `f1ea16ce` | Harden Unicode verification gates | no |

## Q5 — determinism

finding `YEW-F-074`

The hosted determinism job already runs a warmup and two complete `make test`
sweeps, diffs their output, runs and diffs two further PTY sweeps, compares
generated artifacts, and clean-builds the default binary twice. It does not
build the four single-module profiles twice, nor does it compare a same-target
binary built on two independent machines.

Two clean builds of the exact fixed baseline on arm64 macOS produced these
single-module SHA-256 pairs:

| `MODULES` | first | second |
|---|---|---|
| `lsp` | `75b5811bcbde132bcbd9e0fa5105423e130f519fdff9f02d8d9df1ce6289aefb` | `d688c9e9f617d789b272dd4a4e7516d22ede062f3ca95ef85938641c6e7e9bc2` |
| `ai` | `176114c816003fd3eda9dd49d6dff3ae712954a5a16196e84d07c1b1dabb19d6` | `ca4ad5ae12e4de747f43316d570e8be048f149b0fede966ea65649465474f518` |
| `fuss` | `14b0bfa973b170a387c283163262ad080d23042d4df3928c0577a4ad10fe0329` | `0cb9736f263f331e3e5f257dd4ba9c6e27cadee85db0e400395827f75f406871` |
| `plugins` | `e30bda8489c39a9071e5b01407126c3b03a7180a351f4bac60fdc8bc85df7f5d` | `1b746ed05fbb7c8638f788ed252646dfd1165bb13d116b87fe99ee3f915af3af` |

Debug maps first expose changing object mtimes. After stripping at the same
pathname, the remaining Mach-O differences are the generated `LC_UUID` and
its derived ad-hoc signature. Adding `-Wl,-no_uuid` and stripping makes both
`lsp` builds byte-identical at
`99d3e903f772158f0c7903b9cdb98d52a8d762703be492c2e3febc69d14bd031`.
The production link does not disable the UUID, so `YEW-F-074` remains High.

Exact same-target, two-machine hashes were not obtainable from the local
arm64 audit environment. The baseline's green hosted matrix proves four
different targets, not two independent builders of one target; that missing
confirmation is retained as an observation below.

## Q6 — source coverage holes

probed, observations retained

An exact-baseline LLVM source-coverage build ran the complete unit and script
suites and merged their profiles. The script suite passed 93 tests and 931
assertions with one declared skip. All 2,531 unit tests ran and emitted
profiles; instrumentation overhead made the timing-sensitive
`gitcache_blob_batch_reuses_one_child_for_100_requests` assertion fail, so
this run is coverage evidence rather than a green timing verdict.

| Aggregate | Regions | Functions | Lines | Branches |
|---|---:|---:|---:|---:|
| unit + script | 68.97% | 94.22% | 86.45% | 63.56% |

Only two source files containing executable lines recorded zero execution:

| File | Functions | Lines | Classification |
|---|---:|---:|---|
| `src/flcli.c` | 0 / 26 | 0 / 360 | Observation: standalone Fletch CLI frontend; covered behavior below shared APIs, no direct unit/script process test |
| `src/syncli.c` | 0 / 45 | 0 / 1,350 | Observation: standalone syntax CLI frontend; covered behavior below shared APIs, no direct unit/script process test |

Neither file owns a Critical-severity subsystem, so the contract does not
promote either row to a Medium finding. The lowest nonzero line rates were
`shell_cmds.c` 20.50%, `pkg.c` 25.69%, `search_cmds.c` 32.45%, `repl.c`
38.30%, `macrobrowse.c` 43.87%, `loop.c` 45.22%, and `main.c` 45.95%.

## Q7 — `MODULES=""`

finding `YEW-F-075`; cross-check `YEW-F-014`

The clean minimal build contains exactly one shim object per optional module
and no real-only module symbol. Existing command and API matrices prove the
canonical hard-error path for plugin commands, at least 40 FUSS commands,
and all LSP commands except the known `ed.lsp.complete` exception already
filed as `YEW-F-014`. Module-owned CLI dispatch and current documentation do
not advertise a silently retained stripped implementation.

The config-key surface is not honest:

| Option | Minimal-build result |
|---|---|
| `ai.enable` | accepted and stored although AI is absent |
| `lsp.open_in` | accepted and stored although LSP is absent |
| `git.ascii_glyphs` | accepted and stored although FUSS is absent |
| `plug.verify_on_load` | generic `unknown option`, not canonical plugins-module refusal |

One core option table without module ownership is the shared root cause.
Because three user-reachable writes silently become inert state,
`YEW-F-075` is Critical under invariant 3.

## Q8 — `.fl` coverage instrumentation

observation retained; obligation discharged

`make fletch-script-coverage` runs all discovered scripts twice, diffs the
runner output and report, and regenerates `fl-coverage.md`. Coverage-only
`TRACE_LINE` opcodes count nested statements from the existing compiler line
runs; the runner merges per-child records deterministically.

- 93 scripts execute 3,889 statement events and 958 `t.*` calls.
- No discovered script has zero executed statements.
- `t.log` is the only `t.*` surface unused by the passing discovered suite.
  It does execute in the runner's deliberately failing
  `tests/script/meta/assertion_failures.fl` self-test, so this is an
  Observation rather than a missing implementation.
- The ordinary script suite remains green: 93 tests, 931 assertions, zero
  failures, one declared skip.

## Q9 — stale Sprint hard errors

probed, nothing found

The exact repository grep returns 611 source references and is committed
verbatim in Appendix A. Inspection of the built binary reduces user-visible
runtime strings to:

- `option '--batch-strict' lands in Sprint 59`, a still-reserved surface
  explicitly owned by Sprints 37 and 59;
- AI-off guidance naming Sprint 50, and syntax-shadow labels naming Sprints
  44, 47, and 49, all historical attribution for implemented behavior;
- one region invariant crash context naming Sprint 27, an implementation-law
  citation rather than a deferral.

The generic command deferral machinery remains compiled, but no registered
descriptor carries `YEW_CMD_DEFERRED`; the Fletch deferred-name table returns
NULL for every name. No hard-error string falsely claims that a landed surface
is still deferred.

## Supporting evidence

- `make test-audit` retains all 75 findings as hard XFAILs in the default
  profile. The minimal profile runs its 71 applicable audit fixtures and
  reaches the `YEW-F-075` config boundary rather than treating it as
  inapplicable.
- `scripts/bans.sh`, `scripts/check-findings.sh`, and
  `scripts/check-audit-fixtures.sh` pass on the unseeded tree.
- The exact Fletch coverage target passes its built-in two-run determinism
  comparison; the normal script suite and its runner self-tests pass.
- Hosted run `34699266067` is the fixed product baseline's 22-job
  cross-compiler/cross-target record. F15's deliberately failing fixtures are
  audit controls and do not revise that product baseline.

## Unverified observations

1. Same-target binary identity across two independent machines was not
   available locally; current hosted jobs build different target triples.
2. `src/flcli.c` recorded zero direct unit/script execution.
3. `src/syncli.c` recorded zero direct unit/script execution.
4. `t.log` has no passing discovered-suite caller, though the deliberate
   runner-failure self-test executes it.

## Count

Raw 52 · deduped 52 · critical 1 · high 1 · medium 50 · low 0 · unverified 4.

## Appendix A — exact Sprint-reference grep

Command: `grep -rnE 'Sprint [0-9]+' src/ | LC_ALL=C sort`

```text
src/args.c:187:                               "option '%s' lands in Sprint 59", arg);
src/edit/batch.c:127:    /* Sprint 57.13 section 4: `Save As...` is the QUESTION "write to
src/edit/batch.c:154:    /* Sprint 51 registers the complete Git command vocabulary so scripts,
src/edit/batch.c:155:     * completion, and stripped builds agree before the Sprint 52/53 UI
src/edit/batch_test.c:33:/* Sprint 37 pins one VM and one test file per process. */
src/edit/batch_test.h:4:/* Sprint 37: the assertion host installed by yew --batch --test. */
src/edit/bind.h:4:/* Sprint 36: persistent, origin-owned rows above the frozen mode maps. */
src/edit/cmd.c:1131:    /* Sprint 57.13 §4: the FUSS menus' `Copy Path`.  Path-addressed
src/edit/cmd.c:1193:    /* Sprint 57.13 §4: `:saveas` takes NO argument — it opens `:w
src/edit/cmd.c:1210:    /* Sprint 57.13 §4: named after :tabnew / :tabonly, not after the
src/edit/cmd.c:1315:        /* Sprint 21 */
src/edit/cmd.c:1317:        /* Sprint 18.5: the palette itself is Sprint 38's, but the name has
src/edit/cmd.c:1320:        /* Sprint 25 */
src/edit/cmd.c:1322:        /* Sprint 26: the undo branch picker closes s10 §11's deferral. */
src/edit/cmd.c:1324:        /* Sprint 27 §9: the runtime mouse toggle.  The option model that
src/edit/cmd.c:1325:         * PERSISTS it is Sprint 36. */
src/edit/cmd.c:1349:        /* Sprint 18.5 */
src/edit/cmd.c:1351:        /* Sprint 19 */
src/edit/cmd.c:1354:        /* Sprint 21 */
src/edit/cmd.c:1357:        /* Sprint 22 */
src/edit/cmd.c:1360:        /* Sprint 23 */
src/edit/cmd.c:1362:        /* Sprint 24 */
src/edit/cmd.c:1364:        /* Sprint 25 */
src/edit/cmd.c:1366:        /* Sprint 26 */
src/edit/cmd.c:1368:        /* Sprint 27 */
src/edit/cmd.c:1374:        /* Sprint 45: the LSP module surface is registered before its
src/edit/cmd.c:1379:        /* Sprint 48: the AI command boundary is discoverable even when the
src/edit/cmd.c:1382:        /* Sprint 50: explicit privacy and preset surfaces. */
src/edit/cmd.c:1384:        /* Sprint 51: the complete Git command surface is registered before
src/edit/cmd.c:1385:         * the Sprint 52/53 viewers and mutating actions land. */
src/edit/cmd.c:1392:        /* Sprint 56: in-loop profiler report and raw data surfaces. */
src/edit/cmd.c:1394:        /* Sprint 57.10: row-1 numbered jump from inside a group. */
src/edit/cmd.c:1396:        /* Sprint 57.13 §4: the context-menu row commands. */
src/edit/cmd.c:1398:        /* Sprint 57.13 Deliverable 4: the four rows that had no command
src/edit/cmd.c:1464: * Sprint 34 §3: the CMDWORD rules, enforced where a command is BORN.
src/edit/cmd.c:1466: * Sprint 35's round-trip law says a recorded macro and a hand-typed
src/edit/cmd.c:1902:    /* Sprint 48's off-by-default notice precedes even an AI command's
src/edit/cmd.c:52:    /* Sprint 34 §8: CMDWORD -> CmdId, built as commands register.  A
src/edit/cmd.c:604:    /* Sprint 57.13 §4: the footer menu's line-number row. */
src/edit/cmd.c:639:    /* Sprint 18.5 §10.  complete_next/prev stay as the names the keymap
src/edit/cmd.c:683:    /* Sprint 57.13 §4: the document menu's `Save As...`.  It PROMPTS —
src/edit/cmd.c:719:    /* Sprint 27 §5: the tab context menu's rows.  Commands, because
src/edit/cmd.c:725:    /* Sprint 57.13 §4: the tab menu's two "open in split" rows. */
src/edit/cmd.c:732:    /* Sprint 24 §6: the continuous line.  next/prev walk EVERY open
src/edit/cmd.c:749:    /* Sprint 27 §5/§9. */
src/edit/cmd.c:751:     * Sprint 57.13 §5 widens the arity: iarg 1 names the TAB STRIP
src/edit/cmd.c:762:    /* Sprint 27 §8: the keyboard twin of dropping a tab into a group. */
src/edit/cmd.c:772:     * Sprint 57.13 §4: the group picker's own two rows.
src/edit/cmd.c:789:    /* Sprint 25 §9: workspace state. */
src/edit/cmd.c:800:    /* Sprint 26 §6: the three instances. */
src/edit/cmd.c:961:     * Sprint 57.13 §4: the rename confirmation's three answers.
src/edit/cmd.h:144:     * Sprint 34 §3: the motion-space CMDWORD -- "yank", "del_line" --
src/edit/cmd.h:148:     * YEW_CMD_RECORDABLE is set.  That bijection is what Sprint 35's
src/edit/cmd.h:163: * every earlier static descriptor initializer to grow Sprint 18 fields. */
src/edit/cmd.h:187: * Sprint 34 §8: the motion-space word -> command map, built at
src/edit/completion.c:3:/* Compatibility translation unit.  Sprint 44's implementation lives in
src/edit/ed.c:1007: * Sprint 23 could say `ed->buffer` and mean it, because every tab
src/edit/ed.c:1008: * cloned a view of that single buffer.  Sprint 24 gives each tab its
src/edit/ed.c:1089: * that has been nowhere has no history (Sprint 21 §5).
src/edit/ed.c:1098:     * Sprint 23 also kept a write-only registry of every cloned Win and
src/edit/ed.c:1891:     * Sprint 24 §3: a tab that was never read holds no text, so there
src/edit/ed.c:2210:     * on Escape.  Sprint 27 widened this from the border drag alone to
src/edit/ed.c:2218:     * Sprint 27 §5: the context menu is a keymap layer, and it is the
src/edit/ed.c:2235:     * Sprint 24 §4: the picker is MODAL, so it takes the key before the
src/edit/ed.c:2245:     * Sprint 26 §5: the list picker is modal for the same reason the
src/edit/ed.c:2254:     * Sprint 47: panels are transient, not modal.  Scroll keys belong to
src/edit/ed.c:2272:     * Sprint 24 §7.  BEFORE the insert-mode text path: a digit arriving
src/edit/ed.c:2312: * The mouse routing spine used to live here.  Sprint 18.5 joined the
src/edit/ed.c:2442: * Sprint 57.13 §3: is this frame ONLY an open menu's highlight moving?
src/edit/ed.c:2543:         * which is what keeps the Sprint 6-21 goldens byte-identical.
src/edit/ed.c:2581:     * Sprint 27 §5: the context menu is drawn after everything, for the
src/edit/ed.c:2589:        /* Sprint 44: the non-modal completion popup is the final overlay
src/edit/ed.c:2594:        /* Sprint 47: the floating panel is the final overlay.  Its draw
src/edit/ed.c:368: * Sprint 24 §3: a file buffer that has NOT been read.
src/edit/ed.c:462:     * Sprint 25 §6: restored marks become real here, at the first
src/edit/ed.h:151:    /* Sprint 25 §5.  Zeroed = stateless; the driver opts in with
src/edit/ed.h:160:     * Sprint 22: the pane tree.  `focus` is a LEAF pointer, revalidated
src/edit/ed.h:161:     * after any tree mutation.  Sprint 23 gives each tab its own root.
src/edit/ed.h:171:    /* Sprint 27 §1: the router's gesture state.  One per editor: a
src/edit/ed.h:213:    /* Sprint 48: module-owned AI transport state.  The pointer keeps the
src/edit/ed.h:217:    /* Sprint 51: repository detection, porcelain snapshots and Git jobs.
src/edit/ed.h:221:    /* Sprint 52: F mode owns its tree, path selection and transient UI. */
src/edit/ed.h:223:    /* Sprint 54: discovery, lifecycle, grants and picker state remain
src/edit/ed.h:245:     * Sprint 57.13 §3: the MENU MOVED AND NOTHING ELSE DID.
src/edit/ed.h:274:     * Sprint 34: the origin registry (§2).  A value member rather than
src/edit/ed.h:280:    /* Sprint 34 §1: every editor handle a script holds. */
src/edit/ed.h:286:    /* Sprint 38: host-owned, reload-safe named macro registry. */
src/edit/ed.h:289:    /* Sprint 35: resolved-command macro recorder. */
src/edit/ed.h:311:    /* Sprint 37: model/runtime without terminal, input, grid, or loop. */
src/edit/ed.h:321:    /* Sprint 56: lifecycle RSS rows and the exact 10k-key boundary. */
src/edit/ed.h:363:/* Scratch buffers (Sprint 19: job output and the *jobs* table).  The
src/edit/ed.h:370: * The same contract for windows (Sprint 34 §1).  Searches every tab's
src/edit/ed.h:385: * Sprint 24 §3: deferred file buffers.
src/edit/ed.h:401:/* Sprint 22 pane plumbing.  A clone shares the BUFFER and copies the
src/edit/ed.h:402: * view (cursor, viewport); Sprint 21's jumplist is per window and so
src/edit/ed.h:431: * no editor-level twin: Sprint 27 DoD 2 is that the router is the only
src/edit/ed.h:49:    /* Sprint 25 §9: ed.ws.forget.  Its own kind because every other
src/edit/edit_cmds.c:508: * Sprint 57.13 §4: the footer menu's `Line Numbers: <style>` row, and
src/edit/file_cmds.c:85: * Sprint 57.13 §4: the document menu's `Save As...` row.
src/edit/file_cmds.h:11:/* Sprint 57.13 §4: opens the E-mode line seeded `w <current path>`. */
src/edit/flapi_cmds.h:4:/* Registry commands required by Sprint 34's Fletch editor bindings. */
src/edit/job.c:1:/* Sprint 19: child processes, nonblocking and budgeted.
src/edit/job.h:5: * Sprint 19: the generic child-process layer.  Jobs are the editor's only
src/edit/jumplist.c:2: * Sprint 21 §5.  See jumplist.h for the ownership split and why
src/edit/jumplist.c:343: * Sprint 25's schema, landed and uncalled.  DoD 11 greps this file for
src/edit/jumplist.c:400:        /* Path field: kept for Sprint 25, which reopens by it. */
src/edit/jumplist.h:19: * invalidated it.  A mark rides the Sprint 9 choke point for free.
src/edit/jumplist.h:21: * Sprint 25 persists both; this sprint lands the serializer and calls
src/edit/jumplist.h:5: * Sprint 21 §5: navigation history.
src/edit/jumplist.h:85:/* Fed from the Sprint 9 op stream, not from a parallel notification
src/edit/jumplist.h:97: * Sprint 25's schema shape, landed now and called by nobody.  Marks are
src/edit/loop.c:294:     * Sprint 26 §7.2: a sliced rescan is work waiting to be done, so
src/edit/loop.c:330:    /* Sprint 27 §4: the dwell and the drag auto-scroll are CLOCKS.  A
src/edit/loop.c:428:     * core reads one itself (invariant 5) — and Sprint 27's dwell is
src/edit/loop.c:450:         * Sprint 27 §1: focus-out cancels any gesture.  A drag whose
src/edit/loop.c:662:         * Sprint 26 §7.2: a sliced rescan continues here, on the idle
src/edit/lsp_cmds.h:22:/* Sprint 57.13 §4: the rename confirmation's three answers, which the
src/edit/opt.h:4:/* Compatibility include for the Sprint 34 filename. */
src/edit/option.h:4:/* Sprint 36: the single typed option table shared by Fletch and E mode. */
src/edit/option.h:68:/* The Sprint 34 provider remains a narrow compatibility/test seam. */
src/edit/pane_cmds.c:295:/* Border drag (Sprint 22 §5)                                       */
src/edit/pane_cmds.c:2: * Sprint 22 §4/§5: the pane commands.
src/edit/pane_cmds.c:4: * Every one of these dispatches through the Sprint 13 registry; the
src/edit/pane_cmds.c:57:     * Sprint 25 serialized that as "pane 0 is focused" no matter which
src/edit/pane_cmds.h:25: * release, with Esc restoring the entry ratio.  Sprint 27 re-routes it
src/edit/search_cmds.c:2: * Sprint 21 §4: the `:s` surface.
src/edit/search_cmds.c:451:/* Named marks (Sprint 21 §7, closing the Sprint 18 deferral)       */
src/edit/search_cmds.c:4: * Sprint 18's grammar hands the whole substitution body over as ONE
src/edit/sel_actions.c:415: * Sprint 57.13 §4: the `Cut` menu row, and H-mode `x`.
src/edit/sel_actions.c:501: * Sprint 57.13 §4: the `Select All` menu row, and L-mode `g a`.
src/edit/shadow.c:897:        "index — (Sprint 44)",
src/edit/shadow.c:898:        "lsp — (Sprint 47)",
src/edit/shadow.c:899:        "ai — (Sprint 49)",
src/edit/shell.c:145:    (void)is_err; /* stderr attr spans arrive with the theme in Sprint 41 */
src/edit/shell.c:1:/* Sprint 19: the three consumption modes and the *jobs* table. */
src/edit/shell.h:33: * NOT_HANDLED so Sprint 19's ordinary $SHELL -c path keeps ownership. */
src/edit/shell.h:5: * Sprint 19: E mode's shell surface — the three consumption modes over
src/edit/shell_cmds.c:1:/* Sprint 19: the E-mode command surface over the shell layer.  Every
src/edit/ws_cmds.c:169:     * reserved command, not unfinished Sprint 25 work: there is no newer
src/edit/ws_cmds.c:2: * Sprint 25 §9: the workspace-state commands.
src/edit/ws_cmds.h:5: * Sprint 25 §9: the workspace-state commands.
src/fl/ast.h:111: * compiler in Sprint 30 pays for on every walk.
src/fl/ast.h:5: * Sprint 29 deliverable 2: the Fletch AST.
src/fl/compile.c:1006:     * VM dereferenced NULL.  Sprint 33's §7 file segfaulted on it.
src/fl/compile.c:122:    /* Keyed by INSTRUCTION START: Sprint 32's traces must name the
src/fl/compile.c:1:/* Sprint 30 deliverable 6: AST -> bytecode, one pass. */
src/fl/compile.c:959:     * into it dangles -- the same pitfall Sprint 29 pinned for AST
src/fl/compile.h:10: * Compile errors go through DiagCtx with Sprint 29's caret rendering
src/fl/compile.h:5: * Sprint 30 deliverable 6: the single-pass AST-to-bytecode compiler.
src/fl/compile.h:64:/* Sprint 58 F15: emit the existing TRACE_LINE marker for every statement,
src/fl/compile.h:7: * Consumes an AST that already parsed clean -- Sprint 29 guarantees
src/fl/diag.c:1:/* Sprint 29: caret diagnostics for Fletch.  See diag.h for why the
src/fl/diag.h:15: * inside the one directory Sprint 29's DoD 1 permits it to touch.
src/fl/diag.h:57: * Sprint 29's testing strategy requires the former explicitly: message
src/fl/diag.h:5: * Sprint 29: source spans and caret diagnostics for Fletch.
src/fl/diag.h:9: * Sprint 29's prerequisites expect a `DiagCtx` from Sprint 0, whose entry
src/fl/dump.c:2: * Sprint 29 deliverable 5: the AST dumper.
src/fl/dump.c:6: * parser's only output until Sprint 30, so it is the substrate for
src/fl/flapi.h:4:/* Sprint 34: the table-driven editor surface. */
src/fl/flconf.h:49:/* Sprint 54 plugin policy uses the same loaded, atomic trust database as
src/fl/flconf.h:4:/* Sprint 36: ordered, origin-owned editor configuration. */
src/fl/flhook.h:37:/* Sprint 36 produces the other three kinds; defining them here keeps one
src/fl/flhook.h:4:/* Sprint 34: deterministic, contained Fletch hook dispatch. */
src/fl/flruntime.h:42:/* Sprint 58 audit variant: marks statements at every nesting depth. */
src/fl/flruntime.h:4:/* Persistent editor embedding and the Sprint 34 hook dispatch bridge. */
src/fl/flruntime.h:60:/* Sprint 35's bounded register cache.  Registers are lower-case a..z.  A hit
src/fl/gc.c:215:    /* 5. modules -- Sprint 31 fills this; rooted now */
src/fl/gc.c:2: * Sprint 30 deliverable 9: the mark-sweep collector.
src/fl/gc.c:582:     * Bytes VERBATIM.  Sprint 2's escape policy cleans buffer text;
src/fl/gc.c:8:/* clock_gettime, for the collection-pause figures Sprint 33's bench
src/fl/gc.c:9: * suite reports and Sprint 56 gates on. */
src/fl/gc.h:20: *   3. Host callbacks (Sprint 34) take handles by table SLOT, never by
src/fl/gc.h:5: * Sprint 30 deliverable 9: a precise mark-sweep collector with an
src/fl/gc.h:72:     * Recorded because Sprint 33's bench suite reports it and Sprint 56
src/fl/gc.h:94:/* Floor for next_gc.  Sprint 33 gates config load under 1 ms, and a
src/fl/handle.c:2: * Sprint 34 deliverable 1: the generation-checked handle table.
src/fl/handle.h:5: * Sprint 34 deliverable 1: the five editor handle types, spec §4.
src/fl/lex.c:1:/* Sprint 29: the Fletch lexer.  Spec §1 in full, §1.6's motion space
src/fl/lex.c:695:         * `incomplete` and the Sprint 32 REPL asks for another line.
src/fl/lex.c:911:             * Sprint 2's decoder never fails: an invalid byte comes back
src/fl/lex.h:5: * Sprint 29: the Fletch lexer.  Implements spec §1 in full, including
src/fl/lex.h:60:     * parser's path does.  Sprint 33's conformance runner reads its
src/fl/lex.h:77:/* Spec §15.1 pins the count, and Sprint 33 asserts it. */
src/fl/macrolib.h:4:/* Sprint 38: deterministic, reloadable Fletch macro library. */
src/fl/module.c:2: * Sprint 31 deliverable 9: `import`.
src/fl/module.c:585:             * a literal, and Sprint 34's `buf` made it wrong the moment
src/fl/module.h:5: * Sprint 31 deliverable 9: `import`.
src/fl/opcodes.c:1:/* Sprint 30 deliverable 5: the tables and the disassembler, both
src/fl/opcodes.def:21: * frames Sprint 32's stack traces are made of.  0xF0..0xFF is left
src/fl/opcodes.def:2: * Sprint 30 deliverable 5: the Fletch instruction set, once.
src/fl/opcodes.h:53: * surface exactly as fl_ast_dump was Sprint 29's.
src/fl/opcodes.h:5: * Sprint 30 deliverable 5: everything derived from opcodes.def.
src/fl/origin.c:2: * Sprint 34 deliverable 2: the origin registry and the §13 capability
src/fl/origin.c:62:     * loaded.  Sprint 54 hard-codes the number; making it depend on
src/fl/origin.h:126:     * (§7): Sprint 36's config reload and Sprint 54's plugin disable
src/fl/origin.h:28: *     Id 0 is reserved for the user config -- Sprint 54's capability
src/fl/origin.h:32: * Sprint 31 declared FlOriginKind and FlOrigin in value.h for the
src/fl/origin.h:53: * actionable.  Collapsing them into one module index, as Sprint 30's
src/fl/origin.h:5: * Sprint 34 deliverable 2: origins and capabilities, spec §13.
src/fl/origin.h:68: * in 1.0 -- they exist so Sprint 54 can prompt for them. */
src/fl/origin.h:97:/* The user config.  Sprint 54 depends on this number. */
src/fl/parse.c:1:/* Sprint 29: the Fletch parser.  Spec §2's grammar, §1.2's terminator
src/fl/parse.c:213: * a continuation prompt (spec §1.2's rule, shared with Sprint 32).
src/fl/parse.c:49: * Spec §1.2 makes this rule shared with the Sprint 32 REPL's
src/fl/parse.h:10: * arity here: Sprint 30 compiles, Sprint 31 resolves stdlib names.
src/fl/parse.h:5: * Sprint 29 deliverables 3-5: the Fletch parser, its diagnostics, and
src/fl/repl.c:1:/* Sprint 32 §2-§5: the interactive `yew fl` prompt. */
src/fl/repl.c:37: * belongs to Sprint 19's command registry, and scripts/check-cmd-
src/fl/repl.c:398: * Sprint 19 registry -- and every editing key is a YEW_MODE_E binding
src/fl/repl.h:34: * Sprint 32 §3, and three pitfalls live in this one decision:
src/fl/repl.h:5: * Sprint 32 §2-§5: the interactive `yew fl` prompt.
src/fl/std.c:2: * Sprint 31 deliverable 1: native registration, argument helpers, the
src/fl/std.c:396: * fl/origin.c in Sprint 34, which owns the origin registry they read. */
src/fl/std.c:46:    /* Sprint 34: the editor API.  Registered unconditionally -- a
src/fl/std.h:145: * caller-set depth cap (Sprint 32 §5 pins 8).  A cyclic value elides to
src/fl/std.h:161:/* The capability check (spec §13) moved to fl/origin.h in Sprint 34,
src/fl/std.h:5: * Sprint 31 deliverable 1: native registration, the shared argument
src/fl/stdai.c:1:/* Sprint 48: the Fletch front door for owned AI backend definitions. */
src/fl/stdfmt.c:2: * Sprint 31 deliverable 6: the `fmt` module -- a bespoke formatter.
src/fl/stdfmt.c:44: * parent's indent.  Sprint 36 swaps this in under s25's hand-written
src/fl/stdio.c:290:     * ATOMIC, through Sprint 8's own primitive: temp file in the same
src/fl/stdio.c:2: * Sprint 31 deliverable 7: the `io` module.
src/fl/stdio.c:520: * From Sprint 34 the host redirects both streams to the message line
src/fl/stdlist.c:2: * Sprint 31 deliverable 3: the `list` module.
src/fl/stdmap.c:127:     * a three-entry map left one behind.  Sprint 33's suite caught it.
src/fl/stdmap.c:2: * Sprint 31 deliverable 4: the `map` module.
src/fl/stdmath.c:2: * Sprint 31 deliverable 5: the `math` module.
src/fl/stdre.c:2: * Sprint 31 deliverable 8: the `re` module, over Sprint 20's engine.
src/fl/stdre.c:5: * an editor handle constructible only from Sprint 34, and this sprint
src/fl/stdre.c:7: * performance without touching the frozen type set, and when Sprint 34
src/fl/stdstr.c:2: * Sprint 31 deliverable 2: the `str` module.
src/fl/stdstr.c:903:     * strtod, as the Sprint 29 lexer already uses for float literals --
src/fl/suggest.c:1:/* Sprint 32 §7: "did you mean".  The contract is in suggest.h. */
src/fl/suggest.h:5: * Sprint 32 §7: "did you mean".
src/fl/suggest.h:76: * byte-lexicographic through yew_sort_stable.  Sprint 33's determinism
src/fl/trace.c:1:/* Sprint 32 §6: stack traces.  The contract is in trace.h. */
src/fl/trace.h:5: * Sprint 32 §6: runtime error quality.
src/fl/value.c:26:     * Sprint 34 starts handing them out, and until then these entries
src/fl/value.c:270: * survives.  Sprint 31's map.clear did exactly that and left
src/fl/value.c:271: * floor(n/2) entries behind for n >= 3; the Sprint 33 conformance
src/fl/value.c:2: * Sprint 30 deliverables 1-4 and 10: values, strings, lists, and the
src/fl/value.c:77:     * Sprint 34's handles are scalars, not objects: the payload is a
src/fl/value.h:170: * Sprint 34, which owns the registry built on top of them.  They are
src/fl/value.h:176: * What a stack trace calls this function (Sprint 32 §6).
src/fl/value.h:5: * Sprint 30 deliverables 1-4: the Fletch value representation.
src/fl/value.h:60: * keep the struct at 16 bytes either way and give Sprint 34 somewhere to
src/fl/value.h:97: * Bytes are stored VERBATIM, U+DC80..DCFF escapes included.  Sprint 2's
src/fl/vm.c:1262:                 * exist and every string raised; Sprint 33's suite
src/fl/vm.c:1305:             * arm refused every list until Sprint 33's conformance
src/fl/vm.c:1433:             * Sprint 30 could get away with leaving the frame in place,
src/fl/vm.c:1609:         * many runs.  Sprint 32's VM fuzzer asserts all four on every
src/fl/vm.c:1623: * replacement, and Sprint 34's editor hooks.
src/fl/vm.c:166: * Builds the error map {kind, msg} and leaves it in vm->err.  Sprint 32
src/fl/vm.c:213:    /* All formerly deferred prelude names through Sprint 36 are live.
src/fl/vm.c:224: * variable", which assumes globals resolve statically; Sprint 30 makes
src/fl/vm.c:2: * Sprint 30 deliverable 7: the Fletch VM.
src/fl/vm.c:305:/* Sprint 32 §9: internal invariants                                */
src/fl/vm.h:197:    u64 step_limit;              /* 0 = unlimited; Sprint 32 uses it      */
src/fl/vm.h:256:     * Sprint 34: the editor this VM drives, or NULL for a headless run.
src/fl/vm.h:313: * THE ONE TIMEOUT MECHANISM.  Sprint 32's fuzzer uses it so a
src/fl/vm.h:314: * pathological input cannot hang the campaign, and Sprint 54's plugin
src/fl/vm.h:322: * Sprint 32 §8: THERE IS NO BYTECODE VERIFIER, AND THAT IS A DECISION.
src/fl/vm.h:334: * 02-fletch.md req 7 wants ~1 us motion dispatch, and Sprint 30 already
src/fl/vm.h:4:/* Sprint 30 deliverables 7 and 11: the VM state and the host seam. */
src/fl/vm.h:64: * Root 6.  Sprint 30 reserved it for "Sprint 34's handle table" and
src/fl/vm.h:65: * Sprint 34 found it needed for the opposite thing.
src/fl/vmcheck.c:2: * Sprint 32 §8: fl_chunk_check -- a COMPILER ASSERTION, not a security
src/fl/vmcheck.c:68: * Sprint 30 checks `max_stack` ONCE PER CALL and then lets every push
src/flcli.c:10: * returns 2 -- that code belongs to Sprint 37's `--batch`:
src/flcli.c:166: * Sprint 33 conformance suite has to drive the capability-denial path
src/flcli.c:25: * `yew fl FILE` TAKES NO SCRIPT ARGUMENTS this campaign; Sprint 37's
src/flcli.c:2: * Sprint 32 §1: `yew fl` -- the Fletch entry point.
src/flcli.c:565: * Sprint 32 §9's exit-4 row, driven for real rather than described.
src/flcli.c:601:     * the script, and scripts take no arguments until Sprint 37.
src/flcli.h:8: * YEW_EXIT_* code.  Sprint 32 documents --list-natives; Sprint 37 adds
src/mod/ai/ai.c:220:/* Ownership root for Sprint 48 and the following AI sprints.  Concrete
src/mod/ai/ai.c:541:            "AI is off; :ai enable turns it on (Sprint 50)");
src/mod/ai/ai.h:18:/* Sprint 48's editor-facing module boundary.  The stripped-module shim
src/mod/ai/ai.h:79:/* Sprint 49's passive completion provider.  Registration is process-global;
src/mod/ai/backend_curl.c:1:/* Sprint 48: deterministic curl --config transport material. */
src/mod/ai/backend_curl.h:19:    /* Zero selects the pinned Sprint 48 defaults.  Nonzero values come
src/mod/ai/context.c:388:    /* Sprint 50 owns the patterns and block/elide policy.  This remains the
src/mod/ai/context.h:41:/* A standard-C hook keeps Sprint 49's safe default and lets Sprint 50 own
src/mod/ai/context.h:4:/* Sprint 49: the single-buffer, byte-budgeted context boundary.  Multi-file
src/mod/ai/redact.h:5: * Sprint 50's privacy boundary is deliberately conservative.  A cloud
src/mod/ai/registry.c:1:/* Sprint 48: owned, insertion-ordered runtime backend registry. */
src/mod/ai/shadow_ai.c:862:    /* Sprint 43 deliberately advances seq_min on every edit.  A typed
src/mod/git/fussmode.c:1057:        /* F mode owns a deterministic loading frame even when Sprint 53's
src/mod/git/fussmode.c:1696:/* Sprint 57.13 §3: the mouse router's three seams                  */
src/mod/git/fussmode.c:5019:     * through Sprint 12's OSC 52 path — the same register and the same
src/mod/git/fussmode.h:179: * Sprint 57.13 §4: the FUSS menus' `Copy Path` row.
src/mod/git/fussmode.h:64: * Sprint 57.13 §3: the three seams the mouse router needs and only it.
src/mod/git/fussmode.h:80: * Sprint 57.13 §4: everything a FUSS row's menu turns on, in one
src/mod/git/shim.c:360:     * and no caller ever reads the zeroes (Sprint 57.13 §4). */
src/mod/lsp/diag.h:27:    /* Above Sprint 46's 10k render gate, but bounded against hostile or
src/mod/lsp/features.c:2: * Sprint 47 snippet downgrade policy.
src/mod/lsp/lsp.h:16:/* Sprint 45's editor-facing module boundary.  The disabled-module shim
src/mod/lsp/lsp.h:39: * Sprint 57.13 Deliverable 4: the rename confirmation's three answers,
src/mod/lsp/lsp.h:75: * Sprint 57.13 §4: is a server attached to THIS buffer?
src/mod/lsp/rename.c:1342: * keystroke has meant both since Sprint 45.
src/mod/lsp/rename.h:79: * Deterministic unit-test seam for the CONFIRM PHASE (Sprint 57.13
src/mod/lsp/shim.c:150:     * PANEL menu keeps its bare `Close` row (Sprint 57.13 §4). */
src/mod/lsp/shim.c:168:     * whole rather than shown greyed (Sprint 57.13 §4). */
src/mod/plug/manifest.c:83:     * part of Sprint 54's frozen plugin manifest contract. */
src/search/compile.c:1:/* Sprint 20 §5: AST -> Thompson program, plus the reverse program. */
src/search/compile.c:254:/* Minimum codepoints any match consumes.  Sprint 21 uses it to skip
src/search/dfa.c:2: * Sprint 20 §6b: the lazy DFA.
src/search/literal.c:1:/* Sprint 20 §7: the literal prefilter. */
src/search/overlay.c:278:     * the Sprint 20 dispatcher hit.  yew_re_search keeps the window at
src/search/overlay.c:2: * Sprint 21 §3.  See overlay.h for why the scan is bounded twice.
src/search/overlay.h:17: * nothing else — because Sprint 47's LSP document-highlight reuses this
src/search/overlay.h:5: * Sprint 21 §3: viewport-scoped match highlighting.
src/search/parse.c:1054:         * Sprint 21 §2: \c and \C are case directives, not matchable
src/search/parse.c:1:/* Sprint 20 §2/§8: pattern text -> AST, with precise error offsets. */
src/search/parse.c:273: * be visible in Sprint 21's per-keystroke recompile.
src/search/parse.c:440: * place to answer Sprint 21's smartcase question: `\Wfoo` has no
src/search/pike.c:2: * Sprint 20 §6a: the Pike VM.
src/search/regex.c:2: * Sprint 21 §6: the one implementation of "escape this literal".
src/search/regex.c:5: * reuses register `/`, and Sprint 26's finder will want the same thing.
src/search/regex.h:10: * implementation detail: Sprint 21's incremental search recompiles and
src/search/regex.h:131: * Sprint 21 §6.  Appends `s` to `out`, escaped so that compiling the
src/search/regex.h:143: * Sprint 21 §2 inputs to the smartcase table.  "Uppercase literal" is a
src/search/regex.h:23: * Worth knowing: \w and Sprint 16's W-mode word motion deliberately
src/search/regex.h:5: * Sprint 20: the bespoke regex engine.
src/search/regex.h:67:/* `off` is a byte offset into the pattern so Sprint 18's prompt can point
src/search/regex_internal.h:152:     * Sprint 21 §2.  Smartcase asks "did the user type an uppercase
src/search/regex_internal.h:215: * search \b and Sprint 16's W-mode word motion agree about what a word
src/search/replace.c:2: * Sprint 21 §4.  See replace.h for the plan/apply split and why it
src/search/replace.c:359:     * `:5,10s/\A/x/` would fire at line 5 — the same trap Sprint 20's
src/search/replace.c:442:     * cursors still ride the Sprint 9 choke point either way; this
src/search/replace.c:46:     * Sprint 20 §3.
src/search/replace.h:5: * Sprint 21 §4: replacement templates and the substitute driver.
src/search/search.c:2: * Sprint 20 §6: the search entry points.
src/search/search.c:46: * known-small span (Sprint 21 expands replacement templates this way).
src/search/searchui.c:2: * Sprint 21 §1/§2.  See searchui.h for what this layer owns.
src/search/searchui.c:69:     * and Sprint 20's compiler is microseconds on a prompt-sized
src/search/searchui.c:711: * The word under the cursor, via Sprint 16's UAX #29 boundary oracle:
src/search/searchui.h:134: * statusline's wrap indicator.  Both run on the Sprint 15 timer heap
src/search/searchui.h:23: * The four search options.  Sprint 36 owns the option TABLE (`:set`,
src/search/searchui.h:5: * Sprint 21 §1/§2: the search surface over Sprint 20's engine.
src/syncli.c:346:     * for fallback styling; Sprint 41.5 does not ship a GraphQL definition. */
src/term/input.c:367:        /* Sprint 57.13: motion with NO button held.  A terminal only
src/term/input.c:41: * Sprint 27 §8: the mouse sequences are SEPARATE, so YEW_MOUSE=0 can
src/term/input.h:143: * motion with NO button held (SGR base 35), which Sprint 57.13 decodes.
src/term/input.h:38:/* Append-only: these ordinals become serialized data in Sprint 25. */
src/term/render.c:792:    /* Sprint 57 positive control: compiled only into perf_alloc_seed. */
src/term/tty.c:46:/* Sprint 57.13: mode 1003 (any-motion tracking) is armed only while a
src/term/tty.h:126: * Sprint 57.13: any-motion tracking (DEC mode 1003) is armed only while a
src/text/edit.h:19:     * Sprint 21 §5: the changelist is another consumer of this one op
src/text/edit.h:32:    /* Sprint 43: direct consumers of the fixed edit notification list. */
src/text/file.c:314: * Sprint 24 D4: how many times the editor has actually read a file.
src/text/file.h:101:/* Sprint 24 D4 test hook: file reads performed so far.  Counting is the
src/text/file.h:127: * Sprint 25 §7 sets an unreadable state file aside under a
src/text/mark.h:41: * and aborts, which is right for code that owns its marks; Sprint 21's
src/text/register.c:239:     * short list.  Sprint 21 opened `/` via yew_reg_set_search; the rest
src/text/register.c:278:/* Sprint 21 §6: the last accepted search pattern.  `:s//` and `^R /`
src/ui/cmdcomp.c:1125:/* Sprint 18.5 §4: the live filter                                  */
src/ui/cmdcomp.c:628: * Sprint 18.5 §4 / DoD 10: the cached directory listing.
src/ui/cmdcomp.c:674:     * exactly as Sprint 26 §7.2 does for the picker.  `dir` stays open
src/ui/cmdcomp.c:79: * Sprint 18.5 §2: one candidate's rank key, the single-item form of
src/ui/cmdcomp.c:960:         * directory this size is Sprint 26's problem, not the prompt's. */
src/ui/cmdcomp.c:987: * Sprint 36 fills these in.  An empty provider is DATA, not a stub: it
src/ui/cmdcomp.h:145:    const char *name; /* stable id, for logs and (Sprint 34) Fletch */
src/ui/cmdcomp.h:158: * Sprint 18.5 §4: the live filter's cached candidate set.
src/ui/cmdcomp.h:22:     * Sprint 18.5 §4 / DoD 10: how many directory entries the path source
src/ui/cmdcomp.h:252:/* Quote one completion so the Sprint 18 tokenizer reads one argv element. */
src/ui/cmdcomp.h:31:     * unbounded memory, and Sprint 26's finder owns that case with its own
src/ui/cmdcomp.h:86: * Sprint 18.5 §3: what a source is asked for.  Passing a struct rather
src/ui/cmdcomp.h:95:     * Sprint 32 §2: whatever is driving this completion when it is not
src/ui/cmdhist.h:20: * Sprint 25 §8: per-workspace history, closing s18's deferral.
src/ui/cmdline.c:1115: * Sprint 18.5 §10.  Every menu behaviour is a registered command, so it
src/ui/cmdline.c:1116: * is rebindable, recordable, and reachable from Fletch (Sprint 34)
src/ui/cmdline.c:1170: * Sprint 18.5 §8: what a click on a menu row does.
src/ui/cmdline.c:1325: * Sprint 18.5 §7: the part of a candidate that has not been typed yet.
src/ui/cmdline.c:1334: * never typed, and make yew_cmdline_text() -- which Sprint 21's search
src/ui/cmdline.c:1477:     * Sprint 18's rule -- Enter with a menu open accepts instead of
src/ui/cmdline.c:380:    /* Sprint 25 §8.  A stateless session (--clean, --batch, an unusable
src/ui/cmdline.c:475:         * geometry Sprint 18's goldens pinned, now expressed as a spec
src/ui/cmdline.c:476:         * so Sprint 26's picker can pick a different one. */
src/ui/cmdline.c:616: * Sprint 18.5 §6: refilter for the prompt's current text.
src/ui/cmdline.c:626: * a word being typed.  That is Sprint 21's doctrine -- a half-typed line
src/ui/cmdline.c:630: * Sprint 18.5 §9: what the parser already understands, said out loud.
src/ui/cmdline.c:636: * behaviour Sprint 21's doctrine forbids.
src/ui/cmdline.c:809: * Sprint 26 §7.2's idle-path pattern, applied to the completion scan.
src/ui/cmdline.c:863:/* The prompt's current text.  Sprint 21's search-as-you-type needs it
src/ui/cmdline.c:902:         * §6 inverts Sprint 18's rule: a printable key REFILTERS rather
src/ui/cmdparse.c:431:        /* Sprint 21 §7: `'a` resolves through the buffer's 26-slot name
src/ui/cmdparse.c:449:         * Sprint 21: `/pat/` and `?pat?` address the next (previous)
src/ui/cmdparse.c:451:         * engine is Sprint 20's; no pattern logic lives here.
src/ui/cmdparse.h:36:    /* What the leading range resolved to, for Sprint 18.5 §9's hint.
src/ui/complmenu.h:109:/* Sprint 43 compatibility: these two calls retain the arbitration-only
src/ui/ctxmenu.c:2: * Sprint 27 §5, Sprint 57.13 §2.  See ctxmenu.h for the capture-at-open
src/ui/ctxmenu.c:335:     * it describes, matching the original Sprint 27 placement. */
src/ui/ctxmenu.c:590:     * cells the row was drawn with (the Sprint 22 law) — the border
src/ui/ctxmenu.h:169: * A row's label, its action and whether it is a rule.  Sprint 57.13 §4
src/ui/ctxmenu.h:5: * Sprint 27 §5, reshaped by Sprint 57.13 §2: context menus.
src/ui/ctxmenu.h:79: * roles (Sprint 57.13 §6) — this module knows no colours of its own.
src/ui/ctxrows.c:21: *   A ROW is GREYED, NEVER HIDDEN (Sprint 27).  `Stage` with nothing to
src/ui/ctxrows.c:33: * sprint uses the typographic glyph, Sprint 27's shipped rows spell it `...`.
src/ui/ctxrows.c:4: * Sprint 57.13 §4: one builder per context kind.
src/ui/ctxrows.c:63: * Sprint 27 spent a switch per menu kind on this, so every kind added
src/ui/ctxrows.h:166:     * Sprint 57.13 §4 proper.
src/ui/ctxrows.h:25: * THE ACTION TABLE IS THE DISPATCH.  Sprint 27 dispatched a row with a
src/ui/ctxrows.h:39: * What the pointer is over.  NONE/TAB/GROUP keep the values Sprint 27
src/ui/ctxrows.h:5: * Sprint 57.13 §3/§4: WHAT IS UNDER THE POINTER, and WHICH ROWS THAT
src/ui/ctxrows.h:93:     * Sprint 57.13 §3 listed six targets and Deliverable 4 found it
src/ui/draw.c:230: * This is the same mistake Sprint 22's cursor placement made, in the
src/ui/draw.c:231: * sibling helper; the cursor was fixed in Sprint 26 and this one was
src/ui/draw.c:987:/* Sprint 22 §7: panes, borders, dim-inactive                       */
src/ui/draw.c:997: * Sprint 27 §7: the borders come from THE glyph table, so the ASCII
src/ui/draw.h:12:/* Draws the complete Sprint 15 viewport and footer. */
src/ui/draw.h:15:/* Sprint 22 §7: draws every leaf, then the borders their split nodes
src/ui/filter.c:2: * Sprint 26 §7.  See filter.h for the measurement every decision here
src/ui/filter.h:5: * Sprint 26 §7: incremental filtering inside the keystroke budget.
src/ui/glyphs.c:2: * Sprint 27 §7.  See glyphs.h for the »/« rule and the single-decision
src/ui/glyphs.c:50:    /* Reserved for Sprint 52; nothing draws these yet. */
src/ui/glyphs.h:22: * decision is made ONCE, at startup (Sprint 0's single-decision rule),
src/ui/glyphs.h:5: * Sprint 27 §7: ONE table, every chrome glyph in the program.
src/ui/glyphs.h:72:     * Git status — RESERVED for Sprint 52.  They live in the table now
src/ui/groupnav.c:2: * Sprint 24 §6.  See groupnav.h for why the line is continuous.
src/ui/groupnav.c:325:/* Sprint 27 §5/§8: the group menu's rows, as registry commands     */
src/ui/groupnav.c:333: * Sprint 24's exact sequence rather than a second implementation of it.
src/ui/groupnav.c:369:    /* Sprint 24's sequence, in Sprint 24's order — the ordinal
src/ui/groupnav.h:52: * Sprint 27 §5/§8.  add_tab is the keyboard twin of dropping a tab into
src/ui/groupnav.h:5: * Sprint 24 §6: navigation as ONE CONTINUOUS LINE.
src/ui/grouppicker.c:1095:/* The dialog's own rows, as commands (Sprint 57.13 §4)             */
src/ui/grouppicker.c:2: * Sprint 24 §4.  See grouppicker.h for why ticks are a path set.
src/ui/grouppicker.c:511: * Sprint 27 §2: the wheel over the dialog.
src/ui/grouppicker.c:572: * Sprint 57.13 Deliverable 4: the two answers the GP menu rows offer,
src/ui/grouppicker.c:65:     * one arithmetic (the Sprint 22 law). */
src/ui/grouppicker.h:5: * Sprint 24 §4: the group picker.
src/ui/grouppicker.h:73:/* Sprint 27 §2: the wheel.  Moves the focused row — see grouppicker.c
src/ui/grouppicker.h:91: * Sprint 57.13 §4: the dialog's `Toggle` and `Confirm` rows.
src/ui/groups.c:2: * Sprint 24 §1/§2.  See groups.h for why there is no member list.
src/ui/groups.h:119: * Sprint 25 §6: restores the remembered member from workspace state.
src/ui/groups.h:5: * Sprint 24 §1/§2: tab groups, ported from the facsimile model whole.
src/ui/layout.c:18:/* Pane tree (Sprint 22 §2)                                         */
src/ui/layout.c:265:        return false; /* the root leaf refuses; Sprint 23 owns this */
src/ui/layout.c:303:/* Focus (Sprint 22 §4)                                             */
src/ui/layout.c:427:/* Resize (Sprint 22 §5)                                            */
src/ui/layout.h:116: * Sprint 25 serializes the tree through this rather than reaching into
src/ui/layout.h:17: * both a Rect and text must clip through the Sprint 2 width tables, and
src/ui/layout.h:5: * Sprint 22 §1/§2: the pane tree and the layout/draw split.
src/ui/layout.h:91: * refuses to close; Sprint 23 owns the last-pane-of-last-tab case. */
src/ui/menu.c:2: * Sprint 18.5 §5.  See menu.h for what this widget owns and why it is
src/ui/menu.c:325:             * its detail column already reads "Sprint 23: ...".  Dim is
src/ui/menu.h:26:    /* NULL draws the list inline (the command line's menu); Sprint 26's
src/ui/menu.h:5: * Sprint 18.5 §5: the ranked-list widget.
src/ui/menu.h:7: * Extracted from cmdline.c so Sprint 26's list picker is an INSTANCE of
src/ui/menu.h:98: * not fall through to the pane beneath (Sprint 22's law).
src/ui/mouse.c:102: * alone — Sprint 24's group picker registers one over its own rectangle
src/ui/mouse.c:1034: * `1,000.50` and `foo_bar` FOR FREE, because Sprint 16 already fought
src/ui/mouse.c:106: * Sprint 27 moved this out of ed.c: DoD 2 is that no mouse event
src/ui/mouse.c:1131: * half-thing silently would be worse than the option.  Sprint 36 owns
src/ui/mouse.c:1159:        /* ONE undo transaction, through Sprint 10's edit choke point,
src/ui/mouse.c:1580: * Joining a group is Sprint 24's exact sequence, called and never
src/ui/mouse.c:2005:     * hangs on the first scroll — Sprint 4 pinned this and named this
src/ui/mouse.c:2036: * `iarg 1` is the TAB STRIP — Sprint 27's behaviour, kept addressable
src/ui/mouse.c:2067: * would be a second derivation of the layout (Sprint 22's law).  -1
src/ui/mouse.c:376: * Sprint 57.13 §6: the menu's look, from the theme's `menu.*` roles.
src/ui/mouse.c:492: * menus retain Sprint 27's below-right placement.  Pointer menus place
src/ui/mouse.c:571: * statement that drew it (Sprint 22's law).  Only a NONE hit falls
src/ui/mouse.c:921:     * YEW_SRC_MOUSE, and Sprint 27's YEW_SRC_KEY here was simply wrong:
src/ui/mouse.c:940: * TABLE-DRIVEN (57.13 §3).  Sprint 27 spent a switch per menu kind on
src/ui/mouse.c:97:/* Sprint 18.5 §8: the completion menu's first refusal              */
src/ui/mouse.h:126: * Did Sprint 18.5's completion menu claim this event (and act on it)?
src/ui/mouse.h:138: * Sprint 4's FOCUS_OUT — a drag whose release lands in another window
src/ui/mouse.h:170: * Sprint 57.13 §3: WHAT IS UNDER THE POINTER.
src/ui/mouse.h:174: * a test rather than a bug report.  A region hit wins (Sprint 22's
src/ui/mouse.h:18: * `tab_id` / `gid` / path, never as an index (Sprint 23's law, applied
src/ui/mouse.h:198: * §9: the runtime toggle.  Sprint 36 owns the option model that makes
src/ui/mouse.h:209: * half-thing silently would be worse than the option.  Sprint 36 owns
src/ui/mouse.h:226: * - A DOCUMENT CONTEXT MENU → SHIPPED, Sprint 57.13.  Sprint 27 filed
src/ui/mouse.h:239: *   DRAG-AND-DROP → post-1.0, named in Sprint 57.13 §8 so nobody
src/ui/mouse.h:251: * - F-MODE TREE MOUSE (click to expand, drag to stage) → Sprint 52,
src/ui/mouse.h:255: * - LSP HOVER-ON-POINTER and diagnostics tooltips → Sprint 47.  Sprint
src/ui/mouse.h:267: *   would break the layout/hit-test identity Sprint 22 made a law.
src/ui/mouse.h:26: * touching `phase` and without setting `held` — Sprint 4 pinned this:
src/ui/mouse.h:5: * Sprint 27 §1: THE mouse router.
src/ui/mouse.h:7: * Sprint 4 decoded mouse events and threw them away; Sprint 22 built the
src/ui/mouse.h:8: * region registry; Sprint 18.5 joined the two for the completion menu
src/ui/picker.c:1042:         * same law the selection follows, so Sprint 27's click lands on
src/ui/picker.c:2: * Sprint 26 §5.  See picker.h for the three laws this exists to obey.
src/ui/picker.h:155: * Sprint 27 §2: the mouse seams.
src/ui/picker.h:166: * (YEW_PICK_ACCEPT_HERE / _VSPLIT / _HSPLIT).  Sprint 57.13's picker
src/ui/picker.h:5: * Sprint 26 §5: THE list picker.
src/ui/pickers.c:2: * Sprint 26 §6.  See pickers.h.
src/ui/pickers.c:546:/* Undo branch picker — closes Sprint 10 §11                        */
src/ui/pickers.h:11: * The undo branch picker closes Sprint 10 §11's deferral.  Its rows come
src/ui/pickers.h:5: * Sprint 26 §6: the three picker instances.
src/ui/region.c:107:     * Sprint 27 §5.  A context-menu row handler must re-find its target
src/ui/region.c:117:                "menu's target is captured at open time (Sprint 27 §5)");
src/ui/region.c:2: * Sprint 22 §6.  See region.h for the one-source-of-truth rule.
src/ui/region.h:105: * begins (Sprint 57.13's hover repaint) can take back its own without
src/ui/region.h:115: * Sprint 27 §5: freezes the table.  A hit-test while frozen is a BUG
src/ui/region.h:28:     * Sprint 23/24.  The payload sign convention is documented HERE,
src/ui/region.h:29:     * now, although groups do not land until Sprint 24: the click
src/ui/region.h:38:    /* Sprint 57.8: the row-one ` + ` action; payload is unused. */
src/ui/region.h:45:    /* Sprint 24 §4: the group picker's name field and its listing rows
src/ui/region.h:50:     * Sprint 18.5 §5.  Payload is the row's index within the menu's
src/ui/region.h:57:    /* Sprint 44: item index within the focused window's open completion. */
src/ui/region.h:5: * Sprint 22 §6: the clickable-region registry.
src/ui/region.h:60:     * Sprint 26 §5.  Payload is the item's PAYLOAD, never the row
src/ui/region.h:67:     * Sprint 27 §5.  Payload is the row's index within the OPEN menu,
src/ui/region.h:79:    /* Sprint 52: payload is the selected item's interned path id. */
src/ui/statusline.c:491:        /* Sprint 19 §8: the job badge is present iff something is
src/ui/statusline.c:506:         * Sprint 21 §3: `[3/17]`, and `[3/10000+]` past the cap.  The
src/ui/strip.c:20:     * Measured through the Sprint 2 clipper, which is the only thing
src/ui/strip.c:2: * Sprint 23 §3.  See strip.h for why there is exactly one of these.
src/ui/strip.h:15: * label's length, which is the Sprint 22 law and the reason a multibyte
src/ui/strip.h:5: * Sprint 23 §3: the strip layout engine.
src/ui/strip.h:7: * ONE engine, reused verbatim by Sprint 24's row-2 member strip.  That
src/ui/tabs.c:1032:         * the multibyte click-shift the Sprint 22 law forbids.
src/ui/tabs.c:1039:         * Sprint 27 §4.  The SAME cells, against the pre-drag list —
src/ui/tabs.c:1043:         * click-shift the Sprint 22 law forbids.
src/ui/tabs.c:1096: * This is also the hover-preview renderer (Sprint 27 calls it with a
src/ui/tabs.c:1113:        /* Sprint 57.10: numbered 1..n so `alt+N` inside the group has
src/ui/tabs.c:1143:     * Sprint 27 §4: a dwell opens a group's member strip as a drop
src/ui/tabs.c:1160: * scroll indicator is IGNORED — Sprint 27 owns wheel, drag-reorder and
src/ui/tabs.c:1193:     * The sign convention region.h wrote down in Sprint 22, now live:
src/ui/tabs.c:1210:/* Sprint 23 §5/§6: commands and the dirty-close prompt             */
src/ui/tabs.c:1272:/* Sprint 24 §7 / 57.10: numbered jumps and the 500 ms window       */
src/ui/tabs.c:1545: * closing a tab (Sprint 19) — can compact the array while the prompt is
src/ui/tabs.c:1591: * Sprint 27 §5: the tab context menu's rows, as registry commands.
src/ui/tabs.c:1671:     * through Sprint 12's OSC 52 path. */
src/ui/tabs.c:1679: * Sprint 57.13 §4: the tab menu's `Open in Split Right` / `Open in
src/ui/tabs.c:286:     * Sprint 23 pointed every tab at the window it cloned from, so all
src/ui/tabs.c:2: * Sprint 23 §1/§2.  See tabs.h for the stable-id discipline.
src/ui/tabs.c:457:/* Sprint 24 §3: lazy hydration                                     */
src/ui/tabs.c:498:     * Sprint 25 §6 restores top/left into a deferred tab's window, and
src/ui/tabs.c:521:     * Sprint 25 §6: cursors restored into a deferred tab were never
src/ui/tabs.c:559:/* Sprint 23 §3: the tab strip                                      */
src/ui/tabs.c:575: * its index in the array (Sprint 57.10: a group in the middle of the
src/ui/tabs.c:629: * filesystem call on a draw path.  Sprint 25 refines "outside".
src/ui/tabs.c:636:    /* Sprint 25 §6 refines "outside" as promised: a file that was gone
src/ui/tabs.c:686:             * convention was written into region.h in Sprint 22 so the
src/ui/tabs.c:728:     * A parameter, not a renderer: Sprint 22's layout reserves whatever
src/ui/tabs.c:735:    /* Sprint 57.8: even one tab owns a row because its tail carries the
src/ui/tabs.c:747:    /* Sprint 27 §4: a dwell-opened preview needs the row too, or the
src/ui/tabs.c:760: * whole point of Sprint 22 — a second copy of this arithmetic would
src/ui/tabs.c:767:/* Sprint 27 §4: the pre-drag slot table                            */
src/ui/tabs.h:115: * Sprint 24 §3: lazy hydration.
src/ui/tabs.h:157: * Sprint 27 §4: the PRE-DRAG row-1 slot table.
src/ui/tabs.h:205: * Sprint 24 §7 / 57.10: numbered jumps and the 500 ms digit-extension
src/ui/tabs.h:241:/* Sprint 57.10: row-1 entry N from anywhere — see the window comment. */
src/ui/tabs.h:244:/* Sprint 27 §5: the tab context menu's rows.  Commands, not menu-only
src/ui/tabs.h:52:     * Sprint 23 carried one as a placeholder.  Sprint 24 deletes it:
src/ui/tabs.h:5: * Sprint 23 §1: the tab model.
src/ui/tabs.h:64:     * it for a live fact — Sprint 25 §6 checks the disk once, at
src/ui/typejump.c:2: * Sprint 26 §8.  See typejump.h for the four rules.
src/ui/typejump.h:5: * Sprint 26 §8: type-to-jump, for lists that cannot carry a filter
src/ui/typejump.h:6: * line — s24's group picker listing, and (Sprint 52) the F-mode tree,
src/ui/viewport.c:559: * This returned `gutter + relative` until Sprint 25 — right only by
src/ui/viewport.c:561: * therefore rect.x == gutter.  The moment Sprint 22 gave panes an x
src/ui/win.c:43: * Sprint 22 §7: a clicked CELL to a cursor position.
src/ui/win.h:100:    /* Sprint 47: passive document highlights debounce and reject stale
src/ui/win.h:103:    /* Sprint 53 view-local Git presentation. */
src/ui/win.h:113:/* Sprint 14 compatibility names; new code uses the yew_vp_* API. */
src/ui/win.h:118:/* Sprint 22 §7: click-to-focus lands the cursor on the clicked
src/ui/win.h:51:     * Sprint 34: stable for this window's lifetime and never reused.
src/ui/win.h:53:     * Buffers have carried one since Sprint 14 for the same reason a
src/ui/win.h:65:     * separate ones (Sprint 21 §5). */
src/ui/win.h:67:    /* Sprint 21 $3: match highlighting for THIS view. */
src/ui/win.h:79:    /* Sprint 36: sparse values explicitly set at window scope. */
src/ui/win.h:81:    /* Sprint 43: display-only completion state belongs to the view. */
src/ui/win.h:84:    /* Sprint 47: one transient floating panel belongs to this view. */
src/ui/win.h:8:/* Sprint 22 moved Rect here, as this file said it would. */
src/unicode/category.h:7: * Sprint 2 emitted grapheme, width and word-break properties, which is
src/unicode/category.h:8: * everything the renderer and the unit engines need.  Sprint 20's regex
src/unicode/grapheme.h:62:/* TextBuf coordinate wrappers land in Sprint 9. Word_Break lands in
src/unicode/grapheme.h:63: * Sprint 16; case folding in Sprint 20. Glyph attributes and tab-stop
src/unicode/grapheme.h:66: * line breaking is outside 1.0 because Sprint 15 wraps on whitespace and
src/unicode/width.h:43: * UAX #14 line breaking is outside 1.0 because Sprint 15 wraps on whitespace
src/util/xdg.h:11:/* $XDG_CONFIG_HOME/yew, or ~/.config/yew.  Sprint 31's module
src/ws/finder.h:5: * Sprint 18.5 §1: the fuss-derived fuzzy scorer, pulled forward from
src/ws/finder.h:6: * Sprint 26 §1/§2 so the command line can rank and HIGHLIGHT this sprint
src/ws/finder.h:7: * rather than waiting for the finder.  Sprint 26 inherits these as landed
src/ws/fl_emit.c:2: * Sprint 25 §4: the emitter.
src/ws/fl_emit.c:5: * Sprint 36 reimplements this against the same corpus and must produce
src/ws/fl_parse.c:12: * in Sprint 36 instead of just this one.
src/ws/fl_parse.c:2: * Sprint 25 §4: the parser.
src/ws/fl_parse.c:60:     * user-authored source (Sprint 36's config is, and that is where
src/ws/fllit.h:35: * FL_LIT_*, not FL_*, since Sprint 34.
src/ws/fllit.h:37: * Sprint 30's FlType spells six of its tags FL_NIL, FL_BOOL, FL_INT,
src/ws/fllit.h:39: * names.  Nothing included both headers until Sprint 34's editor
src/ws/fllit.h:5: * Sprint 25 §2/§4: Fletch pure data literals — the workspace-state
src/ws/fuzzy.c:147: * Sprint 26's walk yields file paths, so it could scan for the last '/'
src/ws/fuzzy.c:2: * Sprint 18.5 §1.  See ws/finder.h for the scoring table and the three
src/ws/gitignore.c:2: * Sprint 26 §4.  See gitignore.h for the supported/unsupported table
src/ws/gitignore.h:5: * Sprint 26 §4: the .gitignore matcher — bespoke, and a SUBSET.
src/ws/state.h:108:     * The option model is Sprint 36's.  Until it exists this build must
src/ws/state.h:144: * Sprint 25 §6/§7: restoring.
src/ws/state.h:178: * The option MODEL is Sprint 36's; this is the one bridge to it, so
src/ws/state.h:5: * Sprint 25 §3: the v1 workspace-state schema — FROZEN.
src/ws/state.h:71:/* Sprint 36's production parser: real Fletch pure-literal syntax, adapted
src/ws/state.h:77: * Sprint 25 §5: saving.
src/ws/state_emit.c:2: * Sprint 25 §3/§4: emitting the v1 document.
src/ws/state_emit.c:646:     * Options land in Sprint 36.  Until then whatever was READ is
src/ws/state_fletch.c:1:/* Sprint 36: adapt the real Fletch pure-literal value tree to v1's mapper. */
src/ws/state_parse.c:2: * Sprint 25 §6/§7: restoring a workspace, and surviving one that is
src/ws/state_parse.c:953:    /* Step 2: options kept verbatim for Sprint 36. */
src/ws/state_save.c:2: * Sprint 25 §5: save triggers, atomicity, and the single-writer lock.
src/ws/symidx.h:180: * Sprint 47 supplies documentation and LSP items through the same query
src/ws/symidx.h:4:/* Sprint 44: deterministic, no-LSP symbol completion. */
src/ws/symwalk.h:4:/* Sprint 44: cooperative workspace discovery and symbol indexing.
src/ws/walk.c:2: * Sprint 26 §3.  See walk.h for why this is bespoke and why it is not
src/ws/walk.h:5: * Sprint 26 §3: the workspace file walk.
src/ws/workspace.c:2: * Sprint 25 §1.  See workspace.h for why the `path` record exists.
src/ws/workspace.h:16: * and belongs to Sprint 36.
src/ws/workspace.h:5: * Sprint 25 §1: workspace identity.
src/ws/workspace.h:60: * FNV-1a 64 over BYTES.  Exposed because the undo sidecar (Sprint 10)
```
