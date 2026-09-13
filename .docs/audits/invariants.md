# Invariant re-verification

Baseline: `b3f32645e0456dca1a90f73e4e4f2c2fc64003b3`

All fifteen fronts are closed and cross-cutting verification is in progress.
A pending row is not a verdict.

| # | Invariant | Status | Verdict | Findings |
|---:|---|---|---|---|
| 1 | No data loss, ever | pending | — | — |
| 2 | No byte confusion | pending | — | — |
| 3 | No silent stubs | complete | VIOLATED | YEW-F-014, YEW-F-075 |
| 4 | Latency budgets are CI gates | complete | VIOLATED | YEW-F-072, YEW-F-073 |
| 5 | Deterministic rendering | pending | — | — |
| 6 | Terminal restore | complete | HOLDS | — |
| 7 | Bespoke first | complete | HOLDS | — |
| 8 | Single-threaded core | complete | HOLDS WITH FINDINGS | YEW-F-032 |
| 9 | Modal paradigm first | complete | HOLDS | — |
| 10 | Recorder/Fletch round-trip | complete | VIOLATED | YEW-F-023 |

## 3. No silent stubs

Verdict: **VIOLATED: YEW-F-014, YEW-F-075**.

The interaction session built all 16 subsets of `lsp ai fuss plugins` in
isolated build trees on arm64 macOS with Apple clang 21. Each build ran its
complete compiled unit suite, every applicable audit fixture, and the CLI
smoke suite. Bit positions below are LSP, AI, FUSS, and plugins respectively.

| Mask | Modules | Commands | Natives | Unit tests | Audit tests |
|---|---|---:|---:|---:|---:|
| `0000` | none | 374 | 181 | 2,046 | 72 |
| `0001` | plugins | 374 | 181 | 2,110 | 76 |
| `0010` | fuss | 374 | 181 | 2,217 | 72 |
| `0011` | fuss, plugins | 374 | 181 | 2,281 | 76 |
| `0100` | ai | 374 | 181 | 2,163 | 72 |
| `0101` | ai, plugins | 374 | 181 | 2,227 | 76 |
| `0110` | ai, fuss | 374 | 181 | 2,334 | 72 |
| `0111` | ai, fuss, plugins | 374 | 181 | 2,398 | 76 |
| `1000` | lsp | 374 | 181 | 2,204 | 72 |
| `1001` | lsp, plugins | 374 | 181 | 2,268 | 76 |
| `1010` | lsp, fuss | 374 | 181 | 2,375 | 72 |
| `1011` | lsp, fuss, plugins | 374 | 181 | 2,439 | 76 |
| `1100` | lsp, ai | 374 | 181 | 2,308 | 72 |
| `1101` | lsp, ai, plugins | 374 | 181 | 2,372 | 76 |
| `1110` | lsp, ai, fuss | 374 | 181 | 2,479 | 72 |
| `1111` | lsp, ai, fuss, plugins | 374 | 181 | 2,543 | 76 |

The aggregate was 36,764 unit-test executions and 1,168,505,592 assertions,
with zero unexpected failures, plus 1,184 audit-fixture executions with zero
harness failures. The 374-line command inventory and 181-line native inventory
were byte-identical across all profiles (SHA-256
`34decd9ff81d218695f565ab59947601880ad36af5c75379fabe06a1dd03a344` and
`b465ae074dbb94f31cc1a49b8f677c604865edfb7305580168cd8207a2e5c200`).

The session separately invoked every `fl` frontend form and every `syn`
subcommand, including the deliberately incomplete syntax-coverage diagnostic,
in every build. It invoked all `plug` and `pkg` verbs in every build as well;
profiles without plugins returned the canonical module error for every verb.
The option-table inventory and command-boundary units ran in each profile.
The default-profile conformance rerun passed 38/38 Fletch files and all seven
native/spec/grammar/error/opcode/target/ledger coverage checks; the script
coverage report regenerated deterministically.

The broad matrix does not erase two exact exceptions:

- `YEW-F-014`: when LSP is stripped, `ed.lsp.complete` opens core index
  completion instead of returning the module hard error required of the
  `ed.lsp.*` surface.
- `YEW-F-075`: when a module is stripped, module-owned options remain
  inconsistent. `ai.enable`, `lsp.open_in`, and `git.ascii_glyphs` are
  accepted and stored as inert state, while `plug.verify_on_load` reports a
  generic unknown option instead of the plugins-module refusal.

The minimal audit run reproduced both as hard XFAILs. `YEW-F-075` is the
Critical user-reachable silent stub; all other exercised absent-module routes
failed loudly and the stale-Sprint-message review from F15 q9 found no landed
surface still presented as deferred.

## 4. Latency budgets are CI gates

Verdict: **VIOLATED: YEW-F-072, YEW-F-073**.

The sprint's designated-runner session cannot produce a performance verdict
from the committed tree. Both normative entry points were invoked in strict
mode before any advisory substitution:

| Runner ID | Strict result |
|---|---|
| `perf-x86_64-linux-gnu` | exit 75: `designated calibration reference is unavailable; no verdict` |
| `perf-arm64-linux` | exit 75: `designated runner baseline is unavailable: tests/perf/baselines/perf-arm64-linux.txt; no verdict` |

The failure is structural. `tests/perf/calib-reference.txt` and
`tests/perf/calib-reference-arm64.txt` do not exist, the committed x86_64
baseline declares `scale_permille=0 c1=0 c2=0 c3=0`, and the arm64 baseline
does not exist. Both designated workflow jobs are additionally disabled
unless repository variables opt them in. Consequently the required 30-run
noise-floor calculation cannot begin, and neither the 200,000-keystroke
worst-state session nor its fake-clock comment-bomb counterpart can receive a
hard designated verdict. This is `YEW-F-072`; treating the hosted advisory
measurements as equivalent would conceal rather than resolve it.

The gate machinery below that broken evidence chain does behave as designed.
`make perf-s56-gate-selftest` passed all 12 deterministic policy cases plus
the startup, profiler cross-check, aggregate-gate, update, run-suite, and
baseline-guard self-tests. In particular, seeded extra-frame and 2 ms
regressions fail in designated mode, while advisory timing overruns only
warn. The current hosted performance lane and the exact-baseline CI run are
useful functional/sanity evidence, but by contract they are not latency
evidence for release.

`YEW-F-073` independently breaks the immutability side of the gate promise.
Its hard XFAIL creates an isolated baseline-only commit named `Refresh
numbers`, doubles the recorded measurements without an old-to-new rationale,
and proves that the real history guard accepts it. A threshold that can move
without the required evidence is not a stable CI gate even if its comparison
algorithm is correct.

The complete audit reproducer run retained both rows as XFAIL and reported 76
tests with zero harness failures. This verdict does **not** claim that yew
misses a user-facing latency budget; it says the current tree cannot prove or
reliably preserve such a verdict. Remediation remains assigned to Sprint 59.

## 6. Terminal restore

Verdict: **HOLDS** on the audited baseline.

The permanent `make torture-tty-restore` target exercised the exact 8 × 4
signal/moment matrix on arm64 macOS and x86_64 Linux. All 32 trials passed on
each platform (64 executions total) for `SIGSEGV`, `SIGBUS`, `SIGABRT`,
`SIGTERM`, `SIGINT`, `SIGQUIT`, `SIGHUP`, and `SIGKILL` at these boundaries:

| Moment | Live-boundary proof |
|---|---|
| Mid-render | An unsaved edit and its journal were settled first. A resize then forced a distinct repaint; the torture interposer wrote through BSU (`ESC[?2026h`) and stopped yew before ESU. The parent proved the final frame state was an unmatched BSU before sending the tested signal. |
| Inside the fatal handler | `SIGTERM` entered the real fatal path. The interposer stopped its first restore write immediately after the kitty-keyboard pop (`ESC[<u`) and before the complete restore blob, then the parent delivered each tested signal as the second signal. An explicit async-signal-safe acknowledgement removes signal-order assumptions from this boundary. |
| Filter holding typeahead | A real `/bin/sh` filter published its process group, slept, and kept `cat` live. The parent injected literal `iQUEUED` typeahead and proved it was neither rendered nor dispatched before delivering the signal. |
| LSP shutdown budget | The isolated fake LSP completed initialization, received `shutdown`, published that boundary, and delayed its response for one second—beyond yew's 500 ms shutdown budget—while the signal was delivered. |

Every trial captured the terminal stream from its boundary and required the
complete ordered restore sequence: kitty keyboard pop; bracketed-paste,
mouse, focus, and synchronized-update disable; SGR reset; block-cursor reset;
alternate-screen exit; and cursor show. The harness also compared the final
input, output, control, and local termios flags, input/output speeds, and every
control character with the exact pre-yew values. Non-LSP rows used an explicit
empty-server config, so no host `clangd` or user configuration could perturb
the result.

`SIGKILL` cannot be handled by the editor process. In all four `SIGKILL`
moments the independent terminal guardian emitted the same complete restore
sequence and restored termios. A fresh shell-side `reset` on the same PTY then
exited successfully. Finally, the torture checker reopened the journal state
and recovered the exact unsaved `Xbase\n` bytes over the still-on-disk
`base\n`; all four journal-recovery checks passed on both platforms.

The x86_64 run used GCC 13.3.0 in the existing Ubuntu 24.04 audit guest. The
committed harness archive was SHA-256
`3b5ee0ace5629be60477ffb1fb5876070f16afcd3aa60a667d06025c5a2edb0c`;
the guest verified that hash before extracting it. The arm64 run used Apple
clang 21. The module-free warning-clean `torture-build` also passed, while the
runtime target correctly skips profiles without LSP and static musl profiles
that cannot preload the test interposer.

The pre-existing nine-case `restore_` PTY slice passed, covering ordinary
quit, crash, bus error, abort, suspend/resume, notepad-mode termination,
segmentation, suspension, and kill. `scripts/check-sigsafe.sh` also passed.
No terminal-restore exception or invariant-6 finding remains open.

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

## 7. Bespoke first

Verdict: **HOLDS** on the audited baseline.

The product link was inspected on every supported release target. The dynamic
GNU/Linux build has only `libc.so.6` and `libm.so.6` as `DT_NEEDED` entries;
the latter is the platform C math library required by `src/fl/stdmath.c`, not
a third-party dependency. The musl release is a static PIE with no `NEEDED`
entries or undefined symbols. On arm64 macOS, both `otool -L` and the load
commands show only `/usr/lib/libSystem.B.dylib`.

| Target | Evidence | Runtime dependency result |
|---|---|---|
| x86_64 Linux/glibc | `ldd` plus `readelf -d` in the existing Linux audit guest | libc, libm, ELF loader only |
| arm64 Linux/glibc | exact successful link command from CI run `34699266067`, job `103568015395` | project objects plus `-lm`; no external link input |
| x86_64 Linux/musl | CI job `103568015285`; `verify-static-pie.sh` | static PIE; no `NEEDED`, undefined, or executable-stack entry |
| arm64 macOS | local `otool -L`, load commands, and full linker map | libSystem only |

The local arm64 map contains 12,003 lines. Its complete object inventory is
the project's objects followed only by Apple SDK text-based stubs for libm,
libSystem, compiler-rt, dyld, and the libc/kernel/malloc components that make
up libSystem. No Homebrew, package-manager, or other third-party object,
archive, dylib, or symbol provider appears. The hosted arm64 Linux link line
was used because the pre-existing local arm64 Linux VM entered its VZ running
state but could not bring up guest SSH; it was returned to its prior stopped
state without changing the guest. The successful hosted job is pinned to the
same baseline and exposes the entire final link command.

Dependency-adjacent behavior also passed its missing-binary checks:

- `curl`: `ai_curl_probe_messages_and_cache` passed 108 assertions and pins
  the actionable `curl ... not in $PATH` diagnostic plus the local-model
  alternative.
- `git`: `symwalk_fallback_caps_skips_and_repeat_interning` passed 76,384
  assertions with `PATH=/definitely/no/git`, reported the fallback, and still
  populated the workspace symbol index; the runtime taxonomy separately
  reached and named the `git unavailable` state.
- `$SHELL`: `job_shell_resolution_prefers_env` proves that a nonempty
  environment value is the selected executable and that an empty value falls
  through safely. `job_exec_failure_is_not_exit_127` proves a missing selected
  executable becomes `YEW_JOB_EXECFAIL` with `ENOENT`, rather than masquerading
  as exit 127; the shell result renderer formats that state as
  `cannot run <command>: <system error>`.

The Makefile's sole product library argument is `-lm`; `-ldl` belongs only to
the Linux fault-injection test helper. The source and build review found no
hidden product link dependency and no invariant-7 finding remains open.

## 8. Single-threaded core

Verdict: **HOLDS WITH FINDINGS: YEW-F-032**.

The sprint's exact source scan,
`grep -rn 'pthread\|threads\.h\|_Thread_local\|atomic_' src/`, found no
thread API. Its matches are the five `volatile sig_atomic_t` signal flags in
`src/term/tty.c` and identifiers describing atomic file replacement in the
text, plugin-package, and workspace-trust paths. `sig_atomic_t` is the C
signal-handler scalar type, not the C11 atomic or thread API. The arm64 macOS
shipping binary's undefined-symbol inventory likewise contains no pthread,
thread, dispatch, or atomic symbol.

The cross-subsystem runtime proof was built from exact audit-control commit
`3db370c6` with GCC 13.3.0 on an x86_64 Ubuntu 24.04 guest. In one real `Ed`
process it held all of the following active at the observation point:

- the `fakelsp` subprocess had completed initialization and reached
  `YEW_LSP_READY` for a real C file;
- an HTTP AI request had received its first NDJSON token, exposed a live
  ghost, and remained open behind a 30-second server delay;
- `yew_git_refresh` had an in-flight job in yew's job table; and
- three additional `/bin/sh -c "sleep 30"` jobs remained live in that same
  job table.

Only after those conditions were simultaneously true did the child parse
`/proc/self/status`. It observed `Threads: 1`. The permanent regression test
reported `PASS invariant_single_threaded_live_subsystems` and `1 tests, 3
assertions, 0 failures`; its macOS build supplies a platform stub because the
normative kernel observation is Linux `/proc`.

`YEW-F-032` prevents an unconditional clean verdict. Its XFAIL proves that
the release ban can miss a token-pasted `pthread_create`, so the product tree
holds the invariant while its textual enforcement control remains incomplete.
That Medium gate-honesty finding remains Sprint 59 work; this session found no
product thread and no hidden library thread.

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
