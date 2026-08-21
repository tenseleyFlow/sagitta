# yew — session handoff

**Written:** 2026-08-20. **Active implementation frontier:** Sprint 50, AI
opt-in, privacy, redaction, and shipped presets. Sprints 48–49 and Campaign 09
are complete; Campaign 10 is active.

---

## 0. Start here

Read, in order:

1. `.docs/plan/00-decisions.md`
2. `.docs/plan/01-architecture.md`
3. `.docs/plan/02-fletch.md`
4. `.docs/sprints/10-ai/s50-ai-defaults-privacy.md`

Sprint 50 is the binding implementation contract. Sprint 49 is complete; do
not reopen its interfaces except where Sprint 50 explicitly requires the
redaction, workspace-grant, opt-in, preset, documentation, or badge seams.

## 1. Sprint 47 closeout

Sprint 47 completes the 1.0 editor-facing LSP surface:

- Completion and resolve feed the existing explicit menu or passive shadow
  provider; snippets are honestly downgraded instead of partially expanded.
- Hover and signature help use the reusable core floating panel; definition,
  declaration, type-definition and implementation navigation use the jumplist;
  references and hierarchical document symbols use deterministic pickers.
- Every feature is capability-gated and generation-aware, with plain once-only
  feedback when the active server cannot provide it.
- Workspace rename validates the entire `WorkspaceEdit` before mutation,
  resolves positions against each target buffer, applies edits back-to-front,
  records exactly one `YEW_TXN_LSP` undo node per affected buffer, and rolls the
  whole plan back on failure. It changes buffers only and never writes files.
- Program lookup for LSP jobs is PATH-aware without invoking a shell, and the
  checked-in compile-database generator is byte-reproducible.
- The stripped `MODULES=""` surface preserves the core symbol-completion
  fallback while rejecting LSP-only commands through the normal module shim.

Closeout evidence:

- hosted CI run `32189169337` is fully green: full and stripped GCC/Clang,
  ASan/UBSan plus every fixed fuzz campaign, PTY goldens, determinism,
  computed-goto parity, Unicode, Fletch, scripts, torture, syntax assets,
  structural bans, and all committed performance gates;
- the live clangd lane reports 8/8 navigation fixtures, 6 references across
  3 files, a two-file rename round trip back to a clean worktree, one expected
  diagnostic, hover, and clean teardown;
- local full/stripped fast suites are green under GCC and Clang (1,963 tests /
  70,289,311 assertions full; 1,815 / 70,026,273 stripped), as are focused
  rename and capability suites, all Sprint 44 completion PTYs, 50k response
  fuzzing under GCC, Clang and ASan/UBSan, and the LSP performance gates;
- focused Valgrind is clean for rename, capability gates, PATH-aware spawn,
  raw prompt handling and lifecycle; the generated compile database is
  byte-identical across repeated runs.

## 2. Sprint 49 closeout and Sprint 50 objective

Sprint 49 completed the live AI shadow-provider path while preserving the
off-by-default boundary:

- bounded cursor-local context and deterministic FIM/chat prompt builders feed
  the configured backend without placing secrets in prompts, argv, or logs;
- the AI `ShadowProvider` streams ghost text through the provider-neutral
  shadow surface, batches delivery once per frame, preserves matching-prefix
  streams, and cancels HTTP or curl work when input becomes stale;
- typed-prefix acceptance, explicit accept/decline, local accepted-byte
  accounting, error precedence, and provider lifecycle behavior are covered by
  unit, script, fuzz, performance, and production-provider PTY tests;
- the deterministic mock HTTP and curl paths enforce the 150 ms first-token
  budget, while structural gates pin the single prompt/redaction seam and
  prohibit unsafe logging or shell execution.

Closeout evidence:

- strict full and stripped GCC/Clang suites are green; the full GCC tier
  reports 2,050 tests / 70,761,583 assertions and the stripped tier reports
  1,829 tests / 70,026,792 assertions;
- focused Clang ASan/UBSan is green for 66 AI tests / 4,509 assertions and the
  20,000-iteration shadow fuzzer; focused Valgrind is clean for the same AI
  unit surface and both production-provider PTYs;
- every committed PTY passes its double-run determinism gate; the Sprint 49
  stream and mid-stream Escape cases are also green under Valgrind;
- performance gates report context-build p99 at 3.5 microseconds, prompt-build
  p99 at 4.1 microseconds, HTTP and curl first-token p95 at 101 ms, and live
  stream keypress p99 at 57 microseconds.

Sprint 50 now owns the final Campaign 10 shipping boundary: explicit opt-in,
schema-3 per-workspace grants, deny-pattern and path redaction, conservative
local/cloud presets, the honest privacy page, and the four-state `[AI]`
statusline badge. A fresh profile must continue to make zero network syscalls.

## 3. Campaign sequence

1. Sprint 43 — provider-neutral shadow text (complete)
2. Sprint 44 — no-LSP buffer/workspace symbol index (complete)
3. Sprint 45 — bespoke JSON/JSON-RPC and stdio transport (complete)
4. Sprint 46 — LSP lifecycle, capabilities, changes, and diagnostics (complete)
5. Sprint 47 — completion, hover, navigation, references, rename, symbols,
   and the real-clangd milestone (complete)
6. Sprint 48 — plain HTTP, curl TLS transport, streaming parsers, backend
   adapters and API-key resolution (complete)
7. Sprint 49 — context assembly and streamed AI ghost text (complete)
8. Sprint 50 — explicit opt-in, privacy/redaction rules and default presets
   (active frontier)

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
- Keep the bespoke HTTP transport plain-text and loopback-first; deny
  non-loopback HTTP unless `ai.allow_plain_remote` is explicitly enabled.
- Route every HTTPS backend through `curl`; do not add or vendor a TLS library.
- Construct subprocesses with argv arrays. Never interpolate commands through
  a shell, and never place API-key literals in `init.fl`, argv, logs or errors.
- Keep all socket, subprocess and streaming work poll-driven and bounded. No
  threads, blocking request writes, or mid-keystroke DNS resolution.
- Keep fresh-profile AI requests off until Sprint 50's explicit opt-in and
  workspace-grant gates both permit them.
- AI output may feed only the Sprint 43 ghost surface; never mutate `TextBuf`
  before the existing explicit shadow-accept path runs.
- Do not add Tree-sitter, TextMate, a JSON library or another dependency.
- Do not change performance baselines outside Sprint 56 calibration.
- Do not mark Daily Driver `EARNED` from automated evidence.
