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

4. **Captured background-index stall follow-up (2026-10-21).** The user's
   FUSS-scroll sample (`/tmp/yew-git-lag-stack.txt`) spends 1344 of 5471
   main-thread samples in `yew_symwalk_pump`, primarily repeated grapheme
   boundary validation inside symbol extraction. Reproduce the scan using
   read-only workspace input, then remove repeated line-prefix work in the
   owning Unicode/indexing code. Preserve UAX #29 behavior, malformed-byte
   handling, all completion providers and existing indexing limits. Test
   long mixed-text lines and checkpoint/edit boundaries, and measure the
   longest cooperative indexing step rather than only total throughput.
   Workspace scans must yield between word units within a long line,
   retaining resume offsets, syntax state, caps, insertion order, hit counts
   and the batch's recency tick; cancellation/reindex discards continuation.
   Keep the earlier commit-modal and Insert-mode stalls unconfirmed unless
   their own reproduction/evidence establishes the connection.

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

- [x] All three source markers render in exactly one cell in both width modes.
- [x] No source letters masquerade as document text in the gutter.
- [x] Focused shadow/glyph unit and PTY cases pass after reviewed goldens.
- [x] Cursor-only behavior measured before and after the demonstrated fix.
- [x] Regression exercises the unnecessary redraw, not merely the screenshot.
- [x] No unbounded memory/request/region growth in the bounded reproduction.
- [x] Warning-clean build and focused ASan/UBSan checks pass.
- [x] Handoff distinguishes verified fixes from unreplicated symptoms.
- [x] Capture/reproduce the FUSS background-index stall and repair its
      quadratic boundary checks and whole-line budget overrun.
- [x] Forced-yield extraction, Unicode/edit boundaries, bounded fuzz,
      long-line performance and pause-and-scroll PTY regressions pass.
- [ ] Capture and resolve the reported severe Insert-mode stall itself.

Checkpoint: markers and redundant-redraw repair are implemented, **not a
claim that the reported stall is resolved**. The real-server reproduction
repeatedly displays `Doc` at the reported location but remains responsive
in both Line and Insert modes. See `.docs/HANDOFF.md` for exact bounded
measurements, tests, build paths and the live-profile pickup. The user's
terminal/Herdr path has not been covered by the direct-PTY probe.

2026-10-21 follow-up: the user's sample and read-only replay identified
workspace symbol indexing, not Wolf LSP, as the captured FUSS slowdown.
Existing exact Unicode checkpoints and an adjacent-ASCII/CRLF fast path
remove repeated line-prefix scans; the workspace scanner now yields between
words and retains its recency tick. All 69,995 workspace entries survive.
Pause-and-scroll PTY worst arrow: 2149.97 ms installed versus 1.409 ms patched.
`perf_cursor` and the expanded `perf_symidx` pass unchanged budgets; focused
Clang/GCC/sanitizer units, commit-modal PTYs and bounded symbol fuzz pass.
The original Insert-mode and commit-modal symptoms remain field-confirmation
items, not claims made from this FUSS reproduction. See handoff for exact
commands/builds, samples, resource bounds and the separate 59.8 pickup.
