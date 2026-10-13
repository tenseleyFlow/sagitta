# F13 Git and FUSS pre-closeout evidence

- Date: 2026-10-13
- Baseline: `13e41f1fee206a624cb4557fad4f264b1dd84781`
- Host: Darwin 25.4.0 arm64 (macOS 26.4.1)
- Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0

Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and required release/platform campaigns remain.**

This packet prepares F13 for closeout without signing it. Its author also
authored F13's contract reconciliation and therefore cannot be Sprint 59's
independent reviewer. A future reviewer must establish the baseline again,
independently inspect or rerun the cited witnesses, and decide every checkbox
in `.docs/audits/closeout-F13-git.md`.

## Scope and provenance

The original F13 audit is `.docs/audits/audit-13-git.md`. It reviewed detached
product baseline `b3f32645` and covered porcelain-v2 parsing, Git verb and argv
ownership, hostile paths, injected cache time, failed publication, repository
state probes, child environment isolation, FUSS expansion memory, and directory
group opening.

Its four product/control findings remain fixed at the current baseline:

- `YEW-F-017` routed interactive rebase through the canonical Git job policy.
- `YEW-F-018` injected the FUSS display clock and replaced a raw clock-name scan
  with a token-aware call audit.
- `YEW-F-019` made rename record consumption mutation-sensitive.
- `YEW-F-020` replaced the raw formatting-name prohibition with structural
  argv controls.

The current closeout recheck found and closed two contract-only Medium defects
without changing product behavior:

- `YEW-F-120` / S59-A30 aligns obsolete Sprint 51/52 staging and bare-key rules
  with the shipped Sprint 52/53 and compact-FUSS model.
- `YEW-F-121` / S59-A31 replaces raw clock, formatting, and `strlen` scans with
  semantic owner checks that distinguish byte lengths from display-cell width.

At this baseline all 121 audit registry entries pass, the remediation ledgers
report zero active XFAILs, and F13 has no open confirmed finding. The Sprint 58
tier-1 soak remains frozen against product `4c99e134` plus runner-only child
`4f483f31`. Its four grapheme streams occupied four CPU cores while this packet
was assembled. This work did not change or restart that campaign.

## Current-baseline executable evidence

### Strict builds, audit registry, and focused units

Apple Clang, Homebrew GCC 16, and Apple Clang ASan/UBSan built the focused Git,
FUSS, audit, fuzz, PTY, helper, and performance-test surfaces under the locked
warning policy with zero warnings. Each configuration produced the same audit
result:

```text
audit: 121 tests, 0 failures
```

The complete governance target is green: the 121-entry registry and special
invariant binaries pass; finding, fixture, priority, sampling, closeout,
deferral, and remediation-ledger checks and their negative selftests pass. It
reports next ID `YEW-F-122`, zero active XFAILs, and zero deferred/wontfix
findings.

The following unit filters passed identically under Clang, GCC, and
ASan/UBSan. Filters intentionally overlap; their counts are independent focused
slices, not a sum.

| Filter | Tests / assertions per configuration |
|---|---:|
| `git` | 114 / 217,958 |
| `fuss` | 86 / 3,850 |
| `porcelain` | 8 / 1,758 |
| `blame` | 25 / 149, with the harness's expected no-LSan skip |
| `groupfromdir` | 5 / 170 |

The real-repository integration helpers also passed identically in all three
configurations:

| Helper | Result per configuration |
|---|---:|
| Git layer | 15,372 assertions |
| FUSS commands | 544 assertions |
| Git hunks | 254 assertions, 20 patches |
| directory groups | 5,620 assertions |

Together these cover every porcelain record and XY mapping, all-byte
truncation, rename NUL consumption, structural verb/env policy, killed-refresh
publication, 100-request blob reuse, cache invalidation, hostile filenames and
argv, Git diff/hunk generation, blame reuse, FUSS tree construction/navigation,
remembered expansion, visible-row type jump, effective action bindings, commit
cleanup, and sorted one-read directory groups.

### PTY integration

Fifty-eight focused PTY cases passed sequentially in each configuration:

- 35 `fuss_` cases cover drawer startup and layout, Unicode/ASCII and compact
  widths, remembered expansion, depth navigation, type jump, mouse menus,
  group picker/close, the effective-binding actions palette, diff layout
  restoration, first-frame loading, confirmation, conflict/ignored markers,
  both leave keys, and non-repository workspaces.
- 23 `git_editor_` cases cover blame width/staleness, side-by-side fillers and
  synchronized scrolling, every gutter sign outcome including digit-width
  transition, and normal/no-upstream/detached/unborn plus all in-progress and
  conflicted statusline states.

The runners were serialized because the PTY harness owns one fixed
`build/pty-*` state namespace. No concurrent runner result is included.

### Static ownership and repository gates

`scripts/bans.sh` passes. Current semantic scans also establish:

- no temporary-file API, `system`, or `popen` call under `src/mod/git/`;
- no native `inotify`, `kqueue`, or `fanotify` watcher under `src/`;
- no direct `wcwidth` call in `fussmode.c` or `fusstree.c`;
- no Git/job spawn from `src/ui/statusline.c`;
- no `checkout` path in `src/mod/git/gutter.c`;
- no `YEW_CMD_DEFERRED` in the Git module; and
- all direct `YewJobSpec` construction remains in `src/mod/git/git.c`, where
  the structural verb, environment, callback-input, and terminal-rebase unit
  controls exercise it.

The tokenized clock and structural argv controls also pass as audit entries
`YEW-F-018` and `YEW-F-020`; safe display formatting and byte-length operations
are not mislabeled as process or cell-width defects.

### Fuzz and deterministic samples

Bounded deterministic campaigns ran for 10,000 iterations at seed 1 under
Clang, GCC, and ASan/UBSan. Each configuration produced the same corpus count
and hash:

| Campaign | Corpus | Hash |
|---|---:|---|
| FUSS tree/state | 5 | `7f6650298b07dd0e` |
| Git diff | 5 | `7f6650298b07dd0e` |
| gitignore | 5 | `7f6650298b07dd0e` |
| porcelain v2 | 5 | `83b7565d59b80d56` |

These are supporting deterministic samples. They do not replace the contracted
long sanitizer campaigns or the original F13 audit's four independent
200,000-iteration porcelain streams.

### Performance harness controls

The three F13 performance binaries' non-timing policy controls passed under
Clang, GCC, and ASan/UBSan:

```text
perf-git-status-policy: strict/advisory/sanity ok
perf-fuss-policy: strict/advisory/sanity ok
perf-git-gutter-policy: strict/advisory/sanity ok
```

The Sprint 58 grapheme soak currently occupies four CPU cores, so strict Git
status, FUSS, re-diff, sign lookup, and keypress timing was deliberately not
measured in this contaminated window. The selftests prove decision policy and
broken-run handling; they are not substituted for the designated unloaded
strict performance matrix.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-017` | `gitcache_terminal_rebase_inherits_canonical_boundary` and structural job/environment controls pass |
| `YEW-F-018` | `fussdrawer_relative_time_uses_editor_anchor`, blame/diff clock controls, and the token-aware audit entry pass |
| `YEW-F-019` | `porcelain_rename_consumes_two_nuls_and_preserves_newline` and the mutation-sensitive audit entry pass |
| `YEW-F-020` | structural verb/argv units, hostile-path integration, and the semantic audit entry pass |
| `YEW-F-120` | S59-A30 and `66085e54` align staging and compact-FUSS action/type-jump ownership |
| `YEW-F-121` | S59-A31 and `66085e54` align clock, argv, and display-width checks with semantic owners |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

### Sprint 51 — Git foundation

| Row | Primary witnesses |
|---|---|
| s51.1 | warning-clean focused Clang/GCC/ASan builds and stripped-boundary units; exact full/minimal regression and Valgrind remain outstanding |
| s51.2 | semantic static scans, `gitcache_verb_table_and_argv_are_structural`, hostile-path integration, F018/F020 controls |
| s51.3 | eight porcelain tests / 1,758 assertions |
| s51.4 | `porcelain_rename_consumes_two_nuls_and_preserves_newline` plus F019 mutation control |
| s51.5 | 544-assertion FUSS-command and 15,372-assertion Git-layer hostile-path fixtures |
| s51.6 | `porcelain_every_byte_truncation_and_rejections` and matching sanitizer fuzz sample |
| s51.7 | exact and backwards-clock Git-cache tests |
| s51.8 | table-driven `gitcache_every_mutation_schedules_structural_refresh` |
| s51.9 | 200-instant killed-refresh publication-preservation test |
| s51.10 | state-string/filesystem/runtime taxonomy units and semantic `strstr` audit |
| s51.11 | environment fingerprint and canonical job-policy units |
| s51.12 | Git-status policy selftest; unloaded strict timing remains outstanding |
| s51.13 | focused assertion totals and deterministic fixtures; cross-machine release evidence remains outstanding |
| s51.14 | action/type-jump, command-boundary, and permanent-non-goal controls under S59-A30 |

### Sprint 52 — FUSS mode

| Row | Primary witnesses |
|---|---|
| s52.1 | warning-clean focused Clang/GCC/ASan builds; exact full/minimal regression and Valgrind remain outstanding |
| s52.2 | complete status-flag/marker-order units and Unicode/ASCII/conflict PTYs |
| s52.3 | empty direct-`wcwidth` scan and F121 semantic width control |
| s52.4 | complete `fussnav_` direction/parent/enter fixture slice |
| s52.5 | path-owned selection fallback and rebuild-safe-index units |
| s52.6 | expansion-memory absence/reappearance units and remembered-refresh perf fixture |
| s52.7 | complete `fussjump_` slice, effective-actions unit, and action-palette PTY |
| s52.8 | F-mode dispatch-table, action-list, and module-command-boundary units |
| s52.9 | structural argv controls and hostile filename integration under F020/F121 |
| s52.10 | complete `fusscommit_` slice and 544-assertion command integration |
| s52.11 | entry/leave identity and diff-layout restoration units/PTYs |
| s52.12 | passing `fuss_leave_q` and `fuss_leave_esc` PTYs |
| s52.13 | canonical terminal-rebase boundary unit; complete release torture lane remains outstanding |
| s52.14 | live Git/group command and permanent-non-goal controls under S59-A30 |
| s52.15 | FUSS policy selftest; unloaded strict timing remains outstanding |
| s52.16 | 86 focused units / 3,850 assertions and 35 focused PTYs per configuration |

### Sprint 53 — editor Git integration

| Row | Primary witnesses |
|---|---|
| s53.1 | warning-clean focused Clang/GCC/ASan builds; exact full/minimal regression and Valgrind remain outstanding |
| s53.2 | random minimal-oracle, identical allocation-free, raw-byte, budget, and incremental diff units plus fuzz sample |
| s53.3 | Git-hunk helper: 254 assertions and all 20 real `git apply --check --cached` patches |
| s53.4 | 544-assertion FUSS-command integration including dirty-buffer staging paths |
| s53.5 | one-undo discard unit and empty gutter `checkout` scan |
| s53.6 | cached index/base selection units and real-repository command integration |
| s53.7 | `gitcache_editor_reuses_base_across_one_hundred_edits` |
| s53.8 | raw diff budget/size outcomes and atomic runtime publication units |
| s53.9 | 25-case blame slice plus blame fit/omit/stale PTYs |
| s53.10 | row-map/sync-guard units and diff filler/sync-scroll PTYs |
| s53.11 | all statusline PTYs and empty statusline-spawn scan |
| s53.12 | five directory-group units / 170 assertions, 5,620-assertion integration, and group PTYs |
| s53.13 | empty watcher scan plus focus/external/internal/mutate invalidation units |
| s53.14 | Git-gutter policy selftest; unloaded strict timing remains outstanding |
| s53.15 | empty deferred-command scan and permanent-non-goal command controls |
| s53.16 | focused Git/FUSS unit totals and 23 Git-editor PTYs per configuration |

## Explicit blockers before READY

- An independent non-author reviewer has not inspected F13 or assigned its 46
  closeout verdicts.
- The exact complete current-baseline `make test` matrix under both compilers,
  ASan/UBSan, Valgrind, full modules, and `MODULES=""` was not rerun for this
  local packet. Darwin has no project Valgrind/LSan lane.
- The contracted long sanitizer fuzz campaigns were not rerun; the bounded
  deterministic samples are supporting evidence only.
- Strict Git/FUSS timing must be measured on the designated unloaded runner
  after the Sprint 58 soak releases the host.
- Complete terminal-handover torture, cross-machine fixture/object-hash and
  byte-stable golden evidence, hosted CI, and required non-Darwin/platform
  evidence must be established on the eventual closeout baseline.

Accordingly `.docs/audits/closeout-F13-git.md` remains `Reviewer: UNASSIGNED`,
all boxes remain unchecked, and its verdict remains `NOT READY`.
