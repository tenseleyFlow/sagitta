# Closeout: F02 — Terminal and renderer

Date:            2026-10-12
Baseline commit: 4c99e13474c84d75eec3250d61412b53fa41ae00
Reviewer:        UNASSIGNED
Sprints covered: s03, s04, s05, s06

## DoD items (every numbered item from every sprint above)

- [ ] s03.1 `make && make test` green on gcc and clang, `-std=c11 -pedantic -Wall -Wextra -Werror`, ASan/UBSan lanes clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s03.2 Every §2 table row has a dedicated unit assertion; ≥ 30 assertions in `test_tty.c` overall.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s03.3 Restore blob byte-compared in a unit test; changing it breaks a test.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s03.4 Probe machine: 4 scripted-terminal fixtures (modern, kitty-only, sync-only, dumb) produce the expected `TtyCaps`; the dumb fixture reaches DONE only via the fake-clock deadline, and `probe_deadline` returns a value ≤ 50 that a poll loop could use as its timeout.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s03.5 Chunking-independence test passes for splits at every byte offset of every fixture stream.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s03.6 `pending` preservation: interleaved user bytes recovered byte-exact in all fixtures, including the ambiguous-prefix flush case.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s03.7 `scripts/check-sigsafe.sh` passes and is wired into `make test`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s03.8 `yew < /dev/null` (stdin not a tty) exits 1 naming Sprint 37; message text pinned by a smoke check.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s03.9 `YEW_TTY_PROBE=0` and `YEW_PROBE_TIMEOUT_MS` honored (unit-tested via injected env lookup on the probe-config reader).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s03.10 `grep -rn 'tcsetattr' src/ | grep -v term/tty.c` → empty: tty.c is the only termios site.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s03.11 Doc comment in `tty.h` states the restore guarantee and lists the four fatal signals + the atexit + prehook paths (the why carried in comments).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s04.1 `make && make test` green on gcc and clang under the locked flags; ASan/UBSan and valgrind lanes clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s04.2 `_Static_assert(sizeof(Key) == 32)` holds; `input.h` documents the `YEW_KEY_BASE` split and the `code`-vs-`text` divergence.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s04.3 ≥ 120 fixture vectors covering every row of §4, §6, §7, §8, §9; ≥ 300 assertions in `test_input.c`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s04.4 Chunking independence passes for splits at **every** byte offset of every fixture — identical events and identical `dropped` counts.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s04.5 Lone-ESC matrix green, including the kitty case asserting `yew_input_deadline() == -1` and that no deadline is ever armed.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s04.6 Enable blob byte-compared against §8; the kitty push is exactly `\x1b[>21u` and appears only when `caps.kitty_kbd`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s04.7 `make fuzz` runs `fuzz_input` 60 s with zero crashes, hangs or assertion failures under ASan/UBSan; all 8 properties are asserts, not comments.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s04.8 The security rule is proven mechanically: no `emit_key` call is reachable from the paste state (grep + the fuzz property).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s04.9 1 000 well-formed-but-unknown CSI sequences yield 0 events and `dropped == 1000`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s04.10 Progress law: 64 KiB of random bytes drains in ≤ 65536 iterations (asserted).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s04.11 `grep -rn 'read(\|poll(' src/term/input.c` → empty. The parser does no I/O; the event loop owns every syscall.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s04.12 Mouse coordinates leaving `input.c` are 0-based — `CSI <0;1;1M` must yield `col=0,row=0`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s05.1 `make && make test` green on gcc and clang, zero warnings under the locked flags; ASan/UBSan and valgrind lanes clean.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s05.2 `sizeof(Cell) == 20` and `sizeof(YewColor) == 4` assert at compile time; the §1 budget table and its why live in `grid.h` as comments.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s05.3 Round-trip property green for ≥ 5 000 fuzz iterations: replaying the emitted stream reconstructs `back` exactly.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s05.4 Each wide-glyph rule has a firing test: orphan-tail, orphan-head, last-column refusal, cursor snap-left.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s05.5 A frame with no cell or cursor-state change emits exactly 0 bytes; a cursor-only change emits its minimal state delta; a single-cell change emits ≤ 32 bytes (all asserted numerically).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s05.6 Colour: ≥ 40 `rgb_to_256` vectors and all 16 nearest-ANSI vectors pass; every grayscale ramp value round-trips to its own index; ties resolve per the documented rule.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s05.7 Determinism: two renders of one grid are byte-identical, and two put orders reaching the same state produce the same bytes; both in the determinism lane.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s05.8 No I/O in the renderer: `grep -rn 'write(\|fwrite\|printf' src/term/render.c src/term/grid.c` → empty (s03's `printf` ban for `src/term/` still passes).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s05.9 2026 is emitted only by `render.c`, only as a balanced pair: `grep -rn '2026' src/ | grep -v 'term/render.c\|term/tty.c'` → empty, and a 1 000-frame fuzz run counts BSU == ESU.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s05.10 `make perf` runs the §8 scenarios against `tests/perf/baselines/render.txt`; byte-count rows are hard failures, timing rows advisory until Sprint 56.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s05.11 Resize 80×24 → 40×12 → 80×24 forces a full repaint each time and the final grid is byte-identical to the original given the same puts.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s05.12 `render.c` emits no sequence outside the set Sprint 6's VT accepts — pinned by the fuzz assertion here and re-verified from the other side next sprint.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s06.1 `make test-pty` builds and runs green on gcc and clang, zero warnings under the locked flags; wired into `make test` and a CI `pty` lane. Sprint 1's hard-error stub is gone.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s06.2 No libutil: `grep -rn 'forkpty\|openpty\|-lutil' tests/ Makefile` → empty; §1 is the only pty-creation site.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s06.3 **MILESTONE first paint**: `build/demo_paint --scene basic` under the harness matches `tests/pty/goldens/paint_basic.golden` byte for byte on two consecutive runs.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s06.4 ≥ 12 registered pty cases covering the Testing Strategy list; every case has a golden and no golden is orphaned (`bans.sh` cross-checks `goldens/*.golden` against the registry, as it does unit registration).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s06.5 The closed set is enforced: a deliberately seeded `CSI 5 L` in `render.c` makes ≥ 1 case fail with `unknown sequence: ESC [ 5 L`; removing it restores green. The drill is written down in `tests/pty/README`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s06.6 `YEW_PTY_UPDATE=1 make test-pty` rewrites goldens, prints one line per file, and **exits 1**; without it a missing golden is a failure, never an auto-create; `bans.sh` rejects the variable in CI files.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s06.7 Timeout discipline: a case whose child runs `sleep 300` is killed at its budget and reported as a timeout within budget + 1 s, the runner exits 1, and no process survives (`pgrep -f demo_paint` empty afterwards).
      — VERDICT: blocking — independent closeout review pending.
- [ ] s06.8 `restore_crash` proves the restore guarantee end to end: the blob is in the child's output tail and the child is `WIFSIGNALED` with `SIGSEGV`.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s06.9 `input_keys_modern` and `input_keys_legacy` produce the same golden from the same key script.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s06.10 Every case snapshots twice and byte-compares before the golden comparison; a seeded nondeterminism (an uninitialized cell) is reported as "unstable snapshot".
      — VERDICT: blocking — independent closeout review pending.
- [ ] s06.11 valgrind `--track-fds=yes` reports no leaked descriptors across a full pty run; the runner's live-child list is empty at exit.
      — VERDICT: blocking — independent closeout review pending.
- [ ] s06.12 The child environment is constructed, not inherited: a unit test asserts the envp key set matches §2 exactly, and `COLORTERM=truecolor make test-pty` changes no golden.
      — VERDICT: blocking — independent closeout review pending.

## Open audit findings against this front

none

## Deferrals recorded against this front

none

## Verdict

NOT READY — Independent evidence review remains pending.
