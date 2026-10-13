# F12 AI pre-closeout evidence

- Date: 2026-10-13
- Baseline: `5f1f211e138e260127945260ca46a7a5df95baf0`
- Last current/evolved F12-owned product-source change: `0fb50fa5`
- Last dedicated F12 test-surface change: `9bb1415f`
- Host: Darwin 25.4.0 arm64 (macOS 26.4.1)
- Compilers: Apple clang 21.0.0; Homebrew GCC 16.2.0

Disposition: **NOT READY — local remediation evidence is green; independent
current-baseline review and required release/platform campaigns remain.**

This packet prepares F12 for closeout without signing it. Its author also
authored F12's contract reconciliation and therefore cannot be Sprint 59's
independent reviewer. A future reviewer must establish the baseline again,
independently inspect or rerun the cited witnesses, and decide every checkbox
in `.docs/audits/closeout-F12-ai.md`.

## Scope and provenance

The original F12 audit is `.docs/audits/audit-12-ai.md`. It reviewed detached
product baseline `b3f32645` and filed no product finding. Its defensive probes
established the off-by-default transport boundary, pre-transport cloud
redaction, loopback-only bespoke HTTP, hostile chunk framing, edit-triggered
cancellation/process accounting, global shadow staleness, and the complete
stripped-module command surface.

The current closeout recheck found and closed two contract-only Medium
defects without changing product behavior:

- `YEW-F-118` / S59-A28 replaces obsolete Sprint 48/49 future-owner hard
  errors with evidence for Sprint 50's shipped opt-in, enable/disable, trust,
  redaction, presets, privacy, and status surfaces while retaining
  off-by-default, zero-network, stripped-build, and permanent non-goal gates.
- `YEW-F-119` / S59-A29 replaces raw TLS/stats substring scans with semantic
  ownership gates. Required TLS error/routing text and stats API/lifecycle
  owners remain legal; TLS implementation symbols and stats access from
  prompt/transport owners remain forbidden.

At this baseline all 119 audit registry entries pass under Apple Clang, GCC
16, and ASan/UBSan. The remediation ledgers report zero active XFAILs and F12
has zero open confirmed findings. The current product tip includes the
runtime-prefix resolution at `0fb50fa5`; every focused check below uses the
current tree rather than the historical audit snapshot.

The Sprint 58 tier-1 soak remains frozen against product `4c99e134` plus
runner-only child `4f483f31`. Its four grapheme streams occupied four CPU
cores while this packet was assembled. This work did not change, restart, or
claim that separate campaign.

## Current-baseline executable evidence

### Strict builds, audit registry, and focused units

Apple Clang, Homebrew GCC 16, and Apple Clang ASan/UBSan built the AI, HTTP,
audit, fuzz, script, PTY, helper, and performance-test surfaces under the
locked `-std=c11 -pedantic -Wall -Wextra -Werror -Wvla` policy with zero
warnings. Each current-baseline audit binary reported:

```text
audit: 119 tests, 0 failures
```

The complete governance target is green: the 119-entry registry and the three
special invariant binaries pass; finding, fixture, priority, sampling,
closeout, deferral, and remediation-ledger checks and their negative selftests
all pass. It reports next ID `YEW-F-120`, zero active XFAILs, and zero
deferred/wontfix findings.

The complete current AI and HTTP unit filters passed identically under Clang,
GCC, and ASan/UBSan:

| Filter | Tests / assertions per configuration |
|---|---:|
| `ai_` | 107 / 6,297 |
| `http_` | 17 / 467,475 |

The AI filter covers command/module boundaries; editor state, defaults, and
workspace policy; dual debug-body gates; badge states; shipped/user redaction
and path policies; config/registry/Fletch backends; stream and adapter
framing; context, privacy, presets, opt-in, and wire/docs equality; prompts
and trimming; frame batching; both cancellation guards and 500 curl cycles;
local stats; live shadow streaming; key handling and curl argv secrecy; and
schema-3 trust migration and atomic deterministic writes.

The HTTP filter covers URL/routing rejection, exact binary-safe requests,
header-injection refusal, the all-split 20-response corpus, chunk parsing,
caps, real loopback body shapes and timeouts, pooled retry versus fresh
failure, resolver/address fallback, and cancellation that cannot repool a
connection.

### Script and PTY integration

The `ai_` script filter passed identically under Clang, GCC, and ASan/UBSan:

```text
script: 18 tests, 72 assertions, 0 failures, 0 skipped, 0 xfailed, 0 xpassed
```

It exercises defaults/privacy, enable/disable/forget, path exclusions, both
presets and switching, privacy-page opening, enum guards, remote backend and
badge guards, status, workspace origin, and the embedded-runtime gate.

Thirty-eight applicable full-module `ai_` PTY cases passed sequentially in
each configuration. They cover both opt-in branches; escape at every staged
step; literal `send`; local and remote badge states at 200, 80, and 40
columns; the long remote host; live stream and mid-stream escape; and AI
shadow provenance in truecolor, 256-color, and 16-color tiers. The stripped
module badge case is intentionally not selected by a full-module binary.

An initial attempt to run the GCC and sanitizer PTY binaries concurrently was
discarded because the harness intentionally uses one fixed `build/pty-*`
state namespace; the runners interfered and reported opt-in file-state
changes. Both configurations were rerun sequentially from clean per-case
fixtures and passed all 38 applicable cases. No source or user workspace was
modified by the discarded attempt.

### Static privacy, transport, and ownership gates

`scripts/bans.sh` passes, including the seeded AI log-owner control and the
four-reference maximum for the audited debug-body sink. Current source scans
also establish:

- no TLS implementation/library symbol (`SSL_new`, `SSL_connect`, SSL
  read/write/free, `TLS_client_method`, BIO read/write/new,
  `OPENSSL_init_ssl`, or `mbedtls_*`) under `src/mod/ai/`;
- no stats type/API reference in `prompt.c`, `backend.c`, `http.c`, or
  `backend_curl.c`;
- no `system`, `popen`, or raw `sprintf` use in the AI module;
- no FIM sentinel token in the AI module;
- no `YEW_CMD_DEFERRED` in the AI module;
- no `\\w`, `\\d`, or `\\s` shorthand in `runtime/ai-deny.fl`;
- no Unicode arrow in the statusline's ASCII remote marker; and
- one `yew_ai_prompt_build` call site, downstream of context/redaction (the
  other two textual rows are its declaration and definition).

The required TLS error enum, diagnostics, and curl-routing messages remain;
their presence is not confused with an in-process TLS implementation. Stats
persistence/formatting, public API, editor-state lifecycle, and aggregate
event recording remain confined to their S59-A29 owners.

### Fuzz and deterministic samples

Bounded deterministic campaigns ran for 10,000 iterations at seed 1 under
Clang, GCC, and ASan/UBSan. Each configuration produced the same corpus count
and hash:

| Campaign | Corpus | Hash |
|---|---:|---|
| HTTP parser | 15 | `457685ff0e8c5ecd` |
| AI stream | 5 | `7f6650298b07dd0e` |
| AI shadow | 5 | `7f6650298b07dd0e` |
| AI redaction | 5 | `7f6650298b07dd0e` |

These samples cover arbitrary split/framing input, SSE/ndjson streaming,
edit/response/accept interleavings, and shipped-pattern boundary inputs. They
do not replace the contracted 20,000/200,000-iteration sanitizer campaigns or
the original audit's four independent 50,000-iteration HTTP streams.

### Performance harness controls

The three F12 performance binaries' non-timing policy controls passed under
Clang, GCC, and ASan/UBSan:

```text
perf-ai-http-policy: strict/advisory/sanity ok
perf-ai-shadow-policy: strict/advisory/sanity ok
perf-ai-privacy-policy: strict/advisory/sanity ok
```

The four Sprint 58 grapheme soak streams currently occupy four CPU cores, so
strict HTTP, shadow, redaction, path, and keypress timing was deliberately not
measured in this contaminated window. The selftests prove real decision policy
and broken-run handling; they are not substituted for the designated unloaded
strict performance matrix.

## Prior-finding reconciliation

| Finding | Remediation witness at this baseline |
|---|---|
| `YEW-F-118` | S59-A28 and `d8fd10ad` align Sprint 48/49 staging with shipped Sprint 50 commands while preserving off-by-default and stripped-module boundaries |
| `YEW-F-119` | S59-A29 and `d8fd10ad` align TLS/stats checks with their semantic owners; executable scans and repository bans pass |

## Candidate evidence map for the closeout reviewer

These are witnesses to inspect, not pre-approved checkbox verdicts.

### Sprint 48 — HTTP and backends

| Row | Primary witnesses |
|---|---|
| s48.1 | warning-clean focused Clang/GCC/ASan builds; exact full/minimal regression and Valgrind remain outstanding |
| s48.2 | S59-A29 TLS implementation-symbol scan plus URL/routing units |
| s48.3 | 467,475-assertion HTTP filter and all-split response corpus |
| s48.4 | chunk extension/trailer/post-CRLF and labelled trailer-trap units |
| s48.5 | close-delimited live-server success/truncation units |
| s48.6 | 100-then-200 corpus and terminal status units |
| s48.7 | curl argv/key units, redacted-header units, and AI log-owner bans |
| s48.8 | literal-key/userinfo config rejection units and scripts |
| s48.9 | backend error-class/message table units |
| s48.10 | curl absence/version/probe-cache units |
| s48.11 | identical bounded HTTP/stream hashes and empty process-API scan; contracted long runs remain outstanding |
| s48.12 | S59-A28; live opt-in/stats commands, off-by-default units, scripts, and PTYs |
| s48.13 | live HTTP/cancellation fd controls and committed baseline; unloaded strict timing remains outstanding |
| s48.14 | HTTP plus AI assertion counts and deterministic fixtures |

### Sprint 49 — AI shadow

| Row | Primary witnesses |
|---|---|
| s49.1 | warning-clean focused Clang/GCC/ASan builds; exact full/minimal regression and Valgrind remain outstanding |
| s49.2 | loopback-only helpers and performance policy controls; unloaded 100-run strict latency remains outstanding |
| s49.3 | frame batching/rate/growth units and policy controls; strict keypress timing remains outstanding |
| s49.4 | Unicode/grapheme context units including 100 MiB budget-bounded case |
| s49.5 | independent sequence-floor and transport-abort controls plus 500 curl reap cycles |
| s49.6 | exact prompt/FIM units and empty sentinel scan |
| s49.7 | single call-site ownership and context/redaction hard-gate units |
| s49.8 | S59-A29 owner scan plus local stats persistence/render/missing-file units |
| s49.9 | repository AI log-owner ban and dual debug-body gate units |
| s49.10 | registered AI shadow, seven decline paths, accept/undo units, and provenance PTYs |
| s49.11 | S59-A28; default-off zero-transport units plus shipped opt-in scripts/PTYs |
| s49.12 | identical bounded shadow fuzz hash and cancellation accounting; contracted 20,000-iteration sanitizer run remains outstanding |
| s49.13 | mock-only live units and documented helper surface; full test-target rebuild remains for release matrix |
| s49.14 | 107 AI tests/6,297 assertions and deterministic selected PTYs |

### Sprint 50 — defaults and privacy

| Row | Primary witnesses |
|---|---|
| s50.1 | warning-clean focused Clang/GCC/ASan builds; exact full/minimal regression and Valgrind remain outstanding |
| s50.2 | default-off provider/command units and scripts with zero transport starts |
| s50.3 | passing seeded repository log-owner ban and dual debug-body cycle units |
| s50.4 | shipped/user redaction fixture matrices, shorthand scan, and policy perf selftest; strict 2 ms timing remains outstanding |
| s50.5 | cloud zero-byte and loopback exact-elision privacy units for both transports |
| s50.6 | workspace-origin redaction/config lifecycle units and scripts |
| s50.7 | default/user path matrices, `**` rejection, and excluded-context counter |
| s50.8 | schema-3 trust migration, inode replacement, unknown-field, sorting, and atomic-write units |
| s50.9 | both opt-in branches and all escape/literal-send PTYs plus scripts |
| s50.10 | preset parse/execute/switch/unknown script and unit controls |
| s50.11 | all local/remote badge states at three widths plus remote-hide validation |
| s50.12 | readonly privacy page plus body/header wire-field equality units |
| s50.13 | identical bounded redaction fuzz hash; contracted 200,000-iteration run remains outstanding |
| s50.14 | S59-A28, empty deferred-command scan, registered shadow, and end-to-end enable PTYs/scripts |
| s50.15 | 107 AI tests/6,297 assertions, 18 scripts/72 assertions, and 38 applicable PTYs per configuration |

## Explicit blockers before READY

- An independent non-author reviewer has not inspected F12 or assigned the 43
  closeout verdicts.
- The exact complete current-baseline `make test` matrix under both compilers,
  ASan/UBSan, Valgrind, full modules, and `MODULES=""` was not rerun for this
  local packet. Darwin has no project Valgrind/LSan lane.
- The contracted long sanitizer fuzz campaigns were not rerun; the bounded
  deterministic samples are supporting evidence only.
- Strict AI timing must be measured on the designated unloaded runner after
  the Sprint 58 soak releases the host.
- Hosted CI and required non-Darwin/platform evidence must be established on
  the eventual closeout baseline.

Accordingly `.docs/audits/closeout-F12-ai.md` remains `Reviewer: UNASSIGNED`,
all boxes remain unchecked, and its verdict remains NOT READY.
