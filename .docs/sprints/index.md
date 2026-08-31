# yew Sprint Index

67 sprints across 15 campaigns: empty repo → **v1.0.0**, a speed-first,
bespoke-first, modal-paradigm-first terminal editor plus its language,
Fletch. Small bites, clear milestones, testable deliverables at every
stage. Each sprint is independently reviewable and mergeable; every sprint
that lands new surface area also lands its tests (unit, script, pty, fuzz,
or perf as appropriate).

61 whole-numbered sprints plus six fractional ones (18.5, 41.5, 42.5,
55.5, 56.5, 57.5) split off during authoring — the pressure valve working as
designed.
Renumbering is forbidden once files exist, so fractional ids are permanent.

Binding documents — read before any sprint:
- `.docs/sprints/AUTHORING.md` — the sprint-file contract, the shared
  symbol ledger, and the **Landed APIs** list (cite those names exactly;
  it is what keeps 65 files from inventing 65 spellings of the same call)
- `.docs/plan/00-decisions.md` — locked decisions + the 10 invariants
- `.docs/plan/01-architecture.md` — module map, event loop, data model
- `.docs/plan/02-fletch.md` — language requirements incl. the round-trip law

## Campaign map

### Campaign 00 — Scaffolding (`00-scaffolding/`)
- Sprint 0 (s00-repo-and-foundations.md) — Repo layout, the one Makefile
  (MODULES= plumbing from day one), GPL-3.0 LICENSE, util containers
  (arena, vec, strmap, intern, buf, stable sort), diag/log module,
  panic/exit contract, `--version/--help` shell
- Sprint 1 (s01-test-infrastructure.md) — Bespoke unit harness (registry
  table, no ctor attributes), script-test runner skeleton, `make test`,
  CI: gcc/clang/ASan/UBSan/determinism lanes
- Sprint 2 (s02-unicode-foundation.md) — UTF-8 encode/decode, grapheme
  segmentation, width tables (generator script + checked-in generated
  tables), lossless invalid-byte policy, decoder fuzzer. **Invariant 2
  starts here, before any buffer exists**

### Campaign 01 — Terminal (`01-terminal/`)
- Sprint 3 (s03-tty-lifecycle.md) — termios raw mode, alternate screen,
  SIGWINCH, the terminal-restore guarantee (all exit paths incl. crash
  handler), tty capability probing
- Sprint 4 (s04-input-decoding.md) — Escape-sequence state machine, kitty
  keyboard protocol negotiation + legacy fallback, bracketed paste, SGR
  mouse, focus events; normalized KeyEvent model; input fuzzer
- Sprint 5 (s05-grid-and-render.md) — Cell grid double buffer, wide-glyph
  cells, damage diff → minimal escape stream, truecolor/256/16 fallback,
  synchronized output (2026)
- Sprint 6 (s06-pty-test-harness.md) — Fork-under-pty harness: feed keys,
  snapshot grid, byte-compare goldens. **MILESTONE: first paint** — a demo
  binary renders a static screen through the full stack under test

### Campaign 02 — Text engine (`02-text-engine/`)
- Sprint 7 (s07-piece-tree.md) — Piece tree with byte+line subtree counts,
  edit primitives, iterators, line↔offset both directions O(log n)
- Sprint 8 (s08-file-io-no-corruption.md) — Load (binary detect, CRLF/BOM
  policy), atomic save decision table, crash journal, kill -9 torture
  test. **Invariant 1 lands here and never leaves CI**
- Sprint 9 (s09-cursors-and-coords.md) — Typed coordinates (byte/char/
  grapheme/cell), cursor engine, marks with bias rules, multi-cursor
  foundations
- Sprint 10 (s10-undo-tree.md) — Op-log undo tree, transactions,
  serialization format for workspace persistence
- Sprint 11 (s11-buffer-torture.md) — Differential fuzz vs naive oracle,
  1 GB budgets, property tests; buffer perf baselines committed
- Sprint 12 (s12-registers-clipboard.md) — Named registers, kill-ring
  semantics, OSC 52 system clipboard + subprocess fallbacks

### Campaign 03 — Modal core (`03-modal-core/`)
- Sprint 13 (s13-keymap-engine.md) — Key-trie keymaps, mode layers, chord
  timeouts, the named-command registry (one dispatch surface for keys /
  E-mode / Fletch / recorder)
- Sprint 14 (s14-modes-L-I.md) — L and I modes, the vertical slice: open,
  move, insert, save. **MILESTONE: notepad** — yew edits a real file
- Sprint 15 (s15-viewport-statusline.md) — Scrolling, wrap policy, gutter,
  statusline with colored mode chip, message line
- Sprint 16 (s16-modes-W-B.md) — Word unit engine (Unicode boundaries,
  alt-variants), Block unit engine (paragraph/indent/bracket providers;
  syntax provider slots in at Campaign 08)
- Sprint 17 (s17-mode-H-multicursor.md) — Highlight mode over all unit
  engines, char/line/column selections, selection→multi-cursor lift,
  simultaneous multi-cursor editing as one transaction.
  **MILESTONE: modal complete**

### Campaign 04 — Execute & search (`04-execute-search/`)
- Sprint 18 (s18-cmdline.md) — E mode: prompt editing, history,
  completion, internal command surface (the registry, exposed)
- Sprint 18.5 (s18_5-reactive-cmdline.md) — Live fuzzy completion menu:
  fuss-derived scorer (pulls Sprint 26 §1/§2 forward), match
  highlighting, non-destructive selection + ghost text, completion-source
  registry, reusable menu widget, mouse, live argument hints
- Sprint 19 (s19-shell-execution.md) — `$SHELL -c` jobs, streamed output
  buffers, region-through-filter pipes, async job control, timeouts.
  **The tmux half begins**
- Sprint 20 (s20-regex-engine.md) — Thompson NFA/DFA hybrid, no
  backtracking, Unicode classes, literal fast path, regex fuzzer +
  pathological-pattern latency gate
- Sprint 21 (s21-search-replace.md) — Incremental search UI, match
  highlighting, confirm-replace, jumplist/changelist

### Campaign 05 — UI & workspace (`05-ui-workspace/`)
- Sprint 22 (s22-layout-panes.md) — Pane tree (h/v splits), focus nav,
  resize, cells-based layout/draw split, clickable-region registry
- Sprint 23 (s23-tabs.md) — Tab strip, stable tab_ids, overflow scrolling
  (`<` / `>N`), modified markers
- Sprint 24 (s24-tab-groups.md) — Facsimile model: group_id+ordinal, no
  member lists, lazy hydration (deferred tabs), group picker, one-line
  row-1/row-2 rendering, group navigation (walk-through + enter-at-edge)
- Sprint 25 (s25-workspace-state.md) — Dir=workspace, XDG state file in
  Fletch data format (hand-written serializer until Campaign 06; schema
  frozen here), resume tabs/panes/cursors/undo exactly
- Sprint 26 (s26-fuzzy-finder.md) — Fuss-scoring fuzzy engine, file
  finder, buffer switcher, type-to-jump plumbing
- Sprint 27 (s27-mouse-and-feel.md) — Mouse routing via region registry,
  drag resize/reorder, scroll; UI polish pass against the pty goldens

### Campaign 06 — Fletch core (`06-fletch-core/`)
- Sprint 28 (s28-fletch-design-freeze.md) — **The language spec**: grammar,
  motion-literal vocabulary, semantics, capability model. Frozen by
  review before implementation continues
- Sprint 29 (s29-lexer-parser.md) — Lexer, recursive-descent parser, AST,
  caret diagnostics; lexer/parser fuzzers
- Sprint 30 (s30-values-vm.md) — Tagged values, bytecode compiler, VM
  loop, mark-sweep GC, insertion-ordered maps
- Sprint 31 (s31-stdlib.md) — Strings (grapheme-aware), lists, maps, math,
  fmt, sandboxed io, module system
- Sprint 32 (s32-repl-and-errors.md) — `yew fl` REPL/eval, runtime stack
  traces, error-quality pass, VM fuzzer
- Sprint 33 (s33-fletch-conformance-perf.md) — Spec conformance suite,
  bench baselines (config-load < 1 ms gate). **MILESTONE: Fletch hello
  world**

### Campaign 07 — Fletch ↔ editor (`07-fletch-editor/`)
- Sprint 34 (s34-editor-api.md) — buffer/cursor/span/window handles, the
  capability-checked binding layer onto the command registry, `edit{}`
  transactions, `on(...)` event hooks
- Sprint 35 (s35-recorder-roundtrip.md) — Macro recorder → terse Fletch
  source, run-fold smartness, **the round-trip law harness** (invariant 10)
- Sprint 36 (s36-config-system.md) — init.fl loading, options model,
  `bind()`, workspace `.yew.fl` trust prompt, reload; **the good
  default config ships here**. Workspace-state serializer switches to the
  Fletch data path. **MILESTONE: self-config** — yew configured by its
  own language
- Sprint 37 (s37-headless-batch.md) — `yew --batch`, exit codes, stdio
  contract, docs; script tests migrate onto it wholesale
- Sprint 38 (s38-macro-ux.md) — Record/replay UX, macro registers, macro
  editing flow (open recording as a .fl scratch), macro library dirs

### Campaign 08 — Highlighting (`08-highlighting/`)
- Sprint 39 (s39-syntax-engine.md) — Line-state-machine engine,
  incremental invalidation by damage range, attr/theme model
- Sprint 40 (s40-syntax-def-format.md) — Declarative defs in Fletch data →
  compiled tables; def-author docs; B-mode syntax provider hooks in
- Sprint 41 (s41-langpack-1-themes.md) — C, Fletch, sh, make, markdown;
  default dark + light themes; highlight perf gate (full-viewport restyle
  within frame budget)
- Sprint 42 (s42-langpack-2.md) — python, rust, go, js/ts, fortran, json,
  yaml, toml. **MILESTONE: daily driver** — dogfood gate: yew edits
  yew comfortably; maintainers switch. The implementation is complete;
  the field-evidence milestone remains independently pending
- Sprint 41.5 (s41_5-embedded-languages.md) — Cross-definition dispatch:
  markdown fenced blocks, HTML script/style, shell `$(…)`, JS template
  literals + JSX. This permanent id executed after Sprint 42 so the final
  s41/s42 corpus paid the state/golden re-gate once
- Sprint 42.5 (s42_5-native-language-pack.md) — Remove the accidental
  32-entry registry/cache ceilings, index built-in detection, centralize
  syntax-pack test metadata, and ship Wolf plus 28 more definitions for
  exactly 48 built-in modes. **COMPLETE 2026-08-12:** all hard gates green;
  the historical relative perf trip closed through the sprint's documented
  same-machine control adjudication without changing baselines

### Campaign 09 — Completion & LSP (`09-completion-lsp/`)
- **COMPLETE 2026-08-18.** Sprint 47 is complete; Campaign 10 / Sprint 50 is
  the binding implementation frontier.
- Sprint 43 (s43-shadow-text.md) — Ghost-text engine: render, provenance
  styling, accept word/line/all, dismiss, debounce. **COMPLETE 2026-08-12:**
  all required CI lanes green, including the committed shadow performance
  baseline and its ≤ 250 µs frame gate
- Sprint 44 (s44-symbol-completion.md) — Bespoke buffer/workspace symbol
  index, fuzzy ranking — the no-LSP completion tier. **COMPLETE 2026-08-12:**
  fast CI, Sprint 44 PTY goldens, four-seed symbol fuzzing, Clang, focused
  ASan/UBSan, and the committed performance gates are green
- Sprint 45 (s45-json-jsonrpc.md) — Bespoke JSON reader/writer (module
  boundary starts here: `MODULES=lsp`), JSON-RPC framing, pipe transport.
  **COMPLETE 2026-08-13:** strict arena JSON, resumable JSON-RPC, the framed
  job sink, full/stripped module surfaces, deterministic corpus, 200k plain
  and sanitized fuzz campaigns, focused Valgrind and structural gates green
- Sprint 46 (s46-lsp-client-core.md) — Server lifecycle, capability
  negotiation, incremental didChange with generation counters,
  diagnostics UI. **COMPLETE 2026-08-15:** full/stripped GCC and Clang,
  scripts and PTYs, focused ASan/UBSan and Valgrind, 10k sanitized LSP
  message fuzzing, structural checks, and all committed LSP performance
  budgets are green
- Sprint 47 (s47-lsp-features.md) — Completion (menu + shadow), hover,
  goto-def, references, rename, symbols. **MILESTONE: clangd navigates
  yew's own source in CI. COMPLETE 2026-08-18:** the hosted full/stripped
  GCC and Clang matrix, ASan/UBSan, PTY, determinism, fuzz, performance and
  live-clangd lanes are green; clangd proves 8/8 navigation fixtures,
  6 references across 3 files, a clean rename round trip, diagnostics and
  hover. Focused Valgrind is clean.

### Campaign 10 — AI (`10-ai/`)
- **COMPLETE 2026-08-21.** Sprint 50 closed Campaign 10; the AI module has
  no deferred command or runtime path.
- Sprint 48 (s48-http-and-backends.md) — Bespoke HTTP/1.1 for localhost,
  curl-subprocess backend for TLS cloud, ollama + OpenAI-compatible +
  Anthropic adapters (`MODULES=ai`). **COMPLETE 2026-08-19:** full and
  stripped GCC/Clang surfaces, ASan/UBSan, 200k plain and sanitized fuzzing,
  focused Valgrind, live loopback transport tests, and the 500-cycle
  zero-fd-delta resource/performance gate are green
- Sprint 49 (s49-ai-shadow.md) — Context assembly, streaming into ghost
  text, keystroke cancellation, latency budget. **COMPLETE 2026-08-20:**
  full/stripped GCC and Clang, focused ASan/UBSan and Valgrind, deterministic
  production-provider PTYs, 20k plain and sanitized shadow fuzzing, all
  structural/privacy gates, and both 101 ms first-token transport gates are
  green; context p99 is 3.5 us and live-stream keypress p99 is 57 us
- Sprint 49.5 (s49_5-cursor-latency.md) — Restore O(damage) hybrid-number
  cursor rendering, gate idle arrows on the reported tiny Wolf case, and keep
  syntax benchmarks out of the persistent editor log. **COMPLETE
  2026-08-20:** exact-file arrow p99 32.7 us against the 5 ms hard limit;
  fast CI, full PTY, GCC/Clang, and `perf-syn` green with zero log growth
- Sprint 50 (s50-ai-defaults-privacy.md) — Off-by-default opt-in flow,
  redaction rules, the good default configs for local and cloud models.
  **COMPLETE 2026-08-21:** full and stripped GCC/Clang, hosted
  ASan/UBSan and determinism, full PTY, focused current-code Valgrind, the
  hosted Valgrind memory-error scan, and 200k sanitized redaction fuzzing
  (`d67ee79f885b048f`) are clean. AI transport, streaming, and privacy
  performance gates are green. On the exact reported Wolf file, keypress
  p99 is 1.221 ms, arrow-to-paint p99 is 41.6 us, and cold open is 3.726 ms.

### Campaign 11 — FUSS & git (`11-fuss-git/`)
- **COMPLETE 2026-08-22.** Sprint 53 closed Campaign 11; the Git/FUSS module
  has no deferred 1.0 command or runtime path.
- Sprint 51 (s51-git-layer.md) — Subprocess plumbing, porcelain parsers,
  repo detection, wall-clock TTL status cache (`MODULES=fuss`). **COMPLETE
  2026-08-21:** full and stripped GCC/Clang surfaces, fast CI,
  ASan/UBSan, focused Valgrind, deterministic scripts, 200k × 4-seed
  porcelain fuzzing, real-Git fixture integration, and the committed parse
  and keypress latency gates are green
- Sprint 52 (s52-fuss-mode.md) — F mode: dirty-first tree, status glyphs
  (`↑ ✗ ↓`), depth-aware sibling nav, fuzzy jump, git verbs
  (stage/unstage/commit/amend/push/pull/stash/branch/…), mode-colored
  footer, collapsed-state preservation. **COMPLETE 2026-08-21:** strict
  full and stripped builds, the fast tier, 50 focused unit tests / 3,837
  assertions, 333 real-Git workflow assertions, 20,000 live F-mode fuzz
  sequences, all 19 FUSS PTYs in Unicode and ASCII, rebase handover and
  SIGTERM restoration, and the committed performance gates are green
- Sprint 53 (s53-editor-git.md) — Gutter hunk signs, inline blame, diff
  view, branch in statusline, group-open from F mode (ties to tab groups).
  **COMPLETE 2026-08-22:** strict full GCC/Clang and ASan/UBSan suites,
  deterministic PTYs and scripts, real-Git layer/workflow/patch fixtures,
  structural no-spawn/no-watcher/no-checkout gates, and the committed
  100-kLOC gutter performance budgets are green; the pushed closeout SHA is
  gated by the full hosted matrix and explicit Valgrind dispatch

### Campaign 12 — Plugins (`12-plugins/`)
- **COMPLETE 2026-08-26.** Sprint 55.5 closed Campaign 12; Campaign 13 is
  active at Sprint 57.
- Sprint 54 (s54-plugin-system.md) — Fletch package layout, XDG discovery,
  lifecycle, event bus, capability prompts/persistence
  (`MODULES=plugins`). **COMPLETE 2026-08-23:** full/stripped GCC, the
  Clang ASan/UBSan fast tier, deterministic capability/picker/restart PTYs,
  hostile zero-residue teardown, manifest fuzzing, and both plugin
  performance gates are green; the one-hour sanitized manifest campaign is
  wired into the on-demand/nightly CI lane
- Sprint 55 (s55-pkg-and-presets.md) — `yew pkg` git-based
  install/update/lock/verify, the lockfile + tree-hash integrity model,
  and the cloud-storage workflow preset (which trades the atomic-rename
  save for inode identity inside synced trees — the trade is written out).
  **COMPLETE 2026-08-24:** strict full GCC/Clang and plugins-only suites,
  50 package integrations, deterministic lock/recovery tests, cloud/save
  scripts and torture, package-tree fuzzing, structural/docs gates, and all
  three sprint performance budgets are green locally; hosted and explicit
  Valgrind closeout gates remain required on the pushed SHA
- Sprint 55.5 (s55_5-example-plugins.md) — The two shipped example
  plugins in full annotated Fletch (trailing-whitespace; session-notes),
  the plugin-author guide they anchor, and the CI rule that exercises
  both every build so the public plugin API cannot rot. **COMPLETE
  2026-08-26:** exact-tree package installs, source/API/event rot gates,
  deterministic plugin scripts, docs checks, full and stripped GCC/Clang,
  ASan/UBSan, PTY, LSP, torture, determinism, and all performance budgets
  are green on hosted CI run 33025642745 attempt 3

### Campaign 13 — Performance (`13-performance/`)
- **ACTIVE.** Sprint 56 and Sprint 56.5 are repository-complete. Sprint 56's
  pinned designated-hardware evidence remains pending because no self-hosted
  runner is registered. Sprint 57 is the binding implementation frontier.
- Sprint 56 (s56-latency-gates.md) — Profiling pass, reference-hardware
  calibration, all budgets from `00-decisions.md` become CI gates with
  committed baselines. **REPOSITORY COMPLETE 2026-08-27:** profiler, calibration,
  advisory hosted lanes, anti-flap evaluator, transactional baseline update,
  regression gates, and the 5 ms cursor transition repair are implemented;
  promotion of real x86_64/arm64 references, required self-hosted jobs, and
  five-run evidence remain external closeout prerequisites
- Sprint 56.5 (s56_5-workspace-interaction.md) — Deterministic startup workspace
  resolution, restored-state CLI merge, non-Git all-files FUSS drawer, explicit
  tab/split opening, and insertion-preview shadow composition that never erases
  real document cells. **COMPLETE 2026-08-27:** fast CI, strict full/minimal
  GCC/Clang, focused ASan/UBSan and Valgrind, deterministic drawer PTYs,
  sanitizer fuzz, structural gates, and the shadow/FUSS performance budgets are
  green; independent lifecycle review reports no findings
- Sprint 57 (s57-size-and-embedded.md) — Binary-size budgets per MODULES
  config, allocation audit, `x86_64-linux-musl` static embedded profile
  proven on-target. **ACTIVE.**
- Sprint 57.5 (s57_5-fuss-compact-tree.md) — Compact line-free FUSS tree,
  remembered expansion state, open-file ancestry, adaptive drawer width and
  full-screen fallback, plus a bright editor/drawer divider. **QUEUED NEXT.**

### Campaign 14 — Audits & release (`14-audits-release/`)
- Sprint 58 (s58-adversarial-audits.md) — Audit fronts per subsystem
  (unicode, term, text, modal, fletch, syn, lsp, ai, git, ui, persistence),
  fuzz campaigns extended, findings ledger with stable IDs
- Sprint 59 (s59-remediation-docs.md) — Findings burn-down, man pages,
  `yew tutor` (interactive, dogfoods the pty harness), user manual,
  Fletch book chapter 1
- Sprint 60 (s60-v1.0.0-landing.md) — Packaging (AUR, brew tap, RPM spec —
  the fuss distribution set), README, keep-a-changelog CHANGELOG,
  reproducible release builds, `--version` contract. **v1.0.0**

## Milestones

| Milestone | Sprint | Proof |
|---|---|---|
| First paint | 6 | full term stack renders a screen under the pty harness |
| **Notepad** | 14 | open → edit → atomic save, kill -9 torture green |
| Modal complete | 17 | L/W/B/H/I all live; multi-cursor transactional |
| Shell integration | 19 | E mode runs `$SHELL` jobs; region filters work |
| Workspace resume | 25 | quit/reopen restores tabs, panes, cursors exactly |
| **Fletch hello world** | 33 | conformance suite green; `yew fl` REPL ships |
| Self-config | 36 | default config is Fletch; recorder round-trip law in CI |
| **Daily driver** | 42 | field-evidence dogfood gate — implementation readiness alone does not earn it |
| LSP live | 47 | clangd navigates yew's source in CI |
| Embedded proof | 57 | musl static build within size budget runs on target |
| **v1.0.0** | 60 | all campaign closeout gates green |

## Sequencing notes

- Campaigns 00→05 are strictly ordered (each feeds the next).
- Campaign 06 (Fletch core) depends only on 00 — it may run as a parallel
  section any time after Sprint 2; it must land before Campaign 07.
- Campaign 07 needs 03 (command registry, recorder input) + 06.
- Campaigns 08, 09, 10, 11 are architecturally parallel sections once 07
  lands; the implementation-order override is Sprint 42.5 → Sprint 43, so
  Campaign 09 is paused while the native pack lands. The original hard edge
  remains Sprint 40 → Sprint 16's B-mode syntax provider slot.
- Campaign 12 needs 07; 13 and 14 close the arc in order.
- Sprint 25's state schema freezes before the Fletch serializer exists —
  s25 hand-writes the emitter against the frozen schema; s36 swaps the
  implementation, not the format.
- Fractional sprints (`s18_5-…`, `s41_5-…`, `s42_5-…`, `s55_5-…`,
  `s56_5-…`) are the pressure valve when a sprint
  splits — renumbering is forbidden once files exist.

## Sprint file format

Every sprint file uses the five-heading skeleton, with deliverables naming
real paths and real signatures:

```markdown
# Sprint N: Title
## Prerequisites    — sprints + what they landed; binding plan docs
## Goals            — one paragraph; explicit "X lands in Sprint M" deferrals
## Deliverables     — numbered; exact file paths, function signatures,
                      reference tables (escape sequences, edge cases,
                      formats), code blocks, pitfalls; final item is often
                      "Defer: …" naming what must hard-error until its sprint
## Testing Strategy — unit / script / pty / fuzz / perf / regression fixtures
## Definition of Done — checkable, numeric where possible
```

A sprint file must be sufficient for an agent to implement the sprint from
the file alone, with the three plan docs as standing context.

## References (local reading, not dependencies)

| Repo | Why |
|---|---|
| `~/GithubOrgs/FortranGoingOnForty/fuss` | FUSS mode source of truth: glyph vocabulary, git verbs, fuzzy scoring, depth-aware nav |
| `~/GithubOrgs/FortranGoingOnForty/facsimile` | Tab groups data model, lazy hydration, clickable regions, layout/draw split, kitty protocol notes, group picker UX |
| `~/GithubOrgs/tenseleyFlow/Cgfried` | Sprint discipline, util-container set, determinism doctrine, test-integrity culture |
| vim/neovim, kakoune, helix, micro | Prior art for modal semantics and text engines — study, never vendored |
