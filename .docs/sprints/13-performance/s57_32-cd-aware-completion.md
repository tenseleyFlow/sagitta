# Sprint 57.32: cd-Aware Completion

## Prerequisites

- **Sprint 57.23** — `yew_shctx_at` / `YewShCtx` (`src/ui/shctx.h`): the
  lexer scans the body up to the caret through a stack of frames (one per
  open `$(` `(` `` ` `` `<(` `>(` `{`), knows every control operator, and
  hands back the caret's simple command. `yew_comp_shell_route` and the
  PATH source with its `path_filter` mask; `CompFilter.ctx_key`.
- **Sprint 57.24** — `yew_compspec_resolve` walking flags and subcommands;
  generators run with `cwd` = the directory `:!` commands run in
  (`compgen.c`); built-in `make_targets` reads that directory's Makefile;
  `shipped/user` spec loading.
- **Sprint 57.25** — help jobs run with cwd `/` and resolve the command
  word on `PATH`.
- **Sprint 57.26** — the fish oracle; its cache key includes the cwd.
- Binding: invariants 1 (never offer a path that names a different file),
  4, 5.

## Goals

`cd ch7/ && wolf build ou<Tab>` should complete `ou` inside `ch7/`, because
that is where the shell will be when `wolf` runs. Today it completes in the
prompt's directory. As far as the orchestrator knows, fish does not do
this (it completes against `$PWD`); it is new ground, and it is a large
quality-of-life gain.

This sprint computes the **effective directory at the caret** by replaying,
lexically, the `cd`-like commands the line runs before the caret's command,
honouring what the shell honours — sequencing, conditional operators,
subshell scope — and gives every consumer of a directory (paths, `./`
executables, generators, fish) that directory. Commands with a
change-directory FLAG (`git -C dir`, `make -C dir`) get the same treatment
through their spec.

Deferred: the persistent shell session (57.27) will supply the STARTING
directory; until then it is the `:!` directory. `cd` performed inside a
function or alias the line calls is invisible (named limit).

## Deliverables

### 1. The effective directory — `src/ui/shctx.[ch]`

`YewShCtx` gains:

```c
/* Where the caret's command will run, relative to the directory `:!`
 * commands start in ("" = unchanged), or absolute.  Lexically
 * normalised (`a/../b` -> `b`), the way a shell's LOGICAL `cd` (the
 * default, -L) resolves `..`.  Valid only when `cwd_known`. */
char *cwd;
bool cwd_known;
```

The lexer already walks every command before the caret. Track, per frame,
a directory and a "known" bit; a frame opened by `(`, `$(`, `` ` ``, `<(`,
`>(` starts with a COPY of its parent's and is discarded at its close (a
subshell's `cd` never leaks out); a `{ … }` group shares its parent's (same
shell).

**Reference: which completed simple commands change the directory.** The
command must be a whole simple command before the caret, not the caret's
own.

| Command | Effect on the frame's directory |
|---|---|
| `cd DIR`, `pushd DIR`, `cd -- DIR` | set: `DIR` relative to the current value, or absolute |
| `cd` (no operand), `cd ~` | `$HOME` (from the job environment, `yew_job_env()`) |
| `cd -`, `popd` without a preceding `pushd` in the line | unknown |
| `popd` after one or more `pushd` in the line | pop the line's own stack |
| `cd -P DIR` / `cd -L DIR` | as `cd DIR` (lexical) |
| any other command | no change |

**Operands.** Decode the operand as the shell will (quotes and escapes —
the lexer already does). Expand a leading unquoted `~` or `~user`, and
`$HOME`, `${HOME}`, `$PWD`, `$OLDPWD` from the job environment. Any other
expansion (`$X`, `$(…)`, backticks, globs `* ? [`) makes the directory
UNKNOWN — never guess.

**CDPATH.** When `CDPATH` is set in the job environment and the operand
does not start with `/`, `.`, or `..`, try each CDPATH entry in order
(empty entry = current), taking the first that is an existing directory —
bash's rule. If none exists, fall back to the plain relative path.

**Reference: which connectors let a `cd` apply to the caret's command.**

| Line shape | Directory at the caret |
|---|---|
| `cd a && CMD‸` | `a` |
| `cd a; CMD‸`, `cd a` + newline | `a` |
| `cd a \|\| CMD‸` | unchanged (CMD runs only if `cd` FAILED) |
| `cd a & CMD‸` | unchanged (`cd` ran in a background subshell) |
| `cd a \| CMD‸` | unchanged (pipeline stages are subshells in bash; zsh's last-stage exception does not apply to a `cd` in the first stage) |
| `(cd a; CMD‸` | `a` (inside the subshell) |
| `(cd a); CMD‸` | unchanged |
| `{ cd a; }; CMD‸` | `a` |
| `cd a && cd b && CMD‸` | `a/b` |
| `if cd a; then CMD‸` | `a` |
| `cd a \|\| exit; CMD‸`, `cd a \|\| return 1; CMD‸` | `a` (CMD is reached only if `cd` succeeded — the idiom) |
| `cd a \|\| echo no; CMD‸` | unknown (CMD runs in `a` or unchanged) |
| `false \|\| cd a && CMD‸` | unknown (`cd` ran only if `false` failed) |

Model it this way, and say so in the header. A `cd` affects a later
command in its frame only when it is CERTAIN to have run and succeeded
whenever that command runs:

- **Before the `cd`**: frame start, `;`, newline, `&&`, or a reserved word
  (`then` `do` `else` `{` `!`) — fine. After `||` the `cd` may not have
  run: every later command's directory is UNKNOWN.
- **After the `cd`**: `&&`, `;` or newline — applies. `|` or `&` — does
  not apply (subshell). `||` — the command on the right of `||` runs only
  when `cd` FAILED, so it is unchanged; after the whole `||` list, the
  directory is the `cd`'s if the right side was `exit` or `return`
  (whatever their arguments), else UNKNOWN.

Unknown is always the answer when the shell's path is data-dependent;
never guess a directory the command might not run in.

**Pitfall — the caret's own command.** `cd ch<Tab>` completes `ch` in
the directory BEFORE the `cd`; only a `cd` that has completed moves it.

### 2. Change-directory flags — `src/ui/compspec.c`, specs

New flag-map key `changes_dir: true` (validated like the others;
documented in `runtime/completions/README.md`). When resolution consumes
such a flag with a value, later positional path completion and generators
use `value` relative to the effective directory. Set it on `git -C` and
`make -C` (and `--directory` spellings where the tool has them). Do NOT
set it on `tar -C`: tar's `-C` moves where members are written, not where
`-f ARCHIVE` is read.

### 3. Consumers — `src/ui/cmdcomp.c`, `compgen.c`, `compfish.c`, `comphelp.c`

| Consumer | Uses |
|---|---|
| PATH source, every mask | lists `effective/stem-head`; the inserted text stays relative to what the user typed |
| COMMAND-position `./x` (57.23 row 4) | executables under the effective directory |
| generators (`git` branches, `modified`, `make_targets`, …) | `cwd` = effective directory |
| fish oracle | spawned with that cwd |
| help jobs | still cwd `/`, but a command word that is a relative path (`./tool`) resolves against the effective directory |

**Unknown directory** (`cwd_known == false`): every path-like source offers
NOTHING, and the pager footer says why (`cd target unknown`). Falling back
to the prompt's directory would complete names from the wrong place — the
exact failure this sprint removes.

**Nonexistent directory** (`mkdir x && cd x && ls ‸`): offer nothing; the
directory is empty until the command runs.

**Pitfall — caches.** Every cache that keys on a directory — the
`DirListing` head, `ctx_key`, generator keys, the fish key — must key on
the EFFECTIVE directory. Write the regression first: complete `ls s‸`,
then insert `cd sub && ` before `ls` without closing the menu, and assert
the rows now come from `sub/`.

### 4. Showing it

When the effective directory differs from the prompt's, the pager footer
names it (`in ch7/`), so a completion from an unexpected directory is
never a mystery. Truncate from the left to fit.

### 5. Defer

- `cd` inside functions, aliases, `source`d files, or `eval`: invisible.
- The starting directory from a persistent shell session → 57.27.

## Testing Strategy

- **Unit, `tests/unit/test_shctx.c` corpus**: ≥ 40 new rows asserting
  `cwd` and `cwd_known`, including every row of both reference tables, the
  caret's-own-`cd` pitfall, `~`, `$HOME`, `$FOO` (unknown), `cd -`
  (unknown), `pushd`/`popd`, `cd ..` normalisation, a quoted `"my dir"`,
  CDPATH (fixture env), nested subshells, and `$(cd a; x‸)`.
- **Unit, routing and consumers** with a fixture tree: `cd sub && ls ‸`
  lists `sub/`; `(cd sub); ls ‸` lists `.`; `cd $FOO && ls ‸` offers
  nothing with the footer reason; `cd sub && ./‸` finds `sub/`'s
  executables; `git -C sub checkout ‸` runs the branch generator in `sub`
  (stub generator records its cwd); `make -C sub ‸` lists `sub/Makefile`'s
  targets; the §3 cache regression.
- **PTY**: `s57_32_cd_then_complete` (fixture tree; the golden shows the
  `in sub/` footer), `s57_32_cd_unknown`.
- **Fuzz**: `fuzz-shctx` still 60 s clean (the new state is inside the
  lexer); add ten `cd` shapes to its seed corpus.

## Definition of Done

1. `:!cd ch7/ && wolf build ou<Tab>` completes from `ch7/` in the built
   editor (fixture tree in the PTY case).
2. Every row of both reference tables has a passing corpus row.
3. Unknown and nonexistent directories offer nothing and say why.
4. Generators, fish and `./` all follow the effective directory (asserted
   with stubs that record their cwd).
5. `git -C` / `make -C` follow their flag; `tar -C` does not.
6. The two `s57_32_*` goldens pass; full `make test-pty` green.
7. gcc AND clang warning-free (`make CC=gcc-16 BUILD=build-gcc
   FAULTSHIM_ARCH_FLAGS=` for the whole tree — CI is Linux GCC);
   `MODULES=""` builds and passes; ASan/UBSan filtered runs clean;
   `make fuzz-shctx` 60 s clean.
8. `test-fletch test-script test-roundtrip test-roundtrip-coverage
   test-audit` and the `scripts/check-*.sh` gates green.
9. `tests/size/` untouched (the orchestrator runs the CI rebaseline);
   added-bytes estimate in the report.
