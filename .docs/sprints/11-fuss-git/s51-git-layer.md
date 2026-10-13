# Sprint 51: The Git Layer — Subprocess, Porcelain, Cache, Taxonomy

## Prerequisites

- Sprint 0 — `Arena`, `Vec(T)`, `Strmap` (insertion-ordered), `Interner`,
  `Bytebuf`, `yew_sort_stable`, `yew_log`, `YEW_BUG`, and the **module
  registry**: `YEW_MOD_FUSS`, `yew_mod_enabled`, `yew_mod_name`,
  `yew_mod_require`, the `-DYEW_WITH_FUSS` define, and the pinned message
  `this build has no fuss module; rebuild with 'make MODULES="… fuss"'`.
- Sprint 19 — **the process layer this sprint builds on and does not
  duplicate**: `YewJobSpec` (`argv`, `cwd`, `sink`, `in_buf`/`in_span`,
  `timeout_ms`), `YewJob`, `yew_job_spawn` / `_collect_fds` / `_pump` /
  `_reap` / `_signal` / `_find`; `YEW_SINK_COLLECT`; the exec-status pipe
  (`YEW_JOB_EXECFAIL` + `exec_errno`, distinguishable from exit 127);
  `setpgid` on both sides; the §7 environment contract (`PAGER`/
  `GIT_PAGER` already `cat`, `COLUMNS`/`LINES` already removed); and
  **§9's law**: *"Programmatically constructed commands MUST use argv
  arrays, never string interpolation into `cmdline`."* `system()`/
  `popen()` are banned repo-wide and grep-gated.
- Sprint 13 — the `"ed.*"` registry (`yew_cmd_register`, `CmdDesc`,
  `YEW_CMD_DEFERRED`, the reserved `git` domain), `yew_cmd_invoke`.
  Sprint 15 — `yew_msg`, the zero-width git badge slot (filled in s53).
  Sprint 25 — `yew_ws_root()`, every git job's `cwd`.
- Binding: `00-decisions.md` — *"Optional runtime tools: `git` (FUSS
  mode) … degrade gracefully when absent"*, *"All git via subprocess"*,
  bespoke-first (invariant 7 — **libgit2 is not on the table**),
  invariants 3, 4, 5, 8. `01-architecture.md` — `src/mod/git/git.c
  porcelain.c …`, and *"git: never blocks the loop; status cache with
  wall-clock TTL, force refresh after mutating verbs (fuss lesson: use
  wall clock, not cpu_time)"*.
- Source of truth for the port:
  `~/GithubOrgs/FortranGoingOnForty/fuss/src/{git_module,cache_module}.f90`.
- Assumption beyond earlier index entries: Sprint 14's loop exposes a
  monotonic millisecond clock `i64 yew_now_ms(void)` (`CLOCK_MONOTONIC`)
  and the timer heap `yew_timer_at`. If it is spelled differently, this
  sprint adds the one-line adapter in `src/mod/git/git.c`; it must **not**
  call `time(2)` or `clock(3)` (§11).

## Goals

Land everything below F mode: `src/mod/git/git.c` (verb descriptors,
argv-only spawning on the s19 job layer, the environment
passthrough/sanitation table, repo detection, the wall-clock TTL snapshot
cache with forced invalidation after every mutating verb) and
`src/mod/git/porcelain.c` (byte-exact parsers for `status --porcelain=v2
--branch -z`, `diff --name-only -z`, `ls-files -z`, `blame --porcelain` /
`--incremental`, and the pinned `--pretty` record formats for log and
reflog), all producing arena-allocated structs, plus the error taxonomy
that makes not-a-repo, unborn HEAD, detached HEAD, mid-merge, mid-rebase,
mid-cherry-pick/revert and conflicted **enum states** rather than strings
that Sprints 52/53 would have to re-match. No UI lands here: `ed.git.*`
commands needing a screen are `YEW_CMD_DEFERRED` naming Sprint 52 (F mode)
or 53 (gutter/blame/diff view). The unified-diff patch generator and the
index-blob differ live in Sprint 53; this sprint provides the blob fetch
they consume.

## Deliverables

### 1. Files, module boundary, and the availability probe

```
src/mod/git/git.h        public API (this sprint + 52 + 53)
src/mod/git/git.c        verbs, spawning, cache, taxonomy, probes
src/mod/git/porcelain.c  every parser; no I/O, no spawning — pure bytes
src/mod/git/shim.c       compiled INSTEAD when MODULES lacks fuss
tests/unit/test_porcelain.c  tests/unit/test_gitcache.c
tests/fixtures/git/mkrepo.sh
```

`shim.c` follows s45 §8 exactly: every function declared in `git.h` has a
shim body routing through `yew_mod_require(YEW_MOD_FUSS, …)` and reporting
the pinned s00 text through `yew_msg`. Every `ed.git.*` command is
registered in **both** builds so `:git.status` completes and explains
itself (invariant 3); `make MODULES="" test` is the s45 CI lane.

```c
typedef struct { u16 major, minor, patch; } GitVersion;

/* Resolved once per process; cached. false => git absent or too old.   */
bool yew_git_avail(Ed *ed, GitVersion *out);
```

`YEW_GIT_MIN` is **2.20.0**: `--porcelain=v2` needs 2.11,
`--no-optional-locks` 2.14, `ls-files --directory` settles by 2.18, and
2.20 is old enough to be everywhere while letting us stop writing
fallbacks. Probing is `git --version` at F mode's **first use, not at
startup** — a 20 ms cold-start budget has no room for a fork the user may
never need. Absent git ⇒ `YEW_GIT_NO_GIT`, one message, F mode refuses to
open. Not a crash, not a stub.

### 2. Verb descriptors — the table that makes argv the only path

Every git invocation in the tree is a row in one static table. There is no
`yew_git_raw(const char *cmdline)` entry point, ever.

```c
typedef enum {
    YEW_GV_READ,      /* parsed; may run under the TTL cache            */
    YEW_GV_MUTATE,    /* invalidates the cache on completion (§11)      */
    YEW_GV_NET,       /* mutate + network: longer timeout, auth states  */
} GitVerbKind;

typedef struct GitVerb {
    const char *name;        /* "status", "stage", "commit", …          */
    GitVerbKind kind;
    i64         timeout_ms;  /* 0 = none                                */
    bool        needs_repo;  /* refused with YEW_GIT_NOT_REPO           */
    bool        needs_head;  /* refused on an unborn branch             */
} GitVerb;

/* Build argv, spawn on the s19 layer, return the job id. `av` is NULL-
 * terminated and built in the REQUEST ARENA, never on the stack: s19
 * spawns after this call returns and reads argv in the child.          */
u32 yew_git_spawn(Ed *ed, const GitVerb *v, char *const *av,
                  const GitReq *req, char *err, size_t errsz);
```

**Argv construction rules (each is a CI grep or a unit test):**

| Rule | Why |
|---|---|
| `av[0] = "git"`, resolved through `PATH` by `execve` on the s19 path | one binary name, no absolute path baked in |
| Global options come first: `--no-pager`, `-c core.quotepath=false`, `-c status.renames=true`, and `--no-optional-locks` on every `YEW_GV_READ` verb | `--no-pager` survives a user's `core.pager`; quotepath off keeps `-z` paths raw; forcing `status.renames` makes `2` records appear regardless of the user's config, so the parser's behavior does not depend on their `~/.gitconfig` (invariant 5); optional-locks off stops a 500 ms status poll from fighting a terminal's `git add` for `index.lock` |
| **Every path argument is preceded by `--`** and passed as its own element | a file literally named `-f` or `--cached` is a real filename and a real bug in every tool that forgets |
| Paths are workspace-relative, byte-exact as git gave them, never re-quoted | s19 §9's law; `yew_shell_quote` must not appear in `src/mod/git/` at all |
| No element is ever built by `sprintf` from a path | fuss did `write(command,…) 'git add "' // path // '"'` — a filename containing `"`, `$`, a backtick, or `;` is a shell injection *and* a plain breakage. This is the single most important divergence from the original |
| A commit message, tag message, or stash message travels on **stdin** (`git commit -F -`), never as an argv element | messages contain newlines; argv gets `E2BIG` at ~2 MiB and a multi-line `-m` is a formatting lie |

**Pitfall — the awkward filename.** `git status -z` hands back
`src/my file.c` with no quoting at all: consumers carry the length, never
assume NUL-or-space termination, and pass the string as one argv element.
`grep -rn 'sprintf\|bytebuf_printf' src/mod/git/ | grep -v porcelain` is a
CI gate — formatting is legal in the parsers (display strings) and illegal
on the spawn path. Newlines in filenames are legal too:
`--porcelain=v1` quotes them (`"a\nb"`, which fuss's `unquote_filename`
half-handled) and `--porcelain=v2 -z` NUL-terminates instead, which is why
**`-z` is mandatory on every parsed invocation** and a line-oriented
reader is a correctness bug, not a style choice.

### 3. Environment passthrough and sanitation (pinned)

Built on top of s19 §7's table (which already sets `YEW_*`, forces
`PAGER`/`GIT_PAGER=cat`, and removes `COLUMNS`/`LINES`). The parent's
environment is never mutated; this is a copy.

| Variable | Action | Why |
|---|---|---|
| `GIT_DIR`, `GIT_WORK_TREE`, `GIT_COMMON_DIR`, `GIT_INDEX_FILE`, `GIT_OBJECT_DIRECTORY`, `GIT_ALTERNATE_OBJECT_DIRECTORIES`, `GIT_NAMESPACE`, `GIT_CEILING_DIRECTORIES` | **pass through** | the bare-repo dotfiles workflow (`GIT_DIR=~/.dotfiles GIT_WORK_TREE=$HOME`) is a real thing; clearing these silently points yew at a different repository than the user's shell |
| `GIT_CONFIG_GLOBAL`, `GIT_CONFIG_SYSTEM`, `GIT_CONFIG_COUNT`, `GIT_CONFIG_KEY_*`, `GIT_CONFIG_VALUE_*` | **pass through** | test harnesses and containerized setups configure git this way; overriding them makes yew disagree with the shell |
| `GIT_SSH`, `GIT_SSH_COMMAND`, `GIT_ASKPASS`, `SSH_ASKPASS`, `SSH_AUTH_SOCK`, `SSH_ASKPASS_REQUIRE` | **pass through, never set** | the user's credential path is theirs; inventing one is a security decision we have no right to make |
| `GIT_TERMINAL_PROMPT` | **forced `0`** | otherwise an HTTPS push blocks forever reading a username from a pipe that will never carry one. With `0`, git exits nonzero with a stable message and we raise `YEW_GIT_AUTH` |
| `GIT_EDITOR`, `GIT_SEQUENCE_EDITOR` | **forced `false`** | every verb we spawn passes `-F -`, `--no-edit`, or `-m`; if one ever forgets, `false` makes git abort loudly instead of hanging on a pipe forever. Sprint 52's rebase handover sets these deliberately, on that one path, and it is the only exception |
| `EDITOR`, `VISUAL` | left alone | git prefers `GIT_EDITOR`, which we pinned |
| `GIT_PAGER`, `PAGER` | `cat` (s19) plus `--no-pager` on argv | belt and braces; a pager on a pipe is the classic no-output-no-exit hang |
| `GIT_FLUSH` | **forced `1`** | git block-buffers to a non-tty; with `1`, `fetch`/`push` progress reaches the job buffer while it is still interesting |
| `GIT_TRACE`, `GIT_TRACE2*`, `GIT_TRACE_PACKET`, `GIT_TRACE_PERFORMANCE`, `GIT_CURL_VERBOSE`, `GIT_TRANSFER_TRACE` | **removed** | a developer with tracing on would see every parse slow down and every stderr sample polluted; removing them is invisible to anyone who is not debugging git itself |
| `LC_ALL` | **forced `C`** | porcelain formats are locale-independent, but *messages* are not, and both the pty goldens (invariant 5) and §11's two message probes need stable bytes. Documented in the manual: git messages appear in English inside yew |
| `GIT_OPTIONAL_LOCKS` | not set (we use `--no-optional-locks` on argv) | one mechanism, visible in the descriptor table |
| `GIT_LITERAL_PATHSPECS` | **forced `1`** on verbs that take a path | a file named `:(glob)x` or starting with `:` is otherwise read as a pathspec magic prefix. `--` alone does not save you here |

One function builds the block, and `u64 yew_git_env_fingerprint(void)`
(FNV-1a over the pass-through rows in table order) is part of §4's
detection cache key — a `GIT_DIR` change under a running editor must
re-detect the repo, not keep answering about the old one.

### 4. Repo detection — `src/mod/git/git.c`

One invocation answers everything; `rev-parse` prints its outputs in
**argument order**, one per line:

```
git --no-optional-locks rev-parse --path-format=absolute \
    --git-dir --is-inside-work-tree --is-bare-repository --show-toplevel
```

```c
typedef struct GitRepo {
    char *git_dir;      /* absolute; $GIT_COMMON_DIR for a worktree     */
    char *top_level;    /* absolute worktree root                       */
    bool  inside_work_tree, bare;
    u64   env_fp;       /* §3 fingerprint at detection time             */
    i64   detected_ms;
} GitRepo;

GitStatusCode yew_git_detect(Ed *ed, GitRepo *out);
```

- `--show-toplevel` is deliberately last. Current Git exits 128 for that
  operand in a bare repository, after emitting the preceding git-dir / inside
  / bare rows. Parse those three rows even on that narrow nonzero result and
  return `YEW_GIT_BARE`; do not discard valid partial stdout. Worktrees emit
  the fourth top-level row. (The former assumption that bare repositories
  merely omitted the row was disproved against real Git 2.55.)
- `--path-format=absolute` (git ≥ 2.31) is requested but **not required**:
  if `rev-parse` rejects it (exit 129), retry once without it and resolve
  the relative `.git` against `cwd`. CI runners and LTS distros lag.
- Cached per `(yew_ws_root(), env_fp)`; re-detected on focus-gain (Sprint
  53's policy) and whenever `env_fp` changes. Not a repository ⇒
  `YEW_GIT_NOT_REPO`, and F mode offers `ed.git.init` (a `YEW_GV_MUTATE`
  verb) rather than pretending the tree is empty.

### 5. `status --porcelain=v2 --branch -z` — the complete grammar

The invocation, pinned:

```
git --no-pager --no-optional-locks -c core.quotepath=false \
    -c status.renames=true status --porcelain=v2 --branch -z \
    --untracked-files=normal --ignored=no
```

Every record is terminated by a NUL. **There are no newlines separating
records and no quoting of any kind.** Fields inside a record are
space-separated *up to the path*, and the path is everything from the
first byte after the last fixed field to the NUL.

#### 5.1 Header lines (`#` records, always first)

| Record | Fields | Meaning / absence |
|---|---|---|
| `# branch.oid <sha1>` | 40 (or 64) hex, or the literal `(initial)` | `(initial)` ⇒ unborn branch, no commits ⇒ `YEW_GIT_NO_HEAD` |
| `# branch.head <name>` | branch name, or the literal `(detached)` | `(detached)` ⇒ `YEW_GIT_DETACHED`; the name may contain `/` and any byte except space — read to end-of-record, do not stop at a second space |
| `# branch.upstream <name>` | e.g. `origin/trunk` | **absent** when no upstream is configured ⇒ `YEW_GIT_NO_UPSTREAM` (a state, not an error) |
| `# branch.ab +<ahead> -<behind>` | two signed decimals | emitted **only** when `branch.upstream` is present; `+0 -0` means in sync. Absent ⇒ unknown, render nothing, never render `↑0↓0` |
| `# stash <n>` | count | only with `--show-stash`, which we do not pass; the parser accepts and ignores it so a future flag needs no parser change |

Unknown `#` records are skipped, not errors — git adds headers between
versions and a hard parse failure would take F mode down on an upgrade.

#### 5.2 `1` — ordinary changed entry

```
1 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <path>NUL
```

| Field | Bytes | Note |
|---|---|---|
| `XY` | exactly 2 | index state, worktree state; **`.` means unmodified, not a space** |
| `sub` | exactly 4 | `N...` for a non-submodule; `S<c><m><u>` otherwise (`C` commit changed, `M` tracked modified, `U` untracked) |
| `mH` `mI` `mW` | 6-digit octal | modes at HEAD, index, worktree; `000000` = absent |
| `hH` `hI` | object names | **`hI` is the index blob id — Sprint 53 keys its diff-base cache on it** |
| `path` | rest of record | workspace-relative, raw bytes, may contain spaces and newlines |

`XY` alphabet: `. M T A D R C` (`T` = type change, e.g. file → symlink;
fuss had no concept of it and rendered such rows as unchanged).

#### 5.3 `2` — renamed or copied entry (the two-field record)

```
2 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <X><score> <path>NUL<origPath>NUL
```

- `<X>` is `R` or `C`; `<score>` is a similarity percentage `0`–`100`
  with no `%` sign (`R100`, `C075`).
- **The pitfall this format exists to create:** under `-z` the original
  path is a *separate NUL-terminated field*, so a `2` record consumes
  **two** records' worth of the stream. A parser advancing one NUL per
  record reads `origPath` as the next record's kind byte and desynchronizes
  for the rest of the buffer — every row after the first rename is
  garbage. Without `-z` the two paths are TAB-joined instead, which is why
  the two modes cannot share a splitter.

```c
/* One record at a time; returns the number of BYTES consumed (including
 * every NUL the record owns) so the caller advances by the record's own
 * measure and a `2` record cannot desync the stream.                   */
static u64 pv2_record(const u8 *p, u64 n, GitEntry *out, GitParseErr *e);
```

#### 5.4 `u` — unmerged entry

```
u <XY> <sub> <m1> <m2> <m3> <mW> <h1> <h2> <h3> <path>NUL
```

`XY` here is the conflict pair, and its alphabet is **closed**: `DD` both
deleted · `AU` added by us · `UD` deleted by them · `UA` added by them ·
`DU` deleted by us · `AA` both added · `UU` both modified. Anything else
is a parse error, not a new state. `m1/m2/m3` and `h1/h2/h3` are stages 1
(base), 2 (ours), 3 (theirs). **Any `u` record sets
`YEW_GIT_CONFLICTED`** — Sprint 52 renders those rows with the conflict
glyph, and Sprint 53's diff base falls back from `:path` (stage 0 does not
exist during a conflict) to `HEAD:path`.

#### 5.5 `?` untracked and `!` ignored

```
? <path>NUL
! <path>NUL
```

- With `--untracked-files=normal` an untracked **directory** is reported
  once as `? dir/` rather than by listing its contents. Keep it that way:
  fuss's `expand_directory` shelled out to `find` per untracked directory
  on every refresh, so a fresh `node_modules/` cost tens of thousands of
  stats twice a second. A trailing-slash record becomes one collapsed
  node; Sprint 52 expands it on demand with the s26 walk.
- We never pass `--ignored`, so `!` records do not appear in *our* output
  — the parser handles them anyway (three lines) so a future
  `--ignored=matching` invocation cannot produce an unknown-record abort.

#### 5.6 Parsed shape and the fuss boolean mapping

```c
typedef enum { GIT_E_ORDINARY, GIT_E_RENAME, GIT_E_UNMERGED,
               GIT_E_UNTRACKED, GIT_E_IGNORED } GitEntryKind;

typedef struct GitEntry {
    GitEntryKind kind;
    char  x, y;                /* '.' when unmodified                   */
    char *path;      u32 path_len;      /* arena; NOT NUL-required      */
    char *orig_path; u32 orig_len;      /* GIT_E_RENAME only            */
    u8    score;                        /* rename/copy similarity       */
    char  index_oid[65];                /* hI; "" when absent           */
    bool  is_dir;                       /* '?' record ending in '/'     */
    bool  submodule;
    /* derived, exactly as fuss's four booleans + the state it lacked   */
    bool  staged, unstaged, untracked, conflicted;
} GitEntry;
```

| fuss field | fuss (v1) test | yew (v2) test |
|---|---|---|
| `is_untracked` | `status == '??'` | `kind == GIT_E_UNTRACKED` |
| `is_staged` | `s(1:1) /= ' ' .and. /= '?'` | `x != '.'` on `1`/`2` records |
| `is_unstaged` | `s(2:2) /= ' '` and not untracked | `y != '.'` on `1`/`2` records |
| — | (absent) | `conflicted = (kind == GIT_E_UNMERGED)` |

**Pitfall — `.` is not a space.** A parser ported by eye from fuss's v1
tests marks *every* tracked file both staged and unstaged, because
`'.' != ' '` is true twice.

### 6. The incoming set — `diff --name-only -z HEAD...@{upstream}`

```
git --no-pager diff --name-only -z HEAD...@{upstream}
```

- **Three dots, not two.** `HEAD...@{upstream}` is the symmetric
  difference relative to the merge base — the files *they* changed that we
  have not seen. `HEAD..@{upstream}` looks similar and is subtly wrong the
  moment your branch has its own commits. fuss got this right; the comment
  carries the why so nobody "simplifies" it.
- Guarded by §5's `# branch.upstream`: no upstream ⇒ the incoming set is
  empty and the state is `YEW_GIT_NO_UPSTREAM`, not a failure. Running the
  diff without one prints `fatal: no upstream configured` every refresh.
- Output is a `-z` list of workspace-relative paths, matched into the
  snapshot by byte equality on a sorted array (`yew_sort_stable` + binary
  search, the fuss shape). Paths present *only* here (clean locally,
  changed upstream) become entries with all four booleans false and
  `incoming = true` — fuss's `add_incoming_files`, and why `↓` can appear
  alone.
- Refreshed on the status TTL but **only** after a `YEW_GV_NET` verb or an
  upstream-oid change: one extra fork per refresh otherwise, and the
  answer cannot change without a fetch.
- Observe that change with the argv-only read
  `git rev-parse --verify @{upstream}` after status/ignore. Cache its full
  object id and the sorted incoming path set in `GitCtx`; an unchanged id
  reuses that set without rerunning `diff`. A NET completion marks the set
  dirty structurally even before the next oid comparison. This extra cheap
  ref read is what notices a fetch performed by another process while yew
  is running.

### 7. The ignore set — one `ls-files` per refresh

```
git --no-pager ls-files -z --others --ignored --exclude-standard \
    --directory --no-empty-directory
```

- `--directory` collapses a fully-ignored directory to one record ending
  in `/`. That is the difference between one record and 90 000 for
  `node_modules/`, and it is why matching is **dir-prefix matching**:

```c
/* `set` is the sorted -z list. A path matches when it equals a non-dir
 * record, or when a record ending in '/' is a prefix of it.            */
bool yew_git_ignored(const GitIgnoreSet *set, const char *p, u32 n);
```

  Binary search for the exact hit first, then walk the path's directory
  prefixes longest to shortest (`a/b/c.x` → `a/b/`, `a/`) probing the set
  — bounded by the path's slash count, not by the set size.
- Once per refresh, never per file. fuss ran `check-ignore --stdin`
  through a fixed temp file; we write nothing to disk (§8).
- This is git's answer, not Sprint 26's bespoke `.gitignore` matcher. Both
  exist on purpose: s26's must work outside a repository and must never
  fork; inside a repo, git's answer is authoritative and covers
  `core.excludesFile`, index awareness, and everything s26 §4 honestly
  lists as unsupported. F mode uses this one; the divergence is in the
  manual.

### 8. No temp files, ever — and why

fuss routed **every** command through a fixed path — `/tmp/fuss_tmp.txt`,
`/tmp/fuss_git_status.txt`, `/tmp/fuss_all_files.txt`,
`/tmp/fuss_incoming.txt`, `/tmp/fuss_check_ignore_{in,out}.txt` — with no
pid suffix and no `mkstemp`. Two instances in one repository race on the
same six files and read each other's output; on a shared machine a path
owned by someone else is a permission error at best and a symlink in your
path at worst. We use pipes: no temp file exists in `src/mod/git/`,
`grep -rn '/tmp/\|mkstemp\|tmpfile' src/mod/git/` is a CI gate, and output
arrives through `YEW_SINK_COLLECT` (s19 §6), capped:

| Constant | Value | Behavior at the cap |
|---|---|---|
| `YEW_GIT_COLLECT_MAX` | 16 MiB | job killed, `YEW_GIT_FAILED` with `git output too large` — a 16 MiB status means something is deeply wrong |
| `YEW_GIT_READ_TIMEOUT_MS` | 5 000 | `YEW_GV_READ` verbs |
| `YEW_GIT_NET_TIMEOUT_MS` | 120 000 | `YEW_GV_NET`; a clone-sized fetch is legitimately slow, a hung auth prompt is not |
| `YEW_GIT_MAX_INFLIGHT` | 4 | concurrent git jobs; a 5th coalesces onto an identical in-flight request or is refused |

### 9. Blame — `--porcelain` and `--incremental` share one record reader

```
git --no-pager blame --porcelain     -- <path>          (whole file)
git --no-pager blame --incremental -L <lo>,<hi> --contents - -- <path>
```

Both emit **commit-group records**:

```
<sha1> <orig-line> <final-line> <n-lines>      ← group header, 1-based
author <name>
author-mail <<email>>
author-time <epoch-seconds>
author-tz <+HHMM>
committer / committer-mail / committer-time / committer-tz   ← same shapes
summary <first line of the commit message>
boundary                                        ← optional, no value
previous <sha1> <path>                          ← optional
filename <path>
```

- Under `--porcelain` each group is followed by `n-lines` content lines
  prefixed with a **literal TAB**, the rest being raw file bytes. Under
  `--incremental` there are no content lines and groups stream out as
  blame discovers them, in no particular order.
- **The pitfall that defines this parser:** the metadata block is emitted
  **once per commit**, not once per group. Second and later groups of an
  already-seen commit carry only the header, sometimes `previous`, and
  `filename`. A parser expecting `author` after every header stalls or
  attributes lines to the wrong person. Keep a `Strmap` of sha1 →
  `GitCommitMeta` in the request arena and fill each group from it.
- `--contents -` feeds the **dirty buffer** on stdin (s19's `in_buf` /
  `in_span`, zero-copy through `TextIter`) — blame that ignores unsaved
  edits is blame for a file the user is not looking at. Uncommitted lines
  come back as the all-zero sha1; render them `(uncommitted)`, never as
  the author name git invents (`Not Committed Yet` is a placeholder, and
  treating it as data makes a real person of that name indistinguishable).
- `-L <lo>,<hi>` is 1-based inclusive and is what makes Sprint 53's
  viewport-scoped blame affordable.

```c
typedef struct GitBlameLine { u32 lineno; u32 commit;      } GitBlameLine;
typedef struct GitCommitMeta {
    char sha[65]; char *author, *author_mail, *summary;
    i64  author_time; i16 author_tz_min; bool boundary;
} GitCommitMeta;

u32 yew_git_parse_blame(Arena *a, const u8 *buf, u64 n,
                        Vec(GitBlameLine) *lines,
                        Vec(GitCommitMeta) *commits, GitParseErr *e);
```

### 10. Pinned `--pretty` formats for log and reflog

```
log:    git --no-pager log    -z --no-color --date=raw \
          --pretty=format:%H%x00%h%x00%at%x00%an%x00%ae%x00%P%x00%D%x00%s%x00%b
reflog: git --no-pager reflog -z --no-color \
          --pretty=format:%H%x00%h%x00%gd%x00%gs%x00%at%x00%s
```

| Choice | Why |
|---|---|
| `-z` | with `--pretty=format:`, `-z` separates **commits** with NUL instead of newline — the only separator a commit body cannot contain |
| `%x00` between fields | commit metadata and messages may contain every byte except NUL. Fixed field counts distinguish the eight/five field separators from `-z`'s record separator; unlike the former US scheme, a literal `0x1f` in `%s` or `%b` cannot corrupt the columns |
| `%at` (epoch) not `%ar`/`%ad` | relative and formatted dates are locale- and clock-dependent; we format from the epoch ourselves so the pty goldens are byte-stable (invariant 5) |
| `%D` (ref names, no parens) not `%d` | `%d` adds ` (…)` decoration we would have to strip |
| `--no-color` explicit | a user's `color.ui = always` injects SGR into `%s` |
| no `--graph` | fuss piped `log --graph … \| fzf`; the graph is drawing, and drawing is ours (Sprint 52 renders the list into a pane) |

**Pitfall — `format:` vs `tformat:`.** `format:` puts the separator
*between* records, so the last record has no trailing NUL; `tformat:`
terminates every record. We use `format:` and treat end-of-buffer as an
implicit terminator — a parser requiring the trailing NUL silently drops
the oldest commit.

### 11. The snapshot cache — wall clock, ping-pong arenas, forced refresh

```c
#define YEW_GIT_TTL_MS 500        /* fuss's CACHE_TTL = 0.5d0, kept      */

typedef struct GitSnapshot {
    Arena  a;                     /* everything below lives here         */
    GitStatusCode state;          /* §12                                 */
    char  *branch, *upstream, *head_oid;
    i32    ahead, behind;         /* -1 = unknown (no branch.ab)         */
    bool   detached, unborn, conflicted;
    Vec(GitEntry)  entries;       /* sorted by path (yew_sort_stable)    */
    GitIgnoreSet   ignored;
    u32    gen;                   /* bumped on every successful refresh  */
    i64    taken_ms;              /* yew_now_ms() at completion          */
} GitSnapshot;

typedef struct GitCtx {
    GitRepo    repo;
    GitSnapshot snap[2]; u8 live;  /* live = index of the published one  */
    bool  refresh_inflight; u32 inflight_job;
    i64   forced_until_ms;         /* 0; set to -1 by invalidate()       */
} GitCtx;

const GitSnapshot *yew_git_snapshot(Ed *ed);      /* never NULL, never blocks */
bool yew_git_refresh(Ed *ed, bool force);         /* true = job spawned  */
void yew_git_invalidate(Ed *ed);                  /* after a mutation    */
```

**Wall clock — the fuss bug this deliverable is named after.** fuss's
`cache_valid` called `cpu_time()`, which reports **process CPU time**. A
program that spends its life blocked in `read()` accrues almost none, so
`current_time - timestamp` stayed under `0.5` essentially forever and the
TTL never expired; the cache refreshed only via `force_refresh=.true.`
after mutating verbs. That is why fuss *appeared* to work — and why a
commit made in another terminal stayed invisible until you pressed a verb
key. Use `yew_now_ms()` (`CLOCK_MONOTONIC`); not `time(2)`, whose NTP step
backwards makes `now - taken` negative and the snapshot immortal.
`grep -rn 'clock(\|cpu_time\|\btime(' src/mod/git/` is a CI gate.

**Forced refresh is structural.** `yew_git_spawn` reads `v->kind` and
calls `yew_git_invalidate` from the completion callback of every
`YEW_GV_MUTATE`/`YEW_GV_NET` verb — no call site decides. fuss threaded
`force_refresh=.true.` through twenty of them by hand.

**Ping-pong arenas.** A refresh parses into `snap[live ^ 1]`, then flips
`live` in one assignment. So: a failed or timed-out refresh leaves the
published snapshot untouched (F mode keeps drawing the last good tree
instead of blanking); no pointer handed out during a frame can dangle
mid-parse; `gen` gives Sprint 52 a cheap "did the world change" test; and
the inactive arena is `arena_free_all`'d per refresh — one bulk free.

**Never synchronous, always coalesced.** `yew_git_snapshot` returns what
is published and, if the TTL expired with nothing in flight, *schedules* a
refresh; a request made mid-flight sets a `refresh_again` bit rather than
spawning a second job (ten mutating verbs ⇒ two refreshes). Session start
publishes an empty snapshot with `gen == 0` and F mode shows `loading…`.
A blocking fork inside a keystroke is a 5 ms violation on the spot.

### 12. The error taxonomy — states, not strings

```c
typedef enum {
    YEW_GIT_OK = 0,
    YEW_GIT_NO_GIT,        /* exec ENOENT, or version < YEW_GIT_MIN     */
    YEW_GIT_NOT_REPO,
    YEW_GIT_BARE,
    YEW_GIT_NO_HEAD,       /* unborn branch: "# branch.oid (initial)"   */
    YEW_GIT_DETACHED,
    YEW_GIT_NO_UPSTREAM,
    YEW_GIT_CONFLICTED,
    YEW_GIT_MID_MERGE,
    YEW_GIT_MID_REBASE,
    YEW_GIT_MID_CHERRY_PICK,
    YEW_GIT_MID_REVERT,
    YEW_GIT_MID_BISECT,
    YEW_GIT_LOCKED,        /* index.lock held by someone else           */
    YEW_GIT_AUTH,
    YEW_GIT_TIMEOUT,
    YEW_GIT_FAILED,        /* nonzero exit; stderr carried, not parsed  */
    YEW_GIT_PARSE,         /* our bug: malformed porcelain (YEW_BUG)    */
} GitStatusCode;

const char *yew_git_state_str(GitStatusCode c);   /* stable, testable   */
```

**In-progress states come from the filesystem, not from message text.**
One `access(F_OK)` sweep inside `repo.git_dir`, on every refresh:

| Probe (relative to `git_dir`) | State |
|---|---|
| `MERGE_HEAD` | `YEW_GIT_MID_MERGE` |
| `rebase-merge/` **or** `rebase-apply/` | `YEW_GIT_MID_REBASE` |
| `CHERRY_PICK_HEAD` | `YEW_GIT_MID_CHERRY_PICK` |
| `REVERT_HEAD` | `YEW_GIT_MID_REVERT` |
| `BISECT_LOG` | `YEW_GIT_MID_BISECT` |
| `index.lock` | `YEW_GIT_LOCKED` — refresh is skipped this tick, the published snapshot stands, no message (a lock is normal and transient) |
| `sequencer/todo` | refines `MID_CHERRY_PICK` / `MID_REVERT` into a multi-commit sequence (Sprint 52 shows `3/7`) |

Seven `access` calls per refresh cost less than the fork we already did,
and they are exact. Matching `You are in the middle of a merge` out of
stderr is not: it is localized, it drifts between git releases, and it
appears only when a command happens to fail.

**The one place we read message text.** `YEW_GIT_AUTH` is raised when a
`YEW_GV_NET` verb exits nonzero **and** its stderr contains one of
`could not read Username`, `Authentication failed`, `terminal prompts
disabled`, `Permission denied (publickey)`. The substrings pick a
*message* ("push failed: credentials required"), never a control path —
the verb failed either way, and the fallback is `YEW_GIT_FAILED` with raw
stderr. Only substring match in `src/mod/git/`; one table; unit-tested.

### 13. Registered commands and Defer

Working this sprint (no screen needed): `ed.git.info` (repo, branch,
state, snapshot age to the message line), `ed.git.refresh`, `ed.git.log`
(populates the record list — the *viewer* is Sprint 52). Deferred,
`YEW_CMD_DEFERRED` naming the sprint (invariant 3):

- Tree and verb surface — `ed.mode.enter F`, `ed.git.stage`,
  `ed.git.commit`, `ed.git.push`, … → **Sprint 52**.
- `ed.git.hunk.*`, `ed.git.blame.toggle`, `ed.git.diff.view`,
  `ed.group.from_dir` → **Sprint 53**.
- **Non-goals, named as such rather than deferred**: submodule recursion
  (the `S<c><m><u>` field is parsed and exposed; we never descend),
  `git worktree` awareness beyond `GIT_COMMON_DIR` passthrough, signature
  verification (`%G?`), git-LFS, and credential handling of our own —
  each is the business of reimplementing git policy.

## Testing Strategy

- **Fixture repo** (`tests/fixtures/git/mkrepo.sh`): `GIT_AUTHOR_*` and
  `GIT_COMMITTER_*` name/email/date pinned, `commit.gpgsign=false`,
  `init.defaultBranch=trunk`, `core.autocrlf=false` — so **every object
  hash is byte-identical on every machine** and goldens may contain them.
  Contents: staged, modified, staged-and-modified, untracked file,
  untracked directory, ignored file, ignored directory, a rename at score
  100, a copy, a type change (file → symlink), filenames containing a
  space / a `"` and a `$` / a newline, a file named `-n`, a CJK filename,
  a 4 KiB path, a conflicted merge (`UU`), and a detached-HEAD variant.
  `HARNESS_SKIP` when `git` is absent.
- **Unit — `test_porcelain.c`** (pure bytes, no forking): every §5 record
  kind; every `XY` combination against §5.6's mapping; the `2` record
  **byte-consumption** test (rename followed by five ordinary entries — a
  one-NUL-per-record parser fails here and nowhere else); a path with a
  newline surviving intact; `# branch.*` present/absent matrices including
  `(initial)` and `(detached)`; a buffer truncated at **every** byte offset
  never reads out of bounds and returns `YEW_GIT_PARSE`; unknown `#`
  headers and record kinds skipped, not fatal; blame's omitted-metadata
  case (three groups, one commit, metadata on the first only); `format:`
  last-record handling; a `%s` containing `0x1f`.
- **Unit — `test_gitcache.c`** (fake clock): TTL at 499/500/501 ms; the
  fuss bug driven explicitly — a snapshot, then 10 s of *wall* time with
  zero CPU consumed, must expire (this test fails against a `clock(3)`
  implementation and its comment says so); invalidation from a
  `YEW_GV_MUTATE` completion; ping-pong survival (a refresh failing
  mid-parse leaves the published snapshot byte-identical, `gen` unchanged);
  coalescing (10 requests ⇒ 2 spawns); `index.lock` ⇒ refresh skipped.
- **Script** (`tests/script/git_*`, headless C driver until Sprint 37):
  the parsed entry set asserted exactly against the fixture repo, awkward
  filenames included; `ed.git.info` for each mid-* state (produced by
  touching the probe files); the incoming set with and without an upstream;
  dir-prefix ignore matching (a 5 000-file `node_modules/` ⇒ **one**
  record).
- **Argv integrity**: a hook records every spawn's exact `argv` — nothing
  produced by formatting, `--` before every path, the `-n` and
  `"`-containing filenames reaching `execve` byte-identical to porcelain's
  bytes. **Env**: a fake `git` on `PATH` dumps its environment; assert
  every §3 row and that the parent's `environ` is unmutated.
- **Fuzz** (`fuzz_porcelain.c`): random bytes, well-formed-prefix-then-
  garbage, 4 KiB paths, embedded NULs and newlines — 200 000 cases × 4
  seeds; never out of bounds, never loops, always deterministic.
  **Perf** (`tests/perf/git_status.c`): 20 000-entry parse ≤ 8 ms; zero
  allocations outside the snapshot arena (s00 allocation hook).
- Lanes: gcc, clang, ASan/UBSan, valgrind, determinism, `MODULES=""`.

## Definition of Done

1. `make test` green on gcc and clang at `-std=c11 -pedantic -Wall
   -Wextra -Werror`; ASan/UBSan/valgrind/determinism lanes green; the
   `MODULES=""` build compiles and every `ed.git.*` command reports
   `this build has no fuss module; rebuild with 'make MODULES="… fuss"'`
   verbatim.
2. `grep -rn '/tmp/\|mkstemp\|tmpfile\|system(\|popen(' src/mod/git/` →
   empty. `grep -rn 'clock(\|cpu_time\|\btime(' src/mod/git/` → empty.
   `grep -rn 'sprintf\|bytebuf_printf' src/mod/git/ | grep -v porcelain.c`
   → empty.
3. Every record kind in §5 (`#` ×5, `1`, `2`, `u`, `?`, `!`) has a passing
   parse assertion, and every `XY` pair in §5.2/§5.4 drives the §5.6
   mapping table.
4. The rename desync test passes and **fails** when `pv2_record` is
   patched to consume one NUL (the test's comment pins this).
5. All six awkward filenames (space, `"`, `$`, newline, `-n`, CJK) survive
   porcelain → parse → argv → `execve` byte-identical, proven by the argv
   hook.
6. Truncation fuzz: cutting the fixture buffer at every one of its N byte
   offsets never reads out of bounds (ASan) and never returns `YEW_GIT_OK`
   with a partial entry.
7. TTL: the zero-CPU/10-s-wall-clock test expires the cache; the
   fake-clock rows at 499/500/501 ms are exact.
8. Every `YEW_GV_MUTATE`/`YEW_GV_NET` verb invalidates the cache without
   its call site saying so (asserted by a table-driven test that walks the
   whole verb table and checks `gen` advanced).
9. A refresh failing mid-parse leaves the published snapshot byte-identical
   (memcmp of the serialized snapshot) and `gen` unchanged.
10. All 18 `GitStatusCode` values are reachable in tests; the five mid-*
    states are produced by filesystem probes only —
    `grep -rn 'strstr' src/mod/git/git.c` shows hits **only** inside the
    §12 auth-message table.
11. The environment test asserts all 13 rows of §3 and that `environ` is
    unmutated.
12. Perf: 20 000-entry parse ≤ 8 ms; a 500 ms-TTL refresh loop over the
    fixture repo keeps keypress p99 ≤ 5 ms under scripted typing.
13. ≥ 140 assertions across the two unit files; the fixture repo produces
    identical object hashes on two different machines (CI asserts a pinned
    hash list).
14. `ed.mode.enter F` and every Sprint 52/53 command hard-error naming
    their sprint.
