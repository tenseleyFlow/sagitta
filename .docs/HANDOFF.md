# yew — session handoff

**Written:** 2026-08-24. **Active implementation frontier:** Sprint 55.5,
the two shipped example plugins and their executable author-guide coverage.
Campaign 11 and Sprints 51–55 are complete; Campaign 12 is active.

---

## 0. Start here

Read, in order:

1. `.docs/plan/00-decisions.md`
2. `.docs/plan/01-architecture.md`
3. `.docs/plan/02-fletch.md`
4. `.docs/sprints/12-plugins/s55_5-example-plugins.md`

Sprint 55.5 is the binding implementation contract. Sprint 55 is complete; do
not reopen its package, lockfile, integrity, save-policy, or cloud-preset
contracts except where Sprint 55.5 explicitly consumes them.

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

## 2. Sprint 49–50 closeout

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

Sprint 50 closed Campaign 10's shipping boundary: explicit opt-in, schema-3
per-workspace grants, deny-pattern and path redaction, conservative local/cloud
presets, the privacy page, and the four-state `[AI]` statusline badge. A fresh
profile continues to make zero network syscalls.

## 3. Sprint 52–55 closeout and Sprint 55.5 objective

Sprint 52 completes F mode:

- a dirty-first, depth-aware tree preserves selection and collapsed state by
  interned path across refreshes, with lazy untracked-directory expansion;
- Unicode and ASCII renderings cover staged, modified, untracked, incoming,
  conflict, and ignored states without blocking the event loop;
- the full F keymap dispatches recordable commands, including the deferred
  `ed.group.from_dir` word reserved for Sprint 53;
- real panes provide diff, status, blame, history, reflog, and file viewers,
  restoring the pre-F layout byte-identically on exit;
- argv-only Git workflows cover stage/unstage, commit/amend, push/pull/fetch,
  branches, merge/reset/rebase, cherry-pick/revert, stash/tag, discard,
  delete, rename, view, and open with typed destructive confirmations.

Local closeout evidence:

- the fast tier is green at 2,176 tests / 70,773,238 assertions; strict Clang
  and `MODULES=""` parity checks are green;
- 50 focused FUSS unit tests / 3,837 assertions, 333 real-Git workflow
  assertions, and 20,000 live F-mode key sequences pass;
- all 19 FUSS PTYs pass, including CJK, emoji ZWJ, conflict, ignored, incoming,
  ASCII status rows, both leave keys, non-repo behavior, loading, confirmation,
  and byte-exact viewer layout restoration;
- rebase handover restores termios across normal and synthetic SIGTERM exits;
  the 20,000-entry tree builds in 6.629 ms and toggle p99 is 0.123 ms against
  12 ms / 5 ms gates.

Sprint 53 completes Git-aware editing:

- one persistent `git cat-file --batch` transport per workspace supplies the
  index blob; conflict entries fall back to `HEAD`, and untracked files use an
  empty base;
- a collision-safe, linear-space Myers differ drives the two-cell sign column,
  hunk motions, index-only staging, and buffer-only discard as one undo step;
- lazy viewport blame preserves stale annotations while refreshing, and the
  side-by-side diff view keeps aligned rows and synchronized scroll state;
- cached branch, ahead/behind, conflict, phase, and stash state reaches the
  statusline without spawning from render;
- focus, save, and external-job completion invalidate Git state without a file
  watcher, while private read workers cannot create refresh loops;
- `ed.group.from_dir` deterministically adopts or defers directory members and
  reads only the focused file.

Local closeout evidence:

- strict full GCC and Clang suites are green at 2,243 tests / 70,983,393
  assertions, including every deterministic PTY, 87 scripts / 770 assertions,
  smoke, round-trip, syntax assets, structural gates, and live torture;
- Clang ASan/UBSan with Fletch invariant checks is green at 2,224 runnable
  tests / 70,983,075 assertions plus the complete script and PTY matrices;
- the real-Git suites report 15,337 layer assertions, 333 FUSS workflow
  assertions, and 20 accepted patch fixtures / 254 hunk assertions;
- the 100 kLOC / 500-hunk gate settles in about 20–22 ms, keeps its largest
  slice below 3.7 ms, performs viewport lookup below 0.1 microsecond, and
  coalesces 200 edits into one diff.

Sprint 54 completed plugin manifests and deterministic XDG discovery,
zero-residue lifecycle, the namespaced event bus, capability consent and
containment, and the `yew plug` CLI/picker.

Sprint 55 completes plugin distribution and the cloud-save preset:

- `yew pkg` installs, updates, removes, lists and diagnoses Git-backed plugin
  packages without a shell or a dependency on the FUSS Git module;
- deterministic pure-literal lockfiles record exact revisions and a bespoke
  content-tree drift hash, while crash intents recover installs/removals to a
  complete before-or-after state and restore the exact prior trust policy;
- managed-code drift revokes persisted capability grants and re-prompts on
  first use instead of silently transferring consent to changed code;
- the cloud preset selects in-place saves and content conflict checks only for
  configured synced roots, retaining the normal atomic-save default elsewhere;
- save-policy, symlink/hardlink preservation, backup rotation, recovery,
  offline behavior, package docs, Fletch import behavior, fuzz corpora and all
  three sprint performance budgets are covered in-tree.

Local closeout evidence:

- strict full GCC and Clang suites are green at 2,310 tests / 71,007,767
  assertions, plus 89 scripts / 808 assertions, every PTY, Fletch 38/38,
  package integration 50/50, round-trip, structural/docs gates, smoke and live
  save torture;
- the `MODULES=plugins` suite is green at 1,924 tests / 70,052,585 assertions
  and exercises packages without the FUSS module;
- Clang ASan/UBSan is green at 2,291 runnable tests / 71,007,449
  assertions; the 5,000-iteration package-tree fuzz lane and focused package,
  manifest, drift, save-policy and conflict-check Valgrind slices are clean;
- tree-hash p99 is 0.723 ms against 3 ms, lock load/save p99 is 0.822 ms
  against 1 ms, and cloud conflict-scan p99 is 0.207 ms against 2 ms;
- the exact Wolf-file regression remains responsive while workspace indexing:
  raw keypress p99 1.382 ms and paced arrow p99 1.624 ms against 5 ms.
  The defect was editor-side, not a Wolf LSP: startup discovery repeatedly
  scheduled already-due background slices. Yew now spaces completed indexing
  slices by 8 ms and yields them behind the input grace deadline.

The pushed Sprint 55 closeout SHA must pass the complete hosted matrix and the
explicit on-demand Valgrind lane before release. Sprint 55.5 now owns only the
two shipped examples, their author-guide walkthroughs, and executable coverage
of the frozen plugin API; its contract forbids new host code.

## 4. Campaign sequence

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
   (complete; Campaign 10 closed)
9. Sprint 51 — asynchronous Git layer, porcelain parsers and TTL cache
   (complete)
10. Sprint 52 — F mode tree, navigation, viewers and Git verbs (complete)
11. Sprint 53 — editor Git hunks, blame, diff view and groups (complete;
    Campaign 11 closed)
12. Sprint 54 — plugin manifests, lifecycle, events and capabilities
    (complete)
13. Sprint 55 — package distribution, integrity and cloud-save preset
    (complete locally; hosted closeout gates pending)
14. Sprint 55.5 — shipped example plugins and executable author-guide coverage
    (active frontier)

## 5. Daily Driver remains separate and pending

Sprint 42's field milestone remains `PENDING` at:

- 0/10 working days;
- 0/40 dogfood hours;
- 0/200,000 real keystrokes;
- 0/3 abnormal-exit trials;
- 0/20 exact resume cycles.

Automated tests, generated goldens, benchmarks and editing in another editor
never count. A future sprint contributes only if a qualifying yew session is
designated and logged before eligible implementation edits.

## 6. Invariants and cautions

- Preserve byte identity, terminal restoration, deterministic rendering and
  central edit/undo laws ahead of latency or convenience.
- Keep the bespoke HTTP transport plain-text and loopback-first; deny
  non-loopback HTTP unless `ai.allow_plain_remote` is explicitly enabled.
- Route every HTTPS backend through `curl`; do not add or vendor a TLS library.
- Construct subprocesses with argv arrays. Never interpolate commands through
  a shell, and never place API-key literals in `init.fl`, argv, logs or errors.
- Keep all socket, subprocess and streaming work poll-driven and bounded. No
  threads, blocking request writes, or mid-keystroke DNS resolution.
- Keep fresh-profile AI requests off unless Sprint 50's explicit opt-in and
  workspace-grant gates both permit them.
- AI output may feed only the Sprint 43 ghost surface; never mutate `TextBuf`
  before the existing explicit shadow-accept path runs.
- Do not add Tree-sitter, TextMate, a JSON library or another dependency.
- Keep all Git execution argv-only through the existing job layer; paths,
  refs, commit text, and stash text never pass through a shell or formatted
  command line.
- Git editor diffs compare buffer bytes with the index blob, not the worktree;
  conflict fallback is `HEAD:<path>` and untracked files use an empty base.
- Plugin manifests are pure data literals; entry paths must resolve inside the
  package root, and discovery/load order stays deterministic.
- Plugins share yew's VM, process, and address space. Capability gates constrain
  flapi I/O; they are not memory or resource isolation and must not be described
  as a sandbox in user-facing text.
- A failed init or disable must leave zero registrations, timers, closures, or
  partial capability state behind.
- Package transactions must publish code disabled, durably commit the lock and
  exact trust policy, then enable; recovery must complete the proven commit or
  restore the byte-exact previous state.
- Managed plugin drift never inherits persisted grants. Hashes detect ordinary
  drift; they are explicitly not a cryptographic authenticity mechanism.
- Do not relax existing performance baselines outside Sprint 56 calibration;
  a sprint may add the new baseline files its own Definition of Done requires.
- Do not mark Daily Driver `EARNED` from automated evidence.
