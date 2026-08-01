# Sagitta

Sagitta is a speed-first, bespoke-first modal terminal text editor with an
arrow-key focus. Its scripting and macro language, **Fletch** (`*.fl`), is
designed so recorded edits are readable programs and programs can drive the
editor. The implementation is C11 using only the C standard library and POSIX.

Sagitta is currently **pre-1.0 scaffolding**. Editing, the terminal interface,
and Fletch execution land in later sprints; unimplemented surfaces fail with a
message naming the sprint that provides them.

## Build

```sh
make
make test
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

## Sister projects

- [Cgfried](https://github.com/tenseleyFlow/Cgfried)
- [fuss](https://github.com/FortranGoingOnForty/fuss)
- [facsimile](https://github.com/FortranGoingOnForty/facsimile)

Sagitta is licensed under the [GNU General Public License, version 3 only](LICENSE)
(`GPL-3.0-only`).
