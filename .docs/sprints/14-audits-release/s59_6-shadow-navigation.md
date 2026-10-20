# Sprint 59.6: Suggestion markers and cursor responsiveness

## Prerequisites

Sprints 43/56.5 supply passive suggestions, composition and provider
arbitration; Sprint 59 supplies the current latency and memory safeguards.
The binding decisions, architecture and Fletch contracts apply. This is the
user's 2026-10-20 field repair, independent of the numbered Sprint 59 soak
and adjacent branch work. Sprint 59.5 Git interaction work is preserved.

## Goals

Replace ambiguous suggestion-source letters with recognizable one-cell
symbols, then reproduce and resolve cursor-only latency with Wolf LSP
suggestions. Preserve all completion providers, document bytes and normal
navigation. Do not assume that delayed suggestions mean a slow server;
measure editor input response separately from completion response time.

## Deliverables

1. **Source markers**, in `src/ui/glyphs.[ch]` and
   `src/ui/shadowdraw.c`, through the existing glyph API:

   ```c
   const char *yew_glyph(YewGlyph g);
   size_t yew_glyph_len(YewGlyph g);
   u16 yew_glyph_cells(YewGlyph g);
   ```

   | Provider | Unicode | ASCII / width fallback |
   |----------|---------|------------------------|
   | Symbol index | · | . |
   | Language server | ◇ | > |
   | AI | ✦ | * |

   Retain existing provider colors and attributes. Update
   `tests/unit/test_shadow_layout.c`, glyph-width tests, shadow PTY checks
   and intentional goldens; explain the symbols in
   `docs/manual/11-completion.md`.

   **Pitfall:** use the centralized ambiguous-wide fallback. Never pass
   the first byte of a multibyte glyph as a complete character or overwrite
   the line-number column with a two-cell sign.

2. **Reproduce before repairing latency.** Use `ch7/retitle.lu` from the
   reported Wolf workspace, isolated state, bounded cursor-only sequences
   and real `wolf lsp`, with suggestions enabled/disabled controls. Record
   key-to-frame timing, resident memory and request/render behavior. Check
   editor shadow scheduling, redraw, region registration and LSP dispatch;
   inspect the local Wolf server source if evidence points there. No edits
   to the user's scratch documents or live session state.

   **Pitfall:** existing soak workers and terminal backpressure can skew
   timing. Drain PTY output, bound wall time and memory, and distinguish
   end-to-end latency from provider debounce. Do not launch parallel fuzz
   campaigns or relax a latency gate to hide an implementation defect.

3. **Regression and handoff.** Repair the demonstrated cause in its owning
   subsystem; add a deterministic regression alongside existing unit/PTY/
   performance tests. Record measured before/after evidence and any part of
   the report not reproduced in `.docs/HANDOFF.md` and this contract.
   No feature removals, unrelated redesigns or automatic pushes.

## Testing Strategy

- Unit: all three marker bytes/attributes; ASCII and Unicode modes;
  ambiguous-wide fallback, one-cell width and unchanged document bytes.
- PTY: existing shadow provenance/color/no-color and dismissal/acceptance
  cases, plus a cursor-only reproducer with bounded subprocess lifetime.
- Performance: compare suggestion-enabled and disabled movement, repeated
  pauses and bursts. Assert bounded state/request work in a deterministic
  regression; keep host-contention measurements separate from CI budgets.
- Sanitizers: focused shadow/navigation tests under ASan/UBSan. Use at most
  two build jobs; no new soak. Existing bounded fuzz smoke only if relevant
  state transitions change.

## Definition of Done

- [ ] All three source markers render in exactly one cell in both width modes.
- [ ] No source letters masquerade as document text in the gutter.
- [ ] Focused shadow/glyph unit and PTY cases pass after reviewed goldens.
- [ ] Cursor-only behavior measured before and after the demonstrated fix.
- [ ] Regression exercises the cause, not merely the final screenshot.
- [ ] No unbounded memory/request/region growth in the bounded reproduction.
- [ ] Warning-clean build and focused ASan/UBSan checks pass.
- [ ] Handoff distinguishes verified fixes from unreplicated symptoms.
