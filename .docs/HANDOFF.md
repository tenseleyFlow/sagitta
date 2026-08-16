# yew — session handoff

**Written:** 2026-08-15. **Active implementation frontier:** Sprint 47,
LSP Features. Sprint 46 is complete and Campaign 09 remains active.

---

## 0. Start here

Read, in order:

1. `.docs/plan/00-decisions.md`
2. `.docs/plan/01-architecture.md`
3. `.docs/plan/02-fletch.md`
4. `.docs/sprints/09-completion-lsp/s47-lsp-features.md`

Sprint 47 is the binding implementation contract. Implement its deliverables
and meet its Definition of Done before entering Campaign 10.

## 1. Sprint 46 closeout

Sprint 46 turns the Sprint 45 JSON-RPC transport into a real LSP client:

- Fletch-owned per-language configuration starts one server per `(id, root)`,
  owns its copied argv/config data, and respects the stripped-module surface.
- The poll loop owns spawn, initialize/initialized, document open/change/save/
  close, timeout, graceful shutdown, process-group escalation, restart backoff,
  stderr-tail reporting, and automatic document reopen after restart.
- `src/unicode/u16.c|h` is the sole UTF-8/UTF-16 position conversion layer;
  the differential corpus checks every byte offset in both directions.
- Incremental sync records pre-edit positions, batches ordered changes, falls
  back to full sync on overflow, advances versions/generations, and drops stale
  responses before callbacks.
- Diagnostics are generation-aware mark ranges with bounded storage, severity
  gutter signs, undercurl/underline overlays, message hints, status badges,
  next/previous navigation, and the existing picker.
- The live commands are `ed.lsp.info`, `.log`, `.restart`, `.stop`,
  `.diagnostics`, `.diag_next`, and `.diag_prev`. Sprint 47 feature commands
  remain explicit named deferrals; permanent 1.0 non-goals remain documented,
  not stubbed.

Fresh local closeout evidence:

- final `make -j2 check`: 1,871 tests, 70,282,839 assertions, zero failures;
  70 script cases / 605 assertions / zero failures / one intentional skip;
  Fletch conformance, syntax assets, bans, dispatch checks, and smoke green;
- full GCC `make test`: 206 PTYs and 1,871 tests green; full Clang `make test`:
  206 PTYs and 1,871 tests green, warning-free;
- stripped GCC and Clang `MODULES=""` suites: 206 PTYs, 69 applicable scripts,
  and 1,797 tests / 70,025,358 assertions, all green and warning-free;
- focused Clang ASan/UBSan: lifecycle/position/JSON/statusline/unit and LSP
  script slices green; the production LSP dispatcher passes 10,000 sanitized
  fuzz iterations at seed 1;
- focused Valgrind with leak and fd tracking: 18 lifecycle tests (including
  200 spawn/shutdown cycles), diagnostics, sync, the script-runner selftests,
  and the traced `lsp_sync` editor script are clean;
- final perf: edit-note p99 42 ns (UTF-8) and 53.313 us (UTF-16), 10k-diagnostic
  viewport p99 1.307 ms, and framed 50 MiB keypress p99 0.396 ms — all below
  their committed budgets;
- structural checks: no width/cell logic in `u16.c`, no LSP line-number ±1,
  only the two approved position-conversion call sites, all nine contracted
  Sprint 47 command deferrals present, and 230 assertion sites across the
  seven Sprint 46 unit files.

## 2. Sprint 47 objective

Sprint 47 turns the client into the complete 1.0 editor-facing LSP surface:

- `src/mod/lsp/features.c`: completion/resolve, hover, signature help,
  definition/declaration/type/implementation navigation, references,
  document highlights, and capability gating;
- `src/mod/lsp/pickers.c`: deterministic location and hierarchical document-
  symbol pickers on the existing picker chrome;
- `src/mod/lsp/rename.c`: validated, all-or-nothing, multi-buffer
  `WorkspaceEdit` planning and rollback with one `YEW_TXN_LSP` undo node per
  affected buffer and no disk writes;
- `src/ui/panel.c|h`: a reusable core floating panel that also builds and
  tests under `MODULES=""`;
- one completion response feeding the existing explicit menu or passive
  shadow provider, with snippets honestly downgraded rather than expanded;
- the real-clangd milestone lane: 8/8 navigation fixtures, references across
  files, a two-file rename round trip back to a clean worktree, diagnostics,
  hover, and clean process/fd teardown.

The first slice should map the landed Sprint 43/44 shadow/completion APIs,
picker and jumplist APIs, prompt/transaction laws, and Sprint 46 request/
generation helpers against the exact Sprint 47 symbol ledger. Keep the core
panel independent of `YEW_WITH_LSP`; keep every server capability failure a
plain, once-only sentence.

## 3. Campaign sequence

1. Sprint 43 — provider-neutral shadow text (complete)
2. Sprint 44 — no-LSP buffer/workspace symbol index (complete)
3. Sprint 45 — bespoke JSON/JSON-RPC and stdio transport (complete)
4. Sprint 46 — LSP lifecycle, capabilities, changes, and diagnostics (complete)
5. Sprint 47 — completion, hover, navigation, references, rename, symbols,
   and the real-clangd milestone (active)

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
- Convert an LSP position against the target buffer, never the current buffer.
- Validate every rename edit and every generation before mutating any buffer;
  apply edits back-to-front and roll the entire plan back on failure.
- Rename changes buffers only. It never writes source paths to disk.
- Keep snippets downgraded; do not add a partial placeholder/tabstop mode.
- Keep the panel in core and reuse the existing menu, shadow, picker, prompt,
  overlay and jumplist surfaces rather than creating parallel UI machinery.
- Keep malformed-response fuzzing byte-identity checks around rename.
- Do not add Tree-sitter, TextMate, a JSON library or another dependency.
- Do not change performance baselines outside Sprint 56 calibration.
- Do not mark Daily Driver `EARNED` from automated evidence.
