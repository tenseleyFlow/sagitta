# F14 plugins and packages pre-closeout evidence

- Date: 2026-10-13
- Baseline: `be8f65e5986768053228eb739e96029102b82200`
- Host: Darwin 25.4.0 arm64 (macOS 26.4.1)
- Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0

Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and required release/platform campaigns remain.**

This packet prepares F14 for closeout without signing it. Its author also
authored F14's contract reconciliation and the two findings discovered while
assembling this matrix, so cannot be Sprint 59's independent reviewer. A
future reviewer must establish the baseline independently, inspect or rerun
the cited witnesses, and decide every checkbox in
`.docs/audits/closeout-F14-plugins.md`.

## Scope and provenance

The original F14 audit is `.docs/audits/audit-14-plugins.md`. It reviewed
detached product baseline `b3f32645` and covered resolved manifest
containment, callback capability provenance, lifecycle residue, trust
preservation, package verification and failed publication, honest security
wording, error-limit teardown, save-hook containment, and recorder/CMDWORD
integration.

Its three original findings remain fixed at the current baseline:

- `YEW-F-021` reclaims raw hook and registration-ledger tombstones during
  plugin teardown.
- `YEW-F-022` makes the documentation control require the honest negative
  memory/resource-isolation warning rather than banning the required word.
- `YEW-F-023` gives plugin commands stable CMDWORD ownership so recording,
  replay, and command collisions share the normal registry boundary.

The Sprint 59 closeout recheck found and closed four more Medium defects:

- `YEW-F-122` / S59-A32 aligns obsolete package/manual staging and
  contradictory documentation rules with their shipped owners.
- `YEW-F-123` / S59-A33 names binding Fletch Amendment A3 for the runtime
  import row instead of unrelated Amendment A2.
- `YEW-F-125` keeps a mixed Fletch API unit linkable when FUSS is excluded by
  guarding only its synthetic Git/FUSS state.
- `YEW-F-126` / S59-A34 aligns Sprint 54's stripped-plugin wording with the
  older shared module diagnostic already used by the product.

The GCC package run also exposed `YEW-F-124`, recorded under F15 because it is
a compiler-matrix harness defect. The package runner now uses the established
Apple compiler seam only for its arm64/arm64e DYLD interposer while retaining
GCC for product and test objects.

At this baseline all 126 audit registry entries pass and the remediation
ledgers report zero active XFAILs. The separate Sprint 58 tier-1 soak remains
frozen against product `4c99e134` plus runner-only child `4f483f31`. It has
four durable `fuzz_utf8` rows (4/56 overall), and four `fuzz_grapheme` streams
remain active. This work did not change or restart that campaign.

## Current-baseline executable evidence

### Compiler, sanitizer, and focused test matrix

Apple Clang, Homebrew GCC 16, and Apple Clang ASan/UBSan built the focused
plugin, package, audit, fuzz, PTY, and performance surfaces under the locked
warning policy with zero warnings. Each configuration produced these same
functional results:

| Surface | Result per configuration |
|---|---:|
| plugin units | 66 tests / 27,193 assertions |
| shipped plugin example scripts | 3 / 98 |
| Sprint 55 cloud/import scripts | 2 / 34 |
| audit registry | 126 tests / 0 failures |
| package integration | 53 passed / 0 failed |
| focused Sprint 54 PTYs | 9 passed |

The nine PTYs cover capability prompting, allow-once, allow-always and
restart persistence, deny-once, deny-always, picker rendering in normal and
`NO_COLOR` modes, and enable/disable toggling. PTY runners were serialized
because the harness owns shared temporary state.

The complete governance target is green in the default build: the 126-entry
registry and special invariant binaries pass; finding, fixture, priority,
sampling, closeout, deferral, and remediation-ledger checks and their negative
selftests pass. It reports next ID `YEW-F-127`, zero active XFAILs, and zero
deferred/wontfix findings.

### Compile-time module boundaries

An isolated Apple Clang `MODULES=plugins` build, with FUSS excluded, builds
both `yew` and the unit binary warning-clean. It passes all 66 plugin tests,
the module-neutral part of the mixed Fletch state-query test (42 assertions),
and all 53 package integration cases.

An empty `MODULES=''` build also builds `yew` and the unit binary
warning-clean. Its shared module diagnostic test passes 40 assertions, two
cross-module command tests pass 428 assertions, and the mixed state-query
test passes its same 42 module-neutral assertions. Both `yew plug list` and
`yew pkg list` exit 1 with exactly:

```text
this build has no plugins module; rebuild with 'make MODULES="… plugins"'
```

The corresponding full-module Fletch state-query test passes 57 assertions
under Clang, GCC, and ASan/UBSan, including its active Git/FUSS state.

### Package durability and compiler ownership

All four package configurations—default Clang, GCC, ASan/UBSan, and
plugins-only/no-FUSS Clang—pass the complete 53-case integration suite. The
suite covers install/update/remove/list/doctor, ref and option injection,
corrupt locks, offline and timeout behavior, deterministic installs, hostile
names, no repository-code execution, failed publication, and all five
kill-boundary durability/recovery cases.

The initial GCC run passed its first 49 cases and then failed before the
durability tail because the test runner sent Apple's `-arch arm64e` option to
Homebrew GCC. `YEW-F-124` made that failure executable. After `7cdbe9d1`, the
complete GCC suite passes all 53 cases while the product remains GCC-built.

### Fuzz and deterministic samples

Bounded deterministic samples produced the same corpus counts and hashes
under Clang, GCC, and ASan/UBSan:

| Campaign | Iterations | Corpus | Hash |
|---|---:|---:|---|
| Fletch/manifest parse | 10,000 | 120 | `d4c2e4e106e5cdeb` |
| package tree | 1,000 | 5 | `5e701200b5642beb` |

An attempted 10,000-iteration package-tree sample was stopped without a
result after roughly three and a half minutes to avoid competing indefinitely
with the four-core soak. No result from that interrupted attempt is claimed.
The 1,000-iteration deterministic samples are supporting evidence only; they
do not replace either sprint's contracted one-hour campaign.

### Performance controls

`perf_plug --selftest-policy` passes under Clang, GCC, and ASan/UBSan. Package
tree, lockfile, and cloud idle-scan binaries also completed as advisory
correctness runs. The default Clang sample reported package-tree median/p99
840/1,092 microseconds, lock load/save 273/443 microseconds, and cloud idle
scan 204/267 microseconds.

The ASan/UBSan advisory sample reported a plugin-overlay p99 warning (584
microseconds against 200) and package-lock p99 warning (1,198 microseconds
against 1,000); other sampled rows were within their budgets. These runs were
made while four soak workers occupied the host. They are recorded as
contaminated advisory observations, not strict performance verdicts.

### Static ownership and documentation gates

`scripts/bans.sh`, `scripts/check-module-shims.sh`,
`scripts/check-plugin-docs.sh`, and `scripts/check-man.sh` pass. `groff` is not
installed on this host, so the man checker explicitly skipped only its groff
lint while passing the repository-owned contract. Semantic scans establish:

- no `src/mod/git` reference or `yew_git_` symbol under `src/mod/plug/`;
- no `system`, `popen`, or `git pull` call under `src/`;
- no plugin/package silent-success action in an excluded-module shim;
- the authoring and package manuals retain the required negative isolation
  warning and all current command/exit-code sections; and
- `docs/man/yew-pkg.1` is present and owned by the man-page checker.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-021` | 20-by-20 lifecycle and raw-registry-baseline units plus the passing audit control |
| `YEW-F-022` | semantic plugin documentation checker, S59-A32, and the passing audit control |
| `YEW-F-023` | plugin command record/replay and collision units plus the passing audit control |
| `YEW-F-122` | synchronized s54/s55/F14 staging and documentation contracts under S59-A32 |
| `YEW-F-123` | Sprint 55/F14 cite binding Amendment A3 and its conformance fixture under S59-A33 |
| `YEW-F-124` | all 53 GCC package cases, including five durability cases, pass with the host-only shim compiler |
| `YEW-F-125` | plugins-only unit link plus 42 stripped and 57 full state-query assertions |
| `YEW-F-126` | empty-module CLI witnesses and synchronized s54/F14 shared diagnostic under S59-A34 |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

### Sprint 54 — plugin system

| Row | Primary witnesses |
|---|---|
| s54.1 | full warning-clean Clang/GCC/ASan focused builds; empty-module byte-exact CLI witnesses; exact full-suite/Valgrind lanes remain outstanding |
| s54.2 | complete manifest unit slice and 10,000-iteration parser sample; one-hour campaign remains outstanding |
| s54.3 | workspace/block/shadow discovery units and list behavior fixtures |
| s54.4 | reverse-disable, 20-by-20 lifecycle, and raw-registry-baseline units under F021 |
| s54.5 | failing-init zero-residue and trace unit |
| s54.6 | fifth-error, multi-plugin drain, hostile-event, and throwing-disable units |
| s54.7 | closed capability matrix, preflight-before-evaluation, undeclared callback, and six prompt-decision PTYs |
| s54.8 | schema/truncation/sorted trust units plus allow/deny restart PTYs |
| s54.9 | nine focused PTYs per configuration; designated double-run determinism lane remains outstanding |
| s54.10 | plugin list lifecycle/state units and stable script fixtures; exact double-run CLI witness remains for reviewer |
| s54.11 | performance policy selftest and advisory samples; unloaded strict CI timings remain outstanding |
| s54.12 | S59-A32, semantic plugin-doc checker, package non-goal integration cases, and current man-page owner |

### Sprint 55 — package manager and cloud preset

| Row | Primary witnesses |
|---|---|
| s55.1 | 53 package cases in four configurations, plugins-only/no-FUSS build, empty plugin-link scan, and F124/F125 controls; exact complete make-test/Valgrind/determinism lanes remain outstanding |
| s55.2 | empty process-ban scan, structural package argv/ref units, and hostile option/path integration cases |
| s55.3 | lock round-trip/order/corruption units and deterministic three-install integration cases |
| s55.4 | tree-hash sensitivity/independence units and identical bounded sanitizer fuzz samples; one-hour tree campaign remains outstanding |
| s55.5 | managed-drift revocation unit and doctor drift integration cases |
| s55.6 | fast-forward, force-push, moved-tag, changelog-failure, and dry-run integration cases |
| s55.7 | absent-Git, unreachable, timeout, offline list/doctor, and startup-boundary controls |
| s55.8 | no-repository-code, postinstall rejection, and sentinel integration cases |
| s55.9 | shipped-example local-repository install case plus 3 example scripts / 98 assertions |
| s55.10 | cloud/import scripts, A3 conformance owner, and option/default controls |
| s55.11 | retained save/backup unit owners; contracted 100-percent kill torture was not rerun for this packet |
| s55.12 | conflict-shape/counting units and cloud idle-scan advisory sample; unloaded strict timing remains outstanding |
| s55.13 | passing plugin-doc checker and current authoring/package manuals |
| s55.14 | package permanent-non-goal cases and passing assembled-man checker; groff lint remains unavailable locally |
| s55.15 | 66 plugin units / 27,193 assertions, 53 package cases, and performance controls/baselines |

## Explicit blockers before READY

- An independent non-author reviewer has not inspected F14 or assigned its 27
  closeout verdicts.
- The exact complete current-baseline `make test` matrix under both compilers,
  ASan/UBSan, Valgrind, full modules, and `MODULES=''` was not rerun for this
  local packet. Darwin has no project Valgrind/LSan lane.
- The contracted one-hour manifest and package-tree sanitizer campaigns were
  not rerun; bounded deterministic samples are supporting evidence only.
- Strict plugin/package/cloud timing must be measured on the designated
  unloaded runner after the Sprint 58 soak releases the host.
- The Sprint 55 in-place save/backup kill-torture contract, exact PTY/CLI
  determinism reruns, hosted CI, and required non-Darwin/platform evidence
  remain to be established on the eventual closeout baseline.
- Local man checking passed, but groff lint requires a host with groff.

Accordingly `.docs/audits/closeout-F14-plugins.md` remains
`Reviewer: UNASSIGNED`, all boxes remain unchecked, and its verdict remains
`NOT READY`.
