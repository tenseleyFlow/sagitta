# F07 UI — panes, tabs, groups, workspace persistence

Status: in progress
Baseline: 41fef4166fe6bf127f36b8b9f6eb653a454a28c1
Opened: 2026-09-09
Scope: src/ui/, src/ws/
Owners read: Sprint 22 through Sprint 27, Sprint 25

## Q1 — shared Rect draw and hit-test cells

in progress

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

in progress

## Q4 — state ordering and ratio fixpoint

in progress

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

## Q6 — stale workspace-lock ownership

in progress

## Q7 — repository pollution

in progress

## Q8 — picker payload identity across refilters

in progress

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

Raw 2 · deduped 2 · critical 2 · high 0 · medium 0 · low 0 · unverified 0.
