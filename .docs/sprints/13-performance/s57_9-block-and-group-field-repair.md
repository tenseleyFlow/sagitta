# Sprint 57.9: Block Motion and Group Picker Field Repair

## Prerequisites

- Sprint 16 — L/W/B/H/I modal motion and the provider-layered B-mode unit.
- Sprint 24 — tab-group identity, lazy member hydration, and the shared
  new/edit group picker.
- Sprints 57.5–57.8 — compact FUSS interaction, printable type-to-jump,
  off-canvas geometry, action chords, and the modern tab strip.
- Binding plans and invariants 1–5, 7, and 9. This repair may not weaken core
  editing, syntax, tab identity, deterministic rendering, or keyboard reach.

## Goals

Close two field defects found in the Wolf workspace before Sprint 58 fixes its
audit baseline. Repeated B-mode Up/Down motion must visit adjacent structural
blocks without collapsing from a valid sibling to byte zero. In FUSS,
`Alt+g` on a directory must open the existing tab-group picker rooted at that
directory, with no files selected, rather than immediately opening every file.

This is editor-core and FUSS UI work. Wolf LSP is not part of B-mode motion;
the reproducer must remain green with LSP disabled and with `MODULES=""`.

## Deliverables

### 1. Exact adjacent B-mode traversal — `src/edit/block.c`

Pin the field shape with several top-level Wolf-style brace blocks separated
by one blank line. Starting at EOF, consecutive `yew_unit_block.prev()` calls
visit the line containing each preceding top-level opening boundary in order.
The mirror walk with `next()` returns through the same boundaries to EOF.

Sibling discovery must compare blocks in a coordinate that includes their
leading declaration lines. A fallback paragraph or whole-buffer span may not
hide a nearer delimiter scope merely because the cursor is sitting on that
scope's opening boundary. Existing syntax `unit: atom`/`unit: span`, nested
delimiter, indentation, paragraph, scan-cap, monotonicity, and half-open span
contracts remain unchanged.

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

### 3. Binding and help contracts

`A-g` remains bound to `ed.group.from_dir` in F mode, including the panic
keymap and shipped runtime defaults. FUSS action help continues to list the
same command/key pair. Bare `g` remains available to visible-tree type-jump.

### 4. Refresh the Sprint 58 handoff

After local and hosted validation, update `.docs/HANDOFF.md`, this index, and
Sprint 58's prerequisites/baseline. Sprint 58 may open only on the post-57.9
commit-of-record; its no-fixes audit rule remains unchanged.

## Testing Strategy

- Unit: exact Wolf-shaped previous/next block boundary sequence; nested scope
  and paragraph regression suite; monotonicity and scan budget unchanged.
- Unit: `ed.group.from_dir` opens the group picker on a selected directory,
  starts at zero selected files, Escape makes no group, and confirmation opens
  only ticked files.
- Script/FUSS: `Alt+g` dispatches the named command and opens the picker rather
  than creating a group immediately; a direct `yew_group_from_dir()` call
  retains its deterministic bulk-open behavior.
- PTY: open FUSS on a directory, invoke `Alt+g`, verify the chooser surface,
  select a subset, create the group, and prove the chosen members are live.
- Build/regression: warning-clean Clang and GCC, default and `MODULES=""`;
  complete unit/script/PTY suites; ASan/UBSan focused block/group coverage;
  deterministic PTY and existing block/FUSS performance gates.

## Definition of Done

- The checked-in Wolf-shaped reproducer walks every adjacent top-level block
  in both directions and passes without any LSP process or configuration.
- `Alt+g` opens the shared group picker with zero selected files and does not
  create a group until confirmation; cancel is side-effect free.
- Programmatic directory bulk-open, group adoption, ordinals, lazy hydration,
  direct type-to-jump, and FUSS action help retain their pinned behavior.
- Default and core-only builds are warning-free; proportional unit, script,
  PTY, sanitizer, determinism, fuzz, and performance gates are green.
- The exact post-57.9 SHA is pushed and all applicable hosted CI jobs are
  green before Sprint 58's baseline is replaced or any audit front opens.
