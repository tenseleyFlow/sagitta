# F10 syntax pre-closeout evidence

- Date: 2026-10-13
- Baseline: `73f6d4e981dc95d71a0ae7279cfcc2f05c03815a`
- Last current/evolved F10-owned product-source change: `8768947c`
- Last dedicated F10 test-surface change: `1af7378c`
- Host: Darwin 25.4.0 arm64 (macOS 26.4.1)
- Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0

Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and required release/platform campaigns remain.**

This packet prepares F10 for closeout without signing it. Its author also
authored F10's contract reconciliation and therefore cannot be Sprint 59's
independent reviewer. A future reviewer must establish the baseline again,
independently inspect or rerun the cited witnesses, and decide every checkbox
in `.docs/audits/closeout-F10-syntax.md`.

## Scope and provenance

The original F10 audit is `.docs/audits/audit-10-syntax.md`. It reviewed
detached product baseline `b3f32645` and filed three Medium findings:

- `YEW-F-011`: equal-size source replacement with restored metadata could
  retain a stale compiled syntax table. Commit `2c9c7431` makes the source
  hash authoritative for every cache hit while preserving unchanged-source
  reuse.
- `YEW-F-012`: an unresolved embedded definition occupied `aux[ndef]`, in
  conflict with canonical zero tails. Commit `43a82533` keeps pending identity
  in the existing result/idle-load path and clears unused state cells
  unconditionally.
- `YEW-F-013`: the JavaScript/TypeScript known-wrong value-flag rows lacked
  their required fixture-local explanation. Commit `838fd7e1` restores the
  ID-bearing comments, regenerates their goldens, and pins the two intentional
  historical deltas.

The current closeout recheck found and closed two additional contract-only
Medium defects without changing product behavior:

- `YEW-F-113` / S59-A24 replaces obsolete 40-byte, `SynState.def`, and
  pre-owner staging gates with the binding 84-byte qualified-frame invariants
  and delivered Sprint 40–42.5 surfaces.
- `YEW-F-114` / S59-A25 excludes only the legitimate theme owner from syntax
  color/width scans and distinguishes Sprint 42's historical 17-definition
  cohort from Sprint 42.5's checked current 48-definition manifest.

At this baseline all 114 audit registry entries pass under Apple Clang, GCC
16, and ASan/UBSan. The remediation ledgers report zero active XFAILs and F10
has zero open confirmed findings. Dedicated syntax tests last changed in
`1af7378c`; `8768947c` is the current syntax-product tip and its UTF-8 literal
boundary change is included in every result below.

The Sprint 58 tier-1 soak remains frozen against product `4c99e134` plus
runner-only child `4f483f31`. This packet did not change, restart, or claim
that separate campaign.

## Current-baseline executable evidence

### Strict builds, audit registry, and focused units

Apple Clang, Homebrew GCC 16, and Apple Clang ASan/UBSan built the syntax,
theme, editor, audit, fuzz, PTY, and performance-test surfaces under the
repository's locked `-std=c11 -pedantic -Wall -Wextra -Werror -Wvla` policy
with zero warnings. Each audit binary reported:

```text
audit: 114 tests, 0 failures
```

The complete `syn_` unit filter passed identically in all three
configurations:

```text
unit: 165 tests, 52973617 assertions, 0 failures
```

Wall times on the soak-loaded host were 27.34 seconds for Apple Clang, 28.65
seconds for GCC 16, and 102.01 seconds for ASan/UBSan. These are execution
times, not performance verdicts. The filter includes the four-seed
100,000-edit matrices, the all-48-definition state/first-byte control, cache,
splice, settle, embedding, detection, Fortran, Rust-depth, Fletch cross-check,
and Wolf coverage.

The following overlapping integration filters also passed with identical
counts in all three configurations:

| Filter | Tests / assertions per configuration |
|---|---:|
| `theme_` | 14 / 1,076 |
| `degrade_` | 10 / 3,196 |
| `block_` | 39 / 1,099 |

The full `make test-audit` target is green: the 114-entry registry and the
three special invariant binaries pass; finding, XFAIL, priority, sampling,
closeout, deferral, and remediation-ledger checks and their negative
selftests all pass. Governance reports zero active XFAILs, zero
deferred/wontfix findings, and next ID `YEW-F-115`.

### Manifest, strict compiler, coverage, and goldens

The checked runtime contains 48 syntax definitions. The manifest contains 48
base rows plus seven embedded-fixture rows, 55 rows total. Under Clang, GCC,
and ASan/UBSan, `scripts/check-syn-assets.sh` reported:

```text
syntax assets: ok (432 total; cohorts s41=99 s42=144 s41_5=62 s42_5=127; 62 embed)
```

That gate establishes all of the following against one deterministic
manifest:

- every runtime definition has exactly one base row and no manifest base is
  missing from the runtime;
- `yew syn check --strict` accepts all 48 definitions with zero warnings;
- every context, rule, and embedded-language site is covered by the listed
  fixtures;
- all 432 span goldens regenerate byte-identically twice with no orphan;
- cold and warm INI table dumps are byte-identical, cache bypass writes
  nothing, and `yew syn compile --all` is idempotent;
- all 17 deliberately broken definition fixtures emit their exact single
  `line:col` diagnostic;
- the maximum resident-definition/frame matrix is valid for every pack row.

The companion historical-column guard reported 226 unchanged qualification
goldens, 15 approved embedded-language deltas, and two documented fixture
deltas. `.docs/syntax-def.md` and `.docs/themes.md` contain no `TODO` or `TBD`.
Their current sections include the complete shipped INI source and trace,
TextMate porting and new-definition checklists, the heredoc and `unit:`
guidance, Go as the pack template, the JavaScript `value` heuristic, both
themes, and the complete degradation ladder.

Static layout checks remain live for `sizeof(SynState) == 84`,
`sizeof(SynSpan) == 8`, `YEW_ATTR__COUNT <= 64`, `YEW_ATTR__COUNT == 54`, and
`sizeof(Cell) == 20`. `scripts/bans.sh` passed, including the theme-excluding
syntax color scan and the matching-scope width scan.

### Differential, state, cache, and integration controls

The 165-test syntax filter includes these high-volume current-state proofs:

- all eight Sprint 42 languages at four deterministic seeds and 100,000 edits
  per language;
- all 48 current syntax modes at four seeds and 100,000 edits per mode in the
  audit matrix;
- 10,000 randomized state canonicalization round trips;
- all 48 definitions through 40 pushes and 40 pops with exact state recovery,
  first-byte brute-force checks, resident-definition bounds, canonical zero
  tails, and non-embedding root ownership;
- a 1,000,000-line bounded settle wave, 100,000-line fixpoint and block
  controls, all eight splice rows, and the one-splice 10,000-line paste;
- balanced inline, line, deferred, unknown-fallback, continuation, and
  pump-mediated embeds with exact entry-state restoration;
- every source-cache corruption class, content replacement, touched-but-
  unchanged reuse, explicit/environment bypass, and cold/warm table identity;
- syntax-backed B-mode units and the plain-text fallback, including exact
  string/comment containment and no-fixed-point motion.

The all-definition first-byte test detects deliberate narrowing, and the
zero-width rule control terminates after forced UTF-8 progress. The Fletch
cross-check covers every token in the specification example exactly once and
observes all four `motion.*` attributes.

### Fuzz and exact corpus evidence

The committed corpora replayed exactly under Clang, GCC, and ASan/UBSan:

```text
fuzz_syn: corpus=62 exact replay ok
fuzz_syn_def: corpus=65 exact replay ok
```

Bounded deterministic samples produced identical results in all three
configurations:

| Campaign | Volume | Hash |
|---|---:|---|
| syntax line | 100,000 iterations, seed 1 | `969905231f17c740` |
| syntax edit | 100,000 edit operations, seed 1 | `348e98f19d997c4a` |
| definition compiler | 5,000 iterations, seed 1 | `8a1e6dc9dac31c9a` |

These samples do not replace the contracted ten-minute sanitizer campaigns
or the full release fuzz matrix.

### PTY and degradation evidence

The focused PTY harness passed every `s39_`, `s41_`/`s41_5_`, and
`s42_`/`s42_5_` case under Clang, GCC, and ASan/UBSan: 97 selected cases per
configuration. The harness runs every selected case twice from independent
state and requires byte-identical snapshots before comparing the committed
golden.

The set covers toy and deferred-wave rendering; every Sprint 41 language in
dark/light and truecolor/256/16 tiers; embedded fences; both empty and
nonempty `NO_COLOR`; `TERM=dumb`; underline colors; theme switching;
cold/warm cache identity; all Sprint 42 languages; fixed-form Fortran column
73; every Sprint 42.5 language family and theme tier; three-definition
switching; and all-fence lazy loading. No sanitizer diagnostic appeared.

### Performance harness controls

The performance binary's non-timing controls passed under Clang, GCC, and
ASan/UBSan:

```text
perf-syn-policy: strict/advisory/sanity/correctness ok (89 checks)
perf-syn-runtime: checkout runtime pinned; decoy prefix and foreign runtimes refused ok
perf_syn: gate modes behave (8 cases)
```

The four Sprint 58 grapheme soak streams currently occupy four CPU cores.
Strict syntax timing was deliberately not measured in that contaminated
window. The selftests prove policy, runtime provenance, and injected gate
behavior only; they are not substituted for the designated unloaded strict
performance matrix.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-011` | authoritative content-hash cache validation, equal-metadata replacement control, and the full cache/corruption matrix; fix `2c9c7431`; audit PASS in all three configurations |
| `YEW-F-012` | canonical zero tails, pending-embed result/queue ownership, balanced exit controls, and all-48 state matrix; fix `43a82533`; audit PASS in all three configurations |
| `YEW-F-013` | fixture-local ID-bearing heuristic comments, regenerated exact spans, and 226/15/2 historical-column guard; fix `838fd7e1`; audit PASS in all three configurations |
| `YEW-F-113` | S59-A24 and `0488f5df` align current closeout with the qualified-frame state and delivered syntax owners |
| `YEW-F-114` | S59-A25 and `0488f5df` align color ownership and current inventory gates with the theme boundary and checked manifest |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

| Row | Primary witnesses |
|---|---|
| s39.1 | warning-clean three-configuration focused builds and audit/unit/PTY runs; exact full regression/Valgrind remain outstanding |
| s39.2 | live layout assertions plus `syn_state2_layout` and canonical state filters under all configurations |
| s39.3 | green repository bans with the sole theme-owner exclusion |
| s39.4 | depth-cap units and all-48 definition state/first-byte matrix |
| s39.5 | splice-table and one-splice 10,000-line paste controls |
| s39.6 | one-line fixpoint control over the 100,000-line fixture; strict latency matrix remains outstanding |
| s39.7 | budgeted million-line comment wave, speculative viewport, and fixpoint controls |
| s39.8 | four-seed incremental/from-scratch unit oracles and all-language matrices |
| s39.9 | exact 62-case corpus and compiler-identical bounded line/edit samples; contracted timed sanitizer campaigns remain outstanding |
| s39.10 | zero-width UTF-8 progress control and exact corpus replay |
| s39.11 | unchanged-span cache and one-row scroll/fixpoint controls; strict redraw timing remains outstanding |
| s39.12 | manifest strict/coverage/embed gates, theme tests, detection tests, and S59-A24 |
| s39.13 | 342 assertion sites in dedicated syntax unit sources and 52,973,617 executed assertions; strict perf matrix remains outstanding |
| s40.1 | same warning-clean three-configuration focused matrix; exact full regression/Valgrind remain outstanding |
| s40.2 | current syntax author manual sections and zero TODO/TBD scan |
| s40.3 | manifest strict pass plus exact 17-fixture diagnostic table |
| s40.4 | INI and all-48 first-byte brute-force controls, including deliberate narrowing detection |
| s40.5 | syntax B-mode provider filters and INI/atom containment controls |
| s40.6 | 39-test overlapping block integration filter, including plain-text fallback and language structures |
| s40.7 | syntax-backed exact string/comment queries plus heuristic fallback controls |
| s40.8 | cache corruption/content/touch/bypass units and cold/warm CLI identity |
| s40.9 | load-once registry unit and deterministic manifest/generated-table gates |
| s40.10 | live theme commands, embed compiler/runtime, checked pack, and S59-A24 |
| s40.11 | exact 65-case definition corpus and identical 5,000-case bounded hashes; ten-minute sanitizer campaign remains outstanding |
| s40.12 | performance policy/runtime/gate selftests; uncontended strict measurement remains outstanding |
| s40.13 | dedicated assertion inventory and 432 byte-stable manifest goldens |
| s41.1 | warning-clean three-configuration focused matrix; exact full regression/Valgrind remain outstanding |
| s41.2 | all-pack strict check and idempotent `syn compile --all` in the asset gate |
| s41.3 | complete manifest context/rule/embed-site coverage |
| s41.4 | 99 Sprint 41 goldens, exact regeneration, and named language traps in committed fixtures |
| s41.5 | specification-example Fletch token/span cross-check with all four motion attrs |
| s41.6 | `theme_defaults_compile_with_full_attr_coverage` and both shipped themes under the asset/runtime gates |
| s41.7 | syntax heredoc/unit guidance and complete theme author manual |
| s41.8 | `theme_all_108_documented_256_conversions` in all three configurations |
| s41.9 | zero-rehighlight theme-switch unit and PTY; uncontended two-millisecond measurement remains outstanding |
| s41.10 | 10-test degradation unit filter plus empty/nonempty NO_COLOR and TERM=dumb PTYs in all configurations |
| s41.11 | live 20-byte Cell assertion, underline render/theme units, and six underline PTYs per configuration |
| s41.12 | performance harness selftests and comment-wave correctness; designated unloaded budgets remain outstanding |
| s41.13 | qualified-frame, non-embedding, canonical-tail, balanced-exit, and embed compiler/runtime filters |
| s41.14 | 99 Sprint 41 goldens plus focused syntax/theme assertion inventories |
| s42.1 | warning-clean three-configuration focused matrix; exact full regression/Valgrind remain outstanding |
| s42.2 | checked 48-definition manifest, explicit historical cohort metadata, and all-pack strict pass |
| s42.3 | complete current manifest context/rule/embed-site coverage |
| s42.4 | 144 Sprint 42 goldens, exact regeneration, and committed named language traps |
| s42.5 | 20-input/tie-limit Fortran units and fixed-form column-73 PTY in all configurations |
| s42.6 | exact 20-deep Rust exit-state and shallow-edit recovery controls |
| s42.7 | four-seed 100,000-edit Sprint 42 matrix and current all-48 audit matrix |
| s42.8 | all-definition qualified-frame/resident-definition/canonical-tail controls |
| s42.9 | cold-load/lazy pack correctness and performance harness controls; strict current timings remain outstanding |
| s42.10 | exact corpora and compiler-identical bounded fuzz samples; contracted long sanitizer campaigns remain outstanding |
| s42.11 | Go template, aux-consumer, and JavaScript value-heuristic documentation sections |
| s42.12 | 144 Sprint 42 goldens, 432 current goldens total, and focused assertion inventories |

## Remaining blockers

1. No independent reviewer has signed the **current** baseline. The original
   audit and this author-authored packet cannot supply Sprint 59's required
   non-author verdicts.
2. No hosted exact-baseline full matrix exists for `73f6d4e9`; local `trunk`
   is ahead of `origin/trunk`. Sprint 59 requires the complete cross-target
   matrix at the commit a closeout signs.
3. Valgrind and LeakSanitizer evidence is unavailable on this Darwin host.
   Linux leak and descriptor checks remain outstanding.
4. The complete exact-baseline `make test`, full script/PTY/determinism and
   smoke matrices were not rerun here. Focused current checks and historical
   F10 evidence are not substitutes.
5. Strict syntax timing was intentionally withheld while four independent
   soak workers occupy four cores. The designated unloaded Clang/GCC and
   target performance matrix remains outstanding.
6. The contracted ten-minute ASan/UBSan syntax line, edit, and definition
   campaigns and the complete release seed/target matrix remain outstanding.
   Exact corpora, high-volume units, and bounded samples are narrower.
7. The Sprint 58 soak is frozen against product `4c99e134`. It neither blocks
   local Sprint 59 work nor substitutes for Sprint 59's exact closeout matrix.
