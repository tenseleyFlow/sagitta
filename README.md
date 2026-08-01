# Sagitta

Sagitta is a speed-first, bespoke-first modal terminal text editor with an
arrow-key focus. Its scripting and macro language, **Fletch** (`*.fl`), is
designed so recorded edits are readable programs and programs can drive the
editor. The implementation is C11 using only the C standard library and POSIX.

Sagitta is currently **pre-1.0**. Its terminal stack, deterministic renderer,
PTY acceptance harness, piece-tree text engine, byte-exact file loading,
durable save paths, and crash journal are in place. Interactive editing and
Fletch execution land in later sprints; unimplemented surfaces fail with a
message naming the sprint that provides them.

## Build

```sh
make
make test
make torture
make MODULES="lsp ai"
```

The default build enables the `lsp`, `ai`, `fuss`, and `plugins` compile-time
modules. Build products are written under `build/`; the executables are
`build/sagitta` and its `build/sag` symlink.

Unicode behavior is generated from the checked-in Unicode 16.0.0 data under
`ucd/`. The generated `src/unicode/tables.c` is also committed deliberately:
a fresh clone builds offline with only a C compiler, while `make
unicode-tables` reproducibly regenerates the file for review and CI drift
checks. No network access, Python, locale data, or platform `wcwidth` is part
of the build or rendering contract.

## Data-safety contract

Files are loaded with ordinary full-read POSIX I/O—never `mmap`—and remain
byte-exact in memory apart from a detected UTF-8 BOM, which is restored on
save. CRLF, mixed line endings, invalid UTF-8, and binary bytes round-trip
without normalization.

Ordinary saves write and `fsync` a same-directory temporary before rename and
directory `fsync`. Symlinks, hardlinks, and unwritable-directory cases use a
fsynced state-directory backup plus in-place preservation. Per-buffer crash
journals are versioned, checksummed, append-only recovery logs. `make torture`
faults every save syscall boundary and runs external `SIGKILL` campaigns to
prove the destination is always old or new—or recoverable from journal and
backup—never silently corrupted.

## Terminal environment

- `SAG_TTY_PROBE=0` disables live capability queries.
- `SAG_PROBE_TIMEOUT_MS` changes the default 50 ms probe deadline for
  high-latency links. Probing races startup and never delays first paint.
- `SAG_TRUECOLOR=0` or `1` overrides truecolor environment detection.
- `SAG_CLIPBOARD=auto|osc52|wl|xclip|xsel|pb|none` selects the system
  clipboard backend. `cmd:<write-argv>[|<read-argv>]` runs a custom command
  directly without a shell.
- `SAG_OSC52=off|plain|tmux|screen` controls OSC 52 wrapping. `plain` is the
  escape hatch when a multiplexer consumes OSC 52 itself. OSC 52 is
  write-only; clipboard reads use a local subprocess.
- For tmux passthrough, use `set -g allow-passthrough on` on tmux 3.3 or
  newer. If tmux owns clipboard forwarding through `set -g set-clipboard on`
  instead, use `SAG_OSC52=plain` so tmux consumes the unwrapped sequence.
- `SAG_CLIPBOARD_TARGET=c|p|cp` selects the OSC 52 target,
  `SAG_OSC52_MAX` caps encoded payload size at 100,000 bytes by default, and
  `SAG_CLIPBOARD_TIMEOUT_MS` changes the 1,000 ms subprocess deadline.

The terminal lifecycle is terminfo-free. Raw mode restoration is armed for
normal close, `atexit`, internal-error reports, and fatal signals.

## Sister projects

- [Cgfried](https://github.com/tenseleyFlow/Cgfried)
- [fuss](https://github.com/FortranGoingOnForty/fuss)
- [facsimile](https://github.com/FortranGoingOnForty/facsimile)

Sagitta is licensed under the [GNU General Public License, version 3 only](LICENSE)
(`GPL-3.0-only`).
