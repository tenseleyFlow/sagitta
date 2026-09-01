# Sprint 57 Repository Closeout — 2026-09-01

## Status

Sprint 57 is **repository complete** through implementation commit `7781090`.
The implementation frontier advances to Sprint 57.5. This is not a claim that
the release evidence is complete: hosted CI for the unpushed commit chain and
the physical Raspberry Pi Zero 2 W corroboration remain open.

If either external row fails when it becomes runnable, Sprint 57 remediation
reopens before release. The evidence tails are tracked here and in
`.docs/embedded-runs.md`; they are not inferred from local or emulated runs.

## Core-preservation decision

No core Yew feature was removed to meet the footprint gates. Core editing,
Fletch, syntax highlighting, search, workspaces, macros, recovery,
deterministic rendering, terminal restoration, and their full test surfaces
remain present in `MODULES=""`.

The original 900 KiB minimal planning value was below the measured core code
floor: the pinned x86_64 GCC build contains more than that in `.text` and
`.rodata` alone. Amendment S57-A1 therefore sets the hard minimal cap at
1.5 MiB while retaining 900 KiB as a post-1.0 optimization ratchet. The
pinned stripped minimal binary is 1,463,968 bytes; full is 1,947,336 bytes
against the 2 MiB cap. Further footprint work must stop and rebaseline or
defer if it would require observable feature loss or weaker stability.

## Repository evidence

- The full native Apple-silicon suite passes: PTY, Fletch 38/38, scripts
  93 tests / 919 assertions / 0 failures with one intentional skip, package
  51/51, 2,000 round-trip seeds plus corpus, 432 syntax assets, 2,404 unit
  tests / 71,035,366 assertions / 0 failures, policies, smoke, and live-PTY
  clean-save torture.
- A clean independent `MODULES=""` tree passes the full applicable product
  suite: 1,945 unit tests / 70,056,311 assertions / 0 failures; all applicable
  PTY cases; Fletch 38/38; scripts 71 tests / 743 assertions / 0 failures with
  one intentional skip; round-trip, syntax, fuzz corpus, policies, smoke, and
  live-PTY clean-save torture.
- Allocation accounting is alignment-safe on Darwin arm64, coalesces equal
  source spellings across translation units, and proves the seeded renderer
  violation names its exact source location. Shipping allocation rows pass,
  including zero steady-state allocations for render, syntax, regex, layout,
  typing, navigation, and lifecycle.
- Generated runtime blobs and embedded images are byte-identical across
  repeated runs and generators compiled by GCC and Clang. The pinned GCC size
  ledgers regenerate byte-identically.
- Minimal-module boundary checks prove real module objects and symbols are
  absent, public shims are complete, and disabled user actions fail with the
  correct module name rather than silently succeeding.
- The musl static-PIE profile, resolver matrix, RSS ceilings, glibc-ism bans,
  size gates, deterministic image tooling, and native Apple-silicon alignment
  work are implemented and gated.
- Reproducible constrained-target evidence is recorded in
  `.docs/embedded-runs.md`: QEMU rows 1–11 pass at 64 MiB with peak `VmHWM`
  9,764,864 bytes; row 12 cleanly refuses at 32 MiB without an OOM kill.

## Open external evidence

1. Push the current commit chain when explicitly authorized and require the
   hosted matrix to pass on that exact SHA.
2. Run the arm64 static build on a Raspberry Pi Zero 2 W class 512 MiB target
   and record the exact binary/image, date, wall time, and peak RSS in
   `.docs/embedded-runs.md`.

These are Sprint 60 release blockers under S57-A4. They do not block the
repository implementation frontier from proceeding to Sprint 57.5.
