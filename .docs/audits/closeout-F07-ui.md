# Closeout: F07 — UI and workspace

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s22, s23, s24, s25, s26, s27

## DoD items (every numbered item from every sprint above)

- [ ] s22.1 `make test` green: ≥ 40 layout/region unit assertions; both compilers, zero warnings.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s22.2 All pty goldens byte-exact across two consecutive runs (invariant 5).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s22.3 Split refusal: at 80×24, splitting past minimum messages and returns NULL; no 0-width leaf is ever drawn (assert in draw path).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s22.4 SIGWINCH 80×24 → 40×12 → 80×24 restores byte-identical rects and grid.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s22.5 Click-to-focus works through the registry: pty test feeds SGR click, focused pane changes, cursor lands on the clicked grapheme with a double-width char earlier on the line (the desync case).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s22.6 Drag resize: press/motion/release sequence moves a border exactly the motion delta in cells; Esc mid-drag restores the entry ratio.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s22.7 Unicode measurement and coordinate conversion remain centralized in `src/unicode/`; UI code performs geometry only over already-measured cell counts and contains no locale-width call, width table/range, or duplicate text-to-cell conversion.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s22.8 Every `ed.pane.*` command dispatches through the Sprint 13 registry (no direct key→function calls; grep for keymap table entries).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s22.9 Sprint 23 delivered the `ed.tab.*` command surface through the Sprint 13 registry and the pane-tree ownership boundary established here.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s22.10 10k-op fuzz run clean under ASan/UBSan; region table never queried between `frame_begin` and first `add` (assert counter in debug build).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s23.1 ≥ 50 unit assertions across test_tabs/test_strip; gcc+clang clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s23.2 Fuzz oracle run (10k ops, seed pinned + one random lane) green under ASan/UBSan.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s23.3 Pty goldens byte-exact twice in a row; the click-to-switch test passes with a two-CJK-tab strip (multibyte spans).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s23.4 `grep -rn "dirty\|modified" src/ui/tabs.c` shows no stored boolean — the marker is a call into undo state.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s23.5 Close of active tab with 512-op history frees the pane tree and buffer view with zero leaks (valgrind lane).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s23.6 Cap behavior: opening tab 513 fails with a message; `yew_tab_open` return checked at every call site (grep: no bare calls).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s23.7 Dirty-close prompt: `w` on a read-only file aborts the close and shows the I/O error; the tab survives with its text intact.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s23.8 Digit `ed.tab.goto 0` reaches tab 10; out-of-range N messages and stays put.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s23.9 Negative `YEW_REGION_TAB` payloads now identify groups as delivered by Sprint 24; a hand-built-region unit test proves sign decoding cannot confuse a stable group identity with a tab index.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s23.10 Two runs of the full pty suite produce byte-identical grids (invariant 5).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s24.1 ≥ 90 unit assertions across the three test files; gcc+clang clean; ASan/UBSan/valgrind lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s24.2 `grep -n "member" src/ui/groups.h` shows no member array/list/count field on `TabGroup`; `grep -rn "is_resident\|resident" src/ui | grep -v "("` shows no residency flag — both structural questions.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s24.3 Membership fuzz (5k ops, pinned seed + random lane) green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s24.4 Opening a 40-file group performs exactly 1 file read (counted via a test hook in file.c); switching to member 2 performs the 2nd.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s24.5 Walk-through reversibility test green: N rights then N lefts returns to start visiting the same sequence reversed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s24.6 `enter_group_at_edge` vs resume: both unit-tested; the mid-walk path never consults `last_active_member` (asserted via test hook).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s24.7 Digit jump: single digit switches in the same frame (no 500 ms lag — pty test asserts the grid changed before the window expires); `alt+1` `5` reaches tab 15; digit after landing in a group picks by ordinal.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s24.8 Picker ticks survive directory navigation (unit + pty); footer count always equals set size.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s24.9 Row-1/row-2 click agreement: pty test clicks every span of both rows with multibyte labels present; each lands on its payload.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s24.10 Empty groups cannot exist after any op sequence (fuzz asserts count > 0 for every live group id).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s24.11 All new commands dispatch via the registry; Sprint 53 delivered `ed.group.from_dir` through that same route and the group picker.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s24.12 Two consecutive full pty runs byte-identical (invariant 5).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.1 `make test` green under gcc and clang at `-std=c11 -pedantic -Wall -Wextra -Werror`, zero warnings; ASan/UBSan/valgrind/determinism lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.2 **Resume exactness**: the `resume_exact` pty test passes — grid byte-identical after quit/reopen, plus the structural assertions on tabs, panes, cursors (incl. `goal`), and viewports. Two consecutive full runs byte-identical (invariant 5).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.3 Restoring a 40-tab, 3-group workspace performs exactly **1** file read (test-hook counted); switching to a second member performs the 2nd.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.4 Group ids are remapped: the `7, 9, 12` fixture restores to live ids `1, 2, 3` with identical membership, ordinals and `last_active_member` resolution. `grep -n 'group_id = .*file' src/ws/` shows no direct assignment of a file id to a live id.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.5 Round-trip: for all ≥ 20 corpus documents, `emit(parse(d))` is byte-identical to the canonical form, and save→restore→save is a fixpoint. Unknown keys and `# comments` survive parse; comments are not re-emitted (documented).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.6 No floats anywhere in the format: `grep -rn '%f\|%g\|strtod\|atof' src/ws/` → empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.7 Every §7 row exercised: editor starts, exit 0, bad file renamed with a UTC timestamp, never deleted, exactly one status message, retention caps at 5 corrupt files.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.8 Atomicity: the state write issues write → fsync(file) → rename → fsync(dir) in that order (shim intercept log asserted, as in s08 DoD 2). Direct `rename()` and `fsync()` calls remain absent from `src/ws/`; state writers call the shared Sprint 8 atomic file API and own no second copy of the primitive.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.9 Never pollutes the repo: after a scripted session in a git checkout, `git status --porcelain` in the fixture workspace is empty, and `find <workspace> -name '*.fl' -newer <marker>` finds nothing.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.10 Single-writer lock: a second process in the same workspace restores normally, shows the ownership message once, and writes no state; a stale lock (pid gone) is taken over.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.11 Undo is a sibling: `grep -n 'sagu' src/ws/state_emit.c` shows only the reference fields; no undo payload bytes appear in any corpus document.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.12 Fuzz: 100 000 cases × 4 seeds clean; every cap breach classified as corruption rather than crashing or truncating silently.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.13 Sprint 36 retired the hand-written differential only after its final hand-written/Fletch 2×2 matrix passed; the retained state corpus round-trip exercises the shipping Fletch reader and writer and preserves frozen-format, rejection, and idempotence coverage.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s25.14 ≥ 130 assertions across the four unit files; per-workspace history merge tested at both `history.scope` values.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.1 `make test` green on gcc and clang, zero warnings at `-std=c11 -pedantic -Wall -Wextra -Werror`; ASan/UBSan/valgrind/determinism lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.2 Every row of the §1 scoring table has a passing assertion, and the ≥ 30-triple fuss-parity corpus matches exactly.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.3 `src` ranks the directory `src/` above `src/file.c` (explicit test), and no fuzzy path match ever outranks a prefix basename match.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.4 Walk determinism: two walks of the scrambled-creation-order fixture produce byte-identical path lists; the symlink-loop fixture terminates with `n_loops == 1`; `grep -rn 'nftw\|ftw(\|scandir' src/ws/` → empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.5 Every supported `.gitignore` row has an assertion; every unsupported row has an assertion pinning the documented divergence; a `node_modules/` fixture of 5 000 files is pruned with **one** `fstatat` and zero `opendir` calls (counted via a test hook).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.6 **Perf gate**: on 100 000 files, the first filter keystroke does ≤ 5 ms of in-frame work and every narrowing keystroke completes in ≤ 5 ms; baselines committed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.7 Narrowing equivalence: 10 000 random append cases produce results identical to a full rescan (the property that makes the optimization legal).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.8 Selection survives refiltering by payload — the pty test types a character that reorders the list and presses Enter, landing on the item that was highlighted.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.9 The filter line is the s18 widget: `grep -n 'char .*\[\(64\|128\|256\)\]' src/ui/picker.c` shows no ad-hoc input buffer, and prompt edits dispatch through the registry.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.10 Three picker instances live and keyboard-complete; Sprint 10's undo branch picker deferral is closed (its rows come from `yew_undo_list` / `yew_undo_describe` with an injected `now`).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.11 Previewing a file creates no `Buffer` and no `TextBuf` (asserted via the s24 file-read test hook: previewing 20 files performs 20 `pread`s and 0 buffer allocations).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.12 Type-to-jump: appends inside 500 ms, replaces after, exact match under the cursor does not move, deadline clears the hint with no keys (mocked clock).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.13 Later owning sprints delivered all four picker surfaces: symbol, plugin/package, F-mode, and command discovery are live through the shared picker or type-to-jump machinery, with no second list event loop or ad-hoc filter editor.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s26.14 Two consecutive full pty runs byte-identical (invariant 5); ≥ 150 assertions across the five unit files.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.1 `make test` green on gcc and clang, zero warnings at `-std=c11 -pedantic -Wall -Wextra -Werror`; ASan/UBSan/valgrind/determinism lanes green.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.2 **Mouse-event dispatch remains singular**: terminal decoding produces `YEW_EV_MOUSE`, exactly one `src/edit/loop.c` branch calls `yew_mouse_event`, and `src/ui/mouse.c` owns routing; region producers and UI state consumers create no second event-dispatch path.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.3 Every row of the §2 region table has a passing unit assertion; rows marked ignored are proven inert (the event changes no state).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.4 Captured-identity law proven twice: a drag committed after the region table changed under it, and a context-menu row invoked after the strip scrolled — both act on the originally targeted `tab_id`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.5 Wheel over an unfocused pane scrolls that pane, leaves focus and the cursor untouched, and `grep -n 'yew_vp_follow' src/ui/mouse.c` → empty.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.6 Double-click selection equals `yew_unit_word.span` and triple-click equals `yew_unit_line.span` for all ≥ 12 corpus texts; the resulting state is H mode with the corresponding unit borrowed (asserted via the mode chip in a pty golden, not just internally).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.7 Dwell: 400 ms opens the member strip, 399 ms does not; a drag passing over three groups in 300 ms opens none of them.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.8 Drag never mutates `Tabs.v` before release: a unit test snapshots the array at every motion event of a 50-step drag and asserts equality with the pre-press snapshot.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.9 Context menus: geometry clamps at all four edges, never flips; a disabled row is drawn greyed and is unreachable by pointer and by `↑`/`↓`; menus cannot both be open.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.10 **Invariant 9**: every row of §8's audit table has a working keyboard binding, verified by a script test that performs each action with keys only under `YEW_MOUSE=0` and reaches the same state as the mouse path.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.11 `NO_COLOR=1` goldens contain **zero** colour SGR parameters (`grep -c '38;2\|38;5\|\[3[0-7]m\|\[9[0-7]m'` → 0) while remaining legible (active/selected still distinguishable — asserted by an attribute check, not by eye); the 16-colour and `YEW_ASCII=1` variants exist for every element in §7's checklist.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.12 Chrome review complete: all 12 goldens in §7 exist, ×4 variants, and two consecutive full pty runs are byte-identical (invariant 5).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.13 A 1 000-event motion burst produces ≤ 1 render and zero allocations in `mouse.c`; fuzz run (100 000 events × 4 seeds) leaves no stuck drag state.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.14 Later owning sprints delivered the staged UI surfaces: document context menus, F-mode mouse, and tab-into-pane use the shared router and keyboard-equivalent commands; LSP hover-on-pointer remains absent while `ed.lsp.hover` is the live keyboard route.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s27.15 ≥ 140 assertions across the four unit files.
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

none

## Deferrals recorded against this front

none

## Verdict

NOT READY — Independent evidence review remains pending.
