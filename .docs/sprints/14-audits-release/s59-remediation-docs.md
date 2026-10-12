# Sprint 59: Remediation, Manuals, and `yew tutor`

## Prerequisites

- Sprint 58 — the audit is closed: `audit-00.md` totals final,
  `.docs/audits/findings.md` populated with `YEW-F-###` IDs, every
  confirmed finding carrying a reproducer under `tests/audit/` that fails
  at the baseline, dedup done, `invariants.md` holding all ten sessions,
  the fuzz soak running, and `audit-15-ci.md`'s appendix listing every
  remaining `Sprint N` string in `src/`.
- Sprint 58 §3 — the XFAIL linkage across the four test surfaces and
  `make test-audit` as a required lane. This sprint flips entries; it does
  not redesign the mechanism.
- Sprint 37 — `yew --batch`, the stdio contract, exit codes, and
  `tests/script/`'s runner. Every runnable example in the manual and the
  Fletch book goes through `--batch`, and `docs/batch.md` is the source
  the manual's batch chapter consumes.
- Sprint 6 — `PtyCase`, `ptc_keys`, `ptc_settle`, `ptc_snapshot`,
  `ptc_expect_exit`, the golden format and the `YEW_PTY_UPDATE` workflow.
  `yew tutor` runs in CI through this harness unchanged.
- Sprints 34/36 — `fl_hook_add`, `yew_hook_fire`, the frozen event
  inventory, origins, `edit{}`, `bind()`, the option table,
  `yew_config_load_all`. The tutor is a Fletch program under
  `FL_ORIGIN_BUILTIN`; it uses no private door.
- Sprints 13/16/17/18/19/21/24/25/35/38/52 — the surfaces the tutor
  chapters and the manual chapters describe. **Docs are written against
  shipped behavior**: every claim is checked against the sprint file that
  pinned it and, where executable, against a run.
- Sprint 20 — the regex engine's unsupported-construct message ends
  `see man yew-regex`. §4.4 discharges that obligation.
- Binding: `00-decisions.md` (invariant 3 — no silent stubs; invariant 7 —
  bespoke first, which applies to documentation tooling too) and the
  Cgfried closeout precedent: checkbox verdicts with evidence links,
  strikethrough-not-delete, the blunt final verdict line.

## Goals

Burn the audit ledger down Critical→High before any Medium or Low is
touched, prove each fix with its reproducer and a why-comment naming the
finding ID, and close out all fifteen audit fronts with evidence-linked
checkbox documents — with the rule that **deferring anything past 1.0
costs a ledger justification and a user-visible release-notes entry**, so
that no deferral is silent. Then land the documentation 1.0 requires:
three hand-written groff man pages under the zero-dependency law,
`yew tutor` as an interactive tutorial that is a real guided workspace
whose task completion is asserted by Fletch hooks reading real buffer
state and which runs in CI through the Sprint 6 pty harness so it cannot
rot, a user manual under `docs/manual/` with one chapter per campaign area
and every example runnable, and chapter 1 of the Fletch book taking a
reader from keystrokes to a function. Packaging, signing, reproducible
release builds, the README, the CHANGELOG, the `--version` contract change
and the docs freeze are **Sprint 60**; this sprint writes the content
Sprint 60 freezes.

**The gate-of-gates:** Sprint 60 may not start until all fifteen closeouts
read READY. This sprint ends in that state or in an honest NOT READY with
a short list. There is no third option.

The F13 field report in §1.4 is intentionally queued behind the audit and
the Critical/High priority law. It is a user-visible interaction correction,
not permission to change `src/` during Sprint 58 or to leapfrog an open
security, data-loss, or stability finding.

## Deliverables

### 1. The burn-down — severity-ordered remediation campaigns

- **Branches:** `remediate-FNN-<topic>`, one per front or per coherent
  cluster within a front, stacked on `trunk`. Terse imperative commits,
  no co-authors, no trailers.
- **Priority law:** every Critical, then every High, before **any** Medium
  or Low is touched. A merged Low while a Critical is open is a review
  rejection. One exception: a Low fixed as a free side effect of a
  Critical's fix may land in the same commit **if the commit names it**.
  Enforced after the fact by `scripts/check-priority.sh`, which walks
  `git log` over the remediation window and fails on any commit closing
  an M/L that predates the last C/H closure.
- **Burn-down chart** `.docs/audits/burndown.md`, updated **in the same PR
  as each merge** by the merging developer. One table: rows are merge
  dates, columns are open C/H/M/L counts and the total, seeded from
  `audit-00.md`'s deduped totals. A stale burndown is itself a finding
  against F15.
- **Ledger convention:** a resolved row in `findings.md` is struck through
  with its resolution date and fixing commit — **never deleted**. History
  is the point; the ledger is how someone in 2031 learns why a check
  exists.

#### 1.1 Per-finding Definition of Done — all five, or it is not closed

| # | Requirement |
|---|---|
| 1 | The reproducer under `tests/audit/` **passes**, and its XFAIL marker (registry row, `# XFAIL:` header, `PtyCase.xfail_id`, or conformance directive) is removed **in the fix commit** |
| 2 | **Why-comment law:** the fix carries a comment naming the ID and the bug prevented — `/* YEW-F-042: smartcase counted uppercase bytes inside \W, so /\Wfoo/ stopped matching Foo. Count literal codepoints only. */` |
| 3 | The `findings.md` row is struck through with date + commit hash and its status is `fixed` |
| 4 | A `burndown.md` row lands in the same PR |
| 5 | If the fix touches a documented behavior, the man page, manual chapter, or tutor chapter that documents it is updated in the same PR — `scripts/check-docs-sync.sh` (§7.3) fails otherwise |

**Pitfalls.**
- *Fixing the reproducer instead of the bug.* The reviewer's job is to
  confirm the diff lands in the subsystem, not in the test. A fix whose
  entire diff is under `tests/` is a rejection unless the finding was
  against the test.
- *Removing an XFAIL marker without the fix.* XPASS catches it — but only
  if the marker was correct when filed. Spot-check the marker, not just
  the result.
- *Batch-closing rows in a Friday sweep commit detached from the fixes.*
  The strikethrough travels with the fix or inside its PR.

#### 1.2 The cluster hunt — every Critical fix looks for siblings

Before a Critical's PR merges, the same reviewer spends 30–60 minutes on
one question: *does this bug have siblings?* Same pattern elsewhere in the
file; same assumption in the sibling subsystem; same missing check on the
other side of a symmetric pair (forward/backward motion, encode/decode,
save/load, push/pop, enter/leave). Cheap while context is loaded,
expensive next month.

Output is one of two things, and silence is not an option:
- new findings appended to the front's ledger with fresh IDs, through the
  normal pipeline **including a failing reproducer first**; or
- one line in the PR body: `cluster hunt: checked {places}, clean.`

The symmetric-pair list is worth stating because yew is full of them:
`yew_gb_next_bytes`/`_prev_bytes`, `yew_off_to_gcol`/`yew_gcol_to_off`,
`yew_off_to_u16col`/`yew_u16col_to_off`, `yew_undo`/`yew_redo`,
`yew_jump_back`/`_fwd`, `yew_group_add_member`/`_remove_member`,
`yew_plug_enable`/`_disable`, `fl_txn_enter`/`_leave`, encode/decode in
`json.c`, and every `_pre`/`_post` edit-notify pair.

#### 1.3 XFAIL debt burn-down

Full audit of `.docs/audits/xfail-debt.md` (populated since Sprint 33).
Every ID gets **exactly one** verdict, recorded in the ledger:

| Verdict | Action |
|---|---|
| still-valid, fix-scheduled | assign to a remediation branch, or defer under §3 with the target release named |
| fixed-by-remediation | flip to passing, strike through with the fixing commit |
| stale — no longer reproduces | remove the marker, strike the entry, **and file a note explaining why it silently started passing** (an unexplained recovery is a finding waiting to happen) |
| reclassify-wontfix | allowed only with written justification in the ledger, and only for M/L |

**Numeric bar for release** — the mechanism, not a number invented in
advance: zero Critical-tagged and zero High-tagged XFAILs, and a total
≤ **N**, where N is set *after* the triage from the actual post-triage
ledger and written into `closeout-F15-ci.md` as a checkbox. Picking N
before seeing the ledger is theater; this file gives the rule, the ledger
gives the number.

#### 1.4 F13 field remediation — F-mode action discovery

**Admission.** The 2026-09 field report is a reproducible interaction
defect, not a new audit ID by fiat: F13 records the replay and either assigns
an existing or new `YEW-F-###` row, or records why the surface is already
covered. Once the §1 priority law permits it, the remediation branch keeps
bare filename characters exclusively for visible-tree type-to-jump and makes
every previously printable F-mode action reachable without stealing those
characters.

**Default key contract.** In `src/edit/keys_default.c`'s `panic_F` map,
replace the historic `C-g <verb>` action chords with `A-<verb>` direct
bindings. The prior verb is preserved exactly: `A-a` stages, `A-d` opens the
selected-path diff, and, critically, `A-g` invokes `ed.group.from_dir` for
the selected directory. Structural navigation, open, split, refresh, and
Escape remain their existing direct bindings. No unmodified printable
filename byte may become a default action binding.

Add `ed.git.actions`, implemented by
`yew_fuss_cmd_actions(CmdCtx *cx)` in `src/mod/git/fussmode.c` and declared
in `src/mod/git/fussmode.h`. `C-S-/` invokes it in F mode. `C-?` and `C-_`
may be aliases only for terminals that cannot report the modern
Ctrl+Shift+/ event; the displayed primary spelling remains `C-S-/`.
The command opens the shared modal list picker titled `FUSS actions`, with
one deterministic row per effective F-mode Git/group action:

| Picker column | Source | Rule |
|---|---|---|
| key | `yew_key_format_seq` | show the effective sequence, including `A-g` |
| action | `CmdDesc.help` | explain the command in user language |
| payload | registered command name | Enter invokes the selected real command |

Build the list from `ed->keys` and `yew_keystack_lookup`, not from a second
hand-maintained verb table: a higher configured binding suppresses the
shadowed default, and the palette never advertises a key that dispatch will
not receive. Exclude `ed.git.actions` itself to prevent recursive palette
entry. Use the existing FUSS-owned picker allocation/clear path and the
shared picker modal semantics; `/` begins filtering, Enter runs the chosen
command, and Escape closes without input leaking into the document.

**Why.** `yew_fuss_key()` consumes bare printable bytes before dispatch to
preserve type-to-jump. Alt-modified action keys therefore reach the normal
dispatcher without weakening the filename-search rule. The picker provides a
keyboard-first discoverability path for rarely used Git operations and makes
the directory-to-tab-group action visible instead of requiring users to know
an invisible chord.

**Coverage.** Update `tests/unit/test_fussjump.c` to prove every default
action has its Alt sequence, its unmodified prior byte has no action binding
and starts type-to-jump, `C-S-/` resolves to `ed.git.actions`, and effective
configured bindings are listed once. Update `tests/script/group_from_dir.c`
to drive the real `A-g` path on a selected directory and assert the produced
group. Add modern-keyboard PTY coverage in `tests/pty/registry.c` and a
golden for opening/filtering/accepting the action picker, plus the existing
FUSS leave/discard paths now using Alt. The fixture must emit actual
Ctrl+Shift+/ CSI-u input, not a synthetic printable `?`. Run the applicable
full/FUSS-only and `MODULES=""` smoke surfaces so `src/mod/git/shim.c` keeps
the stripped-module hard error intact.

**Pitfalls.** Do not leave `C-g` compatibility rows that make the help text
ambiguous; do not put a hand-rolled search buffer in the action list; do not
invoke a stale picker payload after re-filtering; and do not let an Alt key
arm or clear type-to-jump as though it were filename text.

### 2. Closeout gates — one per audit front

Fifteen files, `.docs/audits/closeout-F01-unicode.md` …
`closeout-F15-ci.md`. Each front's closeout carries **every numbered DoD
item from every sprint file in that front's scope**, so campaign coverage
is complete by construction. The mapping is fixed here:

| Closeout | Sprint DoD items it must carry |
|---|---|
| F01 unicode | s02 |
| F02 terminal | s03, s04, s05, s06 |
| F03 text | s07, s08, s09, s10, s11, s12 |
| F04 modal | s13, s14, s15, s16, s17 |
| F05 exec | s18, s19 |
| F06 regex | s20, s21 |
| F07 ui | s22, s23, s24, s25, s26, s27 |
| F08 fletch | s28, s29, s30, s31, s32, s33, s34, s36 |
| F09 recorder | s35, s38 |
| F10 syntax | s39, s40, s41, s42 |
| F11 lsp | s43, s44, s45, s46, s47 |
| F12 ai | s48, s49, s50 |
| F13 git | s51, s52, s53 |
| F14 plugins | s54, s55 |
| F15 ci | s00, s01, s37, s56, s57 |

Template, verbatim:

```
# Closeout: F<NN> — <front name>

Date:            YYYY-MM-DD
Baseline commit: <hash>
Reviewer:        <who signed this — NOT someone who did the work>
Sprints covered: <list>

## DoD items (every numbered item from every sprint above)

- [x] s08.2 Atomic path issues write/fsync/rename/fsync(dir) in order
      — EVIDENCE: tests/unit/test_save_paths.c::save_order, CI run 4471
- [ ] s08.4 Torture sweep green at 10x
      — VERDICT: blocking — YEW-F-017 open

## Open audit findings against this front

<count by severity, IDs listed, or "none">

## Deferrals recorded against this front

<YEW-F-### -> deferrals.md entry, or "none">

## Verdict

READY | NOT READY — <the shortest honest sentence why>
```

Rules, all machine-checked by `scripts/check-closeouts.sh`:

1. Every numbered DoD item from every mapped sprint file appears exactly
   once. Missing items are the whole failure mode of a closeout document,
   so the checker extracts them from the sprint files themselves rather
   than trusting the closeout to be complete.
2. A checked box **must** carry an `EVIDENCE:` link — a test name, a CI
   run id, a golden path, or a doc section. **A bare `[x]` is invalid.**
3. Every unchecked box carries a `VERDICT:` line, `blocking` or
   `waived: <justification>`.
4. **Waivers on audit Criticals and Highs are forbidden** — those are
   always blocking. The checker greps the cited IDs' severities.
5. The file ends with `READY` or `NOT READY` and one sentence. "We are not
   ready to close F11" is a legitimate mid-sprint state and is written
   plainly.
6. The reviewer named is not an author of any commit in the front's
   remediation branches (checked against `git log`).

### 3. The deferral law — no silent deferrals

Any finding, XFAIL, or known limitation that will not be fixed for 1.0
requires **both** of the following, or it does not get deferred:

1. **A ledger justification.** The `findings.md` body under that ID gains
   a `Deferred:` paragraph stating what does not work, what the user will
   observe, what the workaround is, and what would have to change to fix
   it. One sentence is not enough; "low priority" is not a justification.
2. **A user-visible release-notes entry.** An entry in
   `.docs/audits/deferrals.md`, which Sprint 60 consumes **mechanically**
   into `CHANGELOG.md`'s `[1.0.0]` section under `### Known limitations`.
   The entry is written for a user, not a maintainer: it names the
   behavior, not the code.

```
## YEW-F-088 — fuzzy matching folds case for ASCII only
Filed: F07 · Severity: M · Status: deferred → post-1.0

Deferred: a filename containing non-ASCII characters is findable in the
file picker only with matching case, because the fuzzy scorer's fold is a
32-bit ASCII fold (s26 §2, pinned there deliberately). Full simple folding
would require the case tables the regex engine gained in s20 and a
re-pinning of the fuss-parity corpus, which changes every ranking golden.
Workaround: type the exact case, or use `:e` with tab completion, which
compares bytewise.

Release-notes entry:
  The fuzzy finder matches case-insensitively for ASCII characters only.
  Non-ASCII filenames must be typed with matching case.
```

Only Medium and Low findings may be deferred. `scripts/check-deferrals.sh`
enforces, and each rule has a negative fixture:

- every `deferred` or `wontfix` row has a `Deferred:` paragraph of ≥ 2
  sentences **and** a `Release-notes entry:` block;
- every `deferrals.md` entry maps to a ledger row with one of those two
  statuses;
- **no C or H row carries either status**;
- every release-notes entry's text also appears in **either** the man
  page's `LIMITATIONS` section **or** `docs/manual/15-limits.md` — a
  limitation the user cannot find while using the editor is still silent
  even if it is in the changelog;
- every front's closeout lists its deferrals (template §Deferrals).

**Pinned:** permanent 1.0 non-goals that earlier sprints already
documented (no normalization, no bidi, no lookaround, no snippets, no
markdown rendering, no interactive terminal buffer, no filesystem
watching, `&` literal in replace, `*` aliasing `+`) are **not** deferrals
— they were decisions, not debts, and they belong in §4's `LIMITATIONS`
and §7's chapter 15 without a ledger ID. Do not manufacture findings for
them; do not let them slip out of the docs either.

### 4. Man pages — hand-written groff

`docs/man/yew.1`, `docs/man/yew-fl.1`, `docs/man/yew-pkg.1`. Written
by hand in `man` macros. **No doc generator, no pandoc, no scdoc, no
ronn** — invariant 7 applies to documentation tooling, and a generator is
a build dependency that would have to be present on every packager's
machine (Sprint 60 builds four distribution artifacts; each one would
carry it).

Install target: `$(DESTDIR)$(PREFIX)/share/man/man1/`, wired into
`make install` beside the binary and the `yew` symlink.

#### 4.1 `yew.1` — section outline, pinned

| Section | Content |
|---|---|
| `NAME` | `yew, yew \- a modal terminal editor and its language` |
| `SYNOPSIS` | `yew [OPTIONS] [FILE...]` · `yew --batch SCRIPT [FILE...] [-- ARG...]` · `yew tutor [CHAPTER]` · `yew fl`, `yew pkg`, `yew plug`, `yew syn` cross-referenced |
| `DESCRIPTION` | Three paragraphs, no manifesto: what it is (modal, keyboard-first, single-threaded, no dependencies beyond libc), what Fletch is and that recordings are source, and what it is not (not a vim clone, not an IDE, no plugins outside Fletch) |
| `MODES` | The seven modes as a table: L, W, B (unit) and H, I, E, F (action), each with its statusline chip, what Esc does from it, and which unit engine it drives |
| `KEYS` | Default bindings grouped by mode, generated **by hand** from `runtime/init.fl` and cross-checked mechanically (§4.5). Includes the reserved-and-unbound keys with their messages |
| `OPTIONS` | Every command-line flag: `--batch`, `--test`, `--clean`, `--config`, `--trust-workspace`, `--grant`, `--quiet`, `--theme`, `--version`, `-dumpversion`, `--help`. Deferred flags do not appear — at 1.0 there are none |
| `E-MODE COMMANDS` | Every `:` command with its range policy and argument spec, from the s18 registry metadata |
| `REGULAR EXPRESSIONS` | The full syntax the s20 engine accepts, the four Unicode class definitions, the anchor semantics (`^`/`$` are line anchors unconditionally; `$` matches before a CRLF's `\r`), leftmost-first, ICASE's simple-folding-only rule, and the permanent non-goals with the reason (linear time by design) |
| `CONFIGURATION` | `init.fl` load order, `.yew.fl` and the trust model, `set()`/`bind()`/`on()`, where the option table lives |
| `FILES` | `$XDG_CONFIG_HOME/yew/init.fl`, `themes/`, `plugins/`, `macros/`; `$XDG_STATE_HOME/yew/{log,journal,backup,workspaces,history,trust.fl,tutor.fl}`; `$XDG_CACHE_HOME/yew/syn/`; `<workspace>/.yew.fl` |
| `ENVIRONMENT` | Every `YEW_*` variable the tree reads, in one table with defaults, plus `XDG_*`, `NO_COLOR`, `TERM`, `COLORTERM`, `SHELL`, `PAGER`, `SOURCE_DATE_EPOCH` |
| `EXIT STATUS` | The ledger verbatim: 0 success · 1 error · 2 `--batch` script failure · 3 I/O error · 4 internal bug |
| `LIMITATIONS` | Every permanent non-goal and every §3 deferral, in user language: no Unicode normalization (and why — an editor that normalizes silently rewrites bytes you did not touch), no bidi (RTL renders in logical order), no lookaround/backreferences, no snippet expansion, no markdown rendering in panels, no interactive terminal buffer, no filesystem watching, ASCII-only fuzzy folding, `&` is literal in replacements, `*` aliases `+` for the clipboard, OSC 52 is write-only |
| `SEE ALSO` | `yew-fl(1)`, `yew-pkg(1)`, `git(1)`, `less(1)` |
| `BUGS` | Where the log lives, how to produce a clean repro (`yew --clean`), what to attach |

#### 4.2 `yew-fl.1`

`NAME` · `SYNOPSIS` (the seven forms from s32) · `DESCRIPTION` · `OPTIONS`
(`-e`, `-c`, `--dump-ast`, `--dump-bytecode`, `--list-natives`) ·
`THE REPL` (prompts, `:` commands, `Ctrl-C`/`Ctrl-D`/`Ctrl-L`, result
printing rules) · `LANGUAGE` (a one-screen summary pointing at the spec
and the book: types, falsy set, the motion layer, `edit{}`, error kinds) ·
`MODULES` (the closed seven: `str list map math fmt io re`) ·
`CAPABILITIES` (`fs.read fs.write shell net`, granted per origin) ·
`BATCH MODE` (the stdio contract table and the interactive-refusal table
from `docs/batch.md`) · `EXIT STATUS` (note that `yew fl` never returns 2)
· `ENVIRONMENT` · `SEE ALSO` · `EXAMPLES` — five, each runnable.

#### 4.3 `yew-pkg.1`

`NAME` · `SYNOPSIS` · `DESCRIPTION` (git-based, Fletch-only, no registry,
no dependency resolution — plugins have no dependencies at 1.0) ·
`COMMANDS` (`install`, `update`, `remove`, `list`, `lock`, `verify`) ·
`THE LOCK FILE` (format, what is pinned, how to reproduce an install) ·
`CAPABILITIES AND TRUST` (the four capabilities, the prompt, the trust db,
`--grant` for headless) · `THE HONEST PARAGRAPH` — reproduced from
`plug.h` §7: a plugin runs in the same process and the same VM with no
memory or resource isolation; capability gates cover the file, shell,
network, and clipboard surfaces of the Fletch API and nothing else; this
is not a sandbox. The man page says it in the user's words, because a user
deciding whether to install a plugin needs it more than a maintainer does.
· `FILES` · `EXIT STATUS` · `SEE ALSO`.

#### 4.4 `yew-regex` — discharging Sprint 20's message

Sprint 20 pinned the unsupported-construct error text ending
`see man yew-regex`. That string is a promise. It is kept **without a
fourth hand-maintained page**: the `REGULAR EXPRESSIONS` section lives in
`yew.1`, and `docs/man/yew-regex.7` is a one-line redirect page

```groff
.so man1/yew.1
```

installed to `$(PREFIX)/share/man/man7/`. `man yew-regex` therefore
opens `yew(1)`. **Pitfall:** `.so` is resolved relative to the man
hierarchy root, not the current file — `man1/yew.1`, never
`../man1/yew.1`; and mandoc (which the macOS target ships) honors it
only when the target is inside the same hierarchy, which is why both pages
install under the same `$(PREFIX)/share/man`.

#### 4.5 `scripts/check-man.sh` — both directions, and the troff traps

1. Every flag in `yew_args_parse` appears in `yew.1`'s `OPTIONS`, and
   every `\-`-prefixed option token in the page exists in `--help`.
2. Every `YEW_*` environment variable read anywhere in `src/` appears in
   an `ENVIRONMENT` section of one of the three pages.
3. Every command in the registry with a `:` abbreviation appears in
   `E-MODE COMMANDS`; every command documented there exists.
4. Every default binding in `runtime/init.fl` appears in `KEYS` with the
   same mode, and every binding documented in `KEYS` resolves through the
   built keymap. This is what stops the page from drifting the first time
   a key moves.
5. Every `see man <page>` string in `src/` names an installed page.
6. Every entry in `deferrals.md` and every permanent non-goal appears in
   `LIMITATIONS` (the §3 cross-check).
7. `man --warnings -l` clean under **both** groff and mandoc.

**Pitfalls.** `-` versus `\-`: an ASCII hyphen renders as a hyphen and a
`\-` as a minus, and copy-pasting the wrong one from a man page gives the
user a flag that does not parse — the checker normalizes both, then
asserts the page uses `\-`. A line beginning with `.` or `'` in body text
is a macro invocation, which is how a paragraph starting with `.yew.fl`
silently disappears; escape it `\&.`. Tabs inside a `.TS` table are the
delimiter, so a literal tab in an example destroys the table. Document a
flag the driver accepts and ignores and you have written a bug report, not
a doc line — file it.

### 5. `yew tutor` — a guided workspace, not a text file

The tutorial is a real yew workspace: seven chapters, each a **buffer**
in one tab group, edited by the user with the real editor. Task completion
is decided by **Fletch hooks that read actual buffer state** — not by
counting keystrokes, not by a scripted animation. If the user reaches the
required state by a different route, the task completes; that is the
point.

```
runtime/tutor/
  tutor.fl              driver: workspace setup, task engine, progress
  ch01-arrows.fl … ch07-fuss.fl     chapter definitions
  text/ch01.txt … ch07.txt          starting buffer contents
```

CLI: `yew tutor [CHAPTER] [--list] [--reset] [--check]`.
`CHAPTER` is `1`–`7` or a name. `--list` prints chapters with completion
state and exits 0. `--reset` clears progress. `--check` is the CI entry
point (§6): runs headlessly and exits non-zero if any task in the given
chapter fails its checker against a scripted key replay.

#### 5.1 The workspace

`yew tutor` opens a workspace rooted at
`$XDG_STATE_HOME/yew/tutor/`, seeded on first run by copying
`runtime/tutor/text/` into it. Every chapter is a tab; all seven tabs are
members of one `TabGroup` labelled `tutor` (s24 — the tutorial dogfoods
groups by being one). The chapter the user is on is the group's
`last_active_member`.

**Why a copy and not the installed files:** the user edits these buffers.
The installed `runtime/` tree is read-only on every packaged install
(Sprint 60 ships to `$(PREFIX)/share/yew/runtime`), and an editor that
asks you to type into a file it cannot save is a bad first impression. The
copy also means `--reset` is `rm -rf` plus a re-seed, and a failed tutor
save is a real `YEW_SAVE_PERM` the user can see and learn from.

#### 5.2 The task engine

```fletch
# runtime/tutor/tutor.fl — the whole engine, ~120 lines
#
# task(id, prompt, keys, check)
#   id     stable string, "ch01.t3"; never renumbered (progress keys on it)
#   prompt one line shown in the buffer's task list
#   keys   the CANONICAL key sequence that satisfies this task, in s13
#          spelling. This is what makes the tutorial un-rottable: CI
#          replays exactly these keys (§6) and the task must complete.
#   check  fn(buf, win) -> bool.  Reads real state. Never raises.
task("ch02.t4",
     "Delete the word under the cursor with W-mode del",
     "w <esc> d",
     fn(b, w) b.line(7) == "the fox jumped")
```

Rules, each with a reason:

- **A checker returns a bool and never raises.** It runs inside a
  `buf.change` hook; s34 contains a throwing hook by disabling the
  registration after five errors, which would silently switch the tutorial
  off. A checker that needs to fail reports `false`.
- **Checkers read, never write.** The engine — not the checker — performs
  the one write a completion causes (§5.3), inside one `edit{}`.
- **Checkers are pure functions of buffer and window state.** No clock, no
  counters, no "did the user press the key" — that is exactly what
  keystroke-counting tutorials do and why they lie when a user gets there
  another way.
- Hooks are registered under `FL_ORIGIN_BUILTIN` on `buf.change`,
  `cursor.move`, `mode.enter`, and `win.focus` — the four events that can
  change any state a checker reads. `buf.change` is coalesced per input
  burst (s34), so a 4 000-key paste evaluates the chapter's checkers once.
- Evaluation is bounded: only the **active chapter's** incomplete tasks are
  checked, in order, and evaluation stops at the first incomplete task. A
  chapter is a sequence; checking task 9 while task 2 is open would let a
  user skip forward invisibly.

#### 5.3 What completion does

The engine rewrites its own task line in the buffer from `[ ]` to `[✓]`,
inside one `edit{}` transaction tagged `YEW_TXN_MACRO`, and appends the
next task's prompt if the chapter reveals tasks progressively. Three
consequences worth stating:

- The tutorial is a Fletch program editing a buffer — the thing chapter 6
  teaches, demonstrated by the tutorial itself.
- `u` undoes a completion's *text*, and the engine re-completes on the next
  hook fire because the checker is still true. Progress is state, not
  history.
- No sign column, no gutter, no new `SignKind` (s43's kinds stay three) —
  the tutor owns the buffer, so the buffer is the display.

Progress persists to `$XDG_STATE_HOME/yew/tutor.fl`, a pure Fletch
literal in the s25/s36 canonical form, written atomically:

```fletch
{
  schema: 1,
  version: "1.0.0",
  chapters: {
    ch01: { done: ["ch01.t1", "ch01.t2"], complete: false, },
  },
}
```

**Pitfall:** progress keys on **task id**, never on index. Inserting a
task into chapter 2 must not silently mark a later task complete for
every existing user; that is why ids are stable strings and why the DoD
counts them.

#### 5.4 The seven chapters

| # | Chapter | Teaches | Representative task checkers |
|---|---|---|---|
| 1 | `arrows` — arrows and the mode model | The seven modes, the mode chip, Esc always returns to L, L-mode arrow motion, `i` to insert, `:w` to save | cursor is on line 12 col 1; `ed.mode` is `I`; `ed.mode` is `L` after Esc; the file on disk equals the buffer (`t`-style disk read, proving the save happened) |
| 2 | `units` — W, B, H and multi-cursor | Unit modes as *layers*, not commands; `>`/`<`/`^`/`v` meaning different things per mode; `a>` alt variants; H mode borrowing the previous unit; selection → multi-cursor lift; one transaction | line 7 equals the expected text after a W-mode delete; `win.cursors().len() == 4` after a lift; buffer equals the expected text after a simultaneous edit; `b.undo_depth() == 1` proving the multi-cursor edit was one step |
| 3 | `execute` — E mode and the shell | `:` prompt, history, completion, ranges, `:!cmd`, piping a region through a filter, job buffers, cancelling | the buffer's lines are sorted (region through `sort`); a job buffer named `!wc -l` exists; the filter's undo depth is exactly 1 |
| 4 | `search` — search and replace | `/` and `?`, `n`/`N` relative to direction, smartcase, `:s` with ranges, confirm-replace, the jumplist | cursor is at the 3rd match; buffer equals the post-replace text; `b.undo_depth()` unchanged by `n`; jumplist position restored by a `back` |
| 5 | `workspace` — panes, tabs, groups | Splits and focus movement, tabs and stable ids, making a group, walking it, quitting and resuming | two panes exist with the expected rects; a group named `notes` has 3 members; after `:q` and reopen (CI drives this as two pty runs) the cursor is byte-identical |
| 6 | `fletch` — macros and Fletch basics | `q a` … `q`, `"ap` to read the recording as source, `@a`, editing a macro as a scratch buffer, `set()`/`bind()` in `init.fl` | register `a` is non-empty and parses (`fl.parse` returns no diagnostics); the buffer shows the recorded source; the replayed edit matches the recorded edit byte for byte |
| 7 | `fuss` — FUSS mode and git | F mode's dirty-first tree, the status glyphs, depth-aware sibling nav, type-to-jump, stage/unstage/commit, the footer recolor | the fixture repo's index contains the staged path (checked by a `git status --porcelain=v2` read through the tutor's own git layer); mode is `F`; the tree's selected node is the expected path |

Chapter 7 needs a repository. `yew tutor` creates one under the tutor
workspace on first entry to chapter 7 using the same verb table the editor
uses (`git init`, two commits, then dirty three files). **If `git` is
absent, chapter 7 announces the requirement and is marked `skipped`, not
`complete`** — invariant 3's spirit applied to a tutorial: never pretend a
lesson happened.

#### 5.5 What the tutor may not do

- No new commands, no new editor surface: every action a chapter asks for
  is a shipped binding. A tutorial that needs a special key is teaching a
  different program.
- No modal dialogs, no nag, no telemetry, no network.
- It must be **exitable at any moment** by `:q` with normal
  dirty-buffer semantics, and re-entrant at the same task.
- It must run under `--clean`. A user whose config broke the editor is
  exactly the user who needs the tutorial, so the tutor's chapters may not
  depend on any user binding — only on `runtime/init.fl`'s defaults.

### 6. The tutor in CI — it cannot rot

`make test-tutor` builds and runs `tests/pty/tutor_runner`, a set of
`PtyCase`s registered in `tests/pty/registry.c` beside the existing ones,
using the Sprint 6 harness **unchanged**.

For each chapter, one case:

```c
static void tutor_ch02(PtyCtx *c) {
    ptc_spawn(c, "build/yew", "tutor", "2", "--clean", NULL);
    ptc_settle(c, 50);
    /* Keys come from the chapter's own task records, not from this file:
     * the runner reads runtime/tutor/ch02-units.fl through `yew fl -e`
     * and emits each task's `keys` in order.  A chapter that renames a
     * key updates the test in the same commit, by construction.       */
    for (u32 i = 0; i < c->ntasks; i++) { ptc_keys(c, c->task_keys[i]);
                                          ptc_settle(c, 30); }
    ptc_snapshot(c, "tutor_ch02_done.golden");
    ptc_keys(c, "colon q enter");
    ptc_expect_exit(c, 0);
}
```

The three properties that make this a rot guard rather than a screenshot
test:

1. **The keys come from the chapter**, read out of the `.fl` at test time.
   A binding that moves breaks the chapter and the test together, in one
   commit, which is the only way a tutorial stays true.
2. **The assertion is the task engine's own verdict**, not just the
   golden: after the replay, the case asserts that `tutor.fl` records
   every task in the chapter as done. A golden could match while the
   lesson silently failed; the progress file cannot.
3. **The goldens are byte-compared** like every other pty golden and ride
   the determinism lane's double run, so a tutorial that renders
   differently on two machines fails as loudly as the editor would.

Additional cases: `tutor_list` (`--list` output is byte-stable and exits
0), `tutor_reset` (progress cleared, second run identical to first),
`tutor_resume` (chapter 5's quit/reopen half, two spawns against one
workspace, resume byte-identical), `tutor_clean` (every chapter under
`--clean` with a hostile `init.fl` present in `XDG_CONFIG_HOME`), and
`tutor_nogit` (chapter 7 with a `PATH` containing no `git`: announces,
marks skipped, exits 0).

CI wires `make test-tutor` into `make test` and adds it to the `pty` and
`determinism` lanes. **A tutor case that cannot find `git` on the `lsp`-
style dedicated lane is a FAILURE, not a skip**, matching s47's rule; the
local `make test` prints a loud SKIP.

### 7. The user manual — `docs/manual/`

One chapter per campaign area, written against shipped behavior, in
Markdown, with **every example runnable**.

| File | Covers | Campaign |
|---|---|---|
| `01-install.md` | build from source, `MODULES`, packages, first run, `--clean`, where things live | 00 |
| `02-modes.md` | the mode model, the chip, Esc, the unit-engine idea | 03 |
| `03-files.md` | opening, saving, atomic saves, the crash journal, EOL/BOM/binary, what "no data loss" means in practice | 02 |
| `04-motion.md` | L/W/B units, alt variants, H mode, selections, multi-cursor | 03 |
| `05-search.md` | search, smartcase, replace, confirm, jumplist, the regex syntax with the non-goals | 04 |
| `06-execute.md` | E mode, ranges, `:!`, filters, jobs | 04 |
| `07-workspace.md` | panes, tabs, groups, the workspace state file, resume | 05 |
| `08-fletch.md` | configuring with `init.fl`, `set`/`bind`/`on`, options, the trust model | 06, 07 |
| `09-macros.md` | recording, reading, editing, the library, `--batch` | 07 |
| `10-syntax.md` | languages, themes, writing a definition, `yew syn` | 08 |
| `11-completion.md` | the three tiers, ghost text and its provenance, the menu, LSP setup per language | 09 |
| `12-ai.md` | off by default, how to turn it on, exactly what leaves the machine, redaction | 10 |
| `13-git.md` | FUSS mode, glyphs, verbs, gutter, blame, diff | 11 |
| `14-plugins.md` | installing, capabilities, trust, writing one, the not-a-sandbox paragraph | 12 |
| `15-limits.md` | every permanent non-goal and every §3 deferral, in user language, with workarounds | — |

#### 7.1 Every example is executable

Fenced blocks carry a language tag that says how they are checked:

| Tag | Meaning | Checked by |
|---|---|---|
| ```` ```batch ```` | a `.fl` script run under `yew --batch` against a fixture, with the expected stdout in a following ```` ```output ```` block | `scripts/check-manual.sh` — runs it, byte-compares |
| ```` ```sh ```` | a shell transcript; the command runs, exit status and stdout compared | same |
| ```` ```keys ```` | a key sequence; replayed under the pty harness against the named fixture, final grid compared to a golden | `tests/pty/manual_*.golden` |
| ```` ```fletch ```` | Fletch source; must at minimum **parse** (`yew fl --dump-ast`) with zero diagnostics | same |
| ```` ```text ```` | prose sample, not executed | — |

`scripts/check-manual.sh` extracts every tagged block, runs it in a
sandbox with pinned `XDG_*` (the s37 isolation recipe), and fails on any
mismatch. It is wired into `make test` and CI.

**Pitfall:** an example that "works" because it depends on the author's
config is the classic doc rot. Every extracted example runs with
`--clean`, in a fresh temp `HOME`, with the fixture copied in — the same
isolation a script test gets, for the same reason.

#### 7.2 Writing rules

- Against shipped behavior, verified by running it. If a sentence
  describes what the code *should* do, delete the sentence or file a
  finding.
- No aspiration and no roadmap. The manual describes 1.0.
- Every claim of a guarantee names the mechanism (atomic save + journal;
  transaction rollback; generation counters), because a guarantee without
  a mechanism is the Critical severity's definition.
- Cross-references are relative links; §7.3 checks them.

#### 7.3 `scripts/check-docs-sync.sh`

Fails when documentation and code disagree, in the places where the
disagreement is mechanically visible:

1. Every option in the s36 table appears in a manual chapter with its
   shipped default, and the documented default equals the C constant.
2. Every `YEW_*` env var read in `src/` appears in the manual or a man
   page.
3. Every default binding appears in `02-modes.md`, `04-motion.md`, or the
   man page's `KEYS`, with the correct mode.
4. Every relative link in `docs/` and `README.md` resolves (Sprint 60's
   link audit extends this to the frozen tree).
5. Every `YEW-F-###` cited in docs exists in the ledger.
6. A per-finding hook: a remediation PR touching a file listed in
   `docs/manual/.owners` (a two-column map from source path to manual
   chapter) must also touch that chapter, or state `docs: no user-visible
   change` in the PR body.

### 8. Fletch book chapter 1 — `docs/fletch-book/01-arrows-to-programs.md`

One continuous worked example that carries the reader from keystrokes to a
function. The premise of the language — *recordings are source* — is not
argued; it is demonstrated by doing it once, slowly, with every
intermediate artifact printed in full.

**The example.** A file of bare field names:

```text
id
name
created_at
```

The goal: each line becomes `"id",` — a quoted, comma-terminated string,
which is a real thing people do to build a list literal.

**§1 — the keystrokes.** In L mode, with the cursor on line 1: `l` to be
explicit about the unit, `^` to line home, `i` then `"` then Esc, `$`-style
line end via `l` mode's `end`, `i` then `",` then Esc, then `v` to the next
line. The chapter prints the exact key sequence in s13 spelling and the
buffer after each key, so the reader can follow along without guessing.

**§2 — record it.** `q a`, the same keystrokes, `q`. The statusline shows
`●REC a` throughout (the s38 indicator). Then `"ap` pastes register `a`
into a scratch buffer — and the reader sees **source code**:

```fletch
# yew-macro: 1
# recorded-with: yew 1.0.0
# keymap: default
macro rec_a = @[ l ^ i"\"" esc end i"\"," esc v ]
```

**§3 — read it.** The chapter annotates every token against the man page's
motion vocabulary: why `l` is there at all (mode-context annotation — the
emitter prepends the unit word so the macro means the same thing replayed
from W mode), why the inserts are two separate `i"…"` and not one (an
intervening motion breaks insert concatenation), why the escapes inside
`i"\""` look the way they do, and why there is no key in the source
anywhere — only commands. This is the paragraph that earns the whole
language: *your macro survives rebinding your keyboard.*

**§4 — edit it.** Three edits, each with the before/after and the reason:

1. Rename `rec_a` to `quote_line` — a macro is a `let` of a zero-argument
   function, so the name is just a name.
2. Delete the trailing `v`. The recording moved to the next line because
   the reader pressed `v`; the macro should not, because the caller will
   decide. This is the first moment the reader edits *intent* rather than
   replaying *history*, which no keystroke-register macro system permits.
3. Run it with a count: `3@a` applies it three times — and the chapter
   stops to note that this is **three** undo steps, one per run, because
   that is what `3@a` means (s38's pinned rule), and shows `u` walking
   back through them.

**§5 — generalize it.** The motion block is fixed at `"` and `",`. Turn it
into a function with parameters, and let it act on a whole selection at
once:

```fletch
# Wrap every line of the selection in `open`/`close`.
# One edit{} = one undo step, no matter how many lines.
fn wrap_lines(open, close) {
    edit {
        for c in win.cursors() {         # a selection lifted to cursors
            let s = c.span()             # the line the cursor owns
            s.prepend(open)
            s.append(close)
        }
    }
}

bind("H", "q", fn() wrap_lines("\"", "\","))
```

The chapter then walks four things the reader now has that the recording
did not: parameters, a loop over a real value (`win.cursors()` is a list),
an explicit transaction (one `u`), and a binding — and closes by pointing
out that `@[ … ]` and the structured layer compose freely, so the next
macro can call `wrap_lines` directly.

**§6 — run it headlessly.** The same function in a file, run over three
files without a terminal:

```fletch
# tools/quote.fl
for path in args {
    let b = buf.open(path)
    edit { for l in b.lines() { l.prepend("\""); l.append("\",") } }
    b.save()
}
```

```sh
yew --batch tools/quote.fl -- fields.txt more.txt
```

Every code block in the chapter is tagged per §7.1 and executed by
`check-manual.sh`; the recorded source in §2 is produced by **actually
running the recorder** in the pty harness and pasting the result, so the
chapter cannot drift from the emitter.

### 9. Defer

- Packaging (AUR, brew, RPM, musl tarball), checksums and signing,
  reproducible release builds, README, CHANGELOG, the `--version`
  contract change, the docs freeze and link audit, and the tag →
  **Sprint 60**. This sprint's `deferrals.md` is Sprint 60's input.
- Fletch book chapters 2+ (the structured layer in depth, the editor API,
  writing a plugin, the syntax-definition tutorial) → **post-1.0**. One
  chapter that is genuinely finished beats five that are outlines.
- Tutor chapters beyond the seven (LSP, AI, writing a syntax definition)
  → **post-1.0**; the chapter list is data in `runtime/tutor/`, so adding
  one is content, not code.
- Translated documentation → post-1.0, and only with a mechanism for
  keeping translations from rotting; a stale translated man page is worse
  than none.
- HTML or man-page rendering of `docs/manual/` → post-1.0. Markdown in the
  repo and on the forge is the 1.0 answer; a renderer is a build
  dependency (invariant 7).
- `--batch-strict` — still reserved, still not built (s37 §9 named this
  sprint's docs pass as where it would be decided): **not for 1.0**,
  recorded here so it is a decision rather than an oversight.

## Testing Strategy

- **Ledger and closeout machinery** (`scripts/check-closeouts.sh`,
  `check-priority.sh`, `check-deferrals.sh`, `check-docs-sync.sh`,
  `check-man.sh`, `check-manual.sh`): each gets a negative-fixture matrix
  proving it fails on every rule it claims — bare `[x]`, missing
  `VERDICT:`, a waived Critical, a DoD item absent from a closeout, an
  M/L closure predating the last C/H, a deferral without a release-notes
  entry, a deferral absent from `LIMITATIONS`, a man page missing a flag,
  a manual example whose output drifted, an option default disagreeing
  with its C constant.
- **The reproducers are the tests.** `make test-audit` runs every
  `tests/audit/` case; a `fixed` ledger row whose reproducer still fails,
  or a `closed` row whose XFAIL marker survives, fails CI.
- **Full regression before any closeout is signed:** unit, script, pty,
  roundtrip, fletch conformance, torture, audit, tutor, and the perf lane
  green on all four targets. **A closeout signed against a red tree is
  void**, and `check-closeouts.sh` records the CI run id it was signed
  against so this is auditable after the fact.
- **Re-audit spot check** (fix-verification theater guard): after C/H
  remediation completes, a fresh-context reviewer who fixed nothing
  re-verifies a deterministic 10 % sample (minimum 5) drawn by
  `scripts/audit-sample.sh` from the sorted deduped ID list using a seed
  recorded in `audit-00.md`. Re-verification = build the fix commit, run
  the reproducer, read the diff against the root-cause hypothesis. A
  failure reopens the finding **and doubles the sample**; two failures →
  100 % re-verification of that front. `audit-sample.sh` run twice must
  produce an identical sample.
- **Man pages:** `man --warnings -l` clean under groff and mandoc for all
  four installed pages; `man yew-regex` resolves to `yew(1)` from
  a staged install (`MANPATH` pointed at `DESTDIR`), on Linux and macOS.
- **Tutor:** the §6 case set; plus a mutation drill — rename one binding
  in `runtime/init.fl` without touching the chapter and confirm
  `make test-tutor` fails; restore and confirm green. This drill is
  written down in `runtime/tutor/README`, exactly as s06 §DoD 5 wrote down
  the closed-set drill.
- **Tutor determinism:** the tutor goldens ride the determinism lane's
  double run; `tutor.fl` after a full replay is byte-identical across two
  runs (no timestamps, no counters that are not task ids).
- **Manual:** `check-manual.sh` extracts and runs every tagged block; two
  runs byte-identical; a seeded wrong `output` block must fail.
- **Book:** chapter 1's §2 recording block is regenerated by the pty
  harness and byte-compared against the file — a drift in the emitter
  fails the docs lane, not a reader.

## Definition of Done

1. **Zero open Critical and zero open High findings** in the deduped
   ledger. Every closure satisfies all five per-finding DoD items (§1.1),
   spot-verified by the §Testing re-audit sample with the escalation rule
   applied and the results recorded in `audit-00.md`.
2. Priority law held: `check-priority.sh` green — no M/L-only remediation
   merge predates the last C/H closure, excepting commits that name a free
   side effect.
3. `burndown.md` runs from `audit-00.md`'s deduped totals to zero-C/H, one
   row per merge; final row totals equal the ledger's strikethrough counts.
4. Every Critical fix's PR contains its cluster-hunt result — new findings
   or the explicit `clean` line. 100 %, checked in review.
5. XFAIL ledger: every ID carries exactly one of the four verdicts; zero
   Critical- and zero High-tagged XFAILs remain; the total is ≤ the N
   recorded as a checkbox in `closeout-F15-ci.md`; every `wontfix` has a
   written justification.
6. **All fifteen closeout files exist, `check-closeouts.sh` is green, and
   all fifteen end in READY** — or this sprint's report states NOT READY
   and lists exactly what blocks. Every `[x]` carries an evidence link; no
   Critical or High is waived anywhere.
7. Deferral law enforced: `check-deferrals.sh` green; every `deferred` and
   `wontfix` row has a ≥ 2-sentence justification and a `deferrals.md`
   release-notes entry; every entry's text appears in `LIMITATIONS` or
   `docs/manual/15-limits.md`; no C or H is deferred.
8. Man pages: `yew.1`, `yew-fl.1`, `yew-pkg.1` complete per §4.1–4.3's
   pinned outlines, plus the `yew-regex.7` redirect; `check-man.sh`
   green on all seven checks in both directions; `man --warnings` clean
   under groff **and** mandoc; `make install` places all four pages and a
   staged-install `man yew-regex` opens `yew(1)`.
9. `yew tutor` ships: seven chapters, ≥ 45 tasks total with stable ids,
   every checker a pure read returning bool (`grep` proves no checker
   writes and no checker raises), progress persisted as a valid pure
   literal that round-trips through `fl_data_read`/`fl_data_write`.
10. `make test-tutor` is a `make test` dependency and a CI lane; all seven
    chapter cases plus `tutor_list`, `tutor_reset`, `tutor_resume`,
    `tutor_clean`, and `tutor_nogit` are green; goldens byte-identical
    across the determinism lane's double run; **the rot drill fires** —
    renaming one default binding breaks `make test-tutor`.
11. The tutor runs under `--clean` with a hostile user config present, and
    creates nothing outside `$XDG_STATE_HOME/yew/tutor/`
    (`git status --porcelain` clean in a fixture repo after a full run).
12. `docs/manual/` holds all fifteen chapters; `check-manual.sh` extracts
    and executes every tagged block with zero mismatches; two runs
    byte-identical; a seeded wrong expected-output fails.
13. `docs/fletch-book/01-arrows-to-programs.md` complete with all six
    sections and every worked artifact printed in full; §2's recording is
    machine-generated by the pty harness and byte-compares against the
    committed text; every code block passes its tag's check.
14. `check-docs-sync.sh` green on all six checks; every option default in
    the manual equals its C constant; every `YEW_*` variable is documented
    somewhere.
15. Full regression green on all four targets — unit, script, pty,
    roundtrip, fletch conformance, torture, audit, tutor, manual, perf —
    at the commit each closeout was signed against, with the CI run id
    recorded in the closeout.
16. F13's F-mode action-discovery field report has a written audit
    disposition, and—once the priority law permits—the shipped keymap keeps
    bare filename type-to-jump intact, exposes every action behind Alt,
    opens a selected directory as a group with `A-g`, and proves the
    `C-S-/` action picker by unit, script, PTY, stripped-module, sanitizer,
    and full-regression evidence.
