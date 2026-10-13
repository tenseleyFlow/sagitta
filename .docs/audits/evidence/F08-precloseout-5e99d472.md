# F08 Fletch pre-closeout evidence

- Date: 2026-10-13
- Baseline: `5e99d47287292a915162fa159685f4b7992096ef`
- Last current/evolved F08-owned product-source change: `5d9a74c4`
- Last current/evolved F08-owned test-surface change: `5d9a74c4`
- Host: Darwin 25.4.0 arm64
- Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0

Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and required release/platform campaigns remain.**

This packet prepares F08 for closeout without signing it. Its author also
authored F08's contract reconciliation and therefore cannot be Sprint 59's
independent reviewer. A future reviewer must establish the baseline again,
independently inspect or rerun the cited witnesses, and decide every checkbox
in `.docs/audits/closeout-F08-fletch.md`.

## Scope and provenance

The original F08 audit is `.docs/audits/audit-08-fletch.md`. It reviewed
detached product baseline `41fef416` and filed the High capability defect
`YEW-F-008`: plugin-authored macro source was compiled with trusted config
authority. Commit `8995bb1f` preserves the defining origin through macro
replay, and the executable audit reproducer now passes.

The current closeout recheck found and closed three contract-only Medium
defects without changing product behavior:

- `YEW-F-108` / S59-A19 distinguishes delivered imports, editor APIs,
  recorder round-trip, batch mode, macro libraries, and plugin configuration
  from their historical landing guards. The standalone `yew fl` null host and
  the absence of `io.run`/`io.http` remain intentional boundaries.
- `YEW-F-109` / S59-A20 states the actual ownership rules: lexer/parser ASTs
  use their caller arena, every `FlObj` uses the collector, later host support
  may use checked project allocators, and direct libc allocation remains
  forbidden.
- `YEW-F-110` / S59-A21 keeps the 11/12-kind and 117-core-native snapshots as
  history while current closeout follows the amended 13-kind and complete
  187-native generated inventories.

At this baseline all 110 audit registry entries pass under Apple Clang, GCC
16, and ASan/UBSan. The remediation ledgers report zero active XFAILs and F08
has zero open confirmed findings. Product and product-test sources have not
changed since `5d9a74c4`; later commits through this baseline are other fronts,
documentation, audit controls, and contract reconciliation.

The Sprint 58 tier-1 soak remains frozen against product `4c99e134` plus
runner-only child `4f483f31`. This packet did not change, restart, or claim
that separate campaign.

## Current-baseline executable evidence

### Strict builds, audit registry, and focused units

Apple Clang, Homebrew GCC 16, and Apple Clang ASan/UBSan built the Fletch
editor, unit, audit, conformance, round-trip, fuzz, performance, and PTY
surfaces under the repository's locked
`-std=c11 -pedantic -Wall -Wextra -Werror -Wvla` policy with zero warnings.
The computed-goto differential uses the contract's narrowly scoped GNU mode
for `vm.c`; the rest of that tree retains the locked C flags.

Each of the three current audit binaries reported:

```text
audit: 110 tests, 0 failures
```

Thirty-five current Fletch and successor-owner unit filters passed with
identical counts under Clang, GCC, and ASan/UBSan:

| Slice | Filter | Tests / assertions per configuration |
|---|---|---:|
| lexer/parser | `fl_lex_`; `fl_parse_` | 26 / 487; 21 / 322 |
| VM/GC/compiler | `fl_vm_`; `fl_gc_`; `fl_compile_` | 30 / 629; 15 / 71; 11 / 345 |
| REPL/checks/diagnostics | `fl_repl_`; `fl_chunkcheck_`; `fl_trace_`; `fl_didyoumean_` | 10 / 82; 5 / 67; 7 / 36; 6 / 42 |
| stdlib | `fl_math_`; `fl_str_`; `fl_map_`; `fl_list_`; `fl_re_`; `fl_io_`; `fl_fmt_`; `fl_caps_`; `fl_modules_` | 6 / 65; 9 / 443; 4 / 33; 5 / 42; 14 / 237; 13 / 136; 14 / 2,146; 5 / 37; 9 / 92 |
| host/editor API | `fl_handle_`; `fl_origin_`; `fl_marshal_`; `fl_cmd_`; `fl_api_`; `fl_motion_`; `fl_txn_`; `fl_hook_`; `fl_runtime_` | 8 / 496,129; 6 / 103; 10 / 133; 9 / 126; 10 / 255; 7 / 137; 11 / 100,150; 8 / 149; 9 / 8,153 |
| configuration/state | `fl_options_`; `option_`; `bind_`; `flconf_`; `runtime_defaults_`; `state_diff_`; `state_corpus_` | 1 / 108; 14 / 935; 7 / 123; 14 / 825; 3 / 2,866; 2 / 3; 13 / 12,782 |

The `state_diff_` rows are retirement guards for the completed codec handover,
not a retained second production codec. Darwin ASan supplies no
LeakSanitizer, and Valgrind is unavailable on this host; Linux leak evidence
remains a release blocker.

### Conformance, dispatch, GC stress, and round-trip

`make test-fletch` passed under Clang, GCC, and ASan/UBSan:

```text
39 total, 39 pass, 0 fail, 0 skip, 0 xfail, 0 config
```

All seven generated-coverage checks passed, and the meta-suite proved every
check fires (check 6 has both missing-target and worked-example-drift seeds).
The current committed ledger records:

```text
sections 16/16  productions 47/42  natives 187/187
kinds 13/13  opcodes 60/60
```

The production numerator is allowed to exceed the defined-production count
because `COVERS` tokens can intentionally exercise a production in multiple
fixtures. Current native enumeration independently reports 187 names.

`make test-fletch-determinism` was green in all three configurations: both
runs were byte-identical and the ledger stayed stable. `make
test-fletch-gc-stress` passed all 39 cases in all three configurations with no
opt-outs or failures. Clean switch and computed-goto builds produced
byte-identical traces:

```text
test-fletch-dispatch: both dispatchers agree, byte for byte
```

The Clang, GCC, and ASan/UBSan round-trip harnesses each reported:

```text
roundtrip: ok seeds=2000 base=0 fixtures=6 corpus=21 count-sentinel=1
edit-store-noop=1 cross-keymap=1 third-command-rollback=1 P1-P5
```

The seeded folding self-test also shrank to the intended minimal operation.

### Focused fuzz evidence

Each current Fletch fuzz surface completed a deterministic 100,000-iteration
seed-1 sample under Clang, GCC, and ASan/UBSan. Hashes agreed across all three
configurations:

```text
fuzz_fl_lex:   corpus=120 hash=e70c5f8eac5d689d
fuzz_fl_parse: corpus=120 hash=e5ad5c82462767fc
fuzz_fl_std:   corpus=8   hash=a42459f42f7ccfe8
fuzz_fl_vm:    corpus=18  hash=5e7dfdde26f54456
fuzz_flapi:    corpus=10  hash=413455102f042aee
```

These bounded samples are not substituted for the contracted ten-minute VM,
GC-stress, or full release campaigns. The complete long campaigns remain
release blockers.

### PTY and performance evidence

Current Apple Clang PTY evidence covers `s32_repl_session`,
`s32_bug_restores_the_terminal`, and `s33_hello_world_repl`. The harness
executes each selected case twice internally. GCC/ASan PTY and complete
exact-baseline PTY/determinism runs remain outstanding.

The first strict `make PERF_GATE=1 bench-fletch` attempt, while four Sprint 58
soak workers saturated the host, failed the startup regression comparison and
measured `config_load` at 1.004 ms. That contention-sensitive failure is part
of the evidence and is not hidden. An immediate same-binary repeat passed all
hard gates:

```text
startup             0.032 ms
config_load         0.568 ms
parse_only          0.111 ms
config_reload_500   1.345 ms
macrolib_scan_200   1.803 ms
motion_dispatch     133.654 ns/op
motion_editor       78.206 ms / 1,000,000
query_cur_pos        6.617 ms / 1,000,000
insert_100k         35.390 ms
fib27               12.259 ms
strbuild_join        1.462 ms
strbuild_concat     48.516 ms
map_heavy           32.150 ms
list_sort           19.421 ms
gc_pause             1.026 ms (record only)
concat/join          134.4x
perf_fletch: all hard gates green
```

The repeat is useful local evidence, not permission to average away the first
failure. The designated unloaded performance matrix remains authoritative.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-008` | defining-origin macro replay; plugin denial reproducer; fix `8995bb1f`; audit PASS in all three configurations |
| `YEW-F-108` | S59-A19 and `674d826b` align delivered language/host surfaces while retaining null-host and stdlib boundaries |
| `YEW-F-109` | S59-A20 and `674d826b` express parser-arena, GC-object, checked-host-allocation, and libc-ban ownership semantically |
| `YEW-F-110` | S59-A21 and `bb1e745b` distinguish freeze-time snapshots from current generated 13-kind/187-native inventories |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

| Row | Primary witnesses |
|---|---|
| s28.1 | 16-section current spec; S59-A21 distinguishes the empty-§16 freeze snapshot from later amendments |
| s28.2 | spec preface traceability matrix and eight design-requirement rows |
| s28.3 | current grammar coverage gate; `fl_parse_` 21 / 322 under all configurations |
| s28.4 | spec vocabulary table plus lexer/motion conformance fixtures |
| s28.5 | conformance runner's byte comparison of `14-example.fl` to the spec block |
| s28.6 | pinned spec sentences and corresponding conformance/unit families |
| s28.7 | coverage checks 1 and 6 over every `Conformance:` target |
| s28.8 | S59-A21; 22-word fixture and current 13/13 kind ledger |
| s28.9 | freeze sign-off in the spec; commit-history scope remains for independent review |
| s29.1 | warning-clean Clang/GCC/ASan builds; S59-A20 historical-scope reconciliation |
| s29.2 | `fl_lex_` 26 / 487 plus `01-lexical.fl` |
| s29.3 | `fl_parse_` 21 / 322 and coverage check 2; current 47/42 coverage total |
| s29.4 | `FlNode` static assertion, parser arena controls, allocator ban, S59-A20 |
| s29.5 | parser diagnostic units and committed negative fixtures |
| s29.6 | recovery fixture family within `fl_parse_` |
| s29.7 | pure-literal conformance, state-corpus acceptance, and negative classes |
| s29.8 | `14-example.fl` parse and spec byte-comparison gate |
| s29.9 | 100k lex/parse samples under all configurations; contracted long sanitizer campaigns remain outstanding |
| s29.10 | `fl_repl_` incomplete matrix and conformance REPL inputs |
| s30.1 | warning-clean three-configuration builds and clean computed-goto differential build |
| s30.2 | value/object/opcode compile-time assertions in the strict builds |
| s30.3 | `fl_compile_` 11 / 345 and 60/60 conformance opcode ledger |
| s30.4 | worked-example conformance assertions |
| s30.5 | byte-identical clean switch/computed-goto differential |
| s30.6 | VM/map iteration tests plus cross-process determinism runs |
| s30.7 | `fl_gc_` 15 / 71, three GC-stress suites, ASan; Valgrind remains outstanding |
| s30.8 | object allocation source review, allocator ban, GC stress, S59-A20/`YEW-F-109` |
| s30.9 | compiler/VM closure and loop-binding unit fixtures |
| s30.10 | live import coverage, handle tests, and S59-A19 successor ownership |
| s30.11 | spec A1/A2 history, current 13/13 ledger, S59-A21 |
| s30.12 | committed Fletch smoke baselines and current strict perf run |
| s31.1 | strict builds, stdlib filters, determinism and GC stress; Valgrind remains outstanding |
| s31.2 | seven module tables, 187-name current list, 187/187 ledger, S59-A21 |
| s31.3 | coverage check 3 and all nine stdlib filter families |
| s31.4 | `fl_str_` 9 / 443 and Unicode/state corpus witnesses |
| s31.5 | repository formatting ban and its seeded self-test; exact full policy suite remains reviewer evidence |
| s31.6 | `fl_caps_` 5 / 37, `fl_origin_` 6 / 103, plugin-helper conformance |
| s31.7 | `fl_modules_` 9 / 92 and import-cycle conformance fixtures |
| s31.8 | `fl_fmt_` 14 / 2,146 and 100k `fuzz_fl_std` sample |
| s31.9 | `fl_re_` 14 / 237 plus current regex conformance and F06 bounds |
| s31.10 | generated case-table source and Unicode tests; exact-baseline regeneration/hosted Unicode lane remains outstanding |
| s31.11 | live editor host filters, absent `io.run`/`io.http`, S59-A19 |
| s31.12 | deterministic `math.seed(1)` golden and double-run Fletch determinism |
| s32.1 | strict REPL builds, audit, determinism, GC stress, dispatch differential; Valgrind remains outstanding |
| s32.2 | invocation-form fixtures and shipped help/smoke surface; exact full smoke matrix remains outstanding |
| s32.3 | shared text-editing source path, `fl_repl_`, and absent private 512-byte editor pattern |
| s32.4 | `fl_repl_` incomplete/trial-parse capture tests |
| s32.5 | the two `s32_` PTYs, each replayed twice; cross-configuration/full PTY remains outstanding |
| s32.6 | `fl_trace_` 7 / 36 |
| s32.7 | `fl_didyoumean_` 6 / 42 and suggestion ownership scan |
| s32.8 | 100k VM sample under all configurations; contracted ten-minute ASan and GC-stress fuzz runs remain outstanding |
| s32.9 | `fl_chunkcheck_` 5 / 67 and checked-build coverage |
| s32.10 | chunk invariant tests and repository abort/assert ban; exact full policy suite remains outstanding |
| s32.11 | verifier condition documented in `vm.h` |
| s32.12 | null-host motion conformance, live editor API filters, S59-A19 |
| s33.1 | 39/39 conformance in all configurations, well below 20 s when not soak-starved |
| s33.2 | all seven checks green and every seeded violation fires |
| s33.3 | stable generated ledger across three determinism runs |
| s33.4 | current 16/16, 47/42, 187/187, 13/13, 60/60 totals; S59-A21 |
| s33.5 | worked-example byte comparison and normative assertions |
| s33.6 | current reserved-word/kind/section/opcode fixtures |
| s33.7 | meta-suite 11-case expected result and two check-6 seeds |
| s33.8 | immediate strict repeat green; first soak-contended failure and designated matrix remain blockers |
| s33.9 | committed baseline table and performance policy self-test |
| s33.10 | current 134.4x concat/join ratio |
| s33.11 | three GC-stress suites and byte-identical two-dispatcher run; zero opt-outs |
| s33.12 | live editor filters, round-trip P1–P5, batch surface, S59-A19 |
| s33.13 | script/`-e` conformance, `s33_hello_world_repl`, coverage gate, current docs; exact hosted milestone remains outstanding |
| s34.1 | host/editor filters in all configurations, conformance and GC stress; full regression/Valgrind remain outstanding |
| s34.2 | `FlHandle` static assertion, pointer-free handle header, `fl_handle_` 8 / 496,129 |
| s34.3 | dead-handle matrix under Clang/GCC/ASan; full crafted-handle fuzz campaign remains outstanding |
| s34.4 | forced generation-wrap retirement in `fl_handle_` |
| s34.5 | command/API filters and side-door policy gate; exact full policy suite remains reviewer evidence |
| s34.6 | `fl_cmd_` 9 / 126 command-word round trips |
| s34.7 | `fl_txn_` 11 / 100,150 including 100k nested mutations and escaping rollback |
| s34.8 | `fl_hook_` 8 / 149 plus runtime dispatch fixtures |
| s34.9 | hook containment/disable/re-entry fixtures |
| s34.10 | `fl_caps_`, `fl_origin_`, fixed `YEW-F-008` plugin macro replay |
| s34.11 | A2 in the spec and current 13/13 ledger |
| s34.12 | 133.654 ns motion dispatch and 6.617 ms / million position queries locally; designated matrix remains outstanding |
| s34.13 | host/editor unit filters far exceed 200 assertions; 39-case conformance suite; exact script ownership count remains for reviewer |
| s34.14 | live config/recorder/batch successor surfaces and S59-A19 |
| s36.1 | config/state filters under all configurations; full regression/Valgrind remain outstanding |
| s36.2 | `runtime_defaults_` 3 / 2,866 and `bind_` 7 / 123 |
| s36.3 | current `config_load` repeat at 0.568 ms; designated unloaded gate remains outstanding |
| s36.4 | `fl_options_` 1 / 108 and `option_` 14 / 935 |
| s36.5 | option-set ownership gate and seeded policy test; exact full policy suite remains reviewer evidence |
| s36.6 | shared option-table script/unit coverage |
| s36.7 | `flconf_` 14 / 825 and trust/state fixtures; full PTY review remains outstanding |
| s36.8 | configuration reload/hook/runtime unit and script fixtures |
| s36.9 | `state_diff_` retirement guards, `state_corpus_` 13 / 12,782, S59-A19 |
| s36.10 | clean-mode runtime/default tests and shipped help; exact full smoke remains outstanding |
| s36.11 | local 0.568 ms load and 1.345 ms 500-bind reload; designated matrix remains outstanding |
| s36.12 | live macro-library/plugin/batch successor surfaces and S59-A19 |
| s36.13 | focused unit counts exceed the floor; conformance and selected PTYs; full script/PTY determinism remains outstanding |

## Remaining blockers

1. No independent reviewer has signed the **current** baseline. The original
   audit and this author-authored packet cannot supply Sprint 59's required
   non-author verdicts.
2. No hosted exact-baseline full matrix exists for `5e99d472`; local `trunk`
   is ahead of `origin/trunk`. Sprint 59 requires the complete cross-target
   matrix at the commit a closeout signs.
3. Valgrind and LeakSanitizer evidence is unavailable on this Darwin host.
   Linux leak and descriptor checks remain outstanding.
4. The complete exact-baseline `make test`, full script/PTY/determinism and
   smoke matrices, Unicode regeneration, designated performance matrix, and
   required long/multi-seed fuzz campaigns were not rerun here. Focused
   current checks and historical F08 evidence are not substitutes.
5. The first strict performance attempt failed under active four-core soak
   contention. Its immediate repeat passed every hard gate, but only the
   designated unloaded matrix can resolve the release verdict.
6. The Sprint 58 soak is frozen against product `4c99e134`. At packet
   creation it retains four durable `fuzz_utf8` rows while four
   `fuzz_grapheme` streams run. It neither blocks local Sprint 59 work nor
   substitutes for Sprint 59's exact closeout matrix.
