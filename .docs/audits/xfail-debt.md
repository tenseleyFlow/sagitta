# Expected-failure debt

This is Sprint 58's authoritative cross-surface debt ledger. It begins empty:
the fixed baseline has no expected failures. A confirmed `YEW-F-###` finding
adds one row and keeps it until the finding is closed; deferred and wontfix
rows keep their XFAIL forever.

| ID | Surface | Reproducer | Reason | Status |
|---|---|---|---|---|
| YEW-F-001 | unit | `tests/audit/yew_f_001.c` | fixed-cell chrome widens under `ambiguous_wide` | open |
| YEW-F-002 | unit | `tests/audit/yew_f_002.c` | bounded RI restart delays completed job output | open |
| YEW-F-003 | unit | `tests/audit/yew_f_003.c` | ASCII fast path stores a keycap at the wrong width | open |
| YEW-F-004 | unit | `tests/audit/yew_f_004.c` | full parser rejects bare dotted map keys | open |

The Sprint 33 conformance-only ledger remains at
`tests/fletch/xfail-debt.txt` until the cross-surface runner migration lands.
That historical file is also empty.
