# Sprint 58 findings ledger

Baseline: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`  
Next available ID: `YEW-F-002`

IDs are assigned only after a reproducer fails at the fixed baseline. They
are never reused, renumbered, or deleted. Resolution changes status and keeps
the historical row and body.

| ID | Sev | Status | Front | Title | Reproducer | Violates |
|---|---|---|---|---|---|---|
| YEW-F-001 | M | open | F01 UNI | ambiguous-wide doubles fixed-cell chrome glyphs | tests/audit/yew_f_001.c | s27 §7 |

The width mismatch is visible chrome corruption but the underlying document
bytes remain intact and the user can disable `ambiguous_wide`; that is Medium
under the wrong-but-recoverable rubric. Root-cause hypothesis: the document
width option feeds the global Unicode width table used by chrome, while several
layout slots remain one cell by contract. The reproducer fails at the fixed
baseline and is awaiting second-machine confirmation.

## Unverified observations

Observations live in their front files. They have no IDs and are excluded
from every total.
