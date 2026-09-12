# Sprint 57.9: Block Motion and Group Picker Field Repair

## Prerequisites

- Sprint 16 — L/W/B/H/I modal motion and the provider-layered B-mode unit.
- Sprint 24 — tab-group identity, lazy member hydration, and the shared
  new/edit group picker.
- Sprints 57.5–57.8 — compact FUSS interaction, printable type-to-jump,
  off-canvas geometry, action chords, and the modern tab strip.
- Binding plans and invariants 1–5, 7, and 9. This repair may not weaken core
  editing, syntax, tab identity, deterministic rendering, or keyboard reach.

## Status

The initial local implementation and native qualification completed
2026-09-11. A same-day follow-on field audit reopened the B-mode portion after
the exact `ch4/days.lu` workflow exposed duplicate intra-line stops, skipped
one-line statements, asymmetric Up/Down order, formatting-dependent EOF
behavior, and confusing half-open edge ownership. The broader generic repair
landed in `bb431836`. Its Wolf/C/fallback unit matrix, real-key PTY, sanitizer,
alignment, fuzz, performance, core-only, and complete native default gates are
green. The final default run qualified the clean combined frontier `7d05f79a`
after the queued Sprint 57.10 tab-jump commits landed.

Local evidence includes 2,441 default unit tests / 73,450,393 assertions, the
complete PTY/script/package/round-trip/syntax/policy/smoke/live-torture matrix,
and 1,971 core-only unit tests / 72,471,915 assertions. Four deterministic
200,000-operation block fuzz seeds pass. The detected-source 100,000-motion
gate completes in 18.089 ms with a 0.043 ms maximum; the comma-structural
1,000-motion gate completes in 26.378 ms with a 0.108 ms maximum. Focused
Darwin arm64 ASan/UBSan and alignment/UBSan runs are clean. Apple clang is the
only native compiler available on this host, so GNU GCC remains a hosted row.

The sprint is not closed: its exact commit-of-record has not been pushed or
qualified by hosted CI. Sprint 58 is paused after F08 until that closeout and
the replacement-baseline requalification described in its contract.

## Goals

Close the field defects found in the Wolf workspace before Sprint 58 fixes its
audit baseline. Repeated B-mode Up/Down motion must traverse one canonical,
document-ordered set of source block rows in both directions. It may neither
collapse from a valid row to byte zero nor manufacture a second stop inside
the same line. Compact one-line statements remain rows; formatting-only
continuations and delimiter-only lines do not. In FUSS, `Alt+g` on a directory
must open the existing tab-group picker rooted at that directory, with no files
selected, rather than immediately opening every file. E mode must also expose
one close gesture for the active group, falling back to the active tab when no
group is active.

This is editor-core and FUSS UI work. Wolf LSP is not part of B-mode motion;
the reproducer must remain green with LSP disabled and with `MODULES=""`.

## Deliverables

### 1. Cohesive four-direction B mode — `src/edit/block.c`

For a source buffer with a detected language, Up/Down traverse canonical
physical-line homes in document order. A nonblank logical statement or
declaration is one row. Consecutive line comments remain one row; delimiter-
only layout lines, operator continuations, and parenthesized/bracketed argument
continuations are skipped. Same-indent statements are siblings rather than one
merged indentation unit. The first/last rows lead to byte zero/EOF, and adding
or removing the final newline does not change the interior row sequence.

`prev()` and `next()` must be exact mirrors on those rows. In the exact
`ch4/days.lu` shape, Up from line 10 visits lines 7, 5, and 4—not `print(` and
then the same print line again. The `for ... { ... }` one-liner on line 20 and
each match arm are rows. From the line-15 tail, Up visits lines 13, 12, 11, and
10 in order instead of jumping to byte zero or an arbitrary delimiter.

The source-row policy is generic editor behavior, not Wolf or LSP behavior.
Pin the same structural shape through the real Wolf and C syntax definitions
and through a detected-language fixture with no syntax engine. Also pin a
reformatted C function so split parameters, a split assignment, and brace-only
lines do not introduce stops that its compact equivalent lacks.

Left/Right retain their Sprint 16 meaning: the level-zero provider span's
home/end. Provider spans use half-open point ownership, so arriving at an
exclusive end transfers ownership to the following/enclosing unit rather than
reselecting the unit that just ended. A no-op is valid only when the cursor is
already at the requested edge. Syntax `unit: atom`/`unit: span`, nested
delimiter matching, containment expansion, paragraph fallback, scan caps,
monotonicity, and selection-stack replay remain intact.

Buffers without a detected language preserve paragraph/gap vertical motion;
they do not pay the source-row classifier or turn prose into line motion.

### 2. `Alt+g` opens the shared chooser — `src/ui/groupfromdir.c`

`ed.group.from_dir` resolves the explicitly supplied or FUSS-selected
directory canonically and calls `yew_gp_show(ed, root)`. The picker opens with
its existing basename-derived group name, direct child directory/file rows,
and an empty tick set. Only explicit confirmation creates a group and opens
the selected files; Escape creates nothing.

Keep `yew_group_from_dir()` as the non-interactive programmatic bulk-open API
used by scripts and tests. Large-directory fallback may continue to preselect
the bounded walk result when that API itself elects to show the picker.

Errors name an unreadable or non-directory selection. No new modal, group
model, workspace-state field, dependency, or recursive scan is introduced.

### 3. Close the active group — `src/ui/groupnav.c`

Register `ed.group.close` as a no-argument E-mode command. When the active tab
is grouped, preflight every member by stable tab id and close the complete
group. If any member is modified, refuse before closing anything, matching
`ed.tab.close_others`; do not leave a half-closed group. When the active tab is
ungrouped, delegate to `ed.tab.close` so its dirty prompt and last-tab refusal
remain the single implementation of those policies.

Closing compacts the tab array, group ordinals, and empty-group state through
the existing tab-close path. `ed.group.dissolve` remains distinct: it destroys
only the group container and keeps every tab open.

### 4. Binding and help contracts

`A-g` remains bound to `ed.group.from_dir` in F mode, including the panic
keymap and shipped runtime defaults. FUSS action help continues to list the
same command/key pair. Bare `g` remains available to visible-tree type-jump.

`ed.group.close` is keyboard-reachable from E mode and present in command
completion. No default L-mode binding is required by this field repair.

### 5. Refresh the Sprint 58 handoff

After local and hosted validation, update `.docs/HANDOFF.md`, this index, and
Sprint 58's prerequisites/baseline. Sprint 58 may open only on the post-57.9
commit-of-record; its no-fixes audit rule remains unchanged.

## Testing Strategy

- Unit: exhaustive mirrored block-row sequences for the exact `days.lu` text
  and a C equivalent, both final-newline states, both alternate values, real
  syntax definitions, and syntax-free detected-language fallback.
- Unit: reformatted/compact C equivalence, same-indent siblings, one-line
  scope statements, comment runs, delimiter-only lines, operator and argument
  continuations, horizontal half-open endpoints, nested scopes, paragraphs,
  monotonicity, purity, grapheme boundaries, and scan caps.
- Unit: `ed.group.from_dir` opens the group picker on a selected directory,
  starts at zero selected files, Escape makes no group, and confirmation opens
  only ticked files.
- Script/FUSS: `Alt+g` dispatches the named command and opens the picker rather
  than creating a group immediately; a direct `yew_group_from_dir()` call
  retains its deterministic bulk-open behavior.
- PTY: open the exact Wolf source at line 10 and perform three real B-mode Up
  keys, pinning line 4 column 1 without a duplicate print-line stop.
- PTY: open FUSS on a directory, invoke `Alt+g`, verify the chooser surface,
  select a subset, create the group, and prove the chosen members are live.
- Unit/PTY: grouped `ed.group.close` closes every clean member by id,
  refuses atomically when any member is dirty, delegates ungrouped dirty and
  clean cases to `ed.tab.close`, and is exercised through its E-mode spelling.
- Build/regression: warning-clean Clang and GCC, default and `MODULES=""`;
  complete unit/script/PTY suites; ASan/UBSan focused block/group coverage;
  deterministic PTY and block/FUSS performance gates. The block gate includes
  ordinary detected-source rows and comma-terminated structural rows in
  addition to provider-heavy prose and nested containment.

## Definition of Done

- The exact Wolf and equivalent C reproducers walk one complete, mirrored row
  sequence in both EOF styles. The print call occurs once, the one-line loop
  and match arms occur once, tail statements are not skipped, and the result
  is identical without a syntax engine or LSP process.
- Split parameters/expressions and delimiter-only lines are not rows; compact
  one-line statements are. Left/Right provider edges obey half-open ownership
  without an in-range Right fixed point after a returned end.
- `Alt+g` opens the shared group picker with zero selected files and does not
  create a group until confirmation; cancel is side-effect free.
- Programmatic directory bulk-open, group adoption, ordinals, lazy hydration,
  direct type-to-jump, and FUSS action help retain their pinned behavior.
- `ed.group.close` closes a clean active group as one gesture, refuses a dirty
  group without partial effects, and falls back to the existing active-tab
  close semantics outside a group.
- Default and core-only builds are warning-free; proportional unit, script,
  PTY, sanitizer, determinism, fuzz, and performance gates are green.
- The exact post-57.9 SHA is pushed and all applicable hosted CI jobs are
  green before Sprint 58's baseline is replaced or any audit front opens.
