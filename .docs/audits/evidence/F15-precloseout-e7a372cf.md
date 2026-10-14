# F15 CI and release engineering pre-closeout evidence

- Date: 2026-10-13
- Baseline: `e7a372cfab1b2d6e353e589ff9084db0305953a1`
- Local host: Darwin arm64 (macOS 26.4.1)
- Linux evidence host: NixOS, Linux 7.2.3 x86_64
- Linux compilers: GCC 15.3.0; Clang 21.1.8
- Linux memory checker: Valgrind 3.27.1

Disposition: **NOT READY — the packet baseline's scoped functional and
governance evidence is green, `YEW-F-138` is fixed, and exact successor Linux
GCC/Clang aggregates are completely green with identical assertion totals.
Independent review and the required sanitizer, memory-checker, designated,
cross-target, hosted, and hardware gates remain.**

This packet prepares F15 for closeout without signing it. Its author also
performed F15's contract reconciliation and remediated findings discovered
during the recheck, so cannot be Sprint 59's independent reviewer. A future
reviewer must independently establish the baseline, inspect or rerun the
cited witnesses, and decide every checkbox in
`.docs/audits/closeout-F15-ci.md`.

## Scope and provenance

The original F15 audit is `.docs/audits/audit-15-ci.md`. It covers the
foundation, test infrastructure, headless batch, latency, size, allocation,
musl, embedded, target, and CI contracts inherited from Sprints 0, 1, 37,
56, and 57. Its original and adjacent finding scope is `YEW-F-024` through
`YEW-F-075`, plus `YEW-F-080`, `YEW-F-086`, `YEW-F-088`, `YEW-F-096`,
`YEW-F-097`, and `YEW-F-124`. The Sprint 59 closeout recheck added
`YEW-F-127` through `YEW-F-135`. Subsequent complete-regression passes added
`YEW-F-136` through `YEW-F-138`, for 70 F15 findings in all.

At the named packet baseline all 135 then-filed entries in the global audit
registry pass, the remediation ledgers report zero active XFAILs, and the next
finding id was `YEW-F-136`. Follow-up remediation commits through `87a495cd`
fix that finding, the later selected-build documentation defect, and the
passing unit suite's scheduler-dependent assertion total. The current ledgers
contain 138 fixed entries, zero active XFAILs, and reserve `YEW-F-139` next.
F15 records one Critical, one High, 68
Medium, zero Low, and four unverified observations. No severity was waived or
deferred.

The current checkout was copied to a fresh staging tree on `hasu`; local and
remote `git rev-parse HEAD` both returned the full baseline above. Native
NixOS does not expose standard FHS paths such as `/bin/sh`, so command and
integration evidence ran in its `steam-run` FHS namespace. Results from that
GCC 15/Nix linker are valid functional and portability evidence, but are not
substituted for the pinned GCC 13.3 size lane or designated performance
runner.

The separate Sprint 58 tier-1 soak remains frozen against product
`4c99e134` plus runner-only child `4f483f31`. It has eight durable
`fuzz_utf8`/`fuzz_grapheme` rows (8/56 overall), while all four
`fuzz_textbuf` streams are active. This work did not change, stop, or restart
that campaign.

## Sprint 59 closeout recheck and remediation

The recheck and follow-up complete-regression work found and closed twelve
Medium defects without deleting product features or weakening a quality,
performance, or size gate:

- `YEW-F-127` replaces obsolete landing-day hard-error requirements with
  the shipped file/TUI, clean, batch, script, PTY, fuzz, replay, and grant
  owner surfaces.
- `YEW-F-128` removes the impossible self-referential compatibility symlink
  left by the project rename.
- `YEW-F-129` routes raw foundation greps through the token-aware ban and
  semantic registration controls, so comments and syntax-corpus data do not
  masquerade as implementation violations.
- `YEW-F-130` distinguishes Sprint 1's historical unit-and-smoke timing from
  the expanded current aggregate suite and its dedicated CI/performance
  budgets.
- `YEW-F-131` makes Sprint 56/57 name the shipped `make size` owner and the
  binding eight-row budget table rather than superseded caps or a future
  stub. No cap was raised by this correction.
- `YEW-F-132` changes the reserved `--batch-strict` diagnostic from a false
  Sprint 59 delivery promise to the truthful 1.0 non-availability error.
- `YEW-F-133` keeps the oversize-theme fixture inside the selected disjoint
  build tree by honoring its `TMPDIR`.
- `YEW-F-134` makes the plugin containment fixture compare a standard FHS
  path with its canonical target, permitting `/etc/passwd` itself to be a
  symlink without weakening containment.
- `YEW-F-135` makes temporary governance repositories disable inherited
  commit signing locally, so a developer's global Git policy cannot hang or
  invalidate the selftests.
- `YEW-F-136` gives the generated chapter-7 tutor repository deterministic
  local identity, signing, and hook policy without changing Git behavior in
  ordinary user workspaces.
- `YEW-F-137` makes the man, manual, and documentation-sync gates and their
  negative selftests consume the selected `BUILD=` binaries and helpers.
- `YEW-F-138` makes successful asynchronous clipboard-poll clock reads
  uncounted while preserving fatal clock failures, so passing assertion totals
  do not vary with scheduler timing.

Amendments S59-A35 through S59-A40 bind the affected source-sprint and F15
contract corrections. F133 through F138 are portability and deterministic-test
fixes discovered by the evidence campaign.

### Follow-up complete-regression result

The complete GCC 15 `make test` attempt on the exact packet baseline passed
the warning-clean build, all then-current audit/governance gates, the complete
script suite, and every PTY before chapter 7. `tutor_ch07` then failed to
record its final task: the generated repository had all three lesson files
staged but no interactive commit under the host account's inherited
`commit.gpgsign=true` policy. The preserved sandbox and Git history isolated
that failure as `YEW-F-136`; it was not waived as infrastructure noise.

Commit `c99b4528` localizes identity, signing, and hook policy to the generated
tutor repository. On the same hostile local policy, the focused chapter-7 PTY
now passes and asserts every local setting plus the clean commit postcondition.
The complete arm64 macOS `make test-audit` target also passes all 138 fixtures
and every priority, sampling, closeout, deferral, and remediation-ledger
selftest with zero active XFAILs.

The next exact Linux GCC aggregate passed every registered PTY case, the
complete 93-case script suite, tutor units, the tutor CLI and rot drill,
and all twelve tutor PTYs—including chapter 7—before exposing `YEW-F-137` in
`test-manual`: its Make target requested a nonexistent default-tree binary
instead of the selected disjoint build. After `060cdee3`, a clean witness with
no default `build/yew` passed `test-man`, `test-docs-sync`, and `test-manual`
plus all three negative selftests from `build-f15-gcc`. The Linux host lacks
mandoc, so its focused man witness intentionally skipped formatter/redirect
resolution; the local mandoc check is green and complete cross-host formatter
evidence remains an explicit blocker.

Exact complete GCC and Clang aggregates at `c7db3e57` subsequently passed
all 137 then-registered audit fixtures and governance checks, every registered
PTY, 93 scripts with 951 assertions and one declared skip, every tutor gate,
the documentation/manual gates, 432 syntax assets, release selftests, the
complete unit suite, repository bans, smoke, and live torture. The GCC unit
summary reported 73,852,078 assertions while Clang reported 73,852,072; both
had 3,047 tests, zero failures, and two declared skips. Repeated isolated runs
then showed the unchanged clipboard selection test alternating between 51 and
52 assertions because its clock helper counted each asynchronous poll turn.
`YEW-F-138` records that deterministic-gate defect. Commit `87a495cd` leaves a
clock failure fatal through an uncounted failure path, and 100 focused runs now
all report 49 assertions.

Fresh disjoint GCC 15.3 and Clang 21.1 aggregates at exact clean successor
`0c8dce52` then completed with exit 0. Both passed all 138 audit fixtures and
governance checks, every registered PTY, 93 scripts with 951 assertions and
one declared skip, every tutor gate, the documentation/manual gates, the
round-trip P1-P5 gate, release and embedding selftests, the package suite,
repository bans, smoke, and live-save torture. Each compiler reported exactly
3,047 unit tests, 73,852,064 assertions, zero failures, and two declared
skips. This closes the complete-aggregate portion of `YEW-F-138`; it does not
substitute for the separate byte-compared determinism lane.

## Current-baseline executable evidence

### Compiler, unit, sanitizer, and memory-checker matrix

Fresh full-module aggregates at exact clean successor `0c8dce52` under GCC
15.3 and Clang 21.1 completed with the locked warnings enabled and identical
unit results:

| Configuration | Tests | Assertions | Failures | Notes |
|---|---:|---:|---:|---|
| GCC, full modules | 3,047 | 73,852,064 | 0 | complete `make test`; 2 expected plain-build LeakSanitizer skips |
| Clang, full modules | 3,047 | 73,852,064 | 0 | complete `make test`; same 2 expected skips |
| GCC, `MODULES=''` | 2,540 | 72,821,504 | 0 | 2 expected plain-build skips |

The Nix Clang package lacks `sanitizer/lsan_interface.h`, so the focused
ASan/UBSan evidence used GCC rather than misrepresenting that host-toolchain
limitation as a product failure. The focused run covered args, batch,
profiler, calibration, allocation, and module-boundary units: 75 tests,
9,620 assertions, zero failures. Focused Valgrind runs covered args,
profiler, calibration, allocation, and module boundaries: 53 tests, 8,501
assertions, zero failures, and the configured error exit was not triggered.
These focused runs do not replace the contracted complete sanitizer and
Valgrind lanes.

### CLI, modules, scripts, PTY, and batch durability

The full build reports exactly `yew 1.0.0-dev` and
`modules: lsp ai fuss plugins`. The empty-module build reports
`modules: none`; help exits 0; an unknown option exits 1; and
`--batch-strict` exits 1 with:

```text
yew: error: option '--batch-strict' is not available in yew 1.0
```

In the empty build, `plug list` and `pkg list` both exit 1 with the shared
plugin-module diagnostic. The selected-build module boundary and target-tool
selftests pass.

`make test-script`, its double-run determinism owner, and its budget owner
all pass: 93 tests, 951 assertions, zero failures, and one declared skip.
The suite took 10.573656009 seconds against its 20-second ceiling. The PTY
runner selftest and `s37_batch_never_touches_the_terminal` pass. A fresh
200-attempt SIGKILL batch torture completed with zero corruptions: 16 runs
retained the old file, 184 retained the new file, and 158 journals were
observed and recovered cleanly.

The state-root/log witness created mode 0700 state, kept version stdout and
stderr exact, and recorded `--selftest-bug` in the log while the CLI exited 4
with its pinned diagnostic.

### Governance, portability, install, and determinism controls

The complete packet-baseline `make test-audit` target passes under GCC 15 with
135 registry tests and zero failures. The finding, fixture, priority, sampling, closeout,
deferral, and remediation-ledger checks and all their negative selftests pass
under the account's original `commit.gpgsign=true` global policy. There are
zero deferred or wontfix findings and zero active XFAILs.

The size-tool, size-memory, module-boundary, target-tool, static-PIE-tool,
runtime-blob, generated-artifact two-compiler determinism, embedded-image,
embedded-fixture, embedded-disk, and embedded-QEMU selftests pass. These
selftests validate the gates; they do not claim the actual musl or 64/32 MiB
guest milestones.

A staged install under `/usr` contains an executable, all manuals, runtime
assets, and tutor assets: 102 installed regular files. An explicit
`build-f15-clean-probe` sentinel tree was removed by `make clean` without any
broad deletion. A separate tree relinked full -> minimal -> full without a
clean and reported the correct module set after every transition.

Two disjoint GCC builds on the same Linux host produced byte-identical
binaries with SHA-256
`4f3164164f8eb5464698b22b41f5ada1e70c6c9aa9b281d9d8f36dd11c078a3a`.
This is same-host reproducibility evidence only, not the required
same-target/two-machine result.

### Allocation and performance controls

The debug allocation gate reports zero allocations in steady-state
render-frame, syntax-line, regex-exec, layout, navigation, and typing rows;
insertion uses 2 calls against a ceiling of 40, and the 100 MB open row uses
7 calls against a ceiling of 2,000. Every live count returns to zero. The
allocation target passes 6 units and 8,050 assertions.

The performance default-path, baseline-v2 (16 checks), startup-policy,
profiler-crosscheck-policy, noise-policy, baseline-guard, run/update-suite,
and S56 anti-flap (12/12) selftests pass. Their deliberately injected strict
failures and advisory warnings are expected negative controls. Calibration
on `hasu` measured clock vectors 35,955,526; 6,335,891; and 21,522,230, but
correctly remained ADVISORY because no matching designated reference vector
exists. No strict timing, RSS, frame-count, or profiler-overhead result is
claimed from the loaded evidence host.

### Size provenance

The current eight binding caps are present with their amendment provenance:

| Profile | Cap (bytes) |
|---|---:|
| full | 2,277,376 |
| minimal | 1,785,856 |
| lsp-only | 1,900,544 |
| ai-only | 1,916,928 |
| fuss-only | 1,933,312 |
| plugins-only | 1,884,160 |
| musl-full | 2,424,832 |
| musl-minimal | 1,900,544 |

The non-authoritative GCC 15/Nix linker output exceeds all six hosted GCC
13.3 caps by a uniform 48,224 to 68,872 bytes: full 2,346,248; minimal
1,838,112; lsp-only 1,965,088; ai-only 1,973,448; fuss-only 1,997,856; and
plugins-only 1,932,384. This is recorded as toolchain/linker provenance, not
as permission to move a gate or remove a feature. The pinned GCC 13.3 and
musl rows remain required.

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

### Sprint 0 — repository and foundations

| Row | Primary witnesses |
|---|---|
| s00.1 | warning-clean complete `make test` aggregates under GCC and Clang at exact clean successor `0c8dce52` |
| s00.2 | exact full- and empty-module version output, both exit 0 |
| s00.3 | help/unknown-option CLI witnesses, current registered owner tests, and the complete TUI/batch/clean aggregates under both compilers |
| s00.4 | exact selftest exit-4 stderr and matching log witness |
| s00.5 | current exit-site semantic scan confines the OS exit boundary to `util/log.c`/`main` ownership |
| s00.6 | passing token-aware bans and seeded negative selftests |
| s00.7 | exact quiet version output, mode-0700 state root, and log separation witness |
| s00.8 | 3,047 full-module unit tests / 73,852,064 assertions per compiler, including container/log owners |
| s00.9 | same-tree full -> minimal -> full relink witness |
| s00.10 | 102-file staged install and scoped clean-probe removal |
| s00.11 | source assets and exact clean staged checkout before and after both complete aggregates |

### Sprint 1 — test infrastructure

| Row | Primary witnesses |
|---|---|
| s01.1 | complete current `make test` aggregates under GCC and Clang, including the full unit registry and complete script suite |
| s01.2 | runner format controls and same-host binary identity; complete aggregate double-run stdout remains outstanding |
| s01.3 | explicit 3,047-test current registry with no bare provisional-test path |
| s01.4 | unit-runner filter/list/zero-match selftests in the passing registry |
| s01.5 | structural logging and sink-restoration tests in the passing registry |
| s01.6 | token-aware bans plus seeded qsort/registration negatives |
| s01.7 | semantic explicit-registration and no-constructor controls |
| s01.8 | complete script suite and selected PTY; the complete fuzz and PTY target matrix remains outstanding |
| s01.9 | both full unit compilers plus focused ASan/UBSan and Valgrind; full sanitizer, Valgrind, and hosted `trunk` lanes remain outstanding |
| s01.10 | byte-identical disjoint same-host product binaries; full-suite double run and independent-machine identity remain outstanding |
| s01.11 | empty-module 2,540-test registry, exact version output, CLI failures, and module-boundary control |

### Sprint 37 — headless batch

| Row | Primary witnesses |
|---|---|
| s37.1 | complete GCC/Clang aggregates, full units/scripts/PTYs, and focused sanitizer/Valgrind; complete sanitizer and Valgrind lanes remain outstanding |
| s37.2 | headless PTY no-terminal case and guarded-TTY semantic control |
| s37.3 | passing batch startup/selfcheck units and seeded invariant owner |
| s37.4 | passing batch stdout/stderr/flush script and unit owners |
| s37.5 | exit-code and exact diagnostic golden owners in the 93-case script suite |
| s37.6 | explicit-save, unsaved-warning, and changed-on-disk owners in the passing suite |
| s37.7 | batch command-refusal table and registry-loop owners |
| s37.8 | passing script runner selftests plus double-run determinism target |
| s37.9 | all assertion-surface positive/negative and byte-comparison runner selftests |
| s37.10 | passing migrated script corpus and provenance/registration controls |
| s37.11 | fresh 200-attempt kill-9 run: zero corruptions, 158 journals recovered |
| s37.12 | full script suite 10.573656009 s <= 20 s; designated strict batch-startup evidence remains outstanding |
| s37.13 | current batch documentation/help/manual synchronization controls |
| s37.14 | 93 scripts / 951 assertions, passing args owners, replay/grant coverage, and truthful `--batch-strict` non-goal |

### Sprint 56 — latency gates

| Row | Primary witnesses |
|---|---|
| s56.1 | compiler/unit, focused sanitizer/Valgrind, bans, and performance-policy selftests; complete strict `make perf` matrix remains outstanding |
| s56.2 | profiler layout/ring/default-off units; designated <=2% end-to-end overhead measurement remains outstanding |
| s56.3 | 17 profiler units / 201 assertions in the focused sanitized run plus format/command owners |
| s56.4 | 8 calibration units / 49 assertions and an honest ADVISORY host calibration; matching designated scales/vectors remain outstanding |
| s56.5 | budget-policy controls; designated strict latency, 100 MB, scroll, search, and RSS measurements remain outstanding |
| s56.6 | crosscheck/floor policy selftests; designated seven-combination measurements remain outstanding |
| s56.7 | hard frame-budget negative control; designated frame measurements remain outstanding |
| s56.8 | seeded strict/advisory policy controls pass |
| s56.9 | baseline-v2 structure, why-field, directory, and ban controls pass |
| s56.10 | anti-flap policy selftests pass 12/12, including slowdown and mixed-change negatives |
| s56.11 | committed sessions and structural controls are present; full current assist/ghost-text sessions were not rerun for this packet |
| s56.12 | README/triage and disjoint perf-symbols controls pass |
| s56.13 | committed `100m-allnl` disposition remains owned by the current contract |
| s56.14 | shipped size owner plus profiler/calibration assertion totals above 140; authoritative size execution remains outstanding |

### Sprint 57 — size and embedded

| Row | Primary witnesses |
|---|---|
| s57.1 | compiler units and portable gate selftests; authoritative size, musl, embedded, full sanitizer/Valgrind, target, arm64, and hosted CI lanes remain outstanding |
| s57.2 | binding eight-cap table, measurement README, and size-tool selftests; pinned GCC 13.3 execution remains outstanding |
| s57.3 | additivity-control selftests and binding budget owner; pinned result remains outstanding |
| s57.4 | ledger/meta-tool selftests; pinned fresh-clone ledger regeneration remains outstanding |
| s57.5 | size growth/guard negative selftests, including mixed-change rejection |
| s57.6 | debug allocation gate: zero steady-state allocations, 2 insert calls, 7 open calls, zero live residue, 8,050 assertions |
| s57.7 | RSS policy controls; strict full/minimal/musl RSS profiles remain outstanding |
| s57.8 | bans and static-PIE tool selftests; an actual musl-static artifact inspection remains outstanding |
| s57.9 | resolver/hosts unit owners; the current musl client matrix with full/minimal/no hosts was not rerun |
| s57.10 | musl determinism/budget/ledger-header evidence remains outstanding |
| s57.11 | embedded image/fixture/disk/QEMU selftests pass; actual 64 MiB/32 MiB guests and physical Pi corroboration remain outstanding |
| s57.12 | empty-module full unit registry, CLI, symbol, shim, and parity controls; complete stripped PTY/Fletch/script/editor matrix remains outstanding |
| s57.13 | current-baseline arm64 Linux/macOS full lanes remain outstanding; the older exact product anchor is hosted-green but is not this baseline |
| s57.14 | target-tool, token-ban, module-boundary, allocation, and ledger meta-controls pass |

### XFAIL ceiling

The closeout's recorded ceiling is eight. The current audit registry,
remediation ledgers, and Fletch debt file contain zero active XFAIL ids. The
existing checked XFAIL row is supported, but remains for the independent
reviewer to confirm alongside the 64 sprint rows.

## Explicit blockers before READY

- An independent non-author reviewer has not inspected F15 or assigned its
  64 sprint-row verdicts on the current baseline.
- The complete ASan/UBSan and Valgrind lanes, the full PTY double run, and
  byte-compared aggregate determinism remain outstanding. Exact post-F138 GCC
  and Clang `make test` aggregates at clean successor `0c8dce52` are green
  with identical 73,852,064-assertion unit summaries.
- Strict Sprint 56 designated-runner calibration, latency, noise,
  profiler-crosscheck, frame-count, allocation-overhead, and RSS results are
  outstanding; the loaded `hasu` run is advisory only.
- The authoritative pinned GCC 13.3 size/ledger lane, both musl-static rows,
  the complete module matrix, and the required Linux target jobs remain
  outstanding. The GCC 15/Nix sizes do not authorize gate changes.
- The actual embedded 64 MiB and 32 MiB QEMU gate, peak-memory measurement,
  and physical Pi corroboration remain outstanding.
- Same-target binary identity across two independent machines remains
  outstanding; this packet proves only two disjoint builds on one host.
- Current hosted CI is unavailable until this unpushed baseline reaches a
  hosted branch. The older hosted-green product anchor does not replace it.
- The Sprint 58 tier-1 soak remains 8/56. Sprint 60's release-candidate soak
  is a separate future exact-release-commit gate.
- Local groff/mandoc release evidence and any complete cross-target manual
  install/open checks not covered by portable selftests remain outstanding.

Accordingly `.docs/audits/closeout-F15-ci.md` remains
`Reviewer: UNASSIGNED`; its sprint boxes remain unchecked and its verdict
remains `NOT READY`.
