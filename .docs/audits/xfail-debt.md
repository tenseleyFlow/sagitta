# Expected-failure debt

This is Sprint 58's authoritative cross-surface debt ledger. It begins empty:
the fixed baseline has no expected failures. A confirmed `YEW-F-###` finding
adds one row and keeps it until the finding is closed; deferred and wontfix
rows keep their XFAIL forever.

| ID | Surface | Reproducer | Reason | Status |
|---|---|---|---|---|

The Sprint 33 conformance-only ledger remains at
`tests/fletch/xfail-debt.txt` until the cross-surface runner migration lands.
That historical file is also empty.
