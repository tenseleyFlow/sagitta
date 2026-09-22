# Sprint 57.25: Shell Completion III — Learning From `--help`

## Prerequisites

- **Sprint 57.24** — `YewCompSpec` and its in-memory node tree;
  `yew_compspec_get` (user override, then shipped, then alias index);
  `yew_compspec_resolve` and `YewSpecPoint`; the routing amendments of
  57.24 §4; the generator job discipline of 57.24 §5 (argv form,
  `YEW_SINK_CALLBACK`, `internal`, ≤ 1 job per key, ≤ 4 completion jobs in
  total, arrivals never edit the line, `…` pending marker as state).
- **Sprint 57.23** — `YewShCtx`, the `YEW_COMP_SHELL` dispatcher, the EXEC
  source's PATH resolution ("first PATH element wins").
- `fl_data_write` (`src/fl/data.h`) for the on-disk cache format;
  `yew_xdg_cache_dir`; the option table in `src/edit/option.c`
  (dotted global names such as `statusline.column`, `YEW_OPT_ENUM`).
- Binding: invariants 1 (the cache is written atomically), 4, 5, 7.

## Goals

A spec covers the commands someone wrote one for. The user's own tools —
`shithub`, `lupin`, `fac`, `fackr` — will never ship one, and neither will
most of what is on anyone's `PATH`. But nearly every modern CLI prints its
subcommands and flags in a two-column `--help` table. This sprint runs
`<command> --help` (asynchronously, safely, once per installed version),
parses it into the SAME in-memory spec tree 57.24 resolves against, and
caches the result on disk keyed by the executable's identity. After the
first use, a spec-less command completes as fast as a spec'd one.

Measured on the user's tools: `shithub --help` has 27 parseable rows,
`lupin --help` 13, `fac --help` 5, `fackr --help` 0 (a different layout —
the negative case). All four are native Mach-O executables; `brew` is a
shell script and has a hand-written spec, so it never reaches this layer.

Deferred: fish as an oracle → 57.26 (it is consulted BEFORE this layer
when present; see 57.26). Man-page parsing: non-goal.

## Deliverables

### 1. When a command's help may be run — `src/ui/comphelp.c`

Running a program to ask how it works is not free of risk: a hand-rolled
script may ignore `--help` and do its real job. The policy is a global
option, default `native`:

```c
{"shell.complete_help", YEW_OPT_ENUM, YEW_OPT_GLOBAL, OPT_ENUM("native"),
 complete_help_values /* "native", "all", "off" */, ...}
```

| Value | Runs `--help` for |
|---|---|
| `native` (default) | executables whose first bytes are an ELF or Mach-O magic (table below); never a script |
| `all` | any executable file, scripts included |
| `off` | nothing |

In every mode, NEVER for: a command that has a spec (57.24 wins); a name in
the denylist `rm rmdir dd shred srm wipefs mkfs fdisk diskutil format
shutdown reboot halt poweroff kill killall pkill su sudo doas passwd chsh
login logout exec reset`, matched on the basename and on any name starting
`mkfs.`; a command word that is not an executable regular file after PATH
resolution.

| Magic (first 4 bytes) | Format |
|---|---|
| `7F 45 4C 46` | ELF |
| `FE ED FA CE`, `FE ED FA CF`, `CE FA ED FE`, `CF FA ED FE` | Mach-O 32/64, both byte orders |
| `CA FE BA BE` | Mach-O universal (fat) |
| `23 21` (`#!`) | script — excluded under `native` |

Why `native` and not `all`: a compiled CLI almost always comes from an
argument-parsing framework that implements `--help`; the scripts on a
`PATH` are where hand-rolled `case "$1"` parsing lives. `all` exists for
users who want the coverage and accept that.

### 2. Running it — `src/ui/comphelp.c`

Through 57.24 §5's job discipline, keyed by (resolved realpath, subcommand
path). Differences from a generator:

- `argv` = the RESOLVED executable path, then the subcommand words the
  user has typed (see §4), then `--help`. Never the bare name: PATH could
  change between resolution and spawn.
- `timeout_ms = 1000`, `collect_max = 256 KiB`, no stdin (EOF at once, so a
  tool waiting on input exits instead of hanging).
- `env_set = { "NO_COLOR=1", "TERM=dumb", "COLUMNS=200", "PAGER=cat",
  "MANPAGER=cat", "GIT_PAGER=cat", NULL }`. `COLUMNS=200` stops clap and
  cobra wrapping a description onto a second line.
- Parse stdout; if it yields nothing, parse stderr (Go's `flag` package and
  most BSD tools print usage there). Exit status is ignored: many tools
  exit 1 or 2 after printing help.

### 3. The parser — `src/ui/comphelp.c`

Input: bytes. Output: a node (subcommands, flags) in 57.24's in-memory
form. No regex engine; a hand-written line scanner, because the rules are
positional and the engine would only obscure them.

**A row** is a line that starts with at least two spaces or a tab, then a
TERM, then two or more spaces (or a tab), then a DESCRIPTION. A following
line indented further than the term, with no term of its own, continues
the description (keep the first 60 bytes of the joined text). A term with
no description on its line but an indented description on the next line
is the Go `flag` layout: join them.

**Reference: the layouts this must parse** (each is a fixture file in
`tests/unit/fixtures/help/`, captured from a real tool, with the expected
node as a sibling `.expect` file):

| Layout | Subcommand rows | Flag rows |
|---|---|---|
| clap (Rust) | `Commands:` then `  build   Compile…` | `  -o, --output <FILE>  desc` · `  -h, --help  Print help` |
| cobra (Go) | `Available Commands:` then `  build       desc` | `  -o, --output string   desc` (a type word means it takes a value) · `Global Flags:` → `global` |
| argparse (Python) | `  {build,run,test}` choice line, then `    build   desc` rows | `  -o OUT, --output OUT  desc` |
| click (Python) | `Commands:` then `  build  desc` | `  -o, --output TEXT  desc` |
| commander (Node) | `  build [options] <file>   desc` (strip `[…]` `<…>`) | `  -o, --output <path>  desc` |
| GNU (`ls --help`) | none | `  -a, --all   desc` · `      --block-size=SIZE  desc` |
| Go `flag` | none | `  -o string` then `    \tdesc` on the next line |
| wolf / custom | rows under any heading: `  build          compile…` | as clap |
| BSD usage only | none | none → a NEGATIVE result |

**Classifying a term.**

- Starts with `-`: a FLAG row. Split the term on `, ` and on single spaces
  that precede another `-`. Each part is `-x`, `--long`, `--long=META`,
  `--long[=META]` (→ `arg_optional`), `--long META`, `-x META`. A META is
  `<…>`, `[…]`, an ALL-CAPS word, or one of cobra's type words `string
  strings stringArray int int32 int64 uint float duration ip`. (`bool`
  means NO value.) A META of `<file>` `<path>` `FILE` `PATH` → arg kind
  `path`; `<dir>` `DIR` `DIRECTORY` → `dir`; a META of the form
  `{a,b,c}`, `[a|b|c]` or `WHEN` followed in the description by
  `[possible values: a, b]` or `(always|never|auto)` → `values`; anything
  else → `none`.
- Otherwise, a SUBCOMMAND row only if the term's first word matches
  `^[a-z][a-z0-9_-]*$` and the term is not an ALL-CAPS metavar. A trailing
  `, alias` becomes an alias. `[options]`, `<…>`, `[…]` after the word are
  stripped (commander).
- An argparse choice line `{a,b,c}` contributes its names even when the
  rows below it are absent.
- A row under a heading containing `example` (case-insensitive) is ignored
  — example command lines look like rows and are not subcommands.

**Pitfall — `help` pages that page themselves.** `git --help` execs `man`.
`MANPAGER=cat`/`PAGER=cat` make that terminate; the timeout is the
backstop. (git has a spec, so this is defence in depth.)

**Pitfall — ANSI and backspace overstrike.** `NO_COLOR` is widely but not
universally honoured, and `man` output rendered to a pipe can use
`X\bX` overstrike for bold. Strip CSI sequences and `\b`-overstrike pairs
before classifying; a row whose term contains an ESC byte after stripping
is dropped.

### 4. Laziness down the tree

The root comes from `cmd --help`. A subcommand node discovered that way has
no children or flags yet. When resolution (57.24 §3) descends INTO such a
node — the user typed `shithub auth ` — request `cmd auth --help` for it,
under its own key. Until it answers, the node offers nothing (and the `…`
marker shows). **Pitfall:** only descend on names the parent's help
actually listed. Running `cmd <arbitrary user word> --help` would execute
whatever the user happened to type as a subcommand.

### 5. The cache — `src/ui/comphelp.c`

```
$XDG_CACHE_HOME/yew/completions/help/<16 hex>.fl
```

Key = FNV-1a 64 of `realpath \0 st_mtime \0 st_size \0 sub \0 sub …`, so a
reinstalled or upgraded tool misses the cache and re-learns. File content:

```
{ help_cache: 1, exe: "/opt/homebrew/bin/fac", mtime: 1757000000,
  size: 412345, argv: ["auth"], empty: false,
  node: { subcommands: [...], flags: [...] } }
```

- `empty: true` records a NEGATIVE result, so `fackr` is asked once, not on
  every Tab.
- Written atomically (temp file in the same directory, `fsync`, `rename`),
  through whatever helper the state files already use — do not add a
  second one. A torn write must never be read back as a spec.
- On read, validate `help_cache`, `exe`, `mtime`, `size` against the key's
  inputs; any mismatch is a miss (and the stale file is replaced).
- At most 2000 files; on write past that, delete the oldest by mtime down
  to 1800. The cache directory is created on first write only.
- New command `ed.shell.complete_forget` with an optional argument (a
  command name; none means every entry), plus an E-mode spelling in the
  existing abbreviation table. Not `YEW_CMD_RECORDABLE` — it touches disk,
  not the buffer — so it takes a named `D(...)` roundtrip exclusion and a
  `command_name_valid` verb. Invariant 9: this is how a user makes yew
  re-learn a tool without finding the cache directory.

### 6. Prewarm — `src/ui/cmdline.c`, `src/ui/comphelp.c`

The first Tab on a never-seen tool should not be the one that pays. When,
on the IDLE tick (`yew_cmdline_comp_tick`, never the keystroke path), the
caret sits in ARGUMENT position of a command that has no spec, no fish
answer (57.26) and no cache entry, and policy allows, request its root
help. At most one prewarm in flight; it counts toward 57.24's four-job
cap. The spawn is the expensive part (`posix_spawn` ~1 ms); keeping it off
the keystroke path is what keeps invariant 4.

### 7. Precedence — `src/ui/cmdcomp.c`

For an ARGUMENT-position completion, the first that answers:

1. a spec (57.24) — user override, then shipped;
2. fish (57.26), when present and it has rules for the command;
3. this sprint's help-derived node (cache, else a job);
4. 57.23's behaviour (defaults table, then paths).

While 3 is pending, 4 answers and the `…` marker shows. When 3 arrives,
57.24 §5's arrival rule applies: repaint if the context still matches,
never edit the line.

### 8. Defer

- Fish → 57.26. Man pages → non-goal.
- Parsing `usage:` synopsis lines (`[-abc] [--emit=a|b]`) for BSD tools and
  as a supplement: named, not in this sprint. Row tables carry most of the
  value; synopsis grammar is a sprint of its own.

## Testing Strategy

- **Unit, parser (`tests/unit/test_comphelp.c`)**: one fixture per layout
  row above, each a captured real help text with an `.expect` file listing
  subcommands (name, aliases, desc) and flags (spellings, arg kind,
  `arg_optional`, `global`). Add the four user tools' actual outputs
  (`shithub`, `lupin`, `fac`, `fackr` — capture them into fixtures once;
  the test never runs them): `shithub` yields ≥ 20 subcommand rows, `fackr`
  yields a negative result. Plus hostile fixtures: ANSI-coloured, `\b`
  overstrike, CRLF line ends, a 300 KiB input (truncated at the cap, no
  crash), invalid UTF-8, a line with 10 000 spaces.
- **Unit, policy**: a fixture native executable (build a tiny C program in
  `build/` at test time, or copy `/bin/echo`) runs under `native`; a
  `#!/bin/sh` fixture does not; the same script runs under `all`; nothing
  runs under `off`; a denylisted name never runs in any mode; a command
  with a spec never runs.
- **Unit, laziness**: `tool sub ` requests `tool sub --help` only when the
  root's help listed `sub`; `tool unlisted ` requests nothing.
- **Unit, cache**: hit after first parse; upgrade (touch the executable's
  mtime) misses; `empty: true` suppresses a rerun; a truncated cache file
  is a miss, not a crash; the 2000-file bound prunes to 1800;
  `ed.shell.complete_forget` with `fac` deletes only `fac`'s entries.
- **Unit, prewarm**: entering `fac ` schedules exactly one job on the idle
  tick and none on the keystroke path (assert the job count after the
  keystroke and again after one tick).
- **Fuzz, `tests/fuzz/fuzz_comphelp.c`** + `make fuzz-comphelp`: arbitrary
  bytes into the parser; no crash, no ASan/UBSan report, bounded output.
- **PTY**: `s57_25_help_subcommands` (a fixture native tool on a fixture
  PATH whose help is a clap layout; `tool bu<Tab>` → `tool build `),
  `s57_25_help_pending` (the `…` marker before the answer lands),
  `s57_25_help_negative` (a usage-only tool falls back to paths).

## Definition of Done

1. Every layout fixture parses to its `.expect`; the four user-tool fixtures
   behave as stated (≥ 20 rows for `shithub`, negative for `fackr`).
2. Policy tests pass for `native`, `all`, `off`, the denylist, and spec
   precedence.
3. In the built editor, with a fixture native tool on PATH, the first
   `:!tool bu<Tab>` shows the pending marker and the second, after the job,
   completes `build `; after restarting yew, the first Tab completes
   immediately from the disk cache.
4. No help job is spawned on the keystroke path (asserted count).
5. The cache survives a torn write (truncated file → miss) and prunes at
   2000.
6. `make fuzz-comphelp` runs 60 s clean under ASan/UBSan.
7. The three `s57_25_*` goldens pass; full `make test-pty` green.
8. gcc and clang warning-free; `MODULES=""` builds and passes.
9. `test-fletch test-script test-roundtrip test-roundtrip-coverage
   test-audit` and the three `scripts/check-*.sh` gates green; the new
   command is in the invariant-9 audit list and the roundtrip exclusions.
