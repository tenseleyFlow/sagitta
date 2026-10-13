# F09 recorder pre-closeout evidence

- Date: 2026-10-13
- Baseline: `31c0a64ae72a21e26829bb9bd7c25d85fb1ece2f`
- Last current/evolved F09-owned product-source change: `f2ba7206`
- Last current/evolved F09-owned test-surface change: `f2ba7206`
- Host: Darwin 25.4.0 arm64 (macOS 26.4.1)
- Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0

Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and required release/platform campaigns remain.**

This packet prepares F09 for closeout without signing it. Its author also
authored F09's contract reconciliation and therefore cannot be Sprint 59's
independent reviewer. A future reviewer must establish the baseline again,
independently inspect or rerun the cited witnesses, and decide every checkbox
in `.docs/audits/closeout-F09-recorder.md`.

## Scope and provenance

The original F09 audit is `.docs/audits/audit-09-recorder.md`. It reviewed
detached product baseline `b3f32645` and filed two Medium findings:

- `YEW-F-009`: generator growth made the mandatory planted folding-fault
  self-test miss its fault and exit without exercising the shrinker. Commit
  `1c11ebee` replaced the unstable generated prefix with a deliberate
  96-event witness; the current self-test fails as intended, shrinks to at
  most three events, and names `ed.move.buf.end` and the illegal
  `YEW_CMD_TAKES_COUNT` fold.
- `YEW-F-010`: store-time validation compiled but did not execute a macro, so
  source with an unresolved first-replay call could enter the register.
  Commit `c362cefd` validates the stored program through the replay path while
  retaining transaction rollback and register atomicity.

The current closeout recheck found and closed two additional contract-only
Medium defects without changing product behavior:

- `YEW-F-111` / S59-A22 removes obsolete Sprint 38/41/55 staging guards from
  the current closeout contract and points them at the delivered macro
  browser, ordinary-buffer edit/store flow, Fletch syntax, and package owner.
- `YEW-F-112` / S59-A23 replaces an impossible cross-file collision fixture
  with tests of all seven reachable namespace rows, canonical/bare-alias
  injectivity, and continued source review of the defensive collision branch.

At this baseline all 112 audit registry entries pass under Apple Clang, GCC
16, and ASan/UBSan. The remediation ledgers report zero active XFAILs and F09
has zero open confirmed findings. F09 product and product-test sources have
not changed since `f2ba7206`; later commits through this baseline are other
fronts, documentation, audit controls, and contract reconciliation.

The Sprint 58 tier-1 soak remains frozen against product `4c99e134` plus
runner-only child `4f483f31`. This packet did not change, restart, or claim
that separate campaign.

## Current-baseline executable evidence

### Strict builds, audit registry, and focused units

Apple Clang, Homebrew GCC 16, and Apple Clang ASan/UBSan built the recorder,
round-trip, fuzz, performance, audit, and supporting editor surfaces under
the repository's locked
`-std=c11 -pedantic -Wall -Wextra -Werror -Wvla` policy with zero warnings.
The computed-goto differential uses the contract's narrowly scoped GNU mode
for `vm.c`; the rest of that tree retains the locked C flags.

Each of the three current audit binaries reported:

```text
audit: 112 tests, 0 failures
```

Focused recorder and macro-library units passed with identical counts under
Clang, GCC, and ASan/UBSan:

| Filter | Tests / assertions per configuration |
|---|---:|
| `record_emit_` | 8 / 216 |
| `record_` | 25 / 1,029 |
| `macro_replay_` | 7 / 140 |
| `macrolib_` | 7 / 184 |
| `macro_header_` | 6 / 197 |
| `macrobrowse_` | 6 / 100 |
| `statusline_recording_` | 1 / 8 |
| `fl_replay_runtime_` | 3 / 48 |
| `undo_macro_` | 2 / 51 |

The filters overlap and their counts are therefore not summed. Static source
checks found no `yew_cmd_invoke`, `yew_edit_`, `yew_textbuf_`, or
`yew_reg_set` call in `src/fl/record.c`. The `macro_reg` source identifier is
confined to `src/edit/buf.h`, `src/edit/file_cmds.c`, and
`src/ui/macrobrowse.c`. All eight live `ed.macro.*` command rows are present
without `YEW_CMD_DEFERRED`, and macro scratch creation selects the shipped
`fletch` syntax definition.

### Round-trip law, coverage, and dispatcher differential

The Clang, GCC, and ASan/UBSan round-trip harnesses each reported:

```text
roundtrip: ok seeds=2000 base=0 fixtures=6 corpus=21 count-sentinel=1
edit-store-noop=1 cross-keymap=1 third-command-rollback=1 P1-P5
```

The current-baseline Sprint 58 fieldwork budget also completed 200,000
generated sessions across the same six fixtures and 21-case corpus in
853.72 seconds on the soak-loaded host:

```text
roundtrip: ok seeds=200000 base=0 fixtures=6 corpus=21 count-sentinel=1
edit-store-noop=1 cross-keymap=1 third-command-rollback=1 P1-P5
```

The fault-seeding self-test passed in all three configurations:

```text
test-roundtrip: planted folding fault shrank to the right operation
```

Current live-registry coverage classifies every recordable command: 30 are
generated and 155 are denied with a nonempty, command-specific reason. The
coverage target also verifies every command-name/Fletch-word bijection and
would fail on a new unclassified recordable row.

Clean current-baseline switch and computed-goto round-trip runners produced
byte-identical output. Every session additionally enforces deterministic
source re-emission and two independent AST dumps internally. The required
cross-target retained-artifact comparison remains a release blocker.

### Fuzz and exact corpus evidence

The deterministic recorder fuzzer completed 100,000 seed-1 iterations under
Clang, GCC, and ASan/UBSan with the same result:

```text
fuzz_record: seed=1 iters=100000 corpus=7 hash=3ef9e60949af2c14 ok
```

Each configuration also replayed the committed emitter corpus exactly:

```text
fuzz_record: corpus=7 exact replay ok
```

These bounded samples do not replace the contracted ten-minute ASan emitter
campaign or the broader release campaign.

### Script, PTY, and performance evidence

All 21 `tests/script/macroux_*.fl` cases passed with 60 assertions under
Clang, GCC, and ASan/UBSan. They cover valid/count/last replay, one-run undo,
cache invalidation, headless listing, diagnostics, prelude and uppercase
registers, batch recording refusal, ordinary-buffer editing, atomic store
failures, canonical and bare library names, reload, additive naming, and
replay transactions.

The focused PTY harness passed all three `s35_` recorder cases and all six
`s38_` indicator/edit/browser cases under Clang, GCC, and ASan/UBSan. The
harness runs every selected case twice from independent state and requires
byte-identical snapshots before comparing the committed golden. Exact
full-suite PTY evidence remains for the closeout reviewer.

The recorder policy self-test passed. Strict Clang and GCC measurements were:

| Configuration | Tap overhead | Emit 10,000 | Budgets |
|---|---:|---:|---:|
| Apple Clang | 11 ns/dispatch | 0.773 ms | 200 ns; 10 ms |
| GCC 16 | 10 ns/dispatch | 0.572 ms | 200 ns; 10 ms |
| ASan/UBSan advisory | 48 ns/dispatch | 2.452 ms | within strict values |

The current strict Fletch performance gate also measured the 200-file macro
library rescan at 1.833 ms against its 5 ms budget and passed every hard gate.
This local loaded-host result is useful evidence, not a substitute for the
designated unloaded performance matrix.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-009` | deliberate 96-event injected-fault session; correct ≤3-event shrink and named count-bearing op; fix `1c11ebee`; audit PASS in all three configurations |
| `YEW-F-010` | store-time replay validation, atomic failed store and first-replay controls; fix `c362cefd`; audit PASS in all three configurations |
| `YEW-F-111` | S59-A22 and `905da06f` align current closeout with delivered successor-owned macro UX |
| `YEW-F-112` | S59-A23 and `905da06f` align the namespace requirement with its injective construction while retaining defensive rejection |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

| Row | Primary witnesses |
|---|---|
| s35.1 | warning-clean three-configuration focused builds and round-trip runs; full regression/Valgrind remain outstanding |
| s35.2 | P1–P5 under Clang/GCC/ASan, six fixtures, 21-case corpus, and current-baseline 200k field run; exact hosted matrix remains distinct evidence |
| s35.3 | deliberate fault self-test in all configurations; ≤3 events and correct `ed.move.buf.end` diagnostic |
| s35.4 | identical 100k recorder-fuzzer hash and exact corpus replay in all configurations; ten-minute ASan campaign remains outstanding |
| s35.5 | `record_emit_` 8 / 216, `record_` 25 / 1,029, count sentinel and coverage target |
| s35.6 | forbidden-call scan is empty; inert-VM audit witness; replay edge through `fl_call_chunk` |
| s35.7 | `macro_replay_` 7 / 140, five-run undo and third-command rollback round-trip sentinels |
| s35.8 | replay-source drop and recording-stop coverage in focused units and `s35_` PTYs |
| s35.9 | live registry: 30 generated plus 155 reasoned denies; unclassified-row failure path |
| s35.10 | live registry command-name/Fletch-word bijection in round-trip coverage |
| s35.11 | strict Clang/GCC recorder perf and policy self-test; designated unloaded matrix remains outstanding |
| s35.12 | three focused `s35_` PTYs, each independently repeated and green under Clang/GCC/ASan; full PTY lane remains outstanding |
| s35.13 | focused unit counts exceed 120 assertions; 21 committed round-trip cases plus seven exact emitter corpus entries |
| s35.14 | eight live non-deferred `ed.macro.*` rows and delivered Sprint 38 browser/edit/library surfaces; S59-A22 |
| s38.1 | warning-clean focused builds, P1–P5 and edit-store no-op under all configurations; full regression/Valgrind remain outstanding |
| s38.2 | indicator width/burst units plus `s38_macro_indicator_{80,40,burst}` PTYs |
| s38.3 | five-run undo controls in `macro_replay_` and `undo_macro_` |
| s38.4 | `macrobrowse_` 6 / 100 and browser/action PTYs over deterministic combined list/filter/action behavior |
| s38.5 | ordinary-buffer multi-cursor/undo controls, atomic compile/unknown-word store scripts, edit-flow PTY |
| s38.6 | source scan confines `macro_reg` to exactly three files |
| s38.7 | additive naming and immediate-call script/unit witnesses |
| s38.8 | `macrolib_` 7 / 184, seven reachable namespace rows, injectivity, defensive branch review, broken-file and stable-order controls; S59-A23 |
| s38.9 | `macro_header_` 6 / 197, cross-keymap executable sentinel, provenance-only source review |
| s38.10 | headless replay/list/library scripts and named batch refusal |
| s38.11 | strict 1.833 ms 200-file scan against 5 ms; designated startup/performance matrix remains outstanding |
| s38.12 | all six focused `s38_` PTYs, each independently repeated and green under Clang/GCC/ASan; full PTY lane remains outstanding |
| s38.13 | focused unit floors exceeded; 21 macro UX scripts; Fletch scratch syntax and package ownership scans; S59-A22 |

## Remaining blockers

1. No independent reviewer has signed the **current** baseline. The original
   audit and this author-authored packet cannot supply Sprint 59's required
   non-author verdicts.
2. No hosted exact-baseline full matrix exists for `31c0a64a`; local `trunk`
   is ahead of `origin/trunk`. Sprint 59 requires the complete cross-target
   matrix at the commit a closeout signs.
3. Valgrind and LeakSanitizer evidence is unavailable on this Darwin host.
   Linux leak and descriptor checks remain outstanding.
4. The complete exact-baseline `make test`, full script/PTY/determinism and
   smoke matrices, designated performance matrix, and required long/multi-seed
   fuzz campaigns were not rerun here. Focused current checks and historical
   F09 evidence are not substitutes.
5. Cross-target byte equality needs retained canonical emitted-source
   artifacts from all four target jobs. Local compiler/configuration P1–P5
   runs and the byte-identical switch/computed-goto result are narrower.
6. The Sprint 58 soak is frozen against product `4c99e134`. It neither blocks
   local Sprint 59 work nor substitutes for Sprint 59's exact closeout matrix.
