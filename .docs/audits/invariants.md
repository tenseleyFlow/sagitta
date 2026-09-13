# Invariant re-verification

Baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`

All fifteen fronts are closed, so the ten cross-cutting sessions may now
begin. A pending row is not a verdict.

| # | Invariant | Status | Verdict | Findings |
|---:|---|---|---|---|
| 1 | No data loss, ever | pending | — | — |
| 2 | No byte confusion | pending | — | — |
| 3 | No silent stubs | pending | — | — |
| 4 | Latency budgets are CI gates | pending | — | — |
| 5 | Deterministic rendering | pending | — | — |
| 6 | Terminal restore | pending | — | — |
| 7 | Bespoke first | pending | — | — |
| 8 | Single-threaded core | pending | — | — |
| 9 | Modal paradigm first | complete | HOLDS | — |
| 10 | Recorder/Fletch round-trip | complete | VIOLATED | YEW-F-023 |

## 9. Modal paradigm first

Verdict: **HOLDS** on the audited baseline.

The mouse-disabled audit exercised every currently shipped interaction class.
The focused unit sessions covered all seven source modes; pane focus and
resize; tab activation, reorder, and overflow; group membership, entry, and
the group picker; scrolling and selection; every context-menu row; completion,
document, hover, signature, diagnostic, reference, and plugin pickers; and the
FUSS action list. The focused invocations totalled 126 test executions and
3,765 assertion executions with no failures.

| Surface | Keyboard-only evidence (`YEW_MOUSE=0`) | Result |
|---|---|---|
| Seven modes and selection | `mode_`, shift-arrow, and unit-motion focused units | holds |
| Panes | focus/resize equivalence units | holds |
| Tabs and groups | activate/reorder/overflow/membership/entry units; group-picker units | holds |
| Five picker families | picker, group, plugin, references, diagnostics/symbols focused units | holds |
| Context menus | every-row registry and optional-strip dispatch units | holds |
| Completion and document UI | `complmenu_`, hover, signature, and document-symbol units | holds |
| FUSS | complete 35-case `fuss_` PTY slice | holds |
| Plugin picker | three mouse-disabled plugin-picker PTYs | holds |
| Tutor | not shipped; assigned to Sprint 59 | not applicable |

The PTY proof then ran with `YEW_MOUSE=0`. The plugin-picker scenarios passed,
followed by all 35 `fuss_` scenarios: startup, compact and full-width trees,
ASCII and Unicode rendering, navigation, type-to-jump hints, file and
directory menus, group open/close, the actions palette, diff-layout restore,
loading, discard confirmation, conflict handling, and both leave keys. No
mouse event was available to make an otherwise unreachable surface pass.

The tutor is not a shipped 1.0 surface: F04 recorded it as Sprint 59 work, so
it is not silently credited here. No shipped surface tested in this session
requires a mouse and no invariant-9 finding remains open.

## 10. Recorder/Fletch round-trip

Verdict: **VIOLATED: YEW-F-023**.

The long property session completed with
`seeds=200000 base=0 fixtures=6 corpus=21`. P1–P5 reported zero divergences;
the count-folding sentinel, edit-store no-op, replay under a completely
rebound keymap, and rollback at an erroring third command all passed.
`test-roundtrip-coverage` classified all 162 commands carrying
`YEW_CMD_RECORDABLE`: 23 generator-reachable and 139 explicitly denied with
a nonempty reason. The word bijection passed.

The hand-built edge cases also passed for a cancelled prompt, one-event
multi-cursor capture, a nested macro with one outer undo transaction,
argument-bearing command splitting in event order, and an insert argument
containing `a`, NUL, lone `0x80`, and `z`. The focused recorder, nested-replay,
and plugin-lifecycle sessions completed without unexpected failures.

The plugin edge is nevertheless a direct breach, not a coverage caveat.
`YEW-F-023`'s hard-XFAIL demonstrates that a plugin command executes but is
registered without `YEW_CMD_RECORDABLE` and without a CMDWORD. It therefore
cannot be represented by the recorder as readable Fletch source. The 200,000
generated seeds cannot reach that absent vocabulary entry, so their clean
result does not discharge the cross-subsystem promise.
