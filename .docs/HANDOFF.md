# yew — session handoff

**Written:** 2026-08-12. **Active implementation frontier:** Sprint 43,
Provider-Neutral Shadow Text. Sprint 42.5 is complete and Campaign 09 is
active.

---

## 0. Start here

Read, in order:

1. `.docs/plan/00-decisions.md`
2. `.docs/plan/01-architecture.md`
3. `.docs/plan/02-fletch.md`
4. `.docs/sprints/09-completion-lsp/s43-shadow-text.md`

Sprint 43 is the binding implementation contract. Implement its deliverables
and meet its Definition of Done before entering Sprint 44.

## 1. Sprint 42.5 closeout

Sprint 42.5 ships exactly 48 built-in lexical modes, including Wolf, with
generated discovery indexes, stable language ids, embedded syntax coverage,
goldens, differential and sanitizer rotations, PTY matrices, hard performance
budgets, binary-size enforcement, and documentation. The last qualification
repairs removed an edit-benchmark idle-pump contaminant, accelerated the
syntax-state hot path, stabilized paired embedded-language measurements,
isolated physical-core benchmark runs, and made temperature checks opt-in.

Fresh closeout evidence:

- `make check`: 1,719 tests / 69,905,401 assertions, zero failures;
- `make perf-syn-size`: 28,672 bytes growth against the 48 KiB limit;
- strict GCC and Clang builds: warning-free;
- syntax line, embedded-runtime, settle, degradation, and four-seed
  differential suites: green;
- all Sprint 42.5 hard latency, memory, runtime-data, and scroll budgets:
  green.

The frozen historical 1.2x comparison was not directly adjudicable on this
host. The sprint contract's same-machine fallback was therefore applied on
CPU 5 with 1,001 samples per row:

- frozen control `4058b25`: 39 relative regressions among 88 shared rows;
- current `trunk`: 10 relative regressions among those shared rows, with all
  absolute and hard budgets green;
- current median was no slower than control in 82/88 shared rows, and current
  p99 was no slower in 71/88;
- representative results: `comment_edit` improved from 34,548/62,006 ns to
  343/352 ns, `markdown_line` from 2,425/4,240 ns to 727/771 ns, and
  `viewport_200x100_rust` from 561,851/1,138,682 ns to 521,863/932,299 ns.

This proves the scattered historical relative failures are environmental
baseline drift, not a current-code regression. The committed baseline remains
unchanged; Sprint 56 retains reference-hardware calibration ownership.

## 2. Sprint 43 objective

Sprint 43 builds one provider-neutral shadow-text engine for future local
index, LSP, and AI completion sources. This sprint uses a deterministic fake
provider only; it does not implement the providers themselves.

The required surface includes:

- shadow provider request/delivery types and a closed provider enum;
- per-window live suggestion state with monotonic sequence rejection;
- deterministic edit revalidation and one debounce timer per window;
- exact pre/post edit notifications in the central text mutators;
- overlay-only layout and drawing that never pushes buffer cells;
- per-line, per-kind gutter signs with the shadow marker in sign cell 1;
- word, line, and all acceptance through exactly one central insert call;
- insert-mode arbitration, commands, bindings, options, statistics, unit/PTY,
  determinism, fuzz, and performance coverage.

The first implementation slice is to map the real window, buffer, timer,
edit, render, gutter, command, and option APIs; then land the shadow state,
staleness/revalidation law, and edit-notification seam with focused unit tests.
No API named only in the sprint prose should be assumed to exist unchanged.

## 3. Campaign sequence

1. Sprint 43 — provider-neutral shadow text (active)
2. Sprint 44 — no-LSP buffer/workspace symbol index
3. Sprint 45 — bespoke JSON/JSON-RPC and stdio transport
4. Sprint 46 — LSP lifecycle, capabilities, changes, and diagnostics
5. Sprint 47 — completion, hover, navigation, references, rename, and symbols

There is no LSP client transport yet. The compile-time module flags remain
scaffolding until their owning sprints land.

## 4. Daily Driver remains separate and pending

Sprint 42's field milestone remains `PENDING` at:

- 0/10 working days;
- 0/40 dogfood hours;
- 0/200,000 real keystrokes;
- 0/3 abnormal-exit trials;
- 0/20 exact resume cycles.

Sprint 42.5 recorded no qualifying yew session. Sprint 43 is not automatically
designated: it counts only if a qualifying yew session is opened and logged
before eligible edits. Automated tests, generated goldens, benchmarks, and
editing in another editor never count.

## 5. Invariants and cautions

- Preserve byte identity, terminal restoration, deterministic rendering, and
  central edit/undo laws ahead of latency or convenience.
- Keep shadow text an overlay: it must not mutate the buffer until accepted,
  move the cursor, alter wrapping, or trigger viewport-follow behavior.
- Reject stale provider delivery by sequence and current edit state.
- Keep provider kinds closed and insertion ordered; do not add a generic
  callback or plugin registry.
- Do not add Tree-sitter, TextMate, a regex library, or another dependency.
- Do not change performance baselines outside Sprint 56 calibration.
- Do not mark Daily Driver `EARNED` from automated evidence.
