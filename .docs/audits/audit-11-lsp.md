# F11 LSP — JSON, JSON-RPC, client, and features

Status: closed
Baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`
Opened: 2026-09-12
Closed: 2026-09-12
Scope: `src/mod/lsp/`, `src/unicode/u16.c`
Owners read: Sprints 45, 46, 47

The product-code baseline remained immutable. This front strengthens unit
controls and adds three hard-XFAIL reproducers; no `src/` file changed.

## Q1 — position conversion differential

probed, nothing found

- The independent-reference matrix now covers 200 mixed corpus lines and one
  4 KiB line. Every line contains or is paired with ASCII, astral scalars,
  U+DC80–DCFF raw-byte escapes, tabs, combining text, and CRLF.
- At every byte offset it compares both UTF-8 and UTF-16 position construction,
  protocol-to-offset conversion, direct UTF-16 conversion, and the documented
  backward clamp inside a scalar. The full-module run passed 59,033 assertions;
  the core-only build also passed its 14,797 applicable assertions.
- A deliberately wrong reference counts astral scalars as one UTF-16 unit.
  The fixture must and does produce a disagreement, proving the matrix can
  detect the central failure mode.

## Q2 — zero-based protocol positions and the `+1` ban

finding `YEW-F-016`

- Protocol conversion remains zero-based. The audited construction and inverse
  helpers contain no display adjustment, and the differential matrix requires
  internal row zero to remain protocol row zero.
- Sprint 46's literal source gate is nevertheless not empty. It finds three
  `line + 1` expressions: location-picker display, symbol-picker display, and
  a user-facing rename error. All are legitimate display edges required by
  Sprint 47, but they live under `src/mod/lsp/`, which Sprint 46 prohibited.
- `tests/audit/yew_f_016.c` records the incompatible release contracts as a
  Medium hard-XFAIL for Sprint 59.

## Q3 — pre-operation delete coordinates

probed, nothing found

- `yew_edit_notify_pre` calls `yew_lsp_note_edit` before the text mutation, and
  the delete path converts both `at` and `at + len` in that pre-operation
  buffer. The insert payload is captured by the paired post notification.
- The sync filter passed 4 tests and 266 assertions. Its multi-line deletion
  from `ab\ncd\nef` emits start `(0,1)` and end `(1,2)` against the server's
  old document; its CRLF deletion ends at `(1,0)`.

## Q4 — generation drop for in-flight features

probed, nothing found

- Every feature request site supplies both `RpcPending.buf_id` and the current
  text-buffer generation. The single pump-phase response dispatcher compares
  that generation before `yew_rpc_dispatch`, drops the pending entry, and
  increments `dropped_stale`; callbacks do not duplicate this admission check.
- A new control queues all 12 buffer-scoped response shapes—completion,
  resolve, hover, signature, the four definition-family methods, references,
  document highlight, document symbols, and rename—then edits once. All 12
  responses are dropped before their shared callback: 62 assertions passed.
- The complete lifecycle filter, including the delayed fake-server response,
  passed 37 tests and 4,419 assertions.

## Q5 — rename hydration, cancellation, and rollback

probed, nothing found

- Phase 2 hydrates every affected file before the confirmation phase. Files
  opened by validation are ordinary buffers in ordinary tabs, as Sprint 47
  specifies. In the two-file cancellation control, one pre-existing tab plus
  one hydrated tab remain open after cancel; both buffers and both disk files
  are byte-identical to their pre-rename state.
- The rename filter passed 26 tests and 1,944 assertions. The injected failure
  on a later file aborts its open transaction, rolls every committed file back
  through exactly one LSP undo node, prunes the rollback redo node, preserves
  unrelated user undo history, and leaves all source and disk bytes identical.

## Q6 — malformed `WorkspaceEdit` inertness

probed, nothing found

- `make fuzz-lsp-resp` completed four independent 50,000-iteration streams:
  seeds 1, `0x243f6a8885a308d3`, `0x9e3779b97f4a7c15`, and
  `0xd1b54a32d192ed03`. Final hashes were respectively
  `33a7061ea8db112d`, `83e10631d0b8c351`, `836c6e10a7b4a68b`, and
  `ce68bdf2b2168612`.
- Every iteration feeds mutated completion, hover, location, symbol, and
  workspace-edit shapes through the real handlers/preflight and checks buffer
  generation plus byte identity. All 200,000 iterations passed.

## Q7 — snippet downgrade and its source gate

finding `YEW-F-015`

- The functional policy is intact. The snippet filter passed 4 tests and 4,237
  assertions, including `${1|red,green|}` becoming `red`, nested stripping,
  escape handling, malformed inputs, the `$0` cursor position, and a 4 KiB
  input.
- The specified repository-wide `tabstop|placeholder` scan does not hit only
  the policy paragraph. It finds that paragraph plus ten unrelated core source
  lines. `tests/audit/yew_f_015.c` pins this Medium release-control defect for
  Sprint 59.

## Q8 — stripped-module behavior and size evidence

finding `YEW-F-014`

- A fresh `MODULES=""` unit build succeeds. Its module-boundary test passes 97
  assertions and verifies the exact `yew_mod_require` text for at least 16 LSP
  commands. Baseline hosted run `34699266067` supplies the exact product
  commit's musl static size evidence with the LSP-containing full build and
  the LSP-free minimal build both inside their budgets.
- `ed.lsp.complete` is deliberately excluded from the minimal unit loop. Its
  shim reports INFO and opens core index completion rather than returning the
  exact mandatory module error. `tests/audit/yew_f_014.c` records this useful
  but contract-incompatible exception as a Medium hard-XFAIL for Sprint 59.

## Unverified observations

None.

## Count

Raw 3 · deduped 3 · critical 0 · high 0 · medium 3 · low 0 · unverified 0.
