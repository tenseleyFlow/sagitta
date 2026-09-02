# yew project handoff

**Current checkpoint date:** 2026-09-02

**Current source:** `/Users/mfwolffe/GithubOrgs/tenseleyFlow/sagitta` on
Apple-silicon host `nomad-1`

**Transferred from:** `/home/mfwolffe/GithubOrgs/tenseleyFlow/sagitta` on the
2026-08-27 Linux workstation

**Active branch:** `trunk`

**Hosted-green code/recovery anchor:** `6d742a88` (`origin/trunk` code baseline)

**Current post-Sprint-57.8 code frontier:** `6d742a88`

**Git position:** the complete Sprint 57 through 57.8 chain and its hosted-CI
remediation are pushed. GitHub Actions run `33615806917` is green for all 22
standard push jobs at `6d742a88`; this handoff refresh is documentation-only.

**Active implementation frontier:** Sprint 57.8 is repository-complete. Sprint
58 is next and has not begun. Its hosted commit-of-record baseline gate is now
satisfied by `6d742a88` and run `33615806917`. Designated-hardware timing,
the trigger-specific Valgrind/nightly jobs, and physical Pi corroboration remain
explicit release-evidence tails; none was inferred from a skipped push job.

This document began as the exact frozen Linux-to-Mac transfer record. The
authoritative pickup point is immediately below. The older 2026-08-29 status
is retained afterward as provenance and is explicitly superseded.

## 0. Authoritative pickup point

Sprint 57 and the Sprint 57.5 compact FUSS redesign are repository-complete.
The size gates use the measured
S57-A1 floors without removing core behavior: pinned x86_64 stripped sizes
are 1,463,968 bytes minimal and 1,947,336 bytes full. The full-feature native
suite and an independent `MODULES=""` suite both pass end to end. The minimal
suite retains editing, Fletch, syntax, search, persistence and macros; only
the four documented optional modules are shims.

Sprint 57.5 now ships collapsed-by-default FUSS trees, session expansion
memory, forced-open ancestry for editor-open files, compact `+`/`-` rows,
adaptive quarter-width/full-screen layout, a bright inert edge, and centered
full-screen previews. The complete native full/minimal suites, Darwin arm64
ASan/UBSan, 20,000-iteration sanitized FUSS fuzz, deterministic PTYs,
performance gates, and all six native size profiles are green. Apple-specific
repairs keep the injected durability shim outside sanitizers and use native
Mach-O strip flags.

The deterministic embedded gate passes QEMU rows 1–11 at 64 MiB with peak
`VmHWM` 9,764,864 bytes. The 32 MiB row cleanly names the 48 MiB workload
floor and exits without an OOM kill. Physical Raspberry Pi Zero 2 W class
corroboration is still pending. Hosted CI is green for `6d742a88` in run
`33615806917`. Physical evidence remains a release-blocking tail under S57-A4;
it is neither fabricated from emulation nor used to stall unrelated repository
work.

Sprint 57.6 closes the subsequent field failures. Extensionless binary
buffers now bind no source language, start no LSP, and enter no source-symbol
index while retaining byte-exact editing and save behavior. The exact `encode`
replay fell from 0.8–4.3+ second dispatch stalls to 4.846 ms p99 dispatch with
no multi-second frames or `fortls`. Ancestor-repository Git paths are translated
and filtered to the workspace, removing the fabricated `wolf` row. FUSS bare
filename characters now type-jump across visible rows, with printable actions
behind `C-g`.

Sprint 57.7 closes the remaining off-canvas layout defects. FUSS now reserves
its left span in the shared layout, starts on row zero beside the shifted tab
strip, lays out panes/documents/footer at the remaining width, and paints a
dedicated neutral surface plus separator through the bottom row. The
full-screen fallback remains intact, and repeated enter/leave cycles preserve
the complete pane/tab/cursor/viewport state.

Sprint 57.8 modernizes the tab strip without changing tab identity or command
semantics. Padded bracket-free active/inactive/group/member surfaces use
dedicated dark/light theme roles and deterministic degradation. The strip is
visible for one tab, and an exact ` + ` tail control invokes the existing
`ed.tab.new` command to create and activate a workspace-owned untitled tab.
Right overflow retains priority, and ordinary click, drag, group, CJK, and
FUSS-offset geometry remains green.

The next contract is `.docs/sprints/14-audits-release/s58-adversarial-audits.md`.
Its fixed code baseline is `6d742a88`. The required hosted commit-of-record
evidence is green, but Sprint 58 has not begun: open its audit fronts only as
new sprint work, preserving the fixed baseline and the reproducer-first law.

Hosted Sprint 57.8 closeout evidence at `6d742a88`:

- GitHub Actions run `33615806917` completed successfully on 2026-09-02 with
  all 22 standard push jobs green: GCC, Clang, macOS arm64, hosted arm64,
  musl, modules, size, performance, determinism, sanitizer, PTY, script,
  Fletch, Fletch dispatch, LSP, embedded, allocation, torture, Unicode, and
  structural bans;
- the performance job includes the exact Sprint 56 profiler cross-check that
  failed on the first pushed candidate; the macOS job includes the exact job
  streaming fixture and raw-key burst that failed there. The remediation gives
  profiler dump control transport a 30-second hosted ceiling without changing
  any key latency gate, makes the stream fixture kill the direct pipe-owning
  child, honors Sprint 56 advisory timing policy across latency/plugin/package/
  cloud probes while retaining the 100x sanity ceiling, and synchronizes the
  Darwin split burst on its full 4096-key semantic state before snapshot;
- trigger-specific Valgrind, designated-hardware performance, fuzz-nightly,
  perf-nightly, and torture-nightly jobs were skipped by the push trigger as
  designed. They are not reported as executed evidence.

Most recent Sprint 57.8 validation:

- complete default-module `make test` green: every PTY, Fletch 38/38, scripts
  93/919, package integration 51/51, 2,000 round-trip seeds, fuzz/syntax/
  policy/smoke/torture gates, and 2,419 unit tests / 71,034,731 assertions;
- three complete post-migration PTY executions are deterministic, including
  FUSS Git fixtures, socket-backed AI streams, all chrome degradation tiers,
  CJK clicks, group/drag behavior, and the clicked-new-tab path;
- focused tab/mouse/click/group/theme coverage passes 73 tests / 2,079
  assertions under plain and Darwin arm64 ASan/UBSan; the focused click PTY is
  also sanitizer-clean;
- independent full and `MODULES=""` shipping builds are warning-clean, with
  the core-only product smoke suite green;
- every FUSS performance row passes, including 1.766 ms drawer entry and
  1.047 ms open resolution;
- all six native size profiles pass: 1,452,640 bytes minimal, 1,891,504 full,
  1,571,600 LSP-only, 1,588,496 AI-only, 1,586,752 FUSS-only, and 1,536,096
  plugins-only.

Most recent Sprint 57.7 validation:

- complete default-module `make test` green: every PTY, Fletch 38/38, scripts
  93/919, package integration 51/51, 2,000 round-trip seeds, fuzz/syntax/
  policy/smoke/torture gates, and 2,414 unit tests / 71,034,643 assertions;
- focused drawer/theme tests pass under plain, Darwin arm64 ASan/UBSan, and
  alignment/UBSan; all FUSS-filtered PTYs plus the directory-startup path pass
  from two deterministic executions;
- default, FUSS-only, and core-only shipping builds are warning-clean, with
  FUSS-only and core-only smoke green; deterministic FUSS fuzz passes 20,000
  iterations;
- all FUSS performance rows pass, including 1.763 ms drawer entry and 1.077 ms
  open resolution;
- all six native size profiles pass: 1,452,640 bytes minimal, 1,891,504 full,
  1,555,088 LSP-only, 1,588,496 AI-only, 1,586,752 FUSS-only, and 1,536,096
  plugins-only.

Most recent Sprint 57.6 validation:

- complete default-module `make test` green: all PTYs, Fletch 38/38, scripts
  93/919, package integration 51/51, round-trip/fuzz/syntax/policy/smoke/torture
  gates, and 2,414 unit tests / 71,033,068 assertions;
- exact field workspace replay contains no `wolf`, labels `encode` `utf-8 bin`,
  starts no `fortls`, and records 4.846 ms p99 / 11.667 ms maximum dispatch;
- all 29 FUSS PTYs pass twice; focused Apple-clang ASan/UBSan and a 20,000-run
  deterministic FUSS fuzz campaign pass;
- 20,000-node FUSS performance remains within every budget, including 2.614 ms
  drawer entry and 1.408 ms open resolution;
- all six native shipping sizes pass: 1,452,640 bytes minimal, 1,891,504 full,
  and every single-module profile below its cap; FUSS-only product smoke passes.

Most recent Sprint 57.5 validation:

- full native: 2,409 unit tests / 71,032,846 assertions / 0 failures;
  PTY, Fletch 38/38, scripts 93/919, package 51/51, round-trip, syntax,
  policies, smoke, and live-PTY torture green;
- minimal native: 1,946 unit tests / 70,056,319 assertions / 0 failures;
  all applicable PTYs, Fletch 38/38, scripts 71/743, round-trip, syntax,
  policies, smoke, and live-PTY torture green;
- Darwin arm64 Clang ASan/UBSan: 2,391 tests / 71,032,552 assertions /
  0 failures; sanitized FUSS fuzz seed `91615317`, 20,000 iterations, hash
  `8861851834338528`;
- FUSS PTY: all 29 cases pass twice; the 20,000-node performance rows remain
  within budget, including 2.538 ms entry and 3.819 ms open resolution;
- Apple-silicon stripped size corroboration: minimal 1,452,640 bytes, full
  1,891,472 bytes, and all four single-module profiles under their existing
  caps.

Future pushes still require an explicit request. If a trigger-specific
Valgrind/designated lane or physical Pi evidence fails when run, reopen
remediation before release. Do not weaken core features or stability for a
footprint target; stop, measure, and amend or defer the target instead. S57-A1
already moved the unrealistic 900 KiB minimal value to a post-1.0 ratchet;
Sprint 58 front F15 and Sprint 60 closeout must reassess the size/stability
boundary again.

## 1. Historical 2026-08-29 pickup point — superseded

The following section records the earlier transfer state. Its missing-work
claims were resolved by the authoritative checkpoint above.

Yew is still on **Sprint 57**. The optional embedded runtime and the native
Apple-silicon port are repository-complete locally. Do not start Sprint 58 or
call Sprint 57 complete until the remaining external evidence is closed:

1. Reconcile the locked 900 KiB stripped minimal x86_64 Linux budget with the
   measured roughly 1.85 MiB binary and roughly 1.48 MiB `.text + .rodata`.
   This remains a real contract/scaffold conflict; do not weaken the gate.
2. Implement the repository's missing `make embedded` / `embedded-gate`
   surface and complete all twelve QEMU rows. This host can now run the pinned
   64 MiB x86_64 shape; only the row-1 smoke proof has been run.
3. Supply Sprint 56 designated-runner evidence. Hosted timing remains
   advisory and is not a substitute.
4. Complete the native/default-timing Linux test chains. The exact GNU size
   lane plus a warning-clean GCC/glibc build, product smoke and Fletch
   conformance have now run under x86_64 TCG; the Mac host itself still has
   only Apple clang, and emulation cannot adjudicate the hard timing rows.

The exact current local evidence is:

- Native default `make ... test`: PTY green; Fletch 38/38; scripts
  93 tests / 919 assertions / 0 failures with one intentional skip; package
  51/51; 432 syntax assets; 2,000 round-trip seeds; unit 2,401 tests /
  71,035,338 assertions / 0 failures; policies, smoke checks, and live PTY
  torture green.
- Exact arm64 alignment profile:
  `make test-unit CC=clang ALIGN_SAN=1 BUILD=build-align` passed the same
  2,401 tests / 71,035,338 assertions / 0 failures. This is the Sprint 57
  exact alignment/UBSan gate; `ALIGN_SAN=1` is arm64-only and mutually
  exclusive with the ordinary sanitizer and Valgrind profiles.
- Embedded runtime:
  `make CC=clang BUILD=build-s57-arm64-embedded EMBED_RUNTIME=1 test-runtime-embedded`
  passed. The Mach-O object record is 106,800 bytes; the blank-CWD proof
  emitted `YEWTEST 7 0 0`; runtime asset and consumer filters passed 6 tests /
  713 assertions / 0 failures. The separate `runtime-blob-selftest` also
  passed its generator determinism and error-path checks.
- Full local advisory performance suite:
  `make perf CC=clang PERF_GATE=0 PERF_RUNNER_ID=local-arm64-macos BUILD=build-s57-arm64-perf`
  exited 0 through the primary suite, policy selftests, three complete
  observations, and aggregation. Final medians included 437 us small-file
  typing, 288 us 100 MiB typing, 362 us syntax editing, 225 us many-buffer
  navigation, 1.476 ms 100 MiB search, 461 us assisted typing, 8.193 ms
  default first paint, and a 257-permille spawn-floor fraction. The known
  multicursor, workspace/open, profiler-overhead, and Linux-labelled closed
  RSS rows were WARN/advisory on this non-designated host.
- The full native default suite and live PTY torture are green on Darwin.
  A diagnostic full `SAN=1` PTY run built but exceeded timing/startup
  expectations under instrumentation; it was not substituted for the sprint's
  separate native PTY and exact alignment gates, both of which pass.
- Exact x86_64 glibc size evidence now exists from Ubuntu 24.04 under a
  full-system QEMU guest: GCC 13.3.0 and GNU binutils 2.42 built clean source
  at `a3cd877`. All six hard rows fail without weakening their gates:
  minimal 1,463,968 / 921,600 bytes; lsp-only 1,586,848 / 1,075,200;
  ai-only 1,599,176 / 1,013,760; fuss-only 1,611,424 / 1,064,960;
  plugins-only 1,558,176 / 993,280; full 1,947,336 / 1,572,864. The stripped
  minimal ELF contains 1,009,394 bytes of `.text`, 268,432 of `.rodata`,
  104,280 of `.rela.dyn`, and 54,768 of `.data.rel.ro`; this is a core-size
  conflict, not strip metadata or one optional module.
- Current source at `288d658` also builds warning-clean with GNU GCC 13.3.0
  against glibc in the same Ubuntu guest. Target, static-PIE-tool and runtime
  blob selftests pass; the complete product smoke chain passes; and Fletch is
  38 / 38 with coverage, meta and seeded gate selftests green. The resulting
  x86-64 dynamic PIE has only the expected `libc` and `libm` dependencies.
  The much longer default-timing PTY/unit aggregate remains a native-runner
  item for the same reason as the musl timing rows below.
- The pinned Alpine 3.20.10 musl profile ran natively as x86_64 inside the
  same guest with GCC 13.2.1, musl 1.2.5 and binutils 2.42. Full yew verifies
  as static PIE and passes its 2 MiB size row at 2,066,320 stripped bytes.
  Minimal verifies as static PIE but fails its 1.3 MiB row at 1,554,288 bytes
  (191,140 bytes over). Two independent full builds are byte-identical at
  SHA-256 `06ddbbae911a475444b47f29eb269cbfaa58a5cb2b616e33c37e6e2c19c977bb`.
- The first musl test compile exposed `realpath` hidden behind musl's XSI
  feature-test boundary in seven test/perf translation units. Commit
  `288d658` defines `_XOPEN_SOURCE=700` at those call sites. Apple clang then
  passed the affected objects, scripts 93 / 919 / 0 with one intentional
  skip, and unit 2,401 / 71,035,338 / 0. The resumed musl compile is clean.
  Under full-system TCG, the complete selected PTY sweep is green with a
  measured 2x quiet-window scale, a 60 s case hang ceiling and a 2 h
  aggregate ceiling; these are emulator completion controls, not changed CI
  defaults. Fletch is 38 / 38. Ninety-two script cases passed in the main
  sweep with one intentional skip; the 10,000-replacement case exceeded the
  native musl lane's 30 s ceiling but passed separately at the repository's
  existing 600 s instrumented ceiling (9 assertions). Round-trip (2,000
  seeds plus fixtures), record corpus, syntax corpus, all 432 syntax assets,
  target/static-PIE/runtime-blob selftests and package integration 46 / 0
  also pass. The required resolver matrix is green with normal, minimal and
  absent `/etc/hosts`.
- Do not misreport that TCG evidence as a default-timing `make test` pass.
  `test-syn-def-corpus` reaches its hard-coded 5 s per-input fuzz watchdog,
  and the unit suite reaches the hard clipboard nonblocking latency gate;
  the latter was stopped after 16 minutes because the full suite contains
  more than 71 million assertions. The pinned native Alpine CI lane remains
  required to adjudicate those timing gates and supply the authoritative
  aggregate `make test` result.
- A diagnostic minimal GCC `-flto` build was used only to test whether a
  deferred optimization could plausibly explain the budget gap. Commit
  `bed4fe5` initializes the first set of helper-mediated locals exposed by
  GCC's whole-program analysis and is Apple-clang regression-clean. A second
  broad Fletch warning set remains; Sprint 57 explicitly defers LTO, so no
  shipping flag or measurement definition changed.
- The Mac now has a real row-1 constrained-target smoke proof: QEMU 11.1.1,
  `q35`, `qemu64`, 64 MiB RAM, Alpine `linux-virt` 6.6.142-r0, and
  `busybox-static` 1.36.1-r31 booted a 1,637,146-byte deterministic initramfs.
  Verified full-musl yew printed `yew 1.0.0-dev`, the four-module line, exited
  0, and powered down cleanly; the init shell's `VmHWM` was 612 KiB. This is
  feasibility plus checklist row 1, not the twelve-row milestone. The repo
  still has no `scripts/embed-image.sh`, `make embedded`, or
  `make embedded-gate`, so those surfaces are active implementation work.

Two arm64-only performance-harness defects were found by the definitive run
and fixed without weakening hard invariants:

- the hidden memory RSS collector now uses the normal three-sample startup
  median instead of a noisy one-sample timing gate;
- the AI shadow batching cap now derives from the measured monotonic stream
  duration while remaining a hard count bound. On Darwin the nominal 4 s
  120-tps loop took 4.868 s, delivered 120 frames, and correctly allowed at
  most 148.

### Historical first actions

1. Read, in order:
   - `AGENTS.md`
   - `.docs/plan/00-decisions.md`
   - `.docs/plan/01-architecture.md`
   - `.docs/plan/02-fletch.md`
   - `.docs/sprints/index.md`
   - `.docs/sprints/13-performance/s57-size-and-embedded.md`
   - this file
2. Confirm `git status --short --branch`. The main worktree should be clean and
   ahead of `origin/trunk`; do not push unless explicitly requested.
3. Keep Sprint 57 active. Work the external x86_64 size and target-proof rows
   above before moving the campaign frontier.
4. Use separate `BUILD=` trees for default, embedded, alignment, and
   performance profiles. The profile stamp protects normal reuse but separate
   trees keep evidence unambiguous.
5. Treat §2 and §§4-11 as retained transfer history where their wording says
   “at freeze time” or “WIP”; §0 is the authoritative current status.

## 2. Historical upstream green baseline

`trunk`, `origin/trunk`, and `origin/le01-wolf-refresh` all pointed to:

```text
0246f98e93fee97b9fd5df9183efeb1bf3e5222d
```

The 19-commit `le01-wolf-refresh` series was a clean fast-forward over trunk.
It was fast-forwarded locally and `git push origin trunk` succeeded.

Hosted CI for that exact SHA was fully green:

- GitHub Actions run: `33165592743`
- URL: <https://github.com/tenseleyFlow/sagitta/actions/runs/33165592743>
- Successful normal jobs: performance, bans, Unicode, Fletch dispatch,
  allocation, torture, musl, PTY, Clang, sanitizer, Fletch, hosted arm64,
  determinism, GCC, script, modules, and LSP.
- Expected gated skips only: designated-runner performance and the scheduled
  nightly fuzz/torture/Valgrind lanes.

The last CI defect fixed at the baseline was in
`scripts/s56-perf-gate.sh`: token scanning could overwrite a valid bare
`permille` observation with an empty value. The fix requires a numeric value
after `permille=` and includes exact-format regression rows.

Fresh evidence collected before the push/merge:

- `make -j2 perf-s56-gate-selftest CC=gcc` passed.
- `scripts/bans.sh` passed.
- `git diff --check` passed.
- The earlier fast suite passed 2,394 tests / 71,014,940 assertions / 0
  failures.
- `make test-pty` passed.
- Hosted exact-SHA CI subsequently passed as described above.

This clean upstream SHA remains the recovery anchor for comparing the resumed
implementation with known-good Linux behavior. The transferred work is now a
committed 41-commit chain; do not reset to the anchor unless explicitly asked.

## 3. Project and sprint position

The repository is now named **yew**; `sagitta` remains the historical checkout
and GitHub repository directory name. The shipped binary is `yew`, and its
macro/configuration language is Fletch (`*.fl`).

Campaigns through Sprint 55.5 are repository-complete. Sprint 56 and 56.5
landed the profiling/performance gates and workspace/FUSS/shadow-text behavior.
Sprint 57 is the current binding contract:

- binary-size budgets and per-module ledger;
- allocation audit and allocation-debug gates;
- musl static-PIE embedded profile;
- optional embedded `runtime/` asset;
- minimal-module parity;
- arm64 Linux/macOS and constrained-target proof.

Sprint 57's repository implementation now includes the size tooling,
allocation work, target profiles, musl CI, optional `runtime/` asset, hosted
embedded lanes, and Apple-silicon portability/validation work. Section 2.4's
`make EMBED_RUNTIME=1` path is implemented and green locally on arm64 macOS.

Sprint 57 must **not** yet be called complete:

- The locked 900 KiB stripped minimal budget appears incompatible with the
  current implementation. Measurements were roughly 1.85 MiB stripped, with
  `.text + .rodata` around 1.48 MiB. Treat this as a real scaffold conflict to
  reconcile, not a reason to weaken a gate silently.
- Pinned hardware/QEMU constrained-target proof remains external and
  unavailable.
- True GNU GCC is unavailable locally; the hosted Linux lanes remain the
  compiler evidence source.

Sprint 56's designated hardware evidence also remains external. GitHub had no
registered self-hosted runners or enabling repository variables. Hosted timing
is advisory and must not be promoted into designated baselines.

The separate Sprint 42 Daily Driver field milestone remains unearned unless a
qualifying yew session was explicitly designated and logged. Automated tests,
generated goldens, benchmarks, or editing in another editor do not count.

## 4. Historical dirty working-tree checkpoint and resolution

The following was the exact transfer manifest on 2026-08-27. It is retained
for forensic recovery only. Every listed file was reviewed and the resulting
implementation is committed; it is no longer the expected working state.

At freeze time:

```text
## trunk...origin/trunk
 M Makefile
 M scripts/size-ledger.sh
 M scripts/tests/size-tools.test.sh
 M src/fl/flconf.c
 M src/fl/module.c
 M src/mod/ai/policy.c
 M src/mod/ai/preset.c
 M src/syn/defs.c
 M src/syn/theme.c
 M src/syncli.c
 M tests/unit/registry.c
 M tests/unit/tests.h
?? scripts/gen-runtime-blob.c
?? scripts/tests/runtime-blob.test.sh
?? src/util/runtime_asset.c
?? src/util/runtime_asset.h
?? src/util/runtime_blob.h
?? tests/unit/test_runtime_asset.c
```

The tracked diff at freeze time was 212 insertions and 24 deletions across 12
files. `git diff --check` passed. No full default build or embedded build has
been run against this WIP. The focused `scripts/tests/size-tools.test.sh` test
passed.

File modes at freeze time matter:

- `scripts/tests/runtime-blob.test.sh` is executable.
- `scripts/gen-runtime-blob.c` and all new C/header files are regular
  non-executable source files.

The resumed implementation is the 41-commit range
`0246f98e93fee97b9fd5df9183efeb1bf3e5222d..1e50672`. The major groups are:

- strict type and initial macOS target portability (`f05852b` through
  `1fe9438`);
- optional runtime generation, fallback consumers, end-to-end proof, and
  hosted compiler lanes (`b552fba` through `f2edfc5`);
- Darwin script, unit, package, PTY, fault, path, FUSS, and suspend/resume
  portability (`34d1ef1` through `0d8cff7`);
- sanitizer/alignment and performance-harness portability (`ca6ba2a` through
  `d5f06e4`);
- fresh-build embedded size-output preparation (`1e50672`).

Use `git log --oneline 0246f98e..1e50672` for the exact coherent commit list.

Other worktrees were not touched:

- `.claude/worktrees/cmdline-ux`, branch `cmdline-ux`, at
  `9298ed9d77e3cda473118cf2322b7e6b02d84424`.
- A prunable detached worktree record for
  `/tmp/yew-live-manual.St1fNd/worktree`, at
  `3034a4190582ca13f3e3fc7a3d342a8b89731e44`; its gitdir target no longer
  exists. Do not prune it merely as part of resuming the implementation.

The auxiliary `cmdline-ux` worktree metadata contains absolute Linux paths:
its `.git` file names `/home/mfwolffe/GithubOrgs/tenseleyFlow/yew/...`, while
the main repository's worktree record names the source `sagitta` checkout.
Those bytes were intentionally mirrored exactly, so the auxiliary worktree is
not directly usable on macOS. The main `trunk` checkout is valid. Repair or
recreate the auxiliary worktree only as a separate deliberate operation after
protecting its branch; do not let a broad `git worktree repair` or prune alter
the active Sprint 57 checkpoint.

## 5. Frozen embedded-runtime design

The current `runtime/` corpus contains 55 regular files and 269,757 raw bytes,
including 48 syntax-language files. There are no symlinks. A raw blob exceeds
the sprint's 220 KiB budget, so deterministic compression was selected.

A prototype produced approximately 101,128 packed bytes (374 permille),
leaving roughly 124 KiB for the generated index and decoder.

The compressed wire format is frozen across the generator and decoder:

- LSB-first flag bits.
- Flag bit `0` means literal; flag bit `1` means match.
- One flag byte covers the next eight tokens.
- Match token is little-endian `u16`.
- `token = (distance << 4) | (length - 3)`.
- Distance range: 1..4095.
- Length range: 3..18.
- Deterministic bounded hash-chain search, at most 64 candidates.

Changing any of these requires updating the generator, decoder, determinism
tests, round-trip tests, and the contract in this handoff together.

The intended runtime API is:

```c
size_t yew_runtime_asset_count(void);
const char *yew_runtime_asset_name(size_t index);
bool yew_runtime_asset_has(const char *path);
bool yew_runtime_asset_read(const char *path, Bytebuf *out);
char *yew_runtime_asset_resolve(const char *path);
```

`src/util/runtime_blob.h` defines:

```c
typedef struct YewRuntimeBlobEntry {
    const char *name;
    u32 offset;
    u32 packed_len;
    u32 raw_len;
} YewRuntimeBlobEntry;
```

and accessors for the blob data and index.

Off/default builds must expose a harmless empty runtime-asset API without
linking or referring to generated blob symbols. Embedded builds must link one
generated blob object and retain byte-identical deterministic generation.

## 6. Historical WIP file map and implemented intent

This section describes what arrived uncommitted from Linux. The described
surfaces are now implemented; the wording is retained to explain the design
intent that constrained review.

### Generator and generated interface

`scripts/gen-runtime-blob.c` is the host C11/POSIX generator. Its intended CLI
is `gen-runtime-blob ROOT OUTPUT`. It currently aims to:

- recursively walk with `lstat`;
- reject symlinks, non-regular nodes, and invalid/nonportable relative names;
- globally byte-sort paths without `qsort`;
- compress with the frozen LZSS format;
- emit one `static const u8` blob and a sorted/insertion-ordered index;
- omit timestamps and absolute source roots;
- generate C that includes `util/runtime_blob.h` and exports
  `yew_runtime_blob_data` / `yew_runtime_blob_index`.

`scripts/tests/runtime-blob.test.sh` is the generator/determinism/error-path
selftest. It is integrated as `runtime-blob-selftest`.

### Runtime reader

`src/util/runtime_asset.[ch]` is intended to compile in both off and embedded
modes. It normalizes relative and synthetic `runtime/` paths, collapses `.` and
repeated separators, resolves safe `..`, rejects absolute/escaping paths,
binary-searches the sorted index, and decodes matches with strict bounds and
overlap support. `yew_runtime_asset_read` must decode into a temporary buffer
so caller output is unchanged on failure.

`tests/unit/test_runtime_asset.c`, `tests/unit/registry.c`, and
`tests/unit/tests.h` add off-mode behavior, embedded inventory/round-trip,
canonicalization, invalid-path, and output-preservation coverage. These tests
are reviewed and green in both the native suite and embedded focused run.

### Make integration

The `Makefile` implementation adds:

- `HOSTCC ?= cc` and `EMBED_RUNTIME ?= 0`;
- `-DYEW_EMBED_RUNTIME=$(EMBED_RUNTIME)`;
- host-generator, generated-C, and generated-object variables;
- conditional blob-object linking;
- sorted runtime file and directory prerequisites, so add/delete/rename is
  intended to invalidate generation;
- generated directories in `BUILD_DIRS`;
- the embed flag in `BUILD_PROFILE_KEY` and target-info output;
- generator selftest, embedded unit, and embedded budget targets;
- host-tool compile, deterministic `.tmp` generation, and target-compiler
  compile stages;
- an embedded object section budget of 225,280 bytes.

This wiring is exercised by GNU Make 3.81 on `nomad-1` and by the native-off
and embedded build trees. Hosted GCC/Clang lanes are configured but have not
run for these unpushed commits. The size target now creates its own
`$(BUILD)/tmp` directory, so a fresh embedded build does not depend on an
incidental earlier target.

### Size ledger

`scripts/size-ledger.sh` now includes `build/gen/runtime_blob.o` in the
`runtime.embedded` bucket. `scripts/tests/size-tools.test.sh` creates a fake
generated object and asserts the attribution. That focused test passed before
the freeze.

### Disk-first consumer fallbacks

The contract is strict: existing disk precedence remains byte-for-byte the
default. Embedded assets are the final fallback only after the same disk
lookup would otherwise fail. An explicit runtime environment override remains
authoritative where it was authoritative before.

The consumer implementation provides the following:

- `src/fl/flconf.c`: embedded `init.fl` after installed/repository disk miss;
  no embedded fallback when `YEW_RUNTIME_DIR` is explicitly set; `--clean`
  still bypasses all configuration.
- `src/fl/module.c`: stable synthetic runtime identity, relative imports from
  embedded runtime modules, and asset-read fallback; importer directory, user
  configuration, and runtime disk precedence remain ahead of embedded data.
- `src/syn/defs.c`: on `stat(...)=ENOENT`, load embedded bytes, use zeroed
  synthetic stat metadata, and force source-hash cache validation rather than
  the mtime fast path.
- `src/syncli.c`: compile-all's disk precheck permits known embedded assets and
  calls the same definition loader.
- `src/syn/theme.c`: user path, caller environment, install, and repository
  disk candidates remain first; then embedded `themes/<name>.fl`.
- `src/mod/ai/policy.c`: final fallback to embedded `ai-deny.fl`.
- `src/mod/ai/preset.c`: final fallback to the embedded local/cloud preset.

`docs/ai-privacy.md` is deliberately outside the runtime blob contract. Do
not embed it unless the scaffold decision is explicitly changed.

## 7. Review-point resolution

The 12 implementation review points from the frozen handoff were closed during
the 41-commit resume:

- embedded lookup preserves missing/error classification; theme and decoder
  failure paths preserve ownership and caller output;
- disk/user/importer precedence, explicit `YEW_RUNTIME_DIR`, relative runtime
  imports, synthetic syntax metadata, and hash validation have focused tests;
- compile-all is the intentionally embedded-aware `syncli` seam; unrelated
  disk-only commands were not broadened;
- generator determinism/error paths, runtime file/directory invalidation,
  off-mode linkage, and blank-CWD behavior are covered; hosted embedded
  GCC/Clang coverage is wired and awaits a pushed Linux run;
- the full strict Apple-clang build, native tests, embedded tests, exact arm64
  alignment gate, PTY/torture tests, and advisory performance suite pass.

The remaining review rule is unchanged: do not call the whole sprint complete
while the binary-size and constrained-hardware evidence in §0/§3 is open.

## 8. Current continuation and validation order

1. Reproduce the minimal shipping profile on x86_64 Linux with true GNU GCC
   and inspect the size ledger before proposing any decision-document change.
2. Reconcile the 900 KiB contract conflict explicitly in the sprint/decision
   scaffolding; never silently raise or bypass the gate.
3. Run the pinned QEMU/hardware constrained-target proof and record exact
   toolchain, architecture, artifact, and result evidence.
4. Supply Sprint 56 designated-runner measurements separately from hosted
   advisory evidence.
5. Rerun the relevant Linux GCC/Clang, musl, sanitizer, and embedded lanes
   after any size remediation.
6. Only after those rows close, update the Sprint 57 Definition of Done and
   advance the campaign frontier.

On `nomad-1`, Apple clang is the native compiler and `/usr/bin/gcc` is an
Apple-clang shim. Use the repository's `arm64-macos` target profile. Do not use
the Mac as evidence for x86_64 designated performance or GNU/Linux tooling.
GNU-specific `stat -c`, `size -A`, and strip behavior must continue through
the existing target-specific pathways rather than being papered over in tests.

## 9. Product throughlines that constrain all fixes

- Preserve byte identity, terminal restoration, deterministic rendering, and
  central edit/undo laws ahead of latency or convenience.
- The editor is single-threaded. Concurrency is the poll loop plus subprocesses.
- C is the repository's strict C11 subset: no VLAs, compiler attributes,
  statement expressions, or thread-based escape hatches.
- No new dependencies without an explicit architectural decision.
- Build subprocess argv arrays; never interpolate user data through a shell.
- Runtime discovery remains disk-first by default so distro packages can patch
  their installed `runtime/` tree.
- Embedded generation must contain no timestamps, absolute paths, locale
  ordering, or unstable sorting.
- Excluded feature modules hard-error honestly; they never silently no-op.
- No silent stubs. An unimplemented route names the sprint that owns it.
- Keep latency budgets as gates. Never weaken baselines merely to make a noisy
  machine pass.
- Plugins share the process and VM; capability gates are not a security
  sandbox and must not be described as one.
- AI stays explicitly opt-in. Secrets never enter prompts, argv, logs, or
  errors, and generated text reaches the buffer only through explicit shadow
  acceptance.

## 10. Historical transfer environment

At inspection time, `nomad-1` reported:

```text
hostname: nomad
OS: macOS 26.4.1 / Darwin 25.4.0
architecture: arm64 (Apple T6050)
home: /Users/mfwolffe
free disk: approximately 55 GiB
git: 2.50.1 (Apple)
make: GNU Make 3.81
clang: Apple clang 21.0.0
rsync: openrsync protocol 29
```

The source checkout occupies approximately 17 GiB because it contains 163
`build*` directories. The user explicitly requested that **everything** be
transferred, so the mirror includes `.git`, untracked Sprint 57 work, build
trees, and the in-repository auxiliary worktree. No build directories were
excluded to save space.

The destination checkout did not exist before the transfer. Because the
destination is new, the initial rsync does not use `--delete`.

After the resume was authorized, ignored transferred build artifacts were
removed to recover approximately 33 GiB. The active default, alignment,
embedded, and performance evidence trees were then recreated locally; they
remain ignored build output and are not part of the source checkpoint.

## 11. Historical transfer verification record

The transfer completed on 2026-08-27:

- Full repository rsync: **success**, exit code 0; 17,151,230,220 bytes and
  133,534 files transferred, with all 148,113 entries checked.
- Destination `HEAD`: **verified** as
  `0246f98e93fee97b9fd5df9183efeb1bf3e5222d`.
- Destination dirty manifest: **verified** to match §4, plus this handoff file.
- `.docs/HANDOFF.md` SHA-256: **verified** identical on source and destination
  after the final handoff sync.
- Source-to-destination itemized rsync dry run: **verified empty** after the
  final reconciliation sync.
- Destination size after transfer: approximately 16 GiB on disk, leaving
  approximately 38 GiB free.

Running `git status` independently on both machines refreshed `.git/index`
timestamps, so the first comparison correctly reported only `.git/` directory
metadata and `.git/index` timestamp drift. A final archive-mode reconciliation
was run before the empty dry comparison; no repository content differed.
