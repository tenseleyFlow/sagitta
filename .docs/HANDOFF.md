# yew — session handoff

**Written:** 2026-08-11. **Active implementation frontier:** Sprint 42.5,
Native Language Pack — Wolf and 48 Built-in Modes. **Campaign 09 remains
paused** until the last Sprint 42.5 performance gate is validly green.

Sprint 42.5 implementation is pushed through `6fa99cb` on `trunk`. No Sprint
43–47 implementation has begun.

---

## 0. Start here

Read, in order:

1. `.docs/plan/00-decisions.md`
2. `.docs/plan/01-architecture.md`
3. `.docs/plan/02-fletch.md`
4. `.docs/sprints/08-highlighting/s42_5-native-language-pack.md`

The sprint contract remains binding until every Definition of Done item is
green. Do not move the active frontier to Sprint 43 merely because the code is
implemented.

## 1. Current state

Sprint 42.5's product and test surfaces are implemented:

- exactly 48 built-in lexical modes, including Wolf and 28 other new modes;
- exact-sized built-in registry and first-line-regex storage, with the old
  accidental 32-entry ceilings removed;
- generated exact filename, extension, and shebang indexes;
- stable append-only language ids from `tests/syn/builtin-ids.txt`;
- one checked seven-column syntax manifest driving asset, golden, and fuzz
  consumers;
- 126 new Sprint 42.5 goldens, for 431 syntax assets total;
- 100% context, rule, and embedded-site coverage across the pack;
- family PTY matrices, differential tests, long sanitizer rotations, new
  scale/performance checks, and binary-size enforcement;
- 48-mode documentation in `README.md` and `.docs/syntax-def.md`.

The implementation also fixed two closeout defects:

1. an invalid `first_line` regex could leave freed metadata linked in the
   user-language index; `b495f22` repairs the rollback and pins it with a
   sanitizer regression test;
2. edit latency benchmarks accidentally ran the embedded-language idle pump,
   turning a two-line edit into a whole-state scan; `6fa99cb` restores the
   real frame-budget workload without changing committed baselines.

## 2. Fresh green evidence

- `make check`: 1,718 tests / 69,905,388 assertions, zero failures; asset,
  Fletch, script, bans, dispatch, input, render, signal-safety, and smoke gates
  green.
- Full Clang ASan/UBSan `make test`: 1,702 instrumented tests /
  69,905,106 assertions, zero failures; the complete 300+ PTY suite, scripts,
  fuzz-corpus replay, round-trip, smoke, and live-torture checks are green.
- Eight long ASan/UBSan rotations: Wolf plus C++, Kotlin, Ruby, Perl,
  PowerShell, Haskell, XML, and HCL; four seeds × 100,000 edits for each
  selected pair, roughly 2.4 million assertions per rotation, zero findings.
- Targeted Valgrind: exact registry allocation, the >32 first-line cache,
  lazy load, all 18 cache cases, all seven discovery/reset cases, and the
  invalid-regex rollback are green with zero definite leaks or invalid
  accesses. The intentional impossible-allocation death test is not a
  Valgrind target because `--error-exitcode` replaces its deliberate exit 4.
- Full GCC and Clang build/test lanes and the Sprint 42.5 PTY matrix were
  already green before the final rollback fix; that fix subsequently compiled
  warning-free under GCC Valgrind and Clang sanitizers.
- Binary growth: 24,576 bytes versus the 48 KiB limit.
- Runtime syntax data: 232,720 bytes versus the 1.5 MiB limit.
- Generator/asset determinism and fixture hashes are green.
- New hard performance budgets are green: indexed detection, 48-mode listing,
  cold compile, warm load, runtime data, per-language line/edit/viewport/
  scroll rows, state memory, embedded pumping, inline scan, and definition
  switching.

## 3. The only closure blocker

`make perf-syn` still needs one valid run against the historical 1.2× rows.
Do not alter `tests/perf/baselines/syn.txt` to obtain it.

The last attempted run was invalid for adjudication: unrelated compiler
torture matrices saturated the workstation, the CPU reached its 100 °C
critical limit, and the checked baseline commit `4058b25` itself failed many
of the same provisional development-machine rows. The temporary baseline
worktree was removed. This is external load/thermal evidence, not permission
to waive the gate.

On a cool, quiescent reference machine:

```sh
make perf-syn
make perf-syn-size
```

Expected repaired edit evidence is `edit_settle_100k` below 1 µs in ordinary
conditions, with `report.lines <= 2`; the pre-fix contaminated row was roughly
95 µs. If the historical rows still fail in a valid environment, compare
current `trunk` against `4058b25` on the same pinned CPU and investigate the
code. Do not recalibrate here; Sprint 56 owns reference-hardware calibration.

## 4. Closeout sequence after perf is green

1. Re-run `make check`, `make perf-syn-size`, and `git diff --check` if any
   repair was required.
2. Mark Sprint 42.5 complete in `.docs/sprints/index.md` and resume Campaign
   09 with Sprint 43 as the active contract.
3. Rewrite this handoff around Sprint 43, preserving the Daily Driver totals.
4. Commit and push the closeout documentation.
5. Only then begin `.docs/sprints/09-completion-lsp/s43-shadow-text.md`.

## 5. Daily Driver remains separate and pending

Sprint 42's field milestone remains `PENDING` at:

- 0/10 working days;
- 0/40 dogfood hours;
- 0/200,000 real keystrokes;
- 0/3 abnormal-exit trials;
- 0/20 exact resume cycles.

No qualifying yew session was recorded during Sprint 42.5, so it contributes
zero self-hosting evidence. A later complete implementation sprint must be
explicitly designated before its work begins. Automated tests, generated
goldens, benchmarks, and editing in another editor never count.

## 6. Campaign 09 remains untouched

The intended sequence is unchanged:

1. Sprint 43 — provider-neutral ghost text;
2. Sprint 44 — no-LSP buffer/workspace symbol index;
3. Sprint 45 — bespoke JSON/JSON-RPC and stdio transport;
4. Sprint 46 — LSP lifecycle, capabilities, changes, and diagnostics;
5. Sprint 47 — completion, hover, navigation, references, rename, and symbols.

There is still no Sprint 43 ghost-text implementation and no LSP client
transport. Module flags are scaffolding only.

## 7. Invariants and cautions

- Keep `YEW_SYN_DEF_MAX == 4`, `YEW_SYN_DEPTH_MAX == 16`,
  `YEW_SYN_RESIDENT_MAX == 255`, `sizeof(SynState) == 84`, and four bytes per
  line entry unchanged.
- Do not add Tree-sitter, TextMate, a regex library, or another dependency.
- Do not mark Daily Driver `EARNED` from automated evidence.
- Do not weaken committed performance baselines because of a thermally
  throttled or loaded host.
- Do not begin Sprint 43 until Sprint 42.5 is actually green.
