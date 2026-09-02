# Sprint 58: Adversarial Audits

## Prerequisites

- Sprint 56 — reference-hardware calibration; every budget in
  `00-decisions.md` is a CI gate with a committed baseline on the
  designated runner (`perf-x86_64-linux-gnu`). Front F15 audits the
  thresholds; it does not set them.
- Sprint 57 — `x86_64-linux-musl` static profile proven on target, binary
  size budgets per `MODULES` set, allocation audit. F15 audits its
  ratchets.
- Sprints 57.5–57.8 — compact remembered FUSS trees, binary source-service
  isolation, nested-workspace path normalization, direct visible-tree
  type-to-jump, true off-canvas geometry, and the modern cell-exact tab/new-tab
  strip. The §1 baseline must include these fixes and pass the required hosted
  lanes; no audit front may cite the pre-57.8 tree.
- Sprints 0–55 — everything under audit. Each front's scope names the
  sprint that owns the code; the auditor reads that sprint file first,
  because a finding is a violation of a *pinned* decision, not a
  disagreement with one.
- Sprint 1 — unit harness + registry table, `scripts/bans.sh`,
  `scripts/smoke.sh`, the seven CI lanes. Sprint 6 — `PtyCase` registry
  and goldens. Sprint 33 — the Fletch conformance directive language,
  which already requires `.docs/audits/xfail-debt.md` to exist. Sprint 37
  — `tests/script/` runner and the `t.*` assertion stdlib, whose §9
  deferred `.fl` coverage instrumentation *to this sprint*.
- Cgfried `.docs/sprints/13-audits/s60-adversarial-audits.md` — the
  precedent for front structure, the reproducer-first law, severity
  normalization, and the no-theater rules. Mirror it; do not reinvent it.
- Binding: `00-decisions.md`'s ten invariants (§8 re-verifies each one
  individually) and its quality-bar table.

**Divergence from Cgfried, stated once:** `.docs/audits/` is **tracked**
in yew, not gitignored. Sprint 33 already made `xfail-debt.md` a file
CI reads; a ledger that CI depends on cannot be local-only. Every file
this sprint writes under `.docs/audits/` is committed.

## Baseline status — ESTABLISHED 2026-09-02, AUDIT NOT STARTED

`6d742a88` is the fixed post-Sprint-57.8 code baseline. On Darwin arm64 it passes
the complete default-module `make test` suite, focused plain and ASan/UBSan
tab/mouse/group/theme coverage, a sanitized clicked-new-tab PTY, three
deterministic complete PTY executions, FUSS performance, warning-clean
default/core-only shipping builds and core smoke, and all six native size
profiles. Earlier Sprint 57.7 alignment/UBSan and FUSS fuzz evidence remains
part of the unchanged off-canvas baseline.

GitHub Actions run `33615806917` supplies the required hosted
commit-of-record evidence for that exact hash. All 22 standard push jobs pass,
including performance, determinism, LSP, Fletch dispatch, GNU GCC,
Linux/x86_64, musl, macOS arm64, hosted arm64, sanitizer, PTY, modules, size,
and the embedded gates. Trigger-specific Valgrind, designated-hardware
performance, and nightly campaigns were skipped by the push trigger and are
not inferred as successes.

The baseline gate is satisfied, but no audit front or `YEW-F-###` ID has been
opened. When Sprint 58 begins, every front uses `6d742a88`; if any code
follow-up changes the candidate before then, record and requalify the
replacement hash before opening a front.

## Goals

Run fifteen adversarial audit fronts, one per subsystem, each executed by
a fresh-context reviewer handed a fixed prompt, a file list, and the
front's attack questions. The output is not prose: it is a ledger of
stable-ID findings (`YEW-F-###`), each backed by a committed reproducer
that fails at the baseline commit. Extend the fuzz campaign — this sprint
decides the coverage question, builds the instrument, and pins the
long-run soak schedule that runs from here to the tag and past it. Then
re-verify all ten invariants in dedicated adversarial sessions with their
worst cases spelled out. **Nothing is fixed in this sprint.** Fixing
during auditing contaminates both activities; remediation, closeout gates,
and the deferral law are Sprint 59, and release engineering is Sprint 60.
Two inbound obligations from earlier sprints are discharged here (§9).

## Deliverables

### 1. File set and the audit index — `.docs/audits/audit-00.md`

```
.docs/audits/
  audit-00.md                index, totals, verdict
  audit-01-unicode.md … audit-15-ci.md      one per front
  findings.md                the ledger (§2) — the single source of IDs
  xfail-debt.md              exists since s33; F15 audits it
  fuzz-coverage.md           §6's edge-count report, regenerated weekly
  invariants.md              §8's ten session reports
```

`audit-00.md` opens with a fixed header block: audit date, **baseline
commit** (a `trunk` commit with all CI lanes green including `perf`,
`determinism`, `lsp`, and `fletch-dispatch`), the `MODULES` sets built,
the four target lanes' commit-of-record, the UCD version (16.0.0), and
the external tool versions the audit leaned on (git, clangd, the shells
used by E-mode fixtures). Then:

| Front | File | Raw | Deduped | Crit | High | Med | Low | Unverified |
|---|---|---|---|---|---|---|---|---|
| F01 unicode | `audit-01-unicode.md` | … | … | … | … | … | … | … |

Raw *and* deduped totals are both kept: raw measures reviewer effort,
deduped measures debt. The file ends with one blunt verdict paragraph.
"We are not ready to tag" is a legitimate, expected result of this
sprint; a green summary written over an ugly ledger is the one failure
mode this sprint cannot survive.

### 2. The findings ledger — `.docs/audits/findings.md`

**IDs are `YEW-F-###`**, zero-padded to three digits, assigned in
confirmation order across *all* fronts from one counter. One namespace,
not per-front: a finding gets deduped across fronts (§9) and a per-front
prefix would make the survivor's ID lie about where it lives. IDs are
**never reused, never renumbered, never deleted** — resolution is a
status flip plus strikethrough, per the Cgfried convention.

Row format, one line per finding plus an indented body:

```
| YEW-F-042 | H | confirmed | F06 RE | smartcase flips on `\W` | tests/audit/yew-f-042.fl | s21 §3 |
```

Columns: ID · severity · status · front · one-line title · reproducer
path · violated sprint deliverable.

**Severity taxonomy — five levels, defined against the invariants, not
against feelings.** Normalized across all fronts; auditors may not invent
levels.

| Sev | Name | Definition | Examples of the class |
|---|---|---|---|
| C | Critical | A byte the user owns is lost, changed, or reordered without the user asking (invariants 1, 2); or the terminal is left unusable on any exit path (invariant 6); or a user-reachable path silently no-ops instead of hard-erroring (invariant 3) | save drops a trailing byte; CRLF normalized; a stale LSP edit splices; `yew pkg` silently skips a failed install |
| H | High | Crash, `yew_bug()`, or exit 4 on valid input; undo/redo not byte-exact; nondeterministic rendering or output (invariant 5); a `00-decisions.md` budget exceeded on the designated runner (invariant 4); a documented security rule violated (OSC 52 read, `system()`, argv interpolation) | 1000-cursor edit leaves two undo nodes; a pty golden differs between two runs; keypress p99 6 ms |
| M | Medium | Wrong-but-recoverable behavior the user can see and work around; doc-code mismatch where the doc claims a mechanism the code does not have; a perf cliff inside budget; a diagnostic that misleads | `:s` reports the wrong count; man page documents a flag that is accepted-and-ignored; B-mode returns the wrong level in a legal file |
| L | Low | Polish: message wording, glyph choice, help text ordering | |
| O | Observation | **Not a finding.** No reproducer. Filed in a separate section per front, excluded from all totals | |

Ties escalate: file the higher severity and say you were torn. The dedup
pass (§9) may downgrade, and **downgrades are recorded with a reason, not
applied silently**.

**Status lifecycle** — exactly one status per ID at any time:

| Status | Meaning | Set by |
|---|---|---|
| `open` | filed, reproducer written, not yet re-run by a second party | auditor |
| `confirmed` | reproducer fails at the baseline commit on a second machine | dedup pass (§9) |
| `duplicate → YEW-F-NNN` | same root cause as an earlier ID; the earlier ID wins | dedup pass |
| `assigned` | on a Sprint 59 remediation branch | s59 |
| `fixed` | reproducer passes; why-comment naming the ID landed | s59 |
| `verified` | re-verified by someone who did not write the fix | s59 §6 |
| `closed` | verified + ledger struck through + burndown row | s59 |
| `deferred → post-1.0` | M or L only; requires §Deferral law (s59 §3) | s59 |
| `wontfix` | written justification in the ledger body | s59 |

Forbidden transitions, checked by `scripts/check-findings.sh`: `open` →
`fixed` (skipping confirmation); any C or H reaching `deferred` or
`wontfix`; `closed` without a reproducer path; a `duplicate` whose target
does not exist or is itself a duplicate.

### 3. XFAIL linkage across four test surfaces

Every confirmed finding lands a reproducer **before** Sprint 59 begins,
and every reproducer is marked expected-to-fail citing its ledger ID. The
suite therefore runs from day one and Sprint 59 flips entries green.
**XPASS is a hard failure** — the Cgfried doctrine, applied to yew's
four test surfaces:

| Surface | Marker | Runner behavior |
|---|---|---|
| unit (s01) | `tests/audit/registry.c` row `{ "YEW-F-042", YEW_AUDIT_XFAIL, test_yew_f_042 }` | a row marked XFAIL that passes → `XPASS YEW-F-042` and exit 1 |
| script (s37) | file header `# XFAIL: YEW-F-042 smartcase flips on \W` | zero-assertion rule still applies; an XFAIL test with no failing assertion is an XPASS |
| pty (s06) | `PtyCase.xfail_id` field, added here | golden mismatch is the expected failure; a match is XPASS |
| fletch conformance (s33) | `# XFAIL: YEW-F-042 <reason>` — the directive already exists | unchanged; the ID space widens from `XF-` to accept `YEW-F-` |

Rules, enforced by `scripts/check-audit-fixtures.sh` (greps both
directions):

1. Every `confirmed` ledger row names a reproducer path that exists.
2. Every file under `tests/audit/` and every `YEW-F-` marker names a
   ledger row that exists and is not `duplicate`.
3. An ID that is `deferred` or `wontfix` keeps its XFAIL marker forever —
   that is what makes a deferral visible in CI rather than a paragraph
   nobody reads.
4. Unknown ID, malformed ID, or a marker on a `closed` row → **configuration
   error**, not a skip (s33's rule, extended).
5. `make test-audit` is a `make test` dependency and a required CI lane
   from this sprint on. The audit's tests outlive the audit; five years
   from now `yew-f-042.fl` still explains itself.

**Pitfall.** A reproducer that reads `.docs/audits/` is broken by
construction even though the directory is tracked — the test must be
self-describing in a header comment (ID, one-line title, what correct
behavior looks like), because a reader debugging it in 2031 should not
need the ledger to understand the assertion.

### 4. Reviewer-prompt template — the deliverable, verbatim

This exact text, with `{…}` filled, is handed to each front's
fresh-context reviewer:

```
You are an adversarial auditor for yew, a bespoke C11 modal terminal
editor. You are paid per confirmed bug. You are not paid for style
opinions, refactoring proposals, or praise. Your reputation rests on
findings that survive verification.

FRONT: {front id + name}
BASELINE: commit {hash} — all CI lanes green. Build it yourself:
  make CC=gcc && make test && make MODULES="" && make test-pty
SCOPE: {file list}. Out-of-scope bugs: note the file, hand off, move on.
OWNING SPRINTS: {sprint files} — READ THESE FIRST. A finding is a
violation of a decision those files PIN. If you disagree with a pinned
decision, that is an Observation, not a finding.
ATTACK QUESTIONS (dispatch every one, then go where the code smells):
{the front's numbered questions}

REPRODUCER-FIRST LAW: a finding without a failing reproducer — a unit
test, a .fl script test, a pty case, a fuzz input, or a shell transcript
with exact commands and exact output — is NOT a finding. File it under
"Unverified observations" at the bottom; it does not count in totals.

SEVERITY RUBRIC (normalized; do not invent levels):
  C  a byte the user owns is lost/changed/reordered unasked (inv 1, 2);
     terminal unusable on any exit path (inv 6); a user-reachable silent
     no-op where invariant 3 requires a hard error
  H  crash/yew_bug/exit 4 on valid input; undo not byte-exact;
     nondeterministic output (inv 5); a 00-decisions.md budget missed on
     the designated runner (inv 4); a documented security rule violated
  M  visible wrong-but-recoverable behavior; doc claims a mechanism the
     code lacks; perf cliff inside budget; misleading diagnostic
  L  polish
When torn between two severities, file the higher and say why.

OUTPUT — one block per finding:
  ID:        YEW-F-### (request the next number; never invent one)
  Title:     one line
  Severity:  + one sentence justifying it against the rubric above
  Reproducer: path under tests/audit/ — must FAIL at BASELINE
  Root cause: hypothesis, file:line if you have it; "unknown" is allowed
  Violates:  sprint file + section whose pinned decision this breaks

NO-THEATER RULES: no style opinions, no rewrite proposals, no "consider
using...", no padding a thin front with Lows. Findings only. An empty
front report with three honest observations is an acceptable result and
will not be held against you. A padded front will be.
```

### 5. The fifteen fronts

Each front produces `.docs/audits/audit-NN-<front>.md`: the header block,
one section per attack question (findings, or the explicit line
`probed, nothing found` — **silence is not evidence**), the unverified
observations section, and a closing count.

#### F01 `UNI` — Unicode substrate
Scope `src/unicode/`, `ucd/`, `scripts/gen-unicode-tables.c`; owners s02, s16, s46.
1. Does the round-trip law still hold *at every consumer*, not just in
   `utf8.c` — registers (s12), Fletch strings (s31), JSON `YEW_JSF_RAW_BYTE`
   (s45), `u16.c` (s46)? Build a per-consumer re-encode harness over the
   s02 corpus plus 2²⁴ three-byte inputs.
2. Does any 1.0 caller reach `yew_gb_prev_bytes`'s documented 64-codepoint
   restart bound on a plausible file (s09 motion, s17 rectangular columns,
   s26 label clipping, s43 accept-word)? What does the user see when it
   mis-parities?
3. Is `yew_utf8_encode` of a U+DC80–DCFF escape ever called on a path that
   writes into a `TextBuf`, a register, or a file? Escapes are supposed to
   exist only in the decoded stream (s02 §4); one such call is invariant-2
   corruption. Grep plus a seeded `YEW_BUG` in the encoder's escape branch
   under a debug build.
4. Every glyph the UI ships — `src/ui/glyphs.h`, s46's `✗ ▲ ● ·`, s47's
   `›`, s54's `● ○ ✗ ⊘`, s26's picker marks, s38's `●REC` — is
   `yew_cluster_width` exactly 1 (or 2 where the layout expects 2) under
   `ambiguous_wide` **both** true and false?
5. `make unicode-tables && git diff --exit-code src/unicode/tables.c` from
   a fresh clone with no network: clean? Does `ucd/MANIFEST`'s sha256
   match every checked-in input? Does the 64 KiB `_Static_assert` still
   hold with s16's `tables_wb.c` counted?
6. Does the model's width agree with what the renderer actually places?
   Render a 10 000-cluster mixed corpus, feed the byte stream to the s06
   VT, and diff model column advance against VT column advance.
7. Is the locale ban still total? `grep -rn 'wcwidth|wcswidth|mbrtowc|wchar\.h|setlocale|iconv' src/` — and is s45's annotated `strtod` still
   the only locale-sensitive call, with `LC_ALL=C` actually forced on
   every subprocess (s19 §4, s51 §3)?
**Evidence:** harness output with counts, the VT column diff, grep
transcripts. **Deliverable:** `audit-01-unicode.md`.

#### F02 `TERM` — terminal I/O
Scope `src/term/`, `tests/pty/`; owners s03, s04, s05, s06.
1. Is the restore blob byte-identical on all four exit paths — normal
   quit, `yew_bug` exit 4, each of `SIGSEGV/SIGBUS/SIGABRT/SIGTERM`, and
   `SIGTSTP`/`SIGCONT`? Test under a real pty, comparing termios structs
   field by field before and after, on all four targets.
2. Is the async-signal-safe region still safe after 55 sprints of
   additions? Run `scripts/check-sigsafe.sh`; then read the fatal handler
   by hand for anything reached indirectly (a `yew_log` behind two calls,
   an allocation inside a helper).
3. Does `render.c` still emit only the closed sequence set of s06 §4?
   Drive the whole editor — every mode, every panel, the picker, the
   group picker, the context menu, the tutor — through the VT and assert
   `nerrors == 0`. Any new sequence added since s15's scroll region must
   appear in the s06 table; if it does not, that is the finding.
4. Are BSU/ESU still exactly balanced, one pair per frame, `render.c` the
   only emitter? Count over a 100 000-frame fuzz.
5. Does typeahead coalescing hold under adversarial input — 256 KiB of
   pasted bytes, a burst spanning a resize, a paste containing
   `\x1b[201~`? Assert exactly one frame per burst and that the hostile
   terminator's blast radius is still one undo transaction (s04's
   acknowledged limitation, s14's mitigation).
6. Colour downconversion: is `yew_rgb_to_256` still deterministic and
   tie-resolving to cube, and `→16` to the lower index, on both compilers
   and all four targets? A tie resolved by iteration order is invariant-5.
7. Does anything write to the primary screen? The VT's `primary_written`
   flag must be false across the whole pty suite.
**Evidence:** VT error dumps, termios diffs, frame counts.
**Deliverable:** `audit-02-terminal.md`.

#### F03 `TEXT` — text engine, undo, registers
Scope `src/text/`, `tests/fuzz/oracle.c`, `tests/torture/`; owners s07–s12.
1. **The load-bearing question.** Do each of these survive open → edit →
   save with **every untouched byte identical**: a 0-byte file; a file of
   only ZWJ sequences; a 2 GB file (s08 refuses above 2 GiB — is the
   refusal at the boundary or one byte off?); a file containing NUL bytes;
   a file that is a symlink into a read-only mount; a file replaced on
   disk mid-edit; a file that is a hardlink with three links; a file whose
   directory becomes unwritable between load and save; a file with no
   final newline whose last edit is at EOF?
2. Does the atomic path still issue exactly write(s) → `fsync(file)` →
   `rename` → `fsync(dir)`, in that order, with no `O_TRUNC` on the real
   file? Prove it from the s08 fault-shim intercept log after 55 sprints
   of callers, not from reading `file.c`.
3. Run the s08 torture sweep at 10× iterations across all six decision-table
   rows plus the in-place lane. Does the checker's three-part invariant
   hold in 100 % of iterations? Does journal replay reproduce the
   post-edit buffer exactly whenever the destination holds pre-save
   content?
4. **API drift:** s10 owns `yew_undo_begin/end/abort` and `EditCtx` as the
   single mutation choke point. *(s19, s21 and s53 briefly spelled it
   `yew_txn_begin/commit/abort` during planning; corrected to s10's
   spelling, which the authoring ledger now pins.)* Is there exactly one
   transaction API in the tree? Grep for `yew_txn_` and for any mutation
   reaching `TextBuf` without passing `EditCtx` — a second path means undo
   depth, mark repair, the crash journal, and the syntax damage record are
   each silently wrong for edits taking it.
5. Do 100 000 undo/redo cycles still leave `tb->add.len` unchanged and RSS
   flat? Does `yew_undo_compact` ever run outside a transaction boundary,
   and does any caller hold a `UndoNodeInfo` or blob pointer across one?
6. `MarkRepair` and `MarkId.gen`: construct a slab-slot reuse where an
   undo repairs a mark whose generation moved. Does it refuse, or does a
   different mark teleport?
7. Registers: is `yew_reg_set` still routed only through
   `yew_reg_yank`/`yew_reg_delete`? Does the pinned shift rule (delete →
   register 1 iff the payload contains `0x0A` or is LINEWISE) still hold
   for a blockwise delete whose rows are all short, and for an LSP rename
   (s47) that deletes across lines?
8. Does anything translate EOL bytes on paste? Yank CRLF from a CRLF file,
   paste into an LF file, save both, and diff.
**Evidence:** shim intercept logs, torture iteration counts, oracle
differential output. **Deliverable:** `audit-03-text.md`.

#### F04 `MODAL` — keymap, modes, units, multi-cursor
Scope `src/edit/`, `src/ui/viewport.c`; owners s13–s17.
1. Is the command registry still a bijection with the CMDWORD space
   (s34 `CmdDesc.word`, s35's law)? Every `YEW_CMD_RECORDABLE` command's
   word must map back through `yew_cmd_by_word` to the same `CmdId`, and
   every word must be unique across core **and** plugin registrations.
2. Does `scripts/check-cmd-dispatch.sh` still hold — every `cmd_*` symbol
   appearing exactly twice? Seed a third reference and confirm it fires.
3. Chord resolution: does a layer with a partial match still own the
   sequence to completion after s36 rebinds and s54 plugin layers stack
   above the user layer? Does `<esc>` remain rejected as a prefix in every
   rebuilt keymap, including one assembled from `init.fl` + two plugins?
4. `runtime/init.fl` vs the C defaults: does the shipped config still
   express **every** row of s13/s14/s16/s17's tables, zero missing, zero
   extra? Does it parse under the frozen grammar — specifically, are
   dotted option keys (`clipboard.sync:`, `search.smartcase:`) legal map
   entries under spec §2's `entry` production, and is every global it
   calls (`msg`, `win.cursors`, `set`, `bind`, `on`) actually registered?
5. Multi-cursor transaction law: does a per-cursor failure at cursor 700
   of 1000 roll back all 1000? Does `yew_journal_sync` still fire exactly
   once for the set? Are cursors still visited ascending?
6. Rectangular selection over the s17 fixture line `a漢\tb👨‍👩‍👧‍👦c`: is the
   highlighted cell set still mechanically equal to the deleted cell set
   at every `CCol` pair, including the wide-glyph and tab edges?
7. Does any unit engine return `p` unchanged for an in-range `p` after
   s40 installed the syntax provider? A fixed point makes `4→` hang.
8. Is every feature still keyboard-reachable (invariant 9)? Re-run the
   s27 audit table with `YEW_MOUSE=0` and every surface added since —
   panel, completion menu, tutor, plugin picker.
**Evidence:** bijection dump, keymap build transcripts, the
`YEW_MOUSE=0` reachability table. **Deliverable:** `audit-04-modal.md`.

#### F05 `EXEC` — E mode, shell jobs, filters
Scope `src/ui/cmdline*.c`, `src/edit/job.c`, `src/edit/shell.c`; owners s18, s19.
1. Is there still exactly one text editor in the program? `grep` for
   `char buf[` + a caret index in `src/ui/`; the prompt must still be a
   `TextBuf` + `Cursor`.
2. Do 500 spawn/reap cycles still leave fd delta 0 and zero zombies, with
   every LSP server (s46), git verb (s51), clipboard tool (s12), and AI
   curl subprocess (s48) counted in the same accounting?
3. Does any argv element get built by string interpolation anywhere?
   `grep -rn 'bytebuf_printf.*cmdline|sprintf.*shell' src/` plus a manual
   read of every `YewJobSpec` construction site in s46, s48, s51, s44.
4. Filter deadlock: does the interleaved write/read still hold for a
   region larger than pipe capacity against a command that writes 1 MiB
   before reading a byte? Remove the interleave locally and confirm the
   test hangs — a passing test that would also pass without the mechanism
   is not evidence.
5. Is `yew_shell_quote` still round-trip-exact through `/bin/sh` over
   100 000 fuzz cases including invalid UTF-8 and embedded newlines?
6. Does a nonzero-exit filter still roll back to a byte-identical buffer,
   in exactly one undo transaction, with the journal consistent?
7. Does the typeahead buffered during a foreground filter still replay in
   order, and never dispatch mid-filter?
8. `PAGER`/`GIT_PAGER=cat`, `COLUMNS`/`LINES` unset, `YEW_JOB=1` — is the
   exported environment still exactly s19 §4's table for every job type,
   including the ones s46/s48/s51 spawn?
**Evidence:** fd/zombie accounting, the removed-interleave hang, the
env dump per job type. **Deliverable:** `audit-05-exec.md`.

#### F06 `RE` — regex engine, search, replace
Scope `src/search/`; owners s20, s21.
1. Is the no-materialization law intact? `grep -rn 'textbuf_to_bytes|materialize|backtrack' src/search/` and a read of every
   `yew_re_window_fill` call site for a window over 64 KiB.
2. Does the empty-width guard still live *inside* `addthread`? Move it out
   locally; `(a*)*` against 100 000 `a`s must hang. If it does not, the
   dedicated test is not testing what it claims.
3. Re-run the differential fuzz at 5× budget: 25 000 000 pairs × 4 seeds
   against the backtracking oracle. Is the oracle skip rate still under
   2 %, and are the skipped cases actually the pathological ones rather
   than a silently growing exclusion?
4. Do the pathological-pattern latency gates still hold at the s56
   calibration, or were they calibrated on a machine that made them free?
   `t(10⁵)/t(10⁴) ≤ 15` on all four scaling patterns.
5. Smartcase: do the four non-triggering escape cases still not trigger
   (`\W`, `\x41`, `[[:upper:]]`, `\A`)? Has any consumer added since s21
   (LSP `documentHighlight` s47, plugin search s54) reintroduced a
   `strpbrk` for uppercase bytes?
6. Replace: is it still one undo transaction always — including a confirm
   run terminated with `q`, a 10 000-match replace-all, and a replace
   spanning a multi-cursor set? Are matches still applied back-to-front?
7. Zero-width replace (`:%s/^/> /`) on a file whose first line is empty
   and whose last line has no final newline: correct on both edges?
8. Is `&` still literal, and is that divergence stated in the manual
   (F-cross-check with s59's `docs/manual/`)?
**Evidence:** differential counts, the moved-guard hang, latency tables.
**Deliverable:** `audit-06-regex.md`.

#### F07 `UI` — panes, tabs, groups, workspace persistence
Scope `src/ui/`, `src/ws/`; owners s22–s27, s25.
1. Does layout still compute `Rect`s once, with drawing and hit-testing
   consuming the same values? Instrument the region table and assert that
   every clickable span's cells equal the cells the renderer wrote, over
   the full pty suite at 80×24, 81, 121, and 200×50.
2. Can an empty tab group exist after **any** sequence of operations?
   Fuzz 100 000 membership ops including close-active, reorder-block,
   dissolve-during-walk, and workspace restore mid-sequence.
3. Does opening a 40-file group still perform exactly one file read, and
   does a restore of 40 tabs still perform exactly one? Count through
   `file.c`'s test hook, not by timing.
4. Is `state.fl` still float-free, insertion-ordered, groups-before-tabs,
   and byte-identical across two emissions? Does the permille→ratio→permille
   fixpoint still hold for all 999 values after s36 swapped the emitter to
   the Fletch data path?
5. **Corrupt-state discipline:** truncate `state.fl` at every byte length,
   flip one bit at every byte, and insert an unknown key at every depth.
   Does yew ever fail to start, exit non-zero, prompt, or delete the
   bad file? Are unknown keys still retained for re-emission?
6. Does the workspace lock still check the pid rather than file existence?
   `kill -9` a session and reopen: does the second session write?
7. Does anything pollute the repository? `git status --porcelain` in a
   fixture workspace after a full session including tutor, plugins, and
   LSP must be empty.
8. Picker selection identity: is it still held by payload across
   refilters, in the file picker, buffer switcher, group picker, diagnostic
   picker (s46), symbol picker (s47), and plugin picker (s54)? One row-index
   holder is enough to open a file the user did not choose.
**Evidence:** region/cell diffs, read counts, the corrupt-state matrix.
**Deliverable:** `audit-07-ui.md`.

#### F08 `FL` — Fletch VM, stdlib, editor API
Scope `src/fl/`, `tests/fletch/`; owners s28–s34, s36.
1. **The spec-amendment arithmetic.** Two amendments add error kinds:
   A1 (Sprint 30, `"limit"`) and A2 (Sprint 34, `"handle"`), so §9's set is
   **11 + 2 = 13**. Sprints 31–33 correctly say 12 for their point in time.
   *(A planning-stage collision — both amendments were drafted as "A1" —
   was resolved in s28's §16 amendment registry, which is now the single
   source of ids.)* Does the tree's §16 contain exactly A1/A2/A3 with
   unique ids, is the count 13 everywhere post-s34, and does s33's coverage
   check 4 test the real set or a stale one? A `catch` on each of the 13
   kinds must be individually reachable.
2. **`FlOrigin` — is there exactly one definition?** s34 was reconciled
   onto s31's ledger-compliant `FlOriginKind`/`FL_ORIGIN_*` declaration,
   relocated to `src/fl/origin.h`, with the *enum kind* and the *registry
   id* pinned as distinct numbers (kind `FL_ORIGIN_BUILTIN = 0`; registry
   id 0 reserved for the user config, which is the number s54 depends on).
   Does exactly one declaration exist in the tree, is `YEW_ORIGIN_*` absent
   entirely, and — the question that matters — can a plugin's origin ever
   resolve to the user config's grants by conflating the two zeroes?
   Attack it directly: register a plugin origin first and see what id it
   gets.
3. Is the GC root set still enumerable and enumerated? s30 pins eight
   roots and calls adding one a review event; s34 adds
   `fl_gc_host_root_add` and `fl_gc_root_provider`. Does `gc.c` document
   the current set, and does a `FL_GC_STRESS` run with every host root
   deliberately unregistered actually crash (proving the roots are load-bearing
   and not decoration)? Also: is the stress env var `FL_GC_STRESS` or
   `YEW_FL_GC_STRESS`, and do both spellings exist in DoDs?
4. Does `EDIT_BEGIN`/`EDIT_END` or `TXN_ENTER`/`TXN_LEAVE` exist, and does
   `_Static_assert(FL_OP__COUNT == 60)` still hold with s33's `op:NAME`
   ledger covering every opcode?
5. The side-door law: `grep -rn 'yew_edit_insert|yew_edit_delete|yew_textbuf_|yew_undo_record|yew_reg_set|yew_reg_yank|yew_reg_delete|yew_cset_' src/fl/` must print nothing. Seed one call and confirm `make test` fails.
6. Capability enforcement: for each of `fs.read fs.write shell net`, write
   a plugin-origin script that reaches the capability through (a) a direct
   native, (b) a closure defined in the plugin but called from user config,
   (c) a hook fired by the editor, (d) a macro replayed from a register.
   Does the check read the **defining module of the calling function** in
   all four, per spec §11?
7. Does `edit{}` still produce exactly one undo node for 10⁵ nested
   mutations, and does an error escaping a nested `edit` roll back and
   **re-raise unchanged**?
8. Does `fmt.f` still never pass a user string to printf? Is
   `scripts/bans.sh`'s printf rule still active over `src/fl/`?
9. Does a handle survive its object's death cleanly — a buffer handle
   after `ed.buf.close`, a span handle after its marks collapse, a regex
   handle after 10 000 compilations (the one owning kind)?
**Evidence:** the amendment archaeology, a capability matrix (4 caps ×
4 reach paths), GC-stress crash transcripts. **Deliverable:**
`audit-08-fletch.md`.

#### F09 `REC` — recorder and the round-trip law (invariant 10)
Scope `src/fl/record.c`, `tests/roundtrip/`; owners s35, s38.
1. Run the law at 100× CI budget: 200 000 generated seeds across all six
   initial fixtures. Do P1–P5 hold with zero divergences?
2. Generator coverage: is **every** `YEW_CMD_RECORDABLE` command either
   reached by the generator or on the denylist with a stated reason —
   including every command registered by s39–s55 (`shadow.*`, `compl.*`,
   `ed.lsp.*`, `ed.git.*`, `ed.plug.*`)? `make test-roundtrip --coverage`.
3. Is folding still refused for `YEW_CMD_TAKES_COUNT`? Seed the bug and
   confirm the shrinker names the right op in ≤ 3 events.
4. Does emission stay deterministic and byte-identical across both
   compilers, both dispatch modes, and all four targets?
5. Is replay still the VM and only the VM? `grep -c 'yew_cmd_invoke|yew_edit_|yew_textbuf_|yew_reg_set' src/fl/record.c` → 0, and a stubbed
   `fl_call_chunk` must produce zero editor mutation.
6. Does a macro recorded under one keymap replay identically under
   another? Record under `runtime/init.fl`, replay under a keymap that
   rebinds every key used, and compare buffer, cursor set, and registers.
7. Does the s38 store-time validation still reject a macro that would fail
   at replay, and is the `keymap:` header still provenance-only — no code
   anywhere refuses to run a macro on a keymap mismatch?
8. Does `5@a` still produce exactly five undo nodes, and does an erroring
   third command leave the buffer byte-identical to pre-replay?
**Evidence:** seed counts, coverage report, the cross-keymap comparison.
**Deliverable:** `audit-09-recorder.md`.

#### F10 `SYN` — syntax engine and definitions
Scope `src/syn/`, `runtime/syntax/`, `runtime/themes/`; owners
s39–s42.5, including s41.5.
1. The asymmetric-cap corruption: push 40 contexts past
   `YEW_SYN_DEPTH_MAX`, then pop 40. Is the exit state id **exactly** the
   entry state id, in all 48 definition files?
2. Does every produced state satisfy `depth ∈ [1,16]`, `ndef ∈ [1,4]`,
   valid resident-definition frame references, and canonical zero tails? For
   every non-embedding definition, is `ndef == 1` with all live frames on the
   root definition? For every balanced embedded region, does exit intern to
   the exact pre-entry state id? The obsolete `SynState.def` field must not
   exist; s41.5 replaced that pre-embedding assertion with these invariants.
3. Does the first-byte set still cover every byte a rule can match? Run
   the debug brute-force self-check (256 values × every rule) over all 48
   definitions — a too-small set is a correctness bug, not a slow path.
4. Cache invalidation: `git checkout` a definition to an older version
   with identical size and mtime. Does the hash ladder catch it? Does a
   truncated, bit-flipped, or zero-length `.stab` recompile silently?
5. Does a theme switch still call `yew_syn_line` zero times, and do both
   shipped themes still define all 54 attrs explicitly?
6. Do the 108 tabled 256-colour values still equal `yew_rgb_to_256(hex)`
   after any palette edit? The documentation is a test.
7. Is `NO_COLOR` still honored for **any** value including empty, across
   every surface added since s41 — panel, completion menu, diagnostics,
   shadow text, plugin picker, tutor? `grep -c '38;2|38;5|\[3[0-7]m|\[9[0-7]m'` over the full golden set must be 0.
8. Differential: 100 000 random edits × 4 seeds × 48 syntax modes against
   from-scratch restyling. Zero divergences?
9. Are the known-wrong js/ts goldens (the `}` and `)` value-flag rows)
   still marked as known-wrong with their comment, so a future fix reads
   as an intentional diff rather than a regression?
**Evidence:** state-id equality and embed round-trip dumps, the bounded-state
report, the brute-force self-check report, the cache ladder matrix.
**Deliverable:** `audit-10-syntax.md`.

#### F11 `LSP` — JSON, JSON-RPC, client, features
Scope `src/mod/lsp/`, `src/unicode/u16.c`; owners s45–s47.
1. **Position conversion is the whole front.** Run the differential test
   at every byte offset of every line of a corpus containing astral
   characters, U+DC80–DCFF escapes, tabs, CRLF, and a 4 KiB line — both
   encodings, both directions, zero disagreements. Then run it against a
   deliberately wrong variant to prove the test can fail.
2. Is the `+1` ban still real? s46's DoD greps `'line + 1|line - 1|\.v + 1'` over `src/mod/lsp/`; s47's pickers render 1-based line and column
   for display. Which is true in the tree — a violated gate, or a gate
   that never ran?
3. Does `yew_lsp_note_edit` still compute **both** delete endpoints in
   pre-op coordinates? Construct a multi-line delete and compare the
   emitted range against the document the server holds.
4. Generation counters: fire every one of the nine position-bearing
   methods, edit the buffer while each is in flight, and assert the
   response is dropped. Is the check still in the pump-phase dispatcher
   rather than in nine call sites?
5. Rename: does phase 2 hydrate buffers before phase 3, and if the user
   cancels the confirm, how many buffers and tabs are left open? Does a
   mid-apply failure still roll back every already-committed file with
   exactly one `yew_undo` each?
6. Does a malformed `WorkspaceEdit` ever modify a buffer? 4 × 50 000
   fuzz iterations with byte-identity asserted after each.
7. Snippet downgrade: does `grep -rn 'tabstop|placeholder' src/` still hit
   only the policy paragraph, and does `${1|red,green|}` still yield `red`
   deterministically?
8. Does `MODULES=""` still register every `ed.lsp.*` command with the
   exact shim message, and does the musl static build's size budget hold
   with and without the module?
**Evidence:** the differential matrix, the fuzz byte-identity log, the
cancelled-rename buffer count. **Deliverable:** `audit-11-lsp.md`.

#### F12 `AI` — HTTP, backends, shadow, privacy
Scope `src/mod/ai/`; owners s48–s50 (index entries bind; read those sprint files first).
1. Is AI still **off by default**? Build with `MODULES="lsp ai fuss plugins"`,
   run with an empty config, and assert zero network syscalls and zero
   `curl` spawns across a full editing session — `strace`-free proof via
   the job accounting plus a `curl` shim on `PATH` that fails loudly.
2. Do the documented redaction rules actually run before bytes leave the
   process? Feed a buffer containing an API key, a password, a private
   key block, and a path under `$HOME`, and capture the exact request body
   the backend would send. Every claim in the privacy documentation must
   have a mechanism; a claim without one is Critical (doc says guaranteed,
   code does not guarantee).
3. Does the bespoke HTTP/1.1 client refuse non-localhost destinations, as
   the no-TLS decision requires? Point it at a remote host and at
   `127.0.0.1` spelled six ways (`localhost`, `::1`, `0x7f000001`,
   a DNS name resolving to loopback, a redirect to a remote host).
4. Is chunked transfer decoding total over hostile input? Fuzz the
   response parser with truncated chunks, oversized chunk sizes, negative
   and hex-overflow lengths, and trailers.
5. Does an AI stream still cancel on the keystroke that invalidates its
   prefix, with the process actually reaped rather than orphaned? 200
   arm/cancel cycles → zero zombies, zero leaked fds.
6. Does the shadow staleness law hold for the AI provider specifically —
   one in flight globally, generation drop on every edit, revalidation
   on accept returning −1 rather than splicing? A response landing 400 ms
   late must insert nothing.
7. Does `MODULES=""` and `MODULES="lsp fuss plugins"` still hard-error
   with the canonical message on every `ed.ai.*` surface?
**Evidence:** captured request bodies, the loopback-spelling matrix,
process accounting. **Deliverable:** `audit-12-ai.md`.

#### F13 `GIT` — git layer and FUSS mode
Scope `src/mod/git/`; owners s51–s53.
1. Is there still no `yew_git_raw`? Every invocation must be a row in the
   static verb table, with `--` before every path argument and no argv
   element built by `sprintf`.
2. Porcelain v2 record `2` consumes **two** NUL-terminated fields. Patch
   `pv2_record` to advance one NUL and confirm the test fails — if it
   still passes, the desync is untested.
3. Filenames: a file named `a b`, `a\nb`, `a"b`, `α`, and a name of
   invalid UTF-8 bytes. Do status, stage, unstage, commit, diff, and blame
   all round-trip byte-exactly through the tree and the verbs?
4. Is the TTL still wall-clock (`yew_now_ms`) and never `time(2)` or
   `clock(3)`? Step the system clock backwards 10 minutes mid-session and
   confirm the snapshot still refreshes — the named fuss bug.
5. Does a failed refresh leave the published snapshot byte-identical with
   `gen` unchanged? Kill the git subprocess at 200 random instants during
   refresh and assert it.
6. Does `index.lock` still cause a silent skip rather than an error, and
   do all seven mid-* states resolve from `access(F_OK)` probes only?
7. Are the forced env rows still forced on every verb — `GIT_TERMINAL_PROMPT=0`,
   `GIT_EDITOR=false`, `LC_ALL=C` — and is the parent `environ` still
   never mutated? Run a verb under a hostile parent environment containing
   every removed and forced key.
8. Does F mode's collapsed-state preservation survive a refresh that
   removes the expanded directory, and does the group-open path (s53) still
   route through s24's exact sequence?
**Evidence:** the filename matrix, the patched-parser failure, clock-step
transcript. **Deliverable:** `audit-13-git.md`.

#### F14 `PLUG` — plugins and `yew pkg`
Scope `src/mod/plug/`; owners s54, s55.
1. Manifest validation: does `entry` containment still use realpath **after**
   resolution? Try `src/../../evil.fl`, a symlink to `/etc/passwd`, a path
   with a NUL byte, and a directory whose basename differs from `name`.
2. Does an undeclared capability still error immediately without ever
   prompting? Try to reach `net` from a plugin declaring only `fs`, through
   a closure, a hook, and a bound key.
3. Zero residue: enable and disable 20 plugins 20 times. Do all registry
   lengths return to their pre-enable values, and does a GC cycle reclaim
   every closure? Does an erroring closure inside teardown abort the ledger
   walk?
4. Does the trust db still preserve unknown keys, sort keys bytewise, and
   write atomically? Corrupt it at every byte length and confirm the editor
   starts.
5. Does `yew pkg` verify what it installs? Install from a git URL whose
   ref moves under it, whose tarball is truncated, whose lock file
   disagrees with the checked-out commit. Is any failure silent?
6. Is the security wording still absent? `grep -rn 'sandbox'` over
   user-facing strings must return nothing, and `plug.h`'s §7 paragraph
   must still say plainly that this is not isolation.
7. Does the error limit still auto-disable through the normal teardown
   path rather than a special case, and does a throwing `buf.save` hook
   still let the save proceed with pre-hook content?
8. Does a plugin-registered command still record and replay through the
   recorder (F09 cross-check), and does its CMDWORD still collide-check
   against core?
**Evidence:** the containment matrix, residue counts over 400 cycles,
`yew pkg` failure transcripts. **Deliverable:** `audit-14-plugins.md`.

#### F15 `CI` — build, tests, determinism, gates
Scope `Makefile`, `scripts/`, `.github/workflows/`, `tests/`, `.docs/audits/xfail-debt.md`; owners s00, s01, s11, s33, s56, s57.
1. **XFAIL debt audit.** For every ID in `xfail-debt.md`: does it still
   reproduce? Is it cited correctly? Does any XFAIL exist without a ledger
   ID, or any ledger ID without an XFAIL? Is XPASS actually a hard failure
   on all four surfaces (§3) — seed one on each and confirm.
2. **Ban honesty.** Every rule in `bans.sh` is a shallow textual check a
   plausible refactor defeats. For each rule, seed a violation that the
   grep **misses** but the intent forbids (e.g. `qsort` reached through a
   macro, `system()` spelled via a function pointer, width math done with
   a local table). Each miss is a finding against the rule, not the code.
3. **Gate honesty.** Is any s56 threshold sitting at or below its recorded
   noise floor? Recompute the noise floor from 30 runs and compare. A gate
   inside its own noise is not a gate.
4. **Baseline drift.** Every commit that moved a file under
   `tests/perf/baselines/` — does it carry the required old→new+why
   message? Was any ratchet ever loosened without a stated reason?
5. **Determinism.** Build twice from a clean tree on two machines and
   compare `sha256sum build/yew` for each of the four `MODULES` sets.
   Then run the full suite twice and diff — unit, script, pty, roundtrip,
   fletch conformance, audit.
6. **Coverage holes by module.** Which files under `src/` have no direct
   unit or script test? Produce the table; each row is at least an
   Observation and a row with zero coverage in a Critical-severity
   subsystem is a Medium finding.
7. **The `MODULES=""` lane.** Does every excluded module still hard-error
   with its canonical message on every surface — commands, Fletch API,
   config keys, CLI subcommands, man page cross-references?
8. **`.fl` coverage instrumentation** (s37 §9's obligation to this
   sprint): which `t.*` assertions and which script tests never execute?
   Report per-file executed-statement counts from the VM's line-run table
   (`FlLineRun`), which already exists — no new instrumentation needed.
9. Is `grep -rnE 'Sprint [0-9]+' src/` still producing hard-error messages
   for sprints that have **landed**? Every such message is now a lie about
   an implemented surface. (This list is Sprint 60's docs-freeze input.)
**Evidence:** the seeded-miss table, noise-floor recomputation, the
coverage-hole table. **Deliverable:** `audit-15-ci.md`.

### 6. The fuzz-campaign extension — the coverage decision

**Decision: build bespoke coverage instrumentation.** Rejected: staying
with plain black-box mutation plus dictionaries.

The honest argument, in the order it actually decides the question:

1. **You cannot claim black-box is sufficient without measuring coverage.**
   The alternative's whole case is "our structure-aware mutators reach
   deep enough." That is a claim about edge coverage, and there is no way
   to evaluate it — or to know when a corpus has plateaued — without an
   instrument. The instrument is therefore required as an *audit tool*
   even under the losing hypothesis. Having built it, refusing to feed it
   back into corpus admission would be a deliberate waste.
2. **The no-dependency rule is not violated.** `-fsanitize-coverage=trace-pc-guard`
   is a compiler flag supported by both gcc and clang; the callbacks
   (`__sanitizer_cov_trace_pc_guard`, `__sanitizer_cov_trace_pc_guard_init`)
   are plain C functions **we write** — no library is linked. This is the
   same class of thing as `SAN=1`, which s01 already ships. Invariant 7
   governs what the shipped binary links, and this lane never ships.
3. **The scope is cut to keep it small.** We build coverage-guided **corpus
   admission** only: run an input, check whether any previously unseen edge
   fired, keep it if so. No coverage-guided scheduling, no cmplog, no
   value profiling, no forkserver, no persistent mode. That is ~150 lines
   against the existing `fuzzlib` mutators, which are unchanged.
4. **The targets that matter are deep parsers.** `fuzz_re_compile`,
   `fuzz_fl_parse`, `fuzz_fl_vm`, `fuzz_syn_def`, `fuzz_json`,
   `fuzz_state`, `fuzz_porcelain` all have accept/reject cliffs where
   blind mutation spends its budget on inputs rejected in the first
   twenty bytes. This is exactly where feedback pays.
5. Dictionaries are not the rejected alternative — they cost forty lines
   and land anyway (§6.3). The question under debate was whether to build
   the feedback loop, and the answer is yes.

#### 6.1 `tests/fuzz/cov.c|h` — the instrument

```c
/* Built ONLY into build-cov/.  Never linked into build/yew: the
 * guard array and the callbacks exist because the compiler emitted
 * calls to them, and shipping them would put a write on every basic
 * block of the editor's hot loop.                                     */
#define YEW_COV_BITS 22                       /* 4 Mi edges, 4 MiB     */
extern u8 yew_cov_map[1u << YEW_COV_BITS];    /* 8-bit hit counters    */

void __sanitizer_cov_trace_pc_guard_init(u32 *start, u32 *stop);
void __sanitizer_cov_trace_pc_guard(u32 *guard);

void   yew_cov_reset(void);                   /* zero the map          */
u32    yew_cov_new_edges(void);               /* vs the global seen set*/
void   yew_cov_merge(void);                   /* fold map into seen    */
void   yew_cov_report(FILE *out);             /* per-target edge count */
u64    yew_cov_hash(void);                    /* stable map digest     */
```

Makefile: `COV=1` appends `-fsanitize-coverage=trace-pc-guard` and forces
`BUILD=build-cov` — **sanitized, coverage, and plain object trees must
never mix** (s01's pitfall, restated with a third tree). `make fuzz-cov`
builds every fuzzer against it.

**Pitfalls.**
- The guard array is per-translation-unit and `_init` is called once per
  TU with a different range; a single global counter assigned in call
  order is the only deterministic id assignment. Do not hash the PC — the
  addresses move with ASLR and the corpus becomes machine-specific.
- Counters are 8-bit and **saturate**; a loop executed 10⁶ times must not
  wrap to look like one execution. Saturate explicitly, do not rely on
  `++` wrapping being harmless.
- `yew_cov_new_edges()` must be evaluated *after* the target returns and
  *before* the map is reset, or a crash-in-progress input is admitted with
  a stale map.
- Coverage must not change behavior. `make fuzz-cov` runs each committed
  corpus entry through both `build/` and `build-cov/` binaries and compares
  the target's own output hash — a divergence means the instrumentation is
  perturbing something (usually a timing-dependent branch, which is itself
  a finding).

#### 6.2 Corpus admission loop

```
for each iteration:
    input = mutate(pick(corpus), rng)     # fuzzlib, unchanged
    yew_cov_reset()
    run_target(input)                      # existing per-target invariants
    if yew_cov_new_edges() > 0:
        yew_cov_merge()
        minimize(input)                    # existing binary-chop shrinker
        write tests/fuzz/corpus/<target>/<sha256-prefix>.bin
```

Admitted inputs are **committed**. The corpus never shrinks (s11's rule).
A crashing input still goes to `tests/fuzz/crashes/` and still fails the
build.

#### 6.3 Dictionaries — `tests/fuzz/dict/<target>.txt`

One token per line, `#` comments. They ride along as an extra mutator
("splice a dictionary token at a random offset"), because they are nearly
free and they front-load exactly the tokens a mutator would take hours to
discover:

| Target | Dictionary content |
|---|---|
| `fuzz_re_compile` | `[:alpha:]`, `\b`, `\w`, `{1,1000}`, `(?`, `\x{`, `\u{10FFFF}` |
| `fuzz_fl_parse` | the 22 reserved words, `@[`, `H(`, `i"`, `${`, `0x`, `\u{` |
| `fuzz_syn_def` | every s40 schema key, `at_eol`, `pop:4`, `indent_lt`, `fence_close` |
| `fuzz_json` | `\uDC80`, `1e999`, `-0`, a raw NUL in a string, 128 nested `[` |
| `fuzz_jsonrpc` | `Content-Length:`, `\r\n\r\n`, `Content-Type:`, `jsonrpc` |
| `fuzz_porcelain` | `1 .M`, `2 R.`, `# branch.ab`, `u UU`, `\x00`, `\x1f` |
| `fuzz_state` | every s25 schema key, `ratio_permille`, `goal: -1`, `\xNN` |
| `fuzz_input` | `\x1b[`, `\x1b[200~`, `\x1b[<0;1;1M`, `\x1b[?2026;2$y`, `\x1b[>21u` |

#### 6.4 Two new fuzz targets this sprint

| Target | Subject | Invariants |
|---|---|---|
| `fuzz_undo_serial` | `.sagu` v1 reader (s10 §format) | never crash; truncated tail applies the valid prefix; a 3-way-hash mismatch drops rather than partially applies; round-trip of any accepted file is byte-identical |
| `fuzz_theme` | s41 theme loader | never crash; any accepted theme fills all 54 `ThemeEnt` slots; malformed colour forms raise rather than defaulting silently |

### 7. The long-run soak schedule

Pinned here and running from this sprint through the tag and past it.

| Lane | Cadence | Targets | Budget per target | Seed policy | Failure action |
|---|---|---|---|---|---|
| `fuzz` (CI, per push) | every push | all | 200 000 iters | fixed `FUZZ_SEED=1` | red build |
| `fuzz-nightly` | nightly | all | 30 min | date-derived, printed | minimize, file `YEW-F-###`, commit corpus |
| **`soak`** | continuous from this sprint | tier 1 (below) | **72 h per target**, 4 processes with disjoint seed streams | random base seed, recorded in `fuzz-coverage.md` | as nightly; soak restarts from the grown corpus |
| `fuzz-cov-weekly` | weekly | all | 4 h | corpus-only, no mutation cap | edge-count delta appended to `fuzz-coverage.md`; **a decrease fails the lane** |
| `soak-rc` | once per release candidate | tier 1 + tier 2 | 72 h, on the s56 designated runner, on the exact RC commit | random, recorded in the release notes | blocks the tag |

**Tier 1** (deep parsers, hostile input, or invariant-1/2 adjacent):
`fuzz_utf8`, `fuzz_grapheme`, `fuzz_textbuf`, `fuzz_undo_serial`,
`fuzz_re_compile`, `fuzz_re_diff`, `fuzz_fl_parse`, `fuzz_fl_vm`,
`fuzz_syn_def`, `fuzz_json`, `fuzz_jsonrpc`, `fuzz_state`,
`fuzz_porcelain`, `fuzz_input`.
**Tier 2** (everything else): the remaining ~20 targets, 12 h each.

Rules that make the schedule mean something:

- A soak is **stateful**: it starts from the committed corpus and its
  admissions are committed at the end of each run. A soak that starts
  from an empty corpus every time is a benchmark, not a campaign.
- Every soak run appends one row to `fuzz-coverage.md`: date, commit,
  target, iterations, new edges, total edges, corpus size, findings. The
  weekly lane's monotonicity check reads this file.
- **A plateau is data, not success.** Three consecutive weekly runs with
  zero new edges on a tier-1 target is an Observation on F15 asking
  whether the mutators can reach the remaining code at all.
- Soaks run under ASan+UBSan on alternating weeks; the `alarm(5)` watchdog
  and the 64 MiB arena high-water cap stay on in every lane.

### 8. Invariant re-verification sweep — `.docs/audits/invariants.md`

Ten dedicated adversarial sessions, one per invariant, each run by a
reviewer who did **not** run the front that covers the same code. Each
session gets its worst case spelled out below and produces a section in
`invariants.md`: setup, what was run, what was observed, verdict
(`HOLDS` / `HOLDS WITH FINDINGS: <IDs>` / `VIOLATED: <IDs>`).

| # | Invariant | Worst-case scenario for its session |
|---|---|---|
| 1 | No data loss, ever | A 1.5 GiB file on a real filesystem, opened over a symlink whose target is a 3-link hardlink in a directory that becomes unwritable mid-session, edited by a 10 000-cursor transaction, while the file is replaced on disk by another process, and `kill -9` arrives at every syscall boundary of the save. After every kill: the file is byte-identical to pre-save or post-save, and the journal replays the difference exactly. Then the same run with the disk full at 99.9 % (`fallocate` a filler), and with `fsync` failing via the s08 shim. |
| 2 | No byte confusion | A file whose every line is a different adversarial cluster — a 25-byte ZWJ family, an odd-length RI run of 65, a 300-mark combining stack, a lone `ED A0 80`, every overlong form, a CRLF split across a piece boundary, a BOM followed by a BOM. Open it, move by every unit in every mode, select rectangularly across all of it, yank, paste through a shell filter, replace with a regex, run it through a macro, save. Every untouched byte identical; every reported column matching a hand-computed table. |
| 3 | No silent stubs | Enumerate every command in the registry, every Fletch native, every config key, every CLI subcommand, and every `MODULES` combination (16 sets). Invoke all of them in every build. Every unimplemented path must hard-error with a message; **at 1.0 no message may name a sprint** (§5 F15 q9). A path that returns without acting is Critical. |
| 4 | Latency budgets are CI gates | On the s56 designated runner, with the worst realistic state loaded: a 100 MB file, 1000 cursors, LSP attached and indexing, a git status refresh in flight, an AI stream open, a 50 MiB job streaming into a buffer, syntax settling a comment bomb — measure keypress→paint p99 over 200 000 synthetic keystrokes. Then the same with the s41 comment-bomb fixture under a fake clock so the frame assertion is a hard gate, not a timing hope. |
| 5 | Deterministic rendering | Run the entire pty suite twice on two machines with different libc (glibc and musl), different compilers (gcc and clang), and both dispatch modes (`FL_CGOTO` 0 and 1), under `TZ` set to four zones and `LANG` set to four locales, with `COLORTERM` and `TERM_PROGRAM` set to hostile values. Every golden byte-identical. Then rebuild both binaries and compare sha256. |
| 6 | Terminal restore | Kill the editor with each of `SIGSEGV SIGBUS SIGABRT SIGTERM SIGINT SIGQUIT SIGHUP SIGKILL`, at four moments: mid-render inside a BSU/ESU pair, inside the fatal handler itself (a second signal), while a filter holds typeahead, and while an LSP shutdown is in its 500 ms budget. For each, capture the pty byte tail and the termios struct: the restore blob present and complete, `?1049l` emitted, cursor visible, SGR reset. `SIGKILL` cannot restore — assert instead that the **parent shell** can recover with a single `reset` and that the crash journal survived. |
| 7 | Bespoke first | `ldd build/yew` on each target lists only libc (and nothing at all on musl-static). Read the full link map for a symbol from any library the project does not own. Re-read every dependency-adjacent decision — `curl` as a subprocess, `git` as a subprocess, `$SHELL` — and confirm each degrades gracefully when the binary is absent from `PATH`, with the exact message the sprint pinned. |
| 8 | Single-threaded core | `grep -rn 'pthread|threads\.h|_Thread_local|atomic_' src/`; then read `/proc/self/status`'s `Threads:` under a session with LSP, AI, git, and three shell jobs live — it must read 1. Any library that spawns a thread behind our back (none should exist, per invariant 7) is Critical. |
| 9 | Modal paradigm first | With `YEW_MOUSE=0`, drive every user-visible surface to every reachable state using only the keyboard: all seven modes, panes, tabs, groups and the group picker, all five pickers, the context menu's every action, the completion menu and doc panel, hover and signature panels, the diagnostic list, FUSS mode's every verb, the plugin picker, the tutor. Produce the coverage table; any state reachable only by mouse is a finding. |
| 10 | Recorder round-trip | 200 000 generated seeds (§F09 q1), plus a hand-built adversarial session that uses every recordable command at least once, including plugin-registered ones, a prompt that is cancelled, a multi-cursor edit, a macro that replays another macro, an argument-carrying command that splits the motion block, and an insert payload containing a lone `0x80`. Replay under a keymap that rebinds every key involved. |

**Pinned:** these ten sessions are **not** a summary of the fronts. A
front asks whether a subsystem is correct; an invariant session asks
whether the *promise* survives every subsystem interacting at once. The
scenarios above are deliberately cross-cutting for that reason.

### 9. Sequencing, dedup, and the two inbound obligations

**Sequencing.** Fronts run in parallel except for four edges:
`F01 → F02` (width answers bound render answers); `F03 → F04` (a modal
finding against a broken text engine is not a modal finding);
`F08 → F09` and `F08 → F14` (the recorder and the plugin capability model
both stand on Fletch's origin/amendment questions, which F08 must settle
first); `F15 last` — it audits CI history the other fronts are actively
generating. The ten invariant sessions (§8) run **after** all fronts
close, because their inputs include the fronts' findings.

**Dedup.** After all fronts close: same root cause found twice → one
canonical ID (first-filed wins), the loser flipped to
`duplicate → YEW-F-NNN`, alias recorded in both front files, deduped
totals recomputed into `audit-00.md`. The dedup pass also re-scores every
C and H against the rubric and records any downgrade with its reason.

**Inbound obligations discharged here** (both name Sprint 58 in the tree):

1. **Delete `src/ws/state_legacy.c`.** Sprint 36 §7 landed it as a
   test-only legacy emitter behind `YEW_STATE_LEGACY` with a comment
   saying Sprint 58 deletes it. Before deleting: run the 2×2 differential
   one final time and record the result in `audit-07-ui.md`; the s25
   corpus round-trip becomes the only remaining guard, which is what the
   schema-is-the-contract decision always intended.
2. **`.fl` coverage instrumentation.** Sprint 37 §9 deferred it here.
   Implemented as F15 q8 from the existing `FlLineRun` table — no new
   instrumentation, a ~60-line reporter under `--test`, output to
   `.docs/audits/fl-coverage.md`.

### 10. Defer

- **All fixes → Sprint 59.** A commit that changes `src/` during the audit
  window and is not one of the two obligations in §9 is a review
  rejection. Git history over the window must show only `tests/`,
  `.docs/audits/`, and audit-tooling commits.
- Closeout gates with checkbox verdicts, the deferral law, man pages,
  `yew tutor`, the user manual, the Fletch book → **Sprint 59**.
- Packaging, signing, reproducible release builds, README, CHANGELOG, the
  `--version` contract change, the docs freeze → **Sprint 60**.
- Findings whose only fix is a release-engineering change (version
  strings, install paths, man-page content) are filed under F15 and
  remediated in Sprint 60's lanes; Sprint 59 marks them
  `assigned` with the target sprint named.
- `make fuzz-cov` becoming a required CI lane → **not for 1.0**. It is a
  weekly lane and a soak driver; making a 4 h job blocking on every push
  would trade the determinism lane's speed for coverage we already
  measure. Recorded so it is a decision, not an oversight.

## Testing Strategy

The audit is the testing strategy for the tree. This section tests the
audit.

- **Reproducer discipline** (`scripts/check-audit-fixtures.sh`, wired into
  `make test` and CI): every `confirmed` ledger row names an existing
  reproducer; every reproducer names an existing, non-duplicate ledger row;
  every reproducer carries a self-describing header comment; every
  reproducer **fails at the baseline commit** (verified once, recorded in
  `audit-00.md`, re-verified by the s59 remediation lane flipping them).
- **Ledger lint** (`scripts/check-findings.sh`): ID format `YEW-F-\d{3}`,
  no duplicate IDs, no gap followed by reuse, one status per ID, every
  forbidden transition from §2 rejected — each proven by a fixture ledger
  that must fail.
- **XPASS is a hard failure, proven on all four surfaces**: seed one
  passing XFAIL in `tests/audit/registry.c`, one in a `.fl` script header,
  one `PtyCase.xfail_id` whose golden matches, and one s33 conformance
  file. Each must produce `XPASS <ID>` and exit 1.
- **Instrument self-test** (`tests/unit/test_cov.c`): guard ids assigned in
  deterministic call order across two runs; counters saturate rather than
  wrap; `yew_cov_new_edges()` returns 0 on a replayed input and > 0 on a
  newly reaching one; `yew_cov_hash()` identical across two runs of the
  same corpus.
- **Instrument neutrality**: every committed corpus entry produces the
  same target output hash under `build/` and `build-cov/`.
- **Dedup honesty**: deduped total ≤ raw total per front; every alias
  resolves; every downgrade carries a reason string.
- **Soak plumbing**: a 60-second soak run appends exactly one
  `fuzz-coverage.md` row with the correct commit and seed; a seeded edge
  regression fails the weekly lane.
- **Determinism of the audit's own outputs**: `check-audit-fixtures.sh`,
  `check-findings.sh`, and the coverage reporter each run twice with
  byte-identical stdout.

## Definition of Done

1. All 15 front files exist with the header block, and **every** attack
   question in §5 is dispatched — findings, or an explicit
   `probed, nothing found` line. A question with no line under it fails
   `check-audit-fixtures.sh`.
2. `audit-00.md` complete: baseline commit, `MODULES` sets, four-target
   commits, UCD version, per-front table with raw **and** deduped counts,
   and a verdict paragraph in blunt house style.
3. Every confirmed finding has an ID from one counter, a severity
   justified against §2's rubric verbatim, and a committed reproducer that
   fails at the baseline. Unverified observations are segregated and
   excluded from all totals.
4. `make test-audit` exists, is a `make test` dependency and a required CI
   lane, and runs the reproducers on all four surfaces. XPASS produces a
   hard failure on each of the four, proven by four seeded cases.
5. `scripts/check-findings.sh` and `scripts/check-audit-fixtures.sh` are
   green both directions; each has a fixture ledger proving it fails on
   every rule it claims to enforce (≥ 8 negative fixtures).
6. **Zero fixes merged during the audit window.** `git log` over the window
   touches only `tests/`, `.docs/audits/`, audit tooling, and the two §9
   obligations.
7. Cross-front dedup done; the four sequencing edges respected (front file
   dates prove it); every downgrade recorded with a reason.
8. Coverage instrument landed: `make fuzz-cov` builds every fuzzer into
   `build-cov/`, `test_cov.c` green, neutrality proven over the whole
   committed corpus, and `fuzz-coverage.md` holds a baseline edge count
   for all ~34 targets.
9. Dictionaries exist for the eight targets in §6.3; the two new targets
   (`fuzz_undo_serial`, `fuzz_theme`) are registered in `make fuzz`,
   `make fuzz-cov`, and the tier tables.
10. The first `soak` cycle has completed for **all 14 tier-1 targets**
    (72 h each, seeds recorded); every admitted input is minimized and
    committed; `tests/fuzz/crashes/` is empty or every entry has a ledger
    ID.
11. `invariants.md` holds all ten sessions, each with its §8 worst case
    actually executed, each ending in one of the three verdicts, each run
    by someone who did not run the overlapping front.
12. Both §9 obligations discharged: `src/ws/state_legacy.c` deleted with
    the final differential recorded; `.docs/audits/fl-coverage.md`
    generated from `FlLineRun` with per-file executed-statement counts.
13. The F15 sweep of `grep -rnE 'Sprint [0-9]+' src/` is complete and its
    output is committed as `audit-15-ci.md`'s appendix — Sprint 60's
    docs-freeze input.
14. The full suite is green at the baseline commit on all four targets and
    all seven-plus lanes; a closeout signed against a red tree is void
    (Sprint 59 enforces, this sprint records).
