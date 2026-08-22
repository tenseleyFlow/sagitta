# PTY golden tests

The PTY suite runs the real binaries under a hand-built pseudo-terminal,
answers capability probes with the shared minimal VT parser, and compares the
interpreted screen with a text golden. Each case is executed independently
twice before its golden is consulted; a disagreement is an `unstable snapshot`
failure, not a golden update.

Run the suite through `make test-pty`. To deliberately refresh changed or
missing goldens, run:

```sh
YEW_PTY_UPDATE=1 make test-pty
```

Update mode prints `golden updated: <name>` for every rewritten file and exits
with status 1 even when every rewrite succeeds. Read the diff, rerun normally,
and only then commit it. Every commit that changes a golden must explain the
rendering-behaviour change; a golden diff is an observable terminal change.
Missing goldens never create themselves outside update mode.

The VT sequence set is intentionally closed. As a drill, temporarily seed the
renderer output with `CSI 5 L` (`ESC [ 5 L` in bytes). At least one PTY case
must fail with `unknown sequence: ESC [ 5 L`; removing the seed must restore a
green run. Do not make the VT accept a new sequence unless the sprint adding
that renderer sequence expands the pinned set in the same change.

The timeout cleanup drill is `YEW_PTY_BUDGET_MS=1 make test-pty`. It must fail
within the runner's bounded cleanup path, leave no `demo_paint` child alive,
and preserve the failing case's state directory. A following ordinary
`make test-pty` must be green. The focused `pty_timeout_reaps_child` unit also
runs `/bin/sleep 300` under a 50 ms budget and asserts `SIGKILL`, bounded reap,
an empty live-child list, and a closed master descriptor.

`YEW_PTY_CASE_BUDGET_MS` overrides the default 5-second per-case deadline.
The Valgrind lane uses 60 seconds per case and a two-hour global budget to
account for instrumentation overhead; ordinary runs retain the strict defaults.
The global value bounds the complete sweep while the per-case value remains the
hang detector. `YEW_PTY_EXCLUDE` omits cases whose names contain its value.
Valgrind excludes `notepad_restore_segv` and
`s32_bug_restores_the_terminal`: compiler and sanitizer PTY lanes cover those
deliberate fatal-signal contracts because Valgrind owns the fatal signal and can
hold the independently repeated child indefinitely.

Terminal profiles are deterministic (`modern`, `nokitty`, `nosync`, `dumb`).
The child receives only the exact pinned environment from Sprint 06; the
developer shell environment, including `COLORTERM`, is never inherited.
Failures preserve their per-execution `XDG_STATE_HOME` under `build/` so the
debug log remains available. Passing state directories are removed.
