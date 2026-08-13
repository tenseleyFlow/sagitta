# yew — session handoff

**Written:** 2026-08-13. **Active implementation frontier:** Sprint 46,
LSP Client Core. Sprint 45 is complete and Campaign 09 is active.

---

## 0. Start here

Read, in order:

1. `.docs/plan/00-decisions.md`
2. `.docs/plan/01-architecture.md`
3. `.docs/plan/02-fletch.md`
4. `.docs/sprints/09-completion-lsp/s46-lsp-client-core.md`

Sprint 46 is the binding implementation contract. Implement its deliverables
and meet its Definition of Done before entering Sprint 47.

## 1. Sprint 45 closeout

Sprint 45 opens the real LSP module boundary without coupling the protocol
layers to editor text state:

- `src/mod/lsp/json.c|h` is a strict RFC 8259 parser and deterministic writer
  over an arena-owned, insertion-ordered tree. It preserves embedded NULs and
  invalid user bytes, rejects non-finite numbers, enforces the 128-depth,
  64 MiB and 4,194,304-node limits, and has no LSP-specific identifiers.
- `src/mod/lsp/jsonrpc.c|h` owns resumable `Content-Length` framing,
  classification, integer and string ids, a 256-entry pending table,
  timeouts, cancellation, reply/error mapping and the eight-malformed-message
  kill threshold.
- The Sprint 19 job layer now has a module-neutral framed sink with raw stdout,
  isolated stderr, poll-driven transmit, EOF failure reporting and RPC
  deadlines integrated into the editor's job clock.
- All 17 `ed.lsp.*` commands exist in full and stripped profiles. The live
  Sprint 45 surface reports the exact full-build deferral or the exact
  excluded-module error; Sprint 46/47 surfaces remain loud and named.
- `fuzz_json` and `fuzz_jsonrpc` join `make fuzz`; the writer corpus pins
  byte-identical emission, and the real 4M-node boundary is exercised.

Fresh local closeout evidence:

- JSON/JSON-RPC: 26 tests, 245,630 assertions, zero failures;
- framed transport: 4 tests, 29 assertions, zero failures;
- full GCC unit suite: 1,820 tests, 70,259,386 assertions, zero failures;
- stripped GCC and Clang unit suites: 1,790 tests, 70,013,685 assertions,
  zero failures, with both stripped smoke runs green;
- strict GCC/Clang builds are warning-free in full and stripped profiles;
- both new fuzzers pass 200,000 iterations at seed 1, plain and under Clang
  ASan/UBSan; the focused sanitized unit suites are green;
- Valgrind is clean over 25 non-death JSON/JSON-RPC tests (245,615 assertions)
  and all four framed-transport tests, including fd tracking;
- the arena-only, formatter, no-sort/no-hash, locale-ban, protocol-neutrality,
  crash-corpus and general ban gates are green.

One unrelated local full-Clang run hit the existing clipboard subprocess
test's 2 ms wall-clock budget while every CPU was 93–98% busy with external
Rust builds. Its functional assertions passed, the isolated failure was the
latency assertion only, and the budget was not weakened. The normal remote CI
run on the pushed closeout is the clean-host adjudication.

## 2. Sprint 46 objective

Sprint 46 turns the Sprint 45 protocol foundation into a real client. Its
required surface is:

- per-language server configuration and `(server id, root)` instance sharing;
- spawn, initialize/initialized, graceful shutdown, crash detection, bounded
  restart backoff and actionable give-up diagnostics;
- capability and `positionEncoding` negotiation;
- `file:` URI byte round-trips and the sole UTF-8/UTF-16 position conversion
  family under `src/unicode/`;
- `didOpen`, batched incremental `didChange`, `didSave` and `didClose`, with
  pre-edit coordinates and full-sync degradation on overflow;
- generation checks that drop stale responses before feature callbacks;
- diagnostics stored as marks and rendered through gutter signs, overlays,
  message hints, status badges and the existing picker;
- unit, script, PTY, fuzz, perf, sanitizer and ownership coverage for the
  complete lifecycle.

The first implementation slice should map the existing buffer language,
workspace-root, option, job, edit-notification, timer, mark, overlay, gutter,
message and picker APIs against the Sprint 46 names before introducing the
client structs.

## 3. Campaign sequence

1. Sprint 43 — provider-neutral shadow text (complete)
2. Sprint 44 — no-LSP buffer/workspace symbol index (complete)
3. Sprint 45 — bespoke JSON/JSON-RPC and stdio transport (complete)
4. Sprint 46 — LSP lifecycle, capabilities, changes, and diagnostics (active)
5. Sprint 47 — completion, hover, navigation, references, rename, and symbols

There is no live language-server lifecycle yet. Sprint 45 deliberately stops
at the framed protocol and command/module boundary; Sprint 46 owns spawning
and documents, and Sprint 47 owns editor-facing language features.

## 4. Daily Driver remains separate and pending

Sprint 42's field milestone remains `PENDING` at:

- 0/10 working days;
- 0/40 dogfood hours;
- 0/200,000 real keystrokes;
- 0/3 abnormal-exit trials;
- 0/20 exact resume cycles.

Automated tests, generated goldens, benchmarks and editing in another editor
never count. A future sprint contributes only if a qualifying yew session is
designated and logged before eligible implementation edits.

## 5. Invariants and cautions

- Preserve byte identity, terminal restoration, deterministic rendering and
  central edit/undo laws ahead of latency or convenience.
- Keep `json.c|h` protocol-neutral and arena-only; Sprint 48 reuses it for
  HTTP bodies.
- Never pass framed stdout through `yew_job_safe_prefix`; doing so can retain
  a partial JSON frame forever. Keep stderr out of the frame parser.
- Keep RPC deadlines in the shared job clock so a silent server cannot make a
  pending request immortal.
- Watch item for Sprint 46/47: input reads are budgeted and framing is
  resumable, but parsing begins synchronously once a complete body is present.
  Measure realistic large diagnostics/workspace payloads before adding feature
  traffic; if the event-loop budget is threatened, slice parsing without
  weakening the 64 MiB or node limits.
- Do not add Tree-sitter, TextMate, a JSON library or another dependency.
- Do not change performance baselines outside Sprint 56 calibration.
- Do not mark Daily Driver `EARNED` from automated evidence.
