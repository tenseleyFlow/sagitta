# F11 completion/LSP pre-closeout evidence

- Date: 2026-10-13
- Baseline: `c9529fb9ba2ecd3592d9f1cda8e005f6bf3d3bd5`
- Last current/evolved F11-owned product-source change: `254b2927`
- Last dedicated F11 test-surface change: `151bd0f0`
- Host: Darwin 25.4.0 arm64 (macOS 26.4.1)
- Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0

Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and required release/platform campaigns remain.**

This packet prepares F11 for closeout without signing it. Its author also
authored F11's contract reconciliation and live-harness remediation, so cannot
be Sprint 59's independent reviewer. A future reviewer must establish the
baseline again, independently inspect or rerun the cited witnesses, and
decide every checkbox in `.docs/audits/closeout-F11-lsp.md`.

## Scope and provenance

The original F11 audit is `.docs/audits/audit-11-lsp.md`. It reviewed detached
product baseline `b3f32645` and filed three Medium findings:

- `YEW-F-014`: stripped LSP completion bypassed the module boundary. Commit
  `114f99fb` restores the exact hard error without removing core completion.
- `YEW-F-015`: the snippet-policy scan included unrelated core code. Commit
  `59d318cf` scopes it to the LSP owner and retains the downgrade policy.
- `YEW-F-016`: raw line-arithmetic scans rejected legitimate 1-based display
  conversion. Commit `07ee554b` confines protocol line arithmetic to the
  position boundary and keeps UI display conversion explicit.

The current closeout recheck found and closed three additional Medium defects:

- `YEW-F-115` / S59-A26 replaces obsolete Sprint 43–46 staging clauses with
  evidence for the shipped provider, lifecycle, documentation, and nine
  feature-command owners while retaining stripped-build errors and permanent
  1.0 non-goals.
- `YEW-F-116` / S59-A27 replaces raw whole-workspace symbol and JSON format
  substring scans with owner- and token-aware contracts. No product behavior
  changed.
- `YEW-F-117` found that the required live-clangd harness treated macOS
  `/tmp` and `/private/tmp` paths as different documents. Commit `151bd0f0`
  canonicalizes the fixture root and sends the attached `LspDoc.uri` for
  subsequent requests. This was a test-harness defect; production feature and
  rename requests already used the attached URI.

At this baseline all 117 audit registry entries pass under Apple Clang, GCC
16, and ASan/UBSan. The remediation ledgers report zero active XFAILs and F11
has zero open confirmed findings. F11's current product tip includes the Wolf
default-server integration at `254b2927`; the focused evidence below exercises
the current tree rather than the historical audit snapshot.

The Sprint 58 tier-1 soak remains frozen against product `4c99e134` plus
runner-only child `4f483f31`. Its four grapheme streams occupied four CPU
cores while this packet was assembled. This work did not change, restart, or
claim that separate campaign.

## Current-baseline executable evidence

### Strict builds, audit registry, and focused units

Apple Clang, Homebrew GCC 16, and Apple Clang ASan/UBSan built the shadow,
completion, symbol-index, JSON/JSON-RPC, LSP, Unicode-position, panel, audit,
fuzz, PTY, and performance-test surfaces under the locked
`-std=c11 -pedantic -Wall -Wextra -Werror -Wvla` policy with zero warnings.
Each current-baseline audit binary reported:

```text
audit: 117 tests, 0 failures
```

The complete governance target is green: the 117-entry registry and the three
special invariant binaries pass; finding, fixture, priority, sampling,
closeout, deferral, and remediation-ledger checks and their negative selftests
all pass. It reports next ID `YEW-F-118`, zero active XFAILs, and zero
deferred/wontfix findings.

The following focused unit filters passed with identical counts under Clang,
GCC, and ASan/UBSan:

| Filter | Tests / assertions per configuration |
|---|---:|
| `shadow_` | 48 / 873 |
| `sym` | 49 / 174,326 |
| `complmenu_` | 9 / 241 |
| `json` | 29 / 245,671 |
| `u16_` | 6 / 59,033 |
| `panel_` | 21 / 665 |
| `lsp_` | 131 / 17,963 |

These filters cover stale-shadow rejection and arbitration; the interaction
table and debounce; incremental symbol-index/full-rescan comparison, ranking,
walking, caps, and completion-menu geometry; JSON grammar, number policy,
writer, and framed transport; byte/UTF-16 differential positions; lifecycle,
sync, diagnostics, navigation, completion, capability gates, snippets,
symbols, highlights, URI handling, and atomic rename; and panel clipping and
wide-cell invariants.

### Live clangd milestone

`make test-lsp-live YEW_LSP_CI_REQUIRED=1` passed against the real host
clangd and a disposable git worktree:

```text
clangd: 8/8 goto, 6 refs in 3 files, rename round-trip clean, 1 diagnostic
```

The executable also verified hover text, compiled the renamed tree, restored
the two-file rename to a clean git diff, observed the diagnostic gutter glyph,
shut clangd down without a surviving child, and restored the fixture bytes.
The eighth goto is the compile-database module-define sentinel. The macOS path
alias that exposed `YEW-F-117` is now part of the passing execution path.

### Static owner and determinism gates

The current source passes the binding owner checks:

- shadow mutation has exactly one `yew_edit_insert` owner and its draw path has
  no mutation or viewport-follow/scroll/center call;
- `src/text/edit.c` contains the two ordered edit notifications and no callback
  registry;
- symbol determinism scans are limited to `sym*.c/h` and match no floating
  ranking or raw `qsort` token;
- `JsonValue` remains arena-owned; `json.c/h` has no LSP-specific identifier,
  locale-dependent width operation, sort, or hash emitter; its sole annotated
  real conversion is precision-bearing `%.17g`;
- UTF-16 conversion has no display-width dependency, and LSP callers use only
  the two position-boundary helpers;
- the protocol line-arithmetic scan is empty after `YEW-F-016`;
- the snippet-policy scan finds only its intended owner paragraph; and
- `YEW_TXN_LSP` appears at the enum and the rename apply owner only.

The checked `_Static_assert` boundaries for `JsonValue` and `SymEntry` remain
live in the focused builds. Repository bans and the closeout-contract gates
pass with S59-A26/A27 applied.

### Fuzz and exact deterministic samples

Bounded deterministic campaigns ran for 10,000 iterations at seed 1 under
Clang, GCC, and ASan/UBSan. Each configuration produced the same hash:

| Campaign | Hash |
|---|---|
| shadow | `7f6650298b07dd0e` |
| symbol index | `7f6650298b07dd0e` |
| JSON | `c0867a32a2cfe95f` |
| JSON-RPC | `eb112306ee1a612f` |
| LSP message | `7f6650298b07dd0e` |
| LSP response | `e005668173835d31` |

The JSON/JSON-RPC runs exercise parse/write/parse and arbitrary framing; LSP
message/response runs preserve the no-edit-on-malformed-input contracts; and
the shadow and symbol-index runs exercise staleness and incremental-state
invariants. These bounded samples do not replace the contracted 4 × 50,000 or
200,000-iteration sanitizer campaigns.

### PTY evidence

Thirty unique focused PTY cases passed under Clang, GCC, and ASan/UBSan:

- 13 `s43_` shadow cases;
- four `s44_` completion-menu cases;
- five `lsp_feat_` navigation/rename cases; and
- eight `lsp_diag_` diagnostic/hint cases.

The cases include truecolor/256/16 and no-color provenance, overlay/no-jump,
two-stage escape, accept-word, clamped/flipped completion geometry, atomic
rename apply/refusal/diff/summary behavior, diagnostic pickers and narrow
layout, and message displacement/restoration of the persistent hint. The PTY
harness requires deterministic rerendering before accepting each golden.

### Performance harness controls

The symbol-index and LSP binaries' non-timing policy controls passed under
Clang, GCC, and ASan/UBSan:

```text
perf-symidx-policy: strict/advisory/sanity ok
perf-lsp-policy: strict/advisory/sanity ok
```

The shadow performance binary has no separate policy-only mode. The four
Sprint 58 grapheme soak streams currently occupy four CPU cores, so strict
shadow, symbol-index, and LSP timing was deliberately not measured in this
contaminated window. The selftests prove real decision policy and broken-run
handling; they are not substituted for the designated unloaded strict
performance matrix.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-014` | stripped-module completion boundary and exact hard-error audit; fix `114f99fb`; audit PASS in all three configurations |
| `YEW-F-015` | owner-scoped snippet downgrade gate plus completion/snippet units; fix `59d318cf`; audit PASS in all three configurations |
| `YEW-F-016` | protocol/UI line-boundary ownership and line-zero diagnostic controls; fix `07ee554b`; audit PASS in all three configurations |
| `YEW-F-115` | S59-A26 and `5a8a438f` align provider/lifecycle/feature staging with shipped owners and retained non-goals |
| `YEW-F-116` | S59-A27 and `5a8a438f` establish token-aware owner-scoped symbol/JSON format gates |
| `YEW-F-117` | attached document URI, canonical fixture root, and passing real-clangd milestone; fix `151bd0f0`; audit PASS in all three configurations |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

### Sprint 43 — shadow text

| Row | Primary witnesses |
|---|---|
| s43.1 | warning-clean focused Clang/GCC/ASan builds; exact full regression and Valgrind remain outstanding |
| s43.2 | `shadow_` staleness/revalidation units and bounded shadow fuzz; contracted four long seeds remain outstanding |
| s43.3 | current single-mutation-owner scan and interaction units |
| s43.4 | empty viewport-call scan plus overlay/no-jump PTY |
| s43.5 | overlay/no-jump golden and shadow layout units |
| s43.6 | shadow interaction filter, including delete and both Esc stages |
| s43.7 | accept-word units/PTY across unit modes and Unicode fixtures |
| s43.8 | provider provenance PTYs across color tiers and no-color rendering |
| s43.9 | debounce units and provider request-count controls |
| s43.10 | exact notification-owner scan and edit integration units |
| s43.11 | shadow/completion arbitration and suppression units |
| s43.12 | S59-A26; provider registration/status and stripped-boundary audits |
| s43.13 | committed shadow baseline and functional invariant units; uncontended strict timing remains outstanding |
| s43.14 | 48 focused tests/873 assertions plus deterministic PTY rerenders |

### Sprint 44 — symbol completion

| Row | Primary witnesses |
|---|---|
| s44.1 | warning-clean focused Clang/GCC/ASan builds; exact full regression and Valgrind remain outstanding |
| s44.2 | committed symbol-index baseline and policy selftests; uncontended strict timing remains outstanding |
| s44.3 | incremental/full-rescan differential units and bounded symbol-index fuzz |
| s44.4 | syntax-aware and plain-text symbol-index units with named hits |
| s44.5 | live `SymEntry` assertion plus workspace memory/stat controls |
| s44.6 | S59-A27 owner/token scan and permutation/ranking units |
| s44.7 | completion-menu dispatch units, including fallthrough and redispatch |
| s44.8 | completion acceptance/undo integration units |
| s44.9 | completion geometry units and below/flipped/right-edge PTYs |
| s44.10 | shadow/completion mutual-exclusion units |
| s44.11 | git and bespoke walker fallback/budget controls |
| s44.12 | repeat-walk interner and per-file cap units |
| s44.13 | S59-A26; index placeholder documentation and delivered LSP resolve path |
| s44.14 | 49 symbol tests/174,326 assertions, nine menu tests/241 assertions, and deterministic PTYs |

### Sprint 45 — JSON and JSON-RPC

| Row | Primary witnesses |
|---|---|
| s45.1 | warning-clean focused Clang/GCC/ASan builds; exact full/minimal regression and Valgrind remain outstanding |
| s45.2 | live `JsonValue` assertion and empty allocation-owner scan |
| s45.3 | JSON grammar/number and JSON-RPC framing unit tables |
| s45.4 | invalid-byte JSON/LSP round-trip controls |
| s45.5 | 245,671 JSON-filter assertions, including split-offset and chunking cases |
| s45.6 | JSON depth/node and JSON-RPC content-length rejection units |
| s45.7 | JSON number-vector and lossy-flag units |
| s45.8 | S59-A27 precision-bearing formatter scan and deterministic writer/fuzz hashes |
| s45.9 | empty locale/width scan and locale-rationale audit |
| s45.10 | identical bounded JSON/JSON-RPC hashes in three configurations; contracted 200,000-iteration runs remain outstanding |
| s45.11 | empty LSP-identifier scan over `json.c/h` |
| s45.12 | framed transport/safe-prefix regression units |
| s45.13 | S59-A26 and exact stripped-boundary audit plus configured lifecycle units |
| s45.14 | 29 JSON-filter tests/245,671 assertions and exact fixtures |

### Sprint 46 — LSP client core

| Row | Primary witnesses |
|---|---|
| s46.1 | warning-clean focused Clang/GCC/ASan builds; exact full/minimal regression and Valgrind remain outstanding |
| s46.2 | six UTF-16 differential tests/59,033 assertions in all configurations |
| s46.3 | empty display-width scan and position-boundary call-site scan |
| s46.4 | empty protocol line-arithmetic scan and line-zero diagnostic units |
| s46.5 | sync/change-vector and burst/overflow units |
| s46.6 | capability-shape and encoding-negotiation units |
| s46.7 | lifecycle/backoff, process-group, fd, and shutdown controls; live clangd leaves no child |
| s46.8 | stale-response generation/callback units |
| s46.9 | diagnostic edit-survival oracle units |
| s46.10 | glyph-width/fallback units and diagnostic color/narrow PTYs |
| s46.11 | hint displacement/restoration PTYs |
| s46.12 | committed LSP baseline and policy selftests; uncontended strict timing remains outstanding |
| s46.13 | S59-A26; nine delivered feature/capability owners and retained non-goal audit |
| s46.14 | 131 LSP-filter tests/17,963 assertions plus focused deterministic PTYs |

### Sprint 47 — LSP features and milestone

| Row | Primary witnesses |
|---|---|
| s47.1 | warning-clean focused Clang/GCC/ASan builds and 21 core panel tests; exact full/minimal regression and Valgrind remain outstanding |
| s47.2 | passing required real-clangd milestone at `c9529fb9`, including F117 path-alias regression |
| s47.3 | compile-database row-8 module-define sentinel in the live milestone; standalone two-run reproduction remains for reviewer |
| s47.4 | snippet downgrade filter, policy text audit, and LSP info/script witnesses |
| s47.5 | rename preflight, refusal, injected rollback, and live round-trip controls |
| s47.6 | multi-buffer rename undo-node units |
| s47.7 | no-disk-write rename units and dirty-buffer controls |
| s47.8 | `YEW_TXN_LSP` owner scan and rename apply units |
| s47.9 | navigation/jumplist units and focused feature PTYs |
| s47.10 | nine feature capability-gate and `-32601` units |
| s47.11 | 21 panel tests/665 assertions plus response fuzz clipping controls |
| s47.12 | location/symbol permutation units and deterministic PTY rerenders |
| s47.13 | identical bounded LSP-response fuzz hashes; contracted four long seeds remain outstanding |
| s47.14 | 131 LSP-filter tests/17,963 assertions and live milestone summary |

## Explicit blockers before READY

- An independent non-author reviewer has not inspected F11 or assigned the 70
  closeout verdicts.
- The exact complete current-baseline `make test` matrix under both compilers,
  ASan/UBSan, Valgrind, full modules, and `MODULES=""` was not rerun for this
  local packet. Darwin has no project Valgrind/LSan lane.
- The contracted long sanitizer fuzz campaigns were not rerun; the bounded
  deterministic samples are supporting evidence only.
- Strict shadow, symbol-index, and LSP performance must be measured on the
  designated unloaded runner after the Sprint 58 soak releases the host.
- Hosted CI and required non-Darwin/platform evidence must be established on
  the eventual closeout baseline.

Accordingly `.docs/audits/closeout-F11-lsp.md` remains `Reviewer: UNASSIGNED`,
all boxes remain unchecked, and its verdict remains NOT READY.
