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
one-line constructs, asymmetric Up/Down order, formatting-dependent EOF
behavior, and confusing half-open edge ownership. The first broad repair
landed in `bb431836` and was pushed as part of `a7da188f`, but subsequent field
use showed that it had overcorrected: every ordinary source statement and
every match arm had become a vertical stop. Its local and hosted runs are moot
as Sprint 57.9 behavior qualification.

The replacement policy restores structural B-mode: brace-body openers and
indentation-suite headers are landmarks; assignments, returns, calls, and
match arms are not. Its implementation is locally qualified at `c07791cc`.
The exact real-key `days.lu` Up and trailing-EOF Left cases, Wolf/C/fallback
matrices, default/core aggregates, focused sanitizers, four deterministic fuzz
seeds, and bounded performance rows are green. This evidence replaces the
superseded statement-row qualification.

The sprint is not closed: the documentation commit-of-record has not yet been
pushed or qualified by hosted CI. Sprint 58 is paused after F08 until that
closeout and the replacement-baseline requalification described in its
contract.

## Goals

Close the field defects found in the Wolf workspace before Sprint 58 fixes its
audit baseline. Repeated B-mode Up/Down motion must traverse one canonical,
document-ordered set of structural landmarks in both directions. It may
neither collapse from a valid block to byte zero nor turn ordinary statements
into blocks. Brace-bodied functions and control constructs remain landmarks
even when compacted to one line; match arms and expression statements do not.
Indentation-based languages use suite headers rather than body statements.
In FUSS, `Alt+g` on a directory
must open the existing tab-group picker rooted at that directory, with no files
selected, rather than immediately opening every file. E mode must also expose
one close gesture for the active group, falling back to the active tab when no
group is active.

This is editor-core and FUSS UI work. Wolf LSP is not part of B-mode motion;
the reproducer must remain green with LSP disabled and with `MODULES=""`.

## Deliverables

### 1. Cohesive four-direction B mode — `src/edit/block.c`

For a source buffer with a detected language, Up/Down traverse canonical
structural landmarks in document order. The first unsuppressed `{` on a source
line is the line's brace-body landmark. Braces inside strings/comments and
braces following a match-arm `=>` are not landmarks. A line followed by a
deeper-indented nonblank line is an indentation-suite header unless it is an
expression continuation or closing delimiter. There is at most one vertical
landmark per physical line. The bounded scan retains Sprint 16's 2,000-line
latency ceiling and moves to the buffer edge when no landmark is found.

`prev()` and `next()` must be exact mirrors on those landmarks. In the exact
`ch4/days.lu` shape, functions, `match month {`, and the compact
`for ... { ... }` are stops. `var total`, `total += ...`, return-value lines,
and the individual match arms are not. Up from the bottom or the final match
arm lands on the `match` opener, not every arm. Adding or removing the final
newline does not change the interior sequence.

The structural-landmark policy is generic editor behavior, not Wolf or LSP
behavior. Pin the same shape through the real Wolf and C syntax definitions
and through a detected-language fixture with no syntax engine. Pin compact and
Allman-style C bodies, split parameters/expressions, same-indent statements,
and an indentation-language fallback so formatting cannot manufacture
statement blocks.

Left/Right use the nearest enclosing structural provider span. In detected
source outside a syntax atom, a real delimiter scope wins over incidental
paragraph or statement indentation. Trailing whitespace and final EOL bytes
remain attached to the preceding terminal scope for Left, so EOF in `days.lu`
homes to the final function opener rather than column zero. Half-open ownership
still advances at in-range returned ends; syntax `unit: atom`/`unit: span`,
nested delimiter matching, containment expansion, paragraph fallback, scan
caps, monotonicity, and selection-stack replay remain intact.

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

- Unit: exhaustive mirrored structural-landmark sequences for the exact
  `days.lu` text and a C equivalent, both final-newline states, both alternate
  values, real syntax definitions, and syntax-free detected-language fallback.
- Unit: compact and Allman C bodies, same-indent statements, one-line control
  scopes, match-arm exclusion, indentation-suite headers, expression
  continuations, trailing-EOF horizontal scope ownership, nested scopes,
  paragraphs, monotonicity, purity, grapheme boundaries, and scan caps.
- Unit: `ed.group.from_dir` opens the group picker on a selected directory,
  starts at zero selected files, Escape makes no group, and confirmation opens
  only ticked files.
- Script/FUSS: `Alt+g` dispatches the named command and opens the picker rather
  than creating a group immediately; a direct `yew_group_from_dir()` call
  retains its deterministic bulk-open behavior.
- PTY: open the complete exact Wolf source at EOF and perform a real B-mode Up,
  pinning the `match month {` opener without visiting any match arm.
- PTY: open the complete exact Wolf source at EOF and perform a real B-mode
  Left, pinning the final function's `{` rather than buffer column zero.
- PTY: open FUSS on a directory, invoke `Alt+g`, verify the chooser surface,
  select a subset, create the group, and prove the chosen members are live.
- Unit/PTY: grouped `ed.group.close` closes every clean member by id,
  refuses atomically when any member is dirty, delegates ungrouped dirty and
  clean cases to `ed.tab.close`, and is exercised through its E-mode spelling.
- Build/regression: warning-clean Clang and GCC, default and `MODULES=""`;
  complete unit/script/PTY suites; ASan/UBSan focused block/group coverage;
  deterministic PTY and block/FUSS performance gates. The block gate includes
  dense source landmarks and match-arm/statement-only bounded scans in
  addition to provider-heavy prose and nested containment.

## Definition of Done

- The exact Wolf and equivalent C reproducers walk one complete, mirrored
  structural-landmark sequence in both EOF styles. Functions, `match`, and the
  one-line loop occur once; assignments, return-value statements, calls, and
  match arms never occur. The result is identical without a syntax engine or
  LSP process.
- Compact/Allman braces and indentation-suite headers are stable structural
  stops; split parameters/expressions and same-indent body statements are not.
  Left at trailing EOF resolves the terminal brace scope, and Right has no
  in-range fixed point after a returned end.
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
