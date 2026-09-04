# F02 TERM — terminal I/O

Status: closed
Baseline: `41fef4166fe6bf127f36b8b9f6eb653a454a28c1`
Opened: 2026-09-03
Scope: `src/term/`, `tests/pty/`
Owners read: Sprint 3, Sprint 4, Sprint 5, Sprint 6

The product-code baseline remained immutable. The audit-control changes in
this front add restore, termios, burst, and exact fuzz-count coverage only.

## Q1 — restore bytes and termios on every exit path

probed locally and across the hosted matrix, nothing found

- The real-pty cases cover normal quit, `yew_bug` exit 4, `SIGSEGV`,
  `SIGBUS`, `SIGABRT`, `SIGTERM`, `SIGTSTP`/`SIGCONT`, and the existing
  guardian recovery after `SIGKILL`.
- Every covered exit observes the one static restore blob. The VT must end
  outside the alternate and synchronized screens, with the kitty stack and
  all four input modes cleared and the cursor visible.
- `ptc_check_termios_unchanged` now compares `c_iflag`, `c_oflag`,
  `c_cflag`, `c_lflag`, input/output speeds, and every `c_cc` entry. This
  avoids comparing padding while making a failure name the exact field.
- The focused local runs passed all nine `restore_` cases plus
  `s32_bug_restores_the_terminal` on arm64 macOS. Hosted run `33826009408`
  then passed the full matrix under GCC, Clang, ASan/UBSan, Linux arm64,
  macOS arm64, musl, and `MODULES=""`.

## Q2 — async-signal-safe call graph

probed, nothing found

- `scripts/check-sigsafe.sh`: `sigsafe: ok`.
- Manual traversal covered `yew_tty_lifecycle_mask`,
  `yew_tty_guard_note`, `yew_tty_restore`, `yew_tty_fatal`,
  `yew_tty_signote`, `yew_tty_tstp`, and `yew_tty_cont` inside the marked
  region. Reached operations are assignments plus `sigemptyset`,
  `sigaddset`, `sigprocmask`, `write`, `tcsetattr`, `signal`, `raise`, and
  `sigaction`; none allocates, logs, formats, takes a stdio lock, or calls
  an unmarked yew helper.
- Restore blocks every lifecycle signal, retains the armed guard through
  the terminal write and `tcsetattr`, disarms only afterward, then restores
  the prior mask.

## Q3 — closed emitted sequence set and VT coverage

probed, nothing found in shipped behavior

- Hosted audit-control run `33826009408` drove the complete 451-case pty
  suite through the strict VT with zero parser errors. The five additions
  in this front — two fatal restore paths and three hostile burst paths —
  passed the standalone hosted PTY lane and the `MODULES=""` lane as well
  as the focused local runs. Every snapshot calls `validate_vt`, so an
  unknown sequence, nested/unmatched synchronized update, autowrap
  dependency, orphan wide cell, or ordinary primary-screen text is a hard
  failure.
- The suite includes all seven modes, panels, completion, pickers, group
  picker, context menu, FUSS, LSP, AI, themes, and degraded terminals. The
  tutor does not exist until Sprint 59 and therefore cannot be driven here.

## Q4 — synchronized-output envelope

probed, nothing found

- `build/fuzz_grid --iters=100000 --seed=5802` completed 100,000 mutation
  iterations and checked 195,437 rendered frames. Every nonempty frame had
  exactly one BSU and one ESU; every empty frame had neither; no nesting,
  imbalance, raw control, or unsupported renderer CSI occurred.
- `scripts/check-render.sh` passed. Static enumeration leaves mode 2026 in
  `render.c` plus `tty.c`'s capability probe, reply parser, and idempotent
  crash restore. No editor/UI module emits it.

## Q5 — adversarial typeahead and paste containment

probed, nothing found

- Existing live-editor gates still pass for one 4 KiB raw-key burst and
  one 4 KiB bracketed paste, each contributing one input frame.
- `audit_terminal_paste_256k` sends 262,144 payload bytes plus bracketed
  paste framing. It contributes exactly one content frame and lands at
  byte column 262,145.
- `audit_terminal_burst_resize` establishes the mandatory stopped/resumed
  resize repaint as a control, then repeats the lifecycle with a paste
  divided around the resize. The paste contributes exactly one additional
  input frame and the resized 100×30 grid lands at byte column 129.
- `audit_terminal_hostile_paste_undo` places a literal `CSI 201~` terminator
  before trailing key bytes. The exposed tail remains insert-mode text;
  one undo followed by save restores the original file byte-for-byte.

## Q6 — deterministic colour downconversion

probed, nothing found

- The focused colour/degradation suite passed 7 tests / 112 assertions.
  It pins canonical cube/ramp/ANSI points, the black cube-versus-gray tie,
  the ANSI black-versus-blue tie, environment tiers, and no-colour output.
- `yew_rgb_to_256` selects gray only on strictly smaller distance, so an
  equal distance remains the cube. `yew_rgb_to_16` scans indices 0→15 and
  replaces the winner only on strictly smaller distance, so equal distance
  remains the lower index. There is no sort, hash, locale, or iteration
  source outside those fixed arrays.
- Hosted runs `33815573832` and `33826009408` passed these product tests on
  both compilers and all four target lanes; the front's new tests do not
  alter either conversion routine.

## Q7 — primary-screen writes

See the contract conflict under observations. Ordinary editor cases retain
the strict default: primary-screen text sets `primary_written` and fails the
case unless it declares one of the pinned exceptions below.

## Unverified observations

- Sprint 6 §4's local closed-set table predates accepted output already
  present at the fixed baseline: s17 cursor shape/reset (`CSI Ps SP q`),
  s41 underline colour/reset (`SGR 58;2`/`59`), s12's out-of-band OSC 52
  writer, and s41's optional OSC 11 background query. The test VT correctly
  accepts the first three shipped families (OSC 11 has a fake-I/O unit
  oracle), but the prose inventory was not amended. The planning corpus is
  intentionally ignored and absent from a fresh clone, so a durable
  baseline-failing CI reproducer cannot read or repair that table; under
  Sprint 58's reproducer-first law this remains an observation, not an ID.
- Q3 asks this front to drive the tutor, but the campaign map and Sprint 59
  explicitly make `yew tutor` a next-sprint deliverable. Absence here is not
  a product stub or a finding against the Sprint 58 baseline.
- Q7's literal demand that `primary_written` remain false across the whole
  suite conflicts with three pinned behaviors: fatal diagnostics are
  written only after restoring the cooked primary screen (s03 §4), the
  interactive Fletch REPL deliberately scrolls on primary (s32 §2), and
  `TERM=dumb` has no alternate screen (s41 §9). Seven narrowly located
  `ptc_allow_primary` call sites cover those behaviors plus guardian tests;
  every ordinary editor case remains strict. Changing the pinned outputs to
  make the literal flag false would regress the owning contracts.

## Count

Raw 0 · deduped 0 · critical 0 · high 0 · medium 0 · low 0 · unverified 3.
