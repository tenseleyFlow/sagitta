# Sprint 57.24: Shell Completion II — Command Specs and Generators

## Prerequisites

- **Sprint 57.23** — `yew_shctx_at` and `YewShCtx` (`src/ui/shctx.h`): the
  caret's position, quoting, and the simple command's `argv`/`arg_index`
  after assignment prefixes and precommand wrappers are stripped;
  `dashdash`. The `YEW_COMP_SHELL` dispatcher source and its routing table
  (57.23 §3, "shape beats position"), whose row 6 (a `-` stem) returns
  nothing and whose row 8 consults a C argument-kind defaults table;
  §2's C precommand wrapper table; `yew_shq_quote`; `CompFilter.ctx_key`;
  the prefix-gated ranking of §7.
- **What 57.23 actually landed, beyond its contract** (from its merge
  report — cite these, they are real): the routing function is
  `yew_comp_shell_route`, and it has a row 0 ahead of the table — a word
  holding an active expansion (`$x`, `$(…)`, backtick) offers NOTHING,
  because re-quoting the `$` would name a different file. `YewShCtx` also
  carries `tilde` (leading unquoted `~`) and `expands`. Builtins are their
  own appended kind, `YEW_COMP_BUILTIN`. `CompItem.suffix` carries a
  closing quote or `}` and is appended only when a row is committed.
  `yew_shq_insert` (opener + quoted text) is the public insertion helper.
  `yew_job_env()` returns the environment a `:!` child sees. Row 5 (an
  explicit path) keeps row 8's directories-only mask for `cd`-like
  commands — a spec's `dir` kind must do the same when the stem contains
  a `/`. Unquoted `~ # ^` are escaped everywhere and a leading `=` too
  (zsh EXTENDED_GLOB and `=cmd` expansion); do not relax that.
- **Sprint 19** — the job layer: `yew_job_spawn` with `YewJobSpec.argv`
  (no shell), `YEW_SINK_CALLBACK` + `callback_owner`/`callback_ops`
  (`YewJobCallbackOps.complete` runs once after reap and EOF, then the job
  auto-releases; `destroy` runs exactly once, including at teardown),
  `internal = true` (hidden from the jobs table), `timeout_ms`,
  `collect_max`, `env_set`. `YEW_JOB_MAX` is 32 and spawning past it
  errors rather than queueing.
- **Fletch data documents** — `fl_data_read(FlVm *, src, len, DiagCtx *)`
  (`src/fl/data.h`). `src/mod/plug/manifest.c:563` is the model to copy:
  a per-document `FlVm`, a validated schema, diagnostics naming the key.
- **Runtime assets** — `yew_runtime_asset_read(path, &buf)`, which serves
  both the on-disk runtime and `EMBED_RUNTIME=1` builds; `yew_xdg_config_dir`
  and `yew_xdg_cache_dir` (`src/util/xdg.h`).
- Binding: invariants 3, 4, 5, 7 (a spec is data, not a dependency), 9.

## Goals

57.23 made the engine know WHERE the caret is. This sprint makes it know
WHAT a command accepts: `wolf bu<Tab>` → `wolf build `, `git remote a<Tab>`
→ `add`, `wolf build --emit=<Tab>` → `wir obj bin llvm-ir`, `ssh <Tab>` →
your hosts, `make <Tab>` → the Makefile's targets.

Knowledge lives in **completion specs**: Fletch data files, one per
command, in `runtime/completions/` — the same shape of thing as the 48
syntax definitions in `runtime/syntax/`. A spec is a tree of subcommands,
each with flags, positional argument kinds and descriptions, plus
**generators**: programs whose output lines become candidates (git
branches), run asynchronously through the job layer so no keystroke ever
waits on a subprocess.

Deferred: commands with no spec get nothing new here — `--help` parsing
lands in 57.25 and the fish oracle in 57.26. Specs from plugins and from
the workspace are deferred past this campaign (see §9: a spec can run
programs). Man-page parsing is a non-goal.

## Deliverables

### 1. The spec format — `runtime/completions/<command>.fl`

A spec is one Fletch data map. The top-level map is also the ROOT NODE of
the command tree, so it takes every node key.

**Top level only**

| Key | Type | Req | Meaning |
|---|---|---|---|
| `completion` | int | yes | schema version; must be `1`. Any other value: reject the file, name the key |
| `command` | string or list of strings | yes | every command name this spec answers for |
| `description` | string | no | the command's own description: becomes the `detail` of its row in COMMAND position |
| `precommand` | map | no | this command runs another command: `{ flags_with_args: ["-u", ...] }`. The first word that is not a flag (or a flag's argument) is in COMMAND position — this replaces 57.23 §2's C table row for the command |
| `generators` | map | no | name → generator map, §5 |

**Node keys** (root and every subcommand)

| Key | Type | Meaning |
|---|---|---|
| `name` | string | subcommand name (required on subcommands, forbidden at root) |
| `aliases` | list of strings | other spellings that select this node (`co` for `checkout` only if the tool truly accepts it) |
| `desc` | string | the pager `detail` for this subcommand |
| `flags` | list of flag maps | flags valid at this node |
| `args` | list of arg maps | positional slots in order; only the LAST may set `repeat: true` |
| `subcommands` | list of node maps | children |

**Flag map**

| Key | Type | Meaning |
|---|---|---|
| `long` | string | without the dashes: `"release"` for `--release` |
| `short` | string | one character without the dash |
| `desc` | string | pager detail |
| `arg` | arg map | present ⇔ the flag takes a value |
| `arg_optional` | bool | the value may only be attached with `=` (`--profile-gen[=<dir>]`); a following word is NOT consumed |
| `global` | bool | also valid in every descendant node |

At least one of `long`/`short`. Rows show `--long` (and `-s` as a separate
row when both exist, `detail` identical), so either spelling completes.

**Arg map**

| `kind` | Candidates | Extra keys |
|---|---|---|
| `"path"` | files and directories | `ext`: list of extensions without the dot; directories always shown so the user can descend |
| `"dir"` | directories only | |
| `"exec"` | executables on `$PATH` ∪ builtins | |
| `"command"` | the rest of the line is a NEW command: re-enter COMMAND position (`sudo`, `xargs`, `watch`, `git -c` is NOT this) | |
| `"var"` `"user"` | 57.23's VAR and USER sources | |
| `"host"` `"pid"` `"signal"` | built-in generators, §5 | |
| `"values"` | a fixed list | `values`: strings, or `{ value, desc }` maps |
| `"generator"` | a named generator | `generator`: a built-in name or a key of this spec's `generators` |
| `"none"` | free text; offer nothing | |

Validation rejects: unknown keys (name the key and its path), wrong types,
`repeat` on a non-last slot, a flag with neither `long` nor `short`, a
generator name that resolves to nothing, and duplicate subcommand names or
aliases within one node. A rejected spec is reported ONCE per session with
`yew_msg` naming file and key, and the command behaves as if it had no
spec. It never aborts, and a broken user override never hides a working
shipped spec's commands from the rest of the line.

### 2. Loading — `src/ui/compspec.h`, `src/ui/compspec.c`

```c
typedef struct YewCompSpec YewCompSpec;   /* opaque; owns its FlVm */

/* The spec for `name` (basename of the command word), or NULL.  Loaded
 * lazily on first request and cached for the process; a user file is
 * re-checked by mtime at most once per prompt open. */
const YewCompSpec *yew_compspec_get(Ed *ed, const char *name);
void yew_compspec_invalidate_all(void);   /* test seam; prompt close */
```

Lookup order, first hit wins, **whole-file replace, never merge**:

1. `yew_xdg_config_dir()/completions/<name>.fl` — the user's.
2. `completions/<name>.fl` via `yew_runtime_asset_read` — shipped.
3. A spec whose `command` list names `<name>` as an alias. Build that index
   once, from the shipped runtime's file list (`yew_runtime_asset_count` /
   `yew_runtime_asset_name`), at first use.

Why whole-file: a merge rule for trees of flags and subcommands is a
second language nobody will read. A user who wants to extend `git` copies
the shipped file and edits it.

**Pitfall — the name is the BASENAME of argv[0].** `/usr/local/bin/git`
and `./wolf` must find `git.fl` and `wolf.fl`. A command word that is a
relative path to a script (`./build.sh`) finds `build.sh.fl` only if the
user wrote one.

**Pitfall — the install and embed lists.** `Makefile:3540–3545` installs
`runtime/syntax` and `runtime/themes` by explicit directory. Add
`runtime/completions` there AND to the `EMBED_RUNTIME=1` asset list, and
extend `make test-runtime-embedded` to prove a spec loads from the
embedded image. This project has already lost an evening to an installed
runtime missing files the source tree had.

### 3. Resolution — `src/ui/compspec.c`

Walk `argv[1 .. arg_index-1]` against the tree. The result is what the
caret's word can be.

```c
typedef struct YewSpecPoint {
    const void *node;         /* deepest node reached                      */
    const void *pending_flag; /* the flag whose value the caret's word is  */
    u32 positional;           /* index of the caret's slot at `node`       */
    bool after_equals;        /* caret is in `--flag=<here>`               */
    bool subcommands_allowed; /* node has children, no positional consumed */
} YewSpecPoint;

bool yew_compspec_resolve(const YewCompSpec *spec, const YewShCtx *ctx,
                          YewSpecPoint *out);
```

**Reference: the walk.**

| Word | Effect |
|---|---|
| `--` | flags end; later words are positionals only |
| `--name=value` | consume; if `name` is unknown at this node or an ancestor's `global`, ignore it |
| `--name` taking `arg`, not `arg_optional` | consume it AND the next word |
| `--name` otherwise | consume it |
| `-o` taking `arg` | consume it and the next word |
| `-ovalue` (short with arg, attached) | consume; value is the remainder |
| `-xvf` (bundle) | each letter in turn; the first letter that takes an `arg` swallows the rest of the word as its value, or, at the word's end, the NEXT word |
| unknown `-x` / `--x` | consume it alone. Never guess that it takes a value — a wrong guess eats the subcommand that follows |
| a word equal to a child's `name` or `alias`, while no positional has been consumed at this node | descend |
| anything else | positional: `positional++` (saturating on a `repeat` slot) |

At the caret: stem after `--name=` (57.23 already split `replace` at the
`=` for assignments; do the same here) → the flag's `arg` with
`after_equals`. Previous word is a flag awaiting its value →
`pending_flag`. Otherwise `positional` and `subcommands_allowed`.

**Pitfall — `-` inside values.** `git log --since -2weeks`: once `--since`
awaits a value, the next word is its value even though it starts with `-`.
The walk above gets this right only if "awaiting a value" is checked
BEFORE "starts with `-`". Put that row in the corpus.

### 4. Routing with specs — `src/ui/cmdcomp.c`

Amend 57.23 §3's table. Shape rows still come first: a `$VAR`, a `~user`
and an explicit path (rows 2, 3, 5) win in every position, spec or not —
`git add ./sr<Tab>` completes the path.

| Situation (spec present) | Candidates |
|---|---|
| COMMAND position, EXEC row whose name has a spec | the row's `detail` becomes the spec's `description` |
| `pending_flag` or `after_equals` | the flag's `arg` kind |
| stem starts `-`, not `dashdash` | flags of `node` plus `global` flags of its ancestors |
| `subcommands_allowed` | `node`'s subcommands (and aliases as their own rows), plus the slot-0 arg kind if `node` also has `args` |
| a positional slot | that slot's arg kind; past the last slot of a non-`repeat` node, nothing |
| `precommand` spec | skip its flags per `flags_with_args`, then COMMAND position for the next word |

57.23's C defaults table (row 8) and C wrapper table (§2) remain the
fallback when no spec loads, so a `MODULES=""` build without a runtime
still completes `cd` with directories. `CompFilter.ctx_key` gains the
resolved node path and slot, so moving from `git remote` to
`git remote add` re-enumerates.

### 5. Generators — `src/ui/compgen.h`, `src/ui/compgen.c`

**Built-in generators, in C, no subprocess:**

| Name | Source | Detail |
|---|---|---|
| `hosts` | `Host` lines of `$HOME/.ssh/config` (skip patterns containing `*` `?` `!`), then non-hashed names from `$HOME/.ssh/known_hosts` | `ssh config` / `known host` |
| `signals` | static table `HUP INT QUIT KILL TERM USR1 USR2 STOP CONT TSTP WINCH` with and without `SIG` | the default action |
| `users` | `getpwent` (57.23's USER source) | home directory |
| `make_targets` | parse `GNUmakefile`, `makefile` or `Makefile` (make's own order) in the shell cwd: a line starting with a name of `[A-Za-z0-9_./-]`, optional blanks, then a `:` that is NOT followed by `=` (so `X := y` and `X ::= y` are assignments, not targets). Split multi-target lines (`a b: dep`). Exclude names starting `.` and any containing `%`. Hand-written C scan — the regex engine is not needed and has no lookahead | `make target` |

**Pitfall — never run `make -p` or `make -n`.** Both evaluate the Makefile,
and `$(shell ...)` in a Makefile runs arbitrary commands. Parse the text.

**Pitfall — `hosts` in tests.** A test must never read the developer's real
`~/.ssh`. The generator reads `$HOME` at call time; tests point `HOME` at a
fixture directory.

**Spec generators, run as subprocesses:**

```
generators: {
    branches: { argv: ["git", "for-each-ref", "--format=%(refname:short)",
                       "refs/heads", "refs/remotes"],
                cache_ms: 2000, pass_flags: ["-C", "--git-dir", "--work-tree"] },
}
```

| Key | Meaning |
|---|---|
| `argv` | argv form — NO shell. `YewJobSpec.argv`, never `cmdline` |
| `cache_ms` | how long a result stays fresh; default 2000 |
| `pass_flags` | flags that, if the user's line already carries them BEFORE the caret, are copied into the generator's argv right after `argv[0]` with their values — so `git -C ../other checkout <Tab>` lists ../other's branches |

Output: one candidate per line, `candidate` or `candidate\tdescription`.
Trailing whitespace trimmed; control bytes in either field rendered as `·`;
at most 5000 lines taken; blank lines skipped; order preserved into the
stable ranker.

**The async contract.** This is where invariants 4 and 5 are won or lost.

1. A generator job is `yew_job_spawn` with `argv`, `YEW_SINK_CALLBACK`,
   `internal = true`, `timeout_ms = 1500`, `collect_max = 1 MiB`, `cwd`
   = the directory `:!` commands run in (use the SAME expression
   `yew_shell_run` uses — a generator in a different cwd lists another
   repo's branches), and `env_set = { "NO_COLOR=1", "PAGER=cat",
   "GIT_PAGER=cat", "TERM=dumb", NULL }`.
2. **At most ONE job in flight per key** (generator name + passed flags +
   cwd), and **at most FOUR completion jobs in total**. A request for a
   key already in flight waits for it; a fifth distinct key is not
   spawned. Why: `YEW_JOB_MAX` is 32 and fails rather than queues; fast
   typing must never make the user's own `:!` command fail to spawn.
3. Results land in a cache keyed the same way, stamped with `now_ms`.
   Fresh (`now_ms - stamp < cache_ms`) → served synchronously. Stale →
   served AND a refresh spawned.
4. On arrival, if the prompt is still open AND the menu's `ctx_key`
   still matches the key that asked, refilter and repaint. Otherwise
   just cache. **An arrival never edits the line**: no sole-survivor
   insertion, no LCP insertion. Text changing under the user's fingers
   when a subprocess finishes is not acceptable.
5. While a key is pending and has no cached answer, the pager shows the
   spec's static rows for that slot (if any) and a footer marker
   `…` in the existing footer style. The marker is STATE, so rendering
   stays deterministic.
6. Timeout, non-zero exit or spawn failure: cache an empty answer for
   `cache_ms` so the next keystroke does not respawn it, and `yew_log`
   once. No message in the footer — a missing branch list is not an error
   the user caused.

```c
/* Returns the rows for `key` now (possibly stale, possibly empty) and
 * whether a job is in flight for it.  Never blocks. */
bool yew_compgen_rows(Ed *ed, const YewCompGenKey *key, i64 now_ms,
                      Arena *a, Vec_CompItem *out, bool *pending);
u32 yew_compgen_inflight(void);          /* test seam */
void yew_compgen_cache_clear(void);      /* test seam; prompt close */
```

### 6. The first spec batch — `runtime/completions/`

Chosen by mining the user's shell history (1,327 entries): git 180, cd
159, brew 85, wolf 73 (`build` 71 of them), ssh 69, make 21, uv 17,
cargo 10, gh 9. Each spec is written FROM the tool's own documentation at
authoring time — `wolf --help` and `wolf <sub> --help`, `git help -a`,
`brew commands`, `cargo --list`, `uv help`, `gh help` — and its first line
is a comment naming the tool version it was written against. Descriptions
at most 60 bytes.

| File | Must cover |
|---|---|
| `wolf.fl` | all 23 subcommands (`build run test fmt fix doc init add rm update audit tree why vendor publish cache interface audit-surface profile c-import conform-run lsp`); `build`'s flags exactly as `wolf build --help` prints them, including `--emit` values `wir obj bin llvm-ir`, `-o` taking a path, `--profile-gen` as `arg_optional`, `--std-root` taking a dir; entry-file slots as `path` with `ext: ["lu"]` |
| `git.fl` | every porcelain subcommand `git help -a` lists under "Main Porcelain Commands" and "Ancillary Commands"; `remote` with `add remove rename set-url show prune get-url`; global `-C` (dir) and `-c` (none); generators `branches`, `tags`, `remotes`, and `modified` (`git status --porcelain=v1`, paths only) with `pass_flags` |
| `brew.fl` | `install uninstall reinstall upgrade update search info list outdated tap untap services cleanup doctor`; generators `installed` (`brew list -1`, `cache_ms` 60000) for uninstall/upgrade/info |
| `ssh.fl`, `scp.fl` | `hosts` for the destination; `-i` path, `-p` values none, `-F` path, `-J` host |
| `make.fl` | positional `generator: make_targets`, repeat; `-C` dir, `-f` path, `-j` none |
| `cargo.fl`, `uv.fl`, `gh.fl` | top-level subcommands with descriptions and each subcommand's most common flags |
| `cd.fl`, `pushd.fl`, `rmdir.fl`, `mkdir.fl` | `dir` |
| `kill.fl` | `-s` signal, `-l`, positional `pid` repeat; `-SIGNAL` spellings via `signals` |
| `sudo.fl`, `doas.fl`, `env.fl`, `nice.fl`, `timeout.fl`, `xargs.fl`, `nohup.fl`, `time.fl`, `watch.fl` | `precommand`, mirroring 57.23 §2's table, each with its real flags |

**Pitfall — specs are data about OTHER people's tools.** A subcommand that
does not exist in the installed version is worse than a missing one: the
user runs it and gets an error. Prefer omission to invention.

### 7. The detail column in COMMAND position — `src/ui/cmdcomp.c`

`EXEC` rows whose basename has a spec show its `description`, so `w<Tab>`
shows `wolf  the wolf toolchain` beside `which`. Loading a spec to decorate
a row must not parse every spec on every keystroke: decorate from the alias
index built in §2, which is populated once and holds each spec's
`description` string.

### 8. Documentation — `runtime/completions/README.md`

The schema tables of §1 in user terms, one worked example (`wolf.fl`
abridged), and where a user override lives. It ships with the runtime so
`yew` users can write specs for their own tools.

### 9. Defer

- Commands with no spec → 57.25 (`--help`) and 57.26 (fish). Until then
  they get 57.23's behaviour exactly.
- **Specs from plugins or from a workspace** (`.yew/completions/`) → after
  this campaign, named. A spec's generators run programs; loading one
  from a repository the user merely opened is code execution on open. If
  it lands, it goes through workspace trust (`src/ws/trust.c`) and the
  plugin capability model, not around them.
- Man-page parsing: non-goal.

## Testing Strategy

- **Unit, spec validation (`tests/unit/test_compspec.c`)**: every file in
  `runtime/completions/` loads and validates (iterate the asset list — a
  new file is tested without editing the test). One test per rejection
  rule of §1, asserting the message names the key.
- **Unit, resolution corpus**: ≥ 80 rows, table-driven like 57.23's, each
  asserting the candidate KIND and, for fixed values, the exact set:

| Line (`‸` = caret) | Expect |
|---|---|
| `wolf bu‸` | subcommand `build` |
| `wolf build ‸` | `path` ext `lu` |
| `wolf build --emit=‸` | values `wir obj bin llvm-ir` |
| `wolf build -o ‸` | `path` |
| `wolf build --profile-gen ‸` | `path` ext `lu` (arg_optional does not consume) |
| `git remote a‸` | subcommand `add` |
| `git -C ../x che‸` | subcommand `checkout` |
| `git log --since -2w ‸` | not a flag value; git log's positional |
| `git checkout -- ‸` | `path` (dashdash) |
| `tar -xvf ‸` | `path` (bundle; `f` takes the next word) |
| `sudo git che‸` | subcommand `checkout` |
| `make ‸` | generator `make_targets` |
| `ssh ‸` | generator `hosts` |
| `kill -‸` | flags and `-SIGNAL` rows |
| `git add ./sr‸` | path (shape beats the spec) |

- **Unit, generators** with a fixture `PATH` of tiny scripts: a line
  printer, a `sleep 5` (timeout path, asserts the job is killed and an
  empty answer cached), a non-zero exit, and 6000-line output (cap). The
  concurrency test types 50 keystrokes against one key and against six
  keys: at most one job per key, at most four total, asserted via
  `yew_compgen_inflight` and `yew_job_running_count`.
- **Unit, arrival races**: prompt closed before arrival (no repaint, no
  leak under ASan); context changed before arrival (cached, not shown);
  arrival never changes the line text (compare bytes before and after).
- **Unit, `make_targets`**: a fixture Makefile containing a `$(shell touch
  sentinel)` line; assert targets are listed AND the sentinel file does not
  exist afterwards.
- **Unit, `hosts`**: fixture `HOME` with a config holding `Host a b`,
  `Host *.corp`, `Host !x`; expect `a` and `b` only.
- **PTY** (fixture PATH/HOME; snapshot only after generator jobs settle,
  gated on the frame boundary per the harness, never on a sleep):
  `s57_24_wolf_subcommand`, `s57_24_wolf_emit_values`,
  `s57_24_git_remote_add`, `s57_24_make_targets`, `s57_24_ssh_hosts`,
  `s57_24_generator_pending` (a generator that has not answered yet shows
  static rows and the `…` marker).
- **Embedded runtime**: `make test-runtime-embedded` loads `wolf.fl`.
- **Perf**: `perf_cmdcomp` gains a case resolving `git -C x remote add o`
  against the shipped `git.fl`, warm, p99 under 150 µs.

## Definition of Done

1. Every file in `runtime/completions/` validates; the test iterates the
   asset list.
2. The resolution corpus has ≥ 80 rows and passes, including every row in
   the table above.
3. In the built editor, `:!wolf bu<Tab>` yields `:!wolf build ` and
   `:!wolf build --emit=<Tab>` offers exactly four values.
4. Generator jobs: ≤ 1 per key and ≤ 4 total under the 50-keystroke test;
   a timed-out generator is killed and its empty answer cached.
5. No arrival ever changes the prompt's text (asserted).
6. `make_targets` never executes the Makefile (sentinel test).
7. No test reads the real `$HOME/.ssh`.
8. `make install` into a temp prefix installs `share/yew/runtime/completions/*.fl`;
   `make test-runtime-embedded` loads a spec.
9. The six `s57_24_*` goldens pass; full `make test-pty` green.
10. gcc and clang warning-free; ASan/UBSan filtered runs for compspec,
    compgen, shctx, cmdcomp clean; `MODULES=""` builds and passes.
11. `perf-cmdcomp` green with the new case.
12. `test-fletch test-script test-roundtrip test-roundtrip-coverage
    test-audit` and the three `scripts/check-*.sh` gates green.

## Implementation divergences (recorded at landing)

Where this contract was wrong about the code, or a rule could not hold as
written, the implementation did what the contract intends:

1. **Shipped files are not read through `yew_runtime_asset_read` alone** —
   it serves only the `EMBED_RUNTIME=1` image. `compspec.c` resolves the
   shipped directory the way the runtime's other consumers do:
   `$YEW_RUNTIME_DIR` alone when set; else the installed prefix, but only
   if IT has `completions/` (an older install does not); else the source
   tree's `runtime/`; else the embedded image.
2. **The embed list is automatic** (`find runtime -type f`), so nothing had
   to be added to it; `README.md` ships in the image and is installed too.
3. **A spec owns an arena, not an `FlVm`.** The per-document VM validates
   and is freed; the spec is converted to C structs (`YewSpecNode`,
   `YewSpecFlag`, `YewSpecArg`, public in `compspec.h`) so a keystroke
   walks plain memory. `YewSpecPoint` holds typed pointers and gains
   `flags_ended`, `command_at` (re-enter COMMAND position) and `value_at`.
4. **Prompt close does not drop parsed specs.** `yew_compspec_invalidate_all`
   is the test seam; prompt close calls `yew_compspec_prompt_closed`, which
   makes each user file re-`stat` once on its next lookup. Dropping the
   shipped specs per prompt would re-pay the parse on every first keystroke.
5. **The alias index SCANS** each shipped file's top-level `command` and
   `description` instead of parsing it: parsing all 24 costs ~10 ms (git.fl
   alone 3.5 ms) on the first `:!` keystroke. A spec is parsed and
   validated only when its command is completed.
6. **Additive schema keys** (documented in `runtime/completions/README.md`):
   node `dash_values` (`kill -KILL`), node `after_dashdash`
   (`git checkout -- <path>`), and precommand `operands` / `assignments` —
   `flags_with_args` alone cannot express 57.23's `timeout` (one duration
   word) and `env`/`sudo` (`NAME=value`) rows it was meant to replace.
7. **Precommand specs replace the lexer's table through a lookup**:
   `yew_shctx_at_with(…, YewShWrapperLookup, …)`; `yew_shctx_at` keeps the
   C table, so the lexer and its corpus stay pure. A spec without
   `precommand` un-wraps (whole-file replace); no spec defers to the table.
8. **Two kinds appended**, `YEW_COMP_SPEC` (subcommands, flags, values) and
   `YEW_COMP_GEN` (generator rows), each with a registered source.
9. **`YEW_JOB_MAX` guarantee needs the job layer.** The four-job cap alone
   cannot promise the user's own `:!` never fails (28 user jobs + 4
   generators = full). Generator jobs are `evictable`; a non-evictable
   spawn into a full table kills and releases one. Tested.
10. **Tab while a generator is pending inserts nothing** — neither a sole
    survivor nor an LCP: the set is incomplete. The arrival refills the
    menu (never the text); an empty word Tab asked about may then open it.
11. **`modified` runs `git ls-files --modified --others --exclude-standard`**,
    not `git status --porcelain=v1`: porcelain lines carry status columns
    and repository-root-relative paths; ls-files prints paths relative to
    the directory `:!` runs in.
12. **`pid` is backed by an async `ps -A -o pid= -o comm=` generator**: §5
    names no built-in source for it and POSIX has no subprocess-free
    process list (`/proc` is Linux-only).
13. **wolf has 22 subcommands**, the ones the contract's list names;
    wolf 0.2.14 prints exactly those.
14. **`scp` operands complete as paths**, not hosts: one slot cannot be
    both and local files are the common operand; `-J` completes hosts.
15. **`tar.fl` was added** for the corpus's bundle row; `watch` and `doas`
    are not installed here and were written from their man pages (said in
    each file's header).
16. **git omits `citool gitk gui scalar gitweb instaweb`**: `git help -a`
    lists them but the installed git reports each as "not a git command".
17. **The specs were trimmed to the embedded-image budget** (220 KiB; the
    image records 211 141 bytes with all 24 specs on macOS): each keeps
    every subcommand and its common flags.
18. **The PTY harness gained an opt-in `$HOME`**, unset by default like
    57.18's `$PATH`, so `s57_24_ssh_hosts` reads a fixture `~/.ssh`.
