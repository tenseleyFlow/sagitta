# yew — session handoff

**Written:** 2026-08-19. **Active implementation frontier:** Sprint 49, the
AI shadow provider. Sprint 48 and Campaign 09 are complete; Campaign 10 is
active.

---

## 0. Start here

Read, in order:

1. `.docs/plan/00-decisions.md`
2. `.docs/plan/01-architecture.md`
3. `.docs/plan/02-fletch.md`
4. `.docs/sprints/10-ai/s49-ai-shadow.md`

Sprint 49 is the binding implementation contract. Implement its deliverables
and meet its Definition of Done before beginning Sprint 50's opt-in, privacy,
redaction, and shipped-preset work.

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

## 2. Sprint 48 closeout and Sprint 49 objective

Sprint 48 completed the AI module's transport and backend floor while keeping
the user-facing AI flow off by default:

- the poll-driven plain HTTP/1.1 client covers nonblocking connect, bounded
  send/receive, content-length, chunked and close-delimited framing, timeouts,
  exact-once stale-pool retry and idle connection expiry;
- HTTPS routes through a shell-free `curl` job with secret-free argv and byte
  stdin; API keys resolve indirectly from an environment variable or command
  and are wiped, structurally redacted and excluded from diagnostics;
- one incremental SSE/NDJSON parser feeds Ollama, OpenAI-compatible and
  Anthropic adapters, with deterministic request bodies and a closed error
  taxonomy;
- Fletch `ai.backend(...)`, the insertion-ordered backend registry, reload,
  model listing, ping, backend and log commands are wired through the editor
  loop; stripped builds retain honest module errors;
- request construction and parsing have committed performance baselines, and
  a real loopback harness proves 500 request/response cycles plus pool expiry
  with zero descriptor delta before editor teardown.

Closeout evidence:

- strict full and stripped GCC/Clang builds are warning-free; focused command,
  backend, HTTP, config, key, stream and shim suites are green;
- Clang ASan/UBSan is green for 39 AI tests / 1,910 assertions and 15 HTTP
  tests / 467,453 assertions;
- plain and sanitized HTTP/AI-stream fuzzers each complete 200,000 iterations
  at seed 1 with the same deterministic hash and an empty crash corpus;
- focused Valgrind is clean for AI, live HTTP and the 500-cycle resource gate;
  the final gate reports `fd_delta=0`, with request-build and response-parse
  p99 latency far below the committed 100 microsecond ceilings;
- the fast unit tier reports 2,023 tests / 70,758,941 assertions with zero
  failures, and all structural and smoke checks are green.

Sprint 49 now owns the integration frontier: assemble bounded cursor-local
context, build FIM/chat prompts, register the AI `ShadowProvider`, batch
streamed delivery once per frame, cancel both HTTP and curl work on stale
input, count accepted AI bytes locally, and enforce the 150 ms first-token
gate against the deterministic mock backend. Sprint 50 still owns explicit
opt-in, the redaction policy and patterns, privacy documentation, presets and
the statusline badge.

## 3. Campaign sequence

1. Sprint 43 — provider-neutral shadow text (complete)
2. Sprint 44 — no-LSP buffer/workspace symbol index (complete)
3. Sprint 45 — bespoke JSON/JSON-RPC and stdio transport (complete)
4. Sprint 46 — LSP lifecycle, capabilities, changes, and diagnostics (complete)
5. Sprint 47 — completion, hover, navigation, references, rename, symbols,
   and the real-clangd milestone (complete)
6. Sprint 48 — plain HTTP, curl TLS transport, streaming parsers, backend
   adapters and API-key resolution (complete)
7. Sprint 49 — context assembly and streamed AI ghost text (active)
8. Sprint 50 — explicit opt-in, privacy/redaction rules and default presets

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
- Keep AI commands functionally off until Sprint 50's explicit opt-in flow.
- Do not feed AI output into `TextBuf` or ghost text in Sprint 48; that is the
  binding Sprint 49 boundary.
- Do not add Tree-sitter, TextMate, a JSON library or another dependency.
- Do not change performance baselines outside Sprint 56 calibration.
- Do not mark Daily Driver `EARNED` from automated evidence.
