# Closeout: F11 — Completion and LSP

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s43, s44, s45, s46, s47

## DoD items (every numbered item from every sprint above)

- [ ] s43.1 `make test` green under gcc and clang with `-std=c11 -pedantic -Wall -Wextra -Werror -Wvla`, zero warnings; ASan/UBSan/valgrind lanes clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s43.2 The staleness suite: a suggestion delivered after an edit is **never** inserted — 0 bytes, in all ≥ 20 `shadow_revalidate` cases and in 4 × 50k fuzz iterations.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s43.3 `grep -rn 'yew_textbuf_insert\|yew_textbuf_delete\|yew_edit_' src/edit/shadow.c src/ui/shadowdraw.c` shows `yew_edit_insert` in exactly one function (`shadow_accept_n`) and nothing in the draw file.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s43.4 `grep -rn 'yew_vp_follow\|yew_vp_scroll\|yew_vp_center' src/edit/shadow.c src/ui/shadowdraw.c` → empty (the no-jump law).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s43.5 Overlay proof: the pty golden with a 4-line ghost has every non-ghost row byte-identical to the same frame with `shadow.enable=false`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s43.6 All 12 rows of the §8 interaction table have a firing assertion, including both delete rows and both Esc stages.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s43.7 Accept-word matches `yew_unit_of_mode` for L/W/B and `yew_unit_word` for I/E, on all three CJK/emoji/subword fixtures, `alt` both ways.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s43.8 Provenance is legible without colour: the 16-colour pty goldens for the three providers differ, and the gutter letters `s`/`l`/`a` appear in cell 1 of the sign column with the diagnostic/git cells untouched.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s43.9 Debounce: a scripted 100-keystroke burst yields exactly 1 timer arm and ≤ 1 request per provider; `dropped_stale` is 0 in that run.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s43.10 `yew_edit_notify_pre/post` exist with the §3 call lists in the pinned order; `grep -c 'yew_edit_notify' src/text/edit.c` == 2; no callback registry exists (`grep -rn 'notify_register\|on_edit_add' src/` empty).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s43.11 Menu/ghost exclusivity: the debug assert fires under a test that opens both, and `shadow.suppressed` is proven set/cleared by `yew_compl_*` stubs registered for the test.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s43.12 Later binding Sprints 44, 46, 47, and 49 supersede the original provider and feature staging guards: a full-modules startup registers index, LSP, and AI and `:shadow.stats` reports all three ready; deliberately unregistered test providers remain visible; stripped-module commands retain their exact boundary errors.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s43.13 Perf baselines committed under `tests/perf/baselines/shadow.txt`; the 8-line-ghost frame cost is within budget on the dev machine.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s43.14 ≥ 130 assertions across the four new unit files; determinism lane green on the new goldens.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.1 `make test` green on gcc and clang, zero warnings under the locked flags; ASan/UBSan/valgrind lanes clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.2 **The 0.5 ms budget is a gate:** `yew_symidx_pump` p99 ≤ 500 µs over the 10 000-keystroke perf case, baseline committed under `tests/perf/baselines/symidx.txt`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.3 Incremental correctness: the differential oracle (incremental index == full rescan) is green for 4 seeds × 5 000 edits, including the offset-0-of-10 000-lines case.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.4 Syntax filtering proven both ways: with the real s39 highlighter, a C fixture's comment and string words are absent from the index and its code identifiers are present; with `lang == 0` the same fixture indexes every word. Both asserted by count *and* by named example.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.5 `sizeof(SymEntry) <= 32` (`_Static_assert`); index memory for a 100 kLOC workspace ≤ 32 MiB, reported by `:compl.stats`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.6 Current closeout scopes symbol determinism to its actual owner: `grep -En '(^|[^[:alnum:]_])(float|double|qsort)([^[:alnum:]_]|$)' src/ws/sym*.c src/ws/sym*.h` → empty; the same query on the same index returns byte-identical `SymHit` arrays across two runs and across 8 insertion-order permutations.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.7 Every row of the §5 key table has a firing assertion, including `<cr>`-falls-through and unknown-key-re-dispatch.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.8 One completion accept == exactly one undo node; one undo restores the pre-menu buffer byte-exactly (asserted on a 3-buffer script test).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.9 Menu geometry is computed once: `yew_compl_draw` and the region registration both read `menu.box`/`menu.panel`, and a test asserts the hit-tested row index equals the drawn row index for all 10 rows at three terminal widths, including a width where the box is clamped.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.10 The menu and the ghost never coexist: a script that triggers both asserts `!(w->shadow.live && w->compl.open)` after every dispatch, and the s43 debug assert is proven to fire when the rule is violated.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.11 The walk degrades: with `git` present it honors `.gitignore`; with `git` absent (PATH stripped in the test) it completes via the bespoke walk and messages once. Neither path exceeds its per-pump budget.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.12 Interner discipline: 10 repeat walks over a 200-file tree grow the interner by 0 entries after the first; the per-file symbol cap fires on the 10 000-identifier fixture.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.13 `ComplItem.doc` renders `(no documentation)` for every index item and `symidx.h` states the no-filesystem-watching non-goal; Sprint 47's delivered `completionItem/resolve` path supplies and renders LSP documentation.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s44.14 ≥ 170 assertions across the five new unit files; determinism lane green on the new pty goldens.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.1 `make` and `make test` green on gcc and clang, zero warnings under `-std=c11 -pedantic -Wall -Wextra -Werror -Wvla`, for **both** `MODULES="lsp ai fuss plugins"` and `MODULES=""`; ASan/UBSan/valgrind lanes clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.2 `sizeof(JsonValue) <= 24` (`_Static_assert`); parsing is arena-only — `grep -n 'malloc\|calloc\|realloc\|free(' src/mod/lsp/json.c` → empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.3 Every row of the §3 grammar table, the §4 surrogate table, and the §6 framing-error table has a firing test asserting the exact message.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.4 The invalid-byte round trip holds: a buffer byte `0xFF` → `\uDCff` on the wire → `0xFF` back, asserted end to end through the fake server.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.5 Chunking independence: the 12-message corpus produces the identical message sequence for **every** split offset (≥ 40 000 split cases), and for the 1-byte-at-a-time and single-giant-write servers.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.6 Depth and size limits: 128-deep rejects cleanly with `line:col`; a 64 MiB+1 `Content-Length` is rejected before any allocation; a 4M-node body is rejected; none of the three crashes or hangs.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.7 Number policy: all ≥ 40 vectors pass, `YEW_JSF_LOSSY_NUM` is set exactly where the table says, and `1e309` is a *parse error*, not an infinity.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.8 Determinism: writer output for the corpus is byte-identical across two runs and both compilers; the JSON real-format gate accepts precision-bearing conversions, so `grep -rEn '%[-+ #0-9.*]*[gfe]' src/mod/lsp` matches only the single annotated `snprintf` in `yew_jsonw_real`; no sort and no hash in the emitter (`grep -n 'yew_sort\|strmap' src/mod/lsp/json.c` → empty).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.9 `grep -rn 'setlocale\|wcwidth\|mbrtowc' src/mod/lsp/` → empty; the s02 ban still passes; the locale rationale comment is present above the `strtod` call.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.10 `make fuzz FUZZ_ITERS=200000 FUZZ_SEED=1` exits 0 for `fuzz_json` and `fuzz_jsonrpc`, plain and under ASan/UBSan; `tests/fuzz/crashes/` empty; the parse→write→parse structural-equality invariant holds for every successfully parsed input.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.11 `json.c|h` contains no LSP-specific identifier — `grep -in 'lsp\|textdocument\|rpc' src/mod/lsp/json.c src/mod/lsp/json.h` → empty (Sprint 48 links it for HTTP bodies).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.12 The framed sink bypasses `yew_job_safe_prefix`: the labelled regression test fails when the bypass is removed, and its comment says so.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.13 In a `MODULES=""` build, `:lsp.restart` prints exactly `this build has no lsp module; rebuild with 'make MODULES="… lsp"'`; Sprint 46 delivered full-build server startup, so unconfigured and configured fake-server tests exercise the live no-server, start, and restart paths. None is a silent no-op.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s45.14 ≥ 220 assertions across the six new unit files; determinism lane green on `tests/unit/fixtures/lsp/*.json`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.1 `make test` green on gcc and clang for `MODULES="lsp …"` and `MODULES=""`, zero warnings; ASan/UBSan/valgrind lanes clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.2 **The differential position test passes at every byte offset of every line of the corpus fixture, in both encodings, in both directions** — zero disagreements. Breaking `yew_off_to_u16col` for astral characters fails it (proved once by a deliberately wrong variant in the test file).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.3 `grep -n 'tabw\|width\|cell' src/unicode/u16.c` → empty; all position conversion lives in `src/unicode/`, and `grep -rn 'yew_off_to_u16col\|yew_u16col_to_off' src/mod/lsp/` shows call sites only, inside `lsp_pos_of_off` and its inverse.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.4 No ±1 on line numbers anywhere in `src/mod/lsp/`: `grep -rn 'line + 1\|line - 1\|\.v + 1' src/mod/lsp/` → empty, and a unit test asserts a diagnostic on internal line 0 reports LSP line 0 and displays as line 1.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.5 Incremental `didChange` correctness: every §6 row asserted against a hand-computed expectation, including the multi-line delete whose `end` is pre-op; a 40-key burst is exactly one notification with 40 ordered elements; the overflow path sends one full change.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.6 Capability probing: all five shapes × every capability tested; `positionEncoding` negotiation verified against `fakelsp` answering `utf-8`, `utf-16`, absent, and garbage.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.7 Lifecycle: the backoff table's five rows produce the exact messages; the give-up message contains the last stderr line; after `stop`, `kill(-pgid, 0)` → `ESRCH`, zero zombies, zero leaked fds over 200 spawn/kill cycles.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.8 Stale responses are never applied: with an injected 200 ms server delay and a scripted edit in between, `dropped_stale` > 0 and the recording callback was never invoked.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.9 Diagnostics survive edits: after 1 000 random edits around 50 diagnostics, every surviving range still covers the same text it did (or is collapsed at the deletion point per s09's bias rules), verified against a naive oracle.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.10 Every severity glyph is one cell (`yew_cluster_width == 1`), asserted for the default theme and for an `ambiguous_wide = true` run; a 2-cell override falls back to ASCII with one log line.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.11 `yew_msg_hint` is displaced by `yew_msg` and restored on its expiry (pty golden), and never expires on its own.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.12 Perf: `yew_lsp_note_edit` p99 ≤ 100 µs in both encodings; baselines committed under `tests/perf/baselines/lsp.txt`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.13 Sprint 47 delivered all nine feature commands: each routes through its implementation and exact capability gate rather than `YEW_CMD_DEFERRED`; `workspace/symbol`, code actions, formatting, inlay hints and semantic tokens remain documented in `client.h` as permanent 1.0 non-goals, not stubs.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s46.14 ≥ 220 assertions across the seven new unit files; determinism lane green on the new pty goldens.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.1 `make test` green on gcc and clang for `MODULES="lsp …"` **and** `MODULES=""` (the panel widget is core and is tested in both), zero warnings; ASan/UBSan/valgrind lanes clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.2 **MILESTONE:** the `lsp` CI lane is green — all 8 goto-definition fixtures land on the right file and the anchor-resolved line, references returns ≥ 6 hits across ≥ 3 files, the two-file rename round-trips to `git diff --exit-code` clean and the renamed tree still compiles, the diagnostic fixture fires, and the hover fixture returns text. The lane **fails** when clangd is absent.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.3 `make compile_commands.json` is reproducible: two runs are byte-identical, it is gitignored, and the `#ifdef YEW_WITH_LSP` fixture (row 8) passes, proving the module defines reached the server.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.4 Snippets are downgraded, never expanded: all ≥ 30 vectors pass including the `$0` cursor offset; `grep -rn 'tabstop\|placeholder' src/` returns only comments in the policy paragraph; the policy text appears in `:lsp.info` output (asserted by a script test).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.5 Rename is all-or-nothing: every Phase-2 rejection row leaves the workspace byte-identical, and the fault-injected mid-apply rolls back every committed buffer to byte-identity (checked by hash over all affected buffers).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.6 One undo node per buffer per rename: after a 6-file rename, each buffer's `yew_undo_current` advanced by exactly 1, and one `ed.undo` in each restores it exactly.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.7 Nothing is written to disk by a rename: the fd-accounting/`open` trace over a rename script shows zero writes to any source path, and every affected buffer is dirty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.8 `YEW_TXN_LSP` no longer hard-errors (closing s10 DoD 11) and is used in exactly one place — `grep -rn 'YEW_TXN_LSP' src/` shows the enum, the rename apply loop, and nothing else.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.9 Jumplist discipline: `yew_jump_push` is called before the cursor moves, exactly once per completed jump, zero times for a cancelled picker — asserted for all five navigation commands.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.10 Capability gating: all nine features produce the exact `"<server> does not support <feature>"` sentence when the bit is absent, exactly once per (server, feature); a `-32601` reply achieves the same state.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.11 The panel obeys its `Rect`: fuzz + unit prove it never writes a cell outside `rect`, never splits a cluster, never leaves an orphan wide-glyph continuation (s05's rules re-checked from this side), and computes its geometry exactly once per open/resize.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.12 Determinism: the location and symbol pickers produce identical row order for the same response delivered in three different array orders; determinism lane green on the new pty goldens.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.13 Malformed responses never edit a buffer: 4 × 50k fuzz iterations with byte-identity asserted after each.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s47.14 ≥ 240 assertions across the six new unit files; the milestone lane's summary line is present in CI output.
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

- `YEW-F-117` (Medium): the live-clangd harness treats the `/tmp` scratch-root
  alias and its canonical macOS path as distinct documents.

## Deferrals recorded against this front

none

## Verdict

NOT READY — Independent evidence review remains pending.
