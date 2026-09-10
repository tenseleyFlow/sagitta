# F07 UI — panes, tabs, groups, workspace persistence

Status: closed
Baseline: 41fef4166fe6bf127f36b8b9f6eb653a454a28c1
Opened: 2026-09-09
Scope: src/ui/, src/ws/
Owners read: Sprint 22 through Sprint 27, Sprint 25

## Q1 — shared Rect draw and hit-test cells

probed, nothing found

- The new four-size render control,
  `layout_draw_and_hit_share_every_clickable_cell_at_audit_sizes`, passed
  with 142,876 assertions under both clang and gcc. It builds a real
  three-leaf tree at 80x24, 81x24, 121x24, and 200x50, resets the grid's
  prior-frame damage, renders panes and the tab strip, then walks every
  cell. Each clickable hit lies within the draw pass's fresh damage span and
  within its returned `Rect`.
- Pane hits resolve through the per-frame leaf table to exactly the rendered
  leaf `Rect`; border hits resolve through the split table to the exact
  one-cell divider `Rect` computed by the renderer; tab, scroll, and new-tab
  spans are all contained by the rendered tab-strip `Rect`. This covers the
  odd-width and wide-terminal coordinates where duplicate layout arithmetic
  would drift first.
- Existing focused controls cover the modal rectangles rendered after the
  base frame: panel block rects, picker rows and preview blocks, completion
  rows/panels, context-menu rows, and FUSS rows. The full PTY suite remains
  the terminal-output guard; this in-process control is the companion that
  can inspect the process-local region table cell by cell.

## Q2 — group membership churn

finding YEW-F-007

- The deterministic long-form control executed 100,000 membership operations
  (2,117,365 assertions) over 16 real files. It includes close-active,
  reorder-block, dissolve-during-walk, reopen, ordinal changes, and a full
  state emit/restore every 1,000 operations. It confirms every live group
  remains nonempty and that restore neither loses a member nor moves one to a
  different group.
- The control exposed a separate persistence failure at restore. The compact
  hard-XPASS reproducer in tests/audit/yew_f_007.c has one group ordered
  f0,f1,f2, reorders only its global tab records to f2,f1,f0, and confirms the
  emitted values are 3,2,1. Reopen produces f0,f2,f1.
- This is Critical under the Sprint 58 rubric: a normal workspace save and
  restore reorders user-owned group state without an action from the user.
  The writer is correct; the parser's incremental ordinal attachment is the
  root-cause hypothesis. Remediation belongs to Sprint 59.

## Q3 — group and restore file-read counts

probed, nothing found

- `groups_opening_a_forty_file_group_reads_one_file` passed with 169
  assertions. Creating the 40-member group made no file reads; viewing the
  first member incremented `yew_file_load_count()` exactly once, viewing a
  second member made the second read, and revisiting the resident first tab
  made none.
- `ws_restore_of_forty_tabs_reads_one_file` passed with 92 assertions. A
  fresh restore created 41 tabs (the scratch tab plus the 40 saved paths) and
  made exactly one file read for the active member; switching to another
  member incremented the same test hook to two.
- Both controls count at `text/file.c`'s test hook rather than inferring I/O
  from elapsed time, so a warm filesystem cache cannot hide eager hydration.

## Q4 — state ordering and ratio fixpoint

probed, nothing found apart from `YEW-F-006`

- `state_schema_emits_a_parseable_v1_document`,
  `state_schema_writes_groups_before_tabs`, and
  `state_schema_emission_is_deterministic` passed (9, 6, and 7 assertions).
  They confirm the shipping Fletch data writer emits a parseable v1 document,
  preserves the root/group/tab order, and emits byte-identically when state
  has not changed.
- `state_schema_permille_is_a_fixpoint` passed all 999 legal values (1,004
  assertions), and `state_corpus_contains_no_floats` passed 12,192 assertions.
  Ratios therefore remain integer permille through the persisted format.
- `state_corpus_reemission_is_idempotent` passed 110 assertions. The separate
  root/workspace unknown-key loss is the critical `YEW-F-006` finding under
  Q5, not an ordering or ratio drift.

## Q5 — corrupt-state discipline and unknown-key retention

finding YEW-F-006

- A valid v1 document with unknown keys at the root and under workspace
  reaches the normal state apply path, then loses both keys on re-emission.
  Its unknown option remains, proving the loss is selective rather than a
  rejected document.
- tests/audit/yew_f_006.c is the hard-XPASS reproduction. It records the
  required correct behavior and fails on the immutable baseline with retained
  root=0, workspace=0, options=1.
- This is Critical under the Sprint 58 rubric: a future workspace key is
  user-owned data silently deleted by a normal save. The remediation belongs
  to Sprint 59; no product source changed for this finding.
- The existing malformed-state controls remain healthy:
  `state_corpus_invalid_documents_are_rejected` and
  `state_corpus_invalid_documents_reach_a_result` passed with 56 and 101
  assertions. `state_corpus_unknown_keys_survive_reemission` also passed its
  options-subtree fixture (8 assertions), which is why the new probe places
  equivalent keys at root and workspace depth rather than confusing that
  narrower control with full forward retention.

## Q6 — stale workspace-lock ownership

probed, nothing found

- `ws_save_live_lock_demotes_us_to_a_reader` confirmed that a live owner is
  respected; the reader never writes over its state.
- The existing stale-pid control and the new
  `ws_save_kill9_stale_lock_is_taken_over` control both passed. The latter
  forks a real editor session, waits until it claims the on-disk PID lock,
  kills it with `SIGKILL`, then opens a fresh editor. The new session becomes
  the writer and completes one state write (18 assertions).
- This exercises the exact stale-lock condition rather than treating mere
  lock-file existence as an owner, and proves the recovery is based on the
  recorded PID.

## Q7 — repository pollution

unverified observation — tutor is Sprint 59 scope

- The available controls pass: `ws_save_never_writes_into_the_workspace` and
  `ws_save_leaves_a_git_checkout_clean` exercise a real repository and state
  save; plugin lifecycle controls keep their state under XDG paths; and the
  live Fletch/LSP suite owns and tears down its fake-server sessions without
  workspace writes.
- The attack question specifically requires one full session *including
  tutor*. `yew tutor` is an explicit Sprint 59 deliverable and is absent from
  the immutable Sprint 58 baseline, so that named full-session transcript
  cannot be honestly executed here. This is not a product finding because
  the surface is not yet reachable; it is carried as one unverified
  observation for Sprint 59's tutor fixture rather than claimed as a pass.

## Q8 — picker payload identity across refilters

probed, nothing found

- The generic picker control `picker_selection_survives_a_refilter` passed,
  confirming that a refilter retains the selected payload rather than the
  previously selected row. The existing file finder, buffer switcher, group
  picker, diagnostic picker, and nested symbol-picker controls cover their
  respective payload domains and selection paths.
- The new end-to-end plugin-picker control,
  `plug_lifecycle_picker_refilter_keeps_plugin_identity`, passed with 132
  assertions under both clang and gcc. It selected `bingo` by the stable
  `PlugSys->v` index used as its picker payload, refiltered through the real
  keyboard path, pressed Enter, and proved that only `bingo` toggled while
  the other plugins remained discovered.
- This is the important integration case: refiltering only changes the
  generic picker rows, whereas plugin acceptance resolves the preserved
  payload against the unchanged plugin vector. No examined picker consumer
  confuses selection identity with visual row position.

## Inbound obligation — legacy state codec retirement preflight

The final legacy-versus-Fletch 2x2 differential ran before deletion:

- state_differential_ matched canonical, noncanonical, invalid, deliberate
  difference, non-redundancy, and Sprint 36 handover rows: 6 tests, 247
  assertions, 0 failures.
- state_diff_generated_500_matrix completed the generated 500-document
  matrix: 1 test, 4,500 assertions, 0 failures.

The test-only legacy wrapper and its differential-only tests have been
removed. The retained Sprint 25 corpus round-trip now exercises the shipping
Fletch data reader and writer as the remaining frozen-format guard.

## Count

Raw 2 · deduped 2 · critical 2 · high 0 · medium 0 · low 0 · unverified 1.
