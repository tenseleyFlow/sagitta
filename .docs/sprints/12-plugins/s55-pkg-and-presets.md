# Sprint 55: `yew pkg`, Example Plugins, and the Cloud-Storage Preset

## Prerequisites

- **Sprint 54 — binding, do not modify.** The pure-literal `plugin.fl`
  manifest and `yew_plug_manifest_read`, three-dir discovery with
  `$XDG_DATA_HOME/yew/plugins/` as row 1 (**this sprint owns row 1**),
  origin ids, the registration ledger (`RegKind` incl. `REG_ATTR`,
  `REG_OVERLAY`), the `ctx` field list, capability gating + the trust db
  `plugins:` map, the honest trust text, `yew plug list/enable/disable/
  reload`, the `MODULES=plugins` hard-error string. `pkg.c` is the file
  s54 reserved; `yew pkg` stops hard-erroring here.
- **Sprint 19 — `src/edit/job.c`, and it is CORE.** `YewJobSpec` (argv
  form: "no shell, no `-c` string"), `yew_job_spawn/pump/reap/signal`,
  `YEW_SINK_COLLECT`, the exec-status pipe (`YEW_JOB_EXECFAIL` ≠ exit
  127), `YEW_JOB_TERM_GRACE_MS`, the banned-`system()`/`popen()` gate,
  `PAGER=cat`. This is the subprocess layer `yew pkg` uses (§1).
  **Sprint 51 (index entry only; file unwritten)** puts repo detection,
  porcelain parsers, and the status cache behind `MODULES=fuss`; `yew pkg`
  must not link it, and §1 pins how.
- **Sprint 8 — the machinery the cloud preset reasons about.** The six-row
  save decision table, `atomic_save`/`inplace_save`,
  `YEW_SAVE_CHANGED_ON_DISK`, the `.yew-<base>-<pid>-<n>` temp name, the
  crash journal, `yew_xdg_state_dir()`, and the kill -9 torture harness
  (whose in-place lane this sprint extends).
- **Sprint 36** — `OptDesc`/`yew_opt_set` and its one-table gate,
  `set(map)`, `bind()` with closures + cross-origin `unbind` refusal, the
  trust db, `yew_origin_teardown`, `fl_data_write`/`fl_data_read`, and its
  pinned deferral: **per-path option overrides are expressed with
  `on("buf.open", …)`, not a declarative table**. **Sprint 25** —
  pure-literal format v1 (six value kinds, exhaustive escapes, trailing
  comma after every element, two-space indent, no floats), `yew_ws_key`,
  FNV-1a-64 rendered `%016lx`.
- **Sprints 31/34/37** — `io.*` (`read/write/exists/size/mkdir/glob/env`,
  all `fs`-gated; **no `io.run`, no `io.http`**), the module system and
  spec §11's three-row import table; handles, `edit{}`, `on(...)`,
  `opt.get/set`; `--batch`, `--grant NAME:CAP`, exit codes. **Sprint 39** —
  the closed 54-entry `SynAttr` table and its *meanings-never-colors* law.
  **Sprint 21** — viewport overlays under a 1000 µs frame budget.
- Binding: `00-decisions.md` (Plugins row — "`yew pkg` git-based
  installer. Cloud-storage workflow ships as a preset config, not code";
  optional tools degrade gracefully; invariants 1, 3, 5, 7),
  `01-architecture.md` (`src/mod/plug/pkg.c`, `runtime/`),
  `02-fletch.md` (requirements 6, 8).

**Assumptions beyond earlier index entries.** (a) `yew_xdg_data_dir()`
exists beside `yew_xdg_state_dir()`; if not it lands here in `src/util/`.
(b) s54 froze the `ctx` field *list* but no signatures, and its
Deliverable 9 names the author guide as the contract boundary — so §11's
table is normative, not descriptive. (c) s31 spellings used below:
`re.compile`, `str.{split,contains,starts_with,slice,rfind}`,
`list.{push,len,copy,reverse,contains}`, `fmt.{f,str}`,
`io.{glob,mkdir,exists,size,env}`; adapt if the landed names differ.

## Goals

Land distribution and the cloud workflow: `src/mod/plug/pkg.c`, a
git-backed installer built on s19's **core** job layer so it works with
`MODULES=plugins` and `fuss` excluded; the `plugins.lock` pure-literal
lockfile with pinned field order and a bespoke content tree hash;
verify-on-load whose drift path **revokes persisted capability grants**
instead of silently accepting changed code; offline behaviour and a
network-error contract that never parses git's prose;
`runtime/preset.cloud.fl`, the cloud-storage preset (a config, not a
plugin) that reasons explicitly about s08's rename dance versus a sync
daemon; the six new options it needs; and the author guide plus the `yew
pkg` man section. There is **no central registry and no signing in 1.0** —
pinned in §7, not deferred. The two shipped example plugins under
`examples/plugins/` are summarized in §8–§9 and land in full, with their
READMEs, their script tests and the API-coverage CI gate, in
**`s55_5-example-plugins.md`**; this sprint provides the install path they
use and the guide chapters they are walked in. Budget calibration is
Sprint 56; man assembly and the tutor are Sprint 59.

## Deliverables

### 1. Module boundary and the git subprocess contract — `src/mod/plug/pkg.c`

```c
typedef struct GitRun { int status; Bytebuf out, err; bool timed_out; } GitRun;

/* argv form ONLY (s19 §9's law): every element a separate string, no shell.
 * Headless — `yew pkg` runs before any TUI exists, so this pumps a private
 * poll loop over the job's fds exactly as s19 §5's filter does, minus the
 * tty and typeahead handling.                                             */
bool yew_pkg_git(const char *const *argv, u32 nargv, i64 timeout_ms,
                 bool c_locale, GitRun *out);
```

**The `MODULES=fuss` independence pin.** `src/mod/git/` owns repo
detection, porcelain parsing, and the status cache; `pkg.c` calls none of
it. It needs four things git answers with plumbing — resolve a rev, test
ancestry, list commits, test whether `status --porcelain` is empty — and
none needs a parser worth sharing. Gate: `grep -rn 'src/mod/git\|yew_git_'
src/mod/plug/` prints nothing, and CI builds `MODULES=plugins` (no `fuss`)
and runs the whole pkg suite there.

**Version floor 2.24**, because every user string reaches git behind
`--end-of-options` (§2's injection guard), which landed in 2.24. Below it,
or with git absent, networked subcommands error per §6; `list` and
`doctor`'s hash rows still work, because the tree hash is ours.

Child environment, on top of s19 §7:

| Variable | Value | Why |
|---|---|---|
| `GIT_TERMINAL_PROMPT` | `0` | a credential prompt on a pipe hangs **forever**; this turns the hang into a failure we can report |
| `GIT_ASKPASS` | `""` | same, graphical path |
| `SSH_ASKPASS_REQUIRE` | `never` | same, `ssh://` remotes |
| `GIT_DIR`, `GIT_WORK_TREE`, `GIT_INDEX_FILE`, `GIT_OBJECT_DIRECTORY`, `GIT_COMMON_DIR` | **removed** | inherited from a hook or `git rebase --exec`, these silently retarget every command at *their* repo |
| `LC_ALL` | `C` on parsed invocations only (§5) | plumbing stays byte-stable; `clone`/`fetch` keep the user's locale because we only *display* their stderr |
| `PAGER`, `GIT_PAGER` | `cat` (s19 already) | a paging child with no tty is the classic silent hang |

**Not** cleared: the user's git config. Proxies, `insteadOf` rewrites, and
credential helpers live there; `GIT_CONFIG_NOSYSTEM=1` would look tidy and
break exactly the people with the hardest network.

### 2. Command surface and source resolution

```
yew pkg install <spec> [--rev R | --tag T | --branch B] [--enable]
yew pkg update  [<name>…] [--dry-run] [--discard-local]
yew pkg remove  <name>   [--keep-grants]
yew pkg list    [--long]
yew pkg doctor  [<name>…] [--fix | --accept]      # `verify` is an alias
```

Exit codes per the s00 ledger: 0 ok · 1 usage/refusal · 3 I/O or network ·
4 `yew_bug`. `YEW_PKG_{USAGE,NOT_FF,DIRTY,LOCK}` → 1;
`YEW_PKG_{NET,GIT_MISSING,IO}` → 3.

Spec resolution — a **closed** table in one `static const struct { const
char *prefix, *fmt; }` array:

| Input | Resolves to |
|---|---|
| `gh:user/repo` | `https://github.com/user/repo.git` |
| `gl:user/repo` | `https://gitlab.com/user/repo.git` |
| `cb:user/repo` | `https://codeberg.org/user/repo.git` |
| `sr:~user/repo` | `https://git.sr.ht/~user/repo` |
| `https://…` `http://…` `ssh://…` `git://…` | verbatim |
| `git@host:path/repo.git` (scp-like) | verbatim |
| `/abs`, `./rel`, `file://…` | verbatim — local clone, the development path |
| anything else | error listing every form above |

- **A bare `user/repo` is rejected**: ambiguous with a relative path, and
  defaulting it to GitHub bakes a registry into a tool whose §7 says it
  has none.
- **Option injection.** Every user string that becomes a git argument sits
  behind `--end-of-options` (or `--`, per §5), and refs are validated
  first against `[A-Za-z0-9._/-]{1,255}` with a leading `-` and any `..`
  component rejected. A tag named `--upload-pack=curl…` is the
  remote-code-execution shape here.
- **The directory name comes from the manifest, never the URL.** s54
  requires `name` == basename, so the clone lands in a temp dir, the
  manifest is read, and the destination is `plugins/<manifest.name>`.
  There is no `--name`: renaming breaks the one identity the trust db,
  lockfile, `ctx.name`, and option namespace all key on.
- **`install` writes `enabled: false`** into the trust db and prints
  `yew plug enable <name>`, unless `--enable`. s54's default for an
  *absent* key stays `true` (a hand-copied directory just works); pkg sets
  it explicitly because downloading is not consenting, and a capability
  prompt only means something if the user chose the moment it appears.
- `yew pkg publish`, `search`, `--recurse-submodules`, and any
  `dependencies` handling hard-error naming §7 or s54.

### 3. The lockfile — `$XDG_DATA_HOME/yew/plugins.lock`

Pure-literal Fletch in the s25 v1 format, written with `fl_data_write`
through s08's atomic write, read with `fl_data_read` — literals only, so a
lockfile can never execute.

```fletch
# yew plugin lockfile.  Generated by `yew pkg`; hand-editable, but
# `yew pkg doctor` is the only thing that will agree with your edits.
{
  schema: 1,
  plugins: {
    "session-notes": {
      url: "https://github.com/u/yew-session-notes.git",
      shorthand: "gh:u/yew-session-notes",
      rev: "6b1f0c2d9a4e7f31b8c05de2a7419f3c6d0b8e15",
      pin: "tag:v1.2.0",
      tree: "e3a91c77f0b24d58",
      installed_at: 1785600000,
      updated_at: 1785686400,
    },
  },
}
```

| Field | Type | Rule |
|---|---|---|
| `schema` | int | `1`; unknown top-level **and** per-entry keys preserved across a rewrite (forward compat, same rule as the trust db) |
| `url` | str | the resolved absolute URL actually handed to git |
| `shorthand` | str | what the user typed, or `""`, so `list` echoes it back |
| `rev` | str | exactly 40 lowercase hex — the checked-out commit |
| `pin` | str | `rev:<40hex>` \| `tag:<name>` \| `branch:<name>` \| `head` |
| `tree` | str | 16 lowercase hex (§4) |
| `installed_at`, `updated_at` | int | Unix seconds; the only nondeterministic fields, and never an input to anything |

**Deterministic key order, pinned.** Top level `schema` then `plugins`;
plugin names sorted bytewise with `yew_sort_stable`; within an entry the
fixed order above — never insertion order, never hash order. Why it earns
a DoD item: this file lands in dotfile repos, and a lockfile that
reshuffles on every update makes review worthless; two machines installing
the same set must emit identical bytes (invariant 5).

```c
typedef struct PkgEntry {
    const char *name, *url, *shorthand, *pin;   /* arena-owned */
    char rev[41], tree[17];
    i64  installed_at, updated_at;
    FlValue extra;                              /* preserved unknown keys */
} PkgEntry;
typedef struct PkgLock { Arena a; u32 schema; Vec(PkgEntry) v; } PkgLock;

bool      yew_pkg_lock_load(PkgLock *, DiagCtx *);   /* absent = empty, OK */
bool      yew_pkg_lock_save(const PkgLock *, DiagCtx *);  /* atomic, sorted */
PkgEntry *yew_pkg_lock_find(PkgLock *, const char *name, u32 nlen);
```

**Pitfall — absent versus corrupt.** An absent lockfile is normal (a
hand-copied plugin has none) and loads as empty. A lockfile that fails to
parse is an **error**, and pkg refuses to write over it: regenerating
discards every recorded tree hash, which is the exact record §4 exists to
protect. `--force-relock` is the escape hatch and it names what it
destroys.

### 4. Integrity — the content tree hash

```c
/* 16 lowercase hex + NUL.  false + diagnostic on any I/O error: a hash
 * that silently skips an unreadable file is worse than no hash.          */
bool yew_pkg_tree_hash(const char *dir, char out[17], DiagCtx *dc);
```

```
entries = every regular file and symlink under `dir`, recursively,
          EXCLUDING the top-level ".git/" subtree; empty dirs contribute
          nothing (git does not track them)
rel     = path relative to `dir`, as BYTES
sort entries bytewise by rel (yew_sort_stable)
h = FNV-1a-64 offset basis
for e in entries:
    h = fnv(h, rel); h = fnv(h, "\0")
    if symlink: h = fnv(h, "L\0"); h = fnv(h, readlink(e))    # never followed
    else:       h = fnv(h, (mode & 0111) ? "X\0" : "F\0")
                h = fnv(h, u64 size, little-endian, 8 bytes)  # length prefix
                h = fnv(h, file contents)
    h = fnv(h, "\0")
render "%016lx"
```

- **`.git/` excluded**: it churns on every fetch and `git gc`, and it is
  not the plugin. **Only the executable bit** of the mode is hashed — git
  records nothing else, and a umask difference must not manufacture drift.
  **Symlinks hashed by target, never followed**: following one hashes
  files the plugin does not own and can loop forever. The **length
  prefix** kills the boundary-ambiguity family (`"ab"+"c"` versus
  `"a"+"bc"` hashing alike).
- **Not a cryptographic hash, and the header says so.** FNV-1a-64 detects
  accident and casual drift, not a crafted collision — and upgrading it
  would not change the threat model, because the lockfile sits in the same
  user-writable tree as the plugin, so anyone who can edit the code can
  edit the recorded hash. What it buys is drift detection against
  processes that are not attacking you (a stray `git checkout`, an
  unfinished local patch, another tool) plus forced re-consent when code
  under a granted capability changes. Invariant 7 forbids a crypto
  dependency; claiming security we do not have is a review rejection
  (s54 §7's rule, applied here).

**Verify on load.** `plug.verify_on_load` (bool, default true; §10) makes
s54's loader hash each managed directory before running its entry module:

| Condition | Behaviour |
|---|---|
| tree matches the lock | load normally |
| differs, plugin holds **no** persisted grants | one message-line warning + `yew_log(WARN)`: `plugin "x" changed on disk since install (yew pkg doctor x)`; load |
| differs, plugin **holds** persisted grants | same warning **plus the grants for that plugin are dropped from the trust db**, logged `revoked N persisted grant(s) for "x": code changed`; loads and re-prompts on first use |
| in the lock, directory absent | state `missing`; `doctor --fix` reinstalls at the locked rev |
| directory present, absent from the lock | state `unmanaged` — normal for config-dir plugins; **never** a warning |

The revocation row is why the hash is recorded at all. A plugin directory
is user-writable; "allow always" was answered about the code that was
there then, and letting different code inherit that answer is exactly how
a grant gets abused after the fact. Never silently accept — and never
silently re-lock: `doctor --accept` records local reality, `doctor --fix`
restores the recorded rev. Two verbs because they are opposite intentions,
and merging them is how people destroy the patch they were debugging.
Budget: hashing 20 fixture plugins (median 24 KiB) ≤ 3 ms.

### 5. install / update / remove / list / doctor

Every row is argv, never a string:

| Purpose | argv (after `git`) | parsed | `LC_ALL=C` | net | timeout |
|---|---|---|---|---|---|
| probe | `--version` | first line | yes | no | 5 s |
| clone | `-c advice.detachedHead=false clone --quiet -- URL DIR` | no | no | **yes** | net |
| fetch (branch/head) | `-C DIR fetch --quiet --prune --tags -- origin` | no | no | **yes** | net |
| fetch (tag) | `-C DIR fetch --quiet --prune --no-tags -- origin +refs/tags/T:refs/yew-pkg/tags/T` | no | no | **yes** | net |
| default branch | `-C DIR remote set-head origin --auto`; then `-C DIR symbolic-ref --short refs/remotes/origin/HEAD` | 2nd | yes | 1st | net / 10 s |
| resolve | `-C DIR rev-parse --verify --end-of-options REV^{commit}` | stdout | yes | no | 10 s |
| ff-check | `-C DIR merge-base --is-ancestor --end-of-options CUR TGT` | **exit status only** | yes | no | 10 s |
| changelog | `-C DIR log --no-merges --no-color --format=%h%x09%s --end-of-options CUR..TGT` | stdout | yes | no | 10 s |
| dirty | `-C DIR status --porcelain --untracked-files=no` | **emptiness only** | yes | no | 10 s |
| checkout | `-C DIR checkout --quiet --detach --end-of-options REV` | no | no | no | 30 s |

Net timeout `YEW_PKG_NET_TIMEOUT_MS` (default 60 000, `--timeout`). The
`status` row is an emptiness test, not a parse — which is why it is not an
s51 dependency.

```
install:
 1. spec -> url (§2); reject a leading '-' in every ref argument
 2. tmp = <plugins>/.pkg-tmp-<pid>-<n>     # SAME dir as the destination, so
                                           # rename(2) is atomic, never EXDEV
 3. clone (FULL, not --depth 1)
 4. rev = resolve(--rev R | refs/tags/T | branch head | HEAD)
 5. checkout --detach rev
 6. yew_plug_manifest_read(tmp) -> name, caps_wanted     (s54 validation)
 7. dest = <plugins>/<name>; exists -> error naming update/remove
 8. tree = yew_pkg_tree_hash(tmp)          # after checkout, before rename
 9. rename(tmp, dest); fsync(<plugins>)    # s08's durability rule
10. upsert lock entry; yew_pkg_lock_save; print name, rev[0..12], pin, and
    the DECLARED capabilities; set enabled:false unless --enable
 *. any failure -> yew_rmtree(tmp), nothing else changed
```

- **Full clone.** `--depth 1` breaks `git log CUR..TGT` (the changelog
  delta), breaks pinning to an older tag, and breaks the ancestry test
  that makes update fast-forward-only. A Fletch plugin is tens of
  kilobytes; buying all three back is not a trade.
- **`.git` is kept** (update needs it) and excluded from the hash (§4).
  **Submodules are never recursed**; a `.gitmodules` yields one warning
  that its content is absent, because recursing multiplies the trust
  surface across repositories the user never named.
- **Nothing from the repository executes at install time.** No `Makefile`,
  no `install.sh`, no `postinstall` key (s54 rejects unknown keys anyway).
  Clone, checkout, read the manifest with `fl_data_read`, hash. A plugin's
  code first runs when the user enables it. Post-install hooks are the one
  feature every package manager regrets; 1.0 does not ship the regret.

```
update (per named plugin, or all, in lockfile order):
 1. pin == "rev:…" -> "pinned at <rev12>", no network, done
 2. branch/head -> fetch --prune --tags; tag -> fetch exactly that remote tag
    into the private `refs/yew-pkg/tags/` namespace
 3. tgt = tag:T -> refs/yew-pkg/tags/T^{commit}
                 | branch:B -> refs/remotes/origin/B
                 | head -> refs/remotes/origin/HEAD
 4. tgt == cur -> "up to date", done
 5. merge-base --is-ancestor cur tgt:  0 -> legal · 1 -> REFUSE · else -> exit 3
 6. print the changelog delta `%h<TAB>%s`, oldest last, capped at 50 lines
    plus "… and N more".  --dry-run stops HERE, having touched nothing.
 7. status --porcelain non-empty -> refuse, list files, name --discard-local
 8. checkout --quiet --detach tgt
 9. tree = tree_hash(dir); rev/tree/updated_at updated; lock saved
```

- **Never a merge, never a pull.** `git pull` joins s19's `system()`/
  `popen()` grep gate. Pull is fetch+merge; a merge can conflict; a
  conflicted working tree inside a plugin directory is a broken plugin
  with no visible cause. We fetch, prove descendancy, check out a detached
  rev — so the tree is always a checkout of a rev the lockfile names and
  never a branch someone can accidentally commit to.
- The refusal, verbatim:

```
yew pkg: refusing to update "trailing-ws": 0f4d8a1177c3 is not an ancestor
  of 91b2ee40cd77 — the branch was force-pushed or history was rewritten.
  Inspect it (git -C <dir> log --oneline --all), then either
  `yew pkg install --rev <sha>` deliberately, or pin a rev you trust.
```

- **A moved tag is treated identically.** `pin: "tag:v1.2.0"` whose object
  changed since install *is* rewritten history, and mutating a release tag
  is the cheapest supply-chain move there is. Detecting it costs one
  comparison.  The private ref above is deliberate: ordinary tag fetches
  reject or overwrite a moved local tag depending on flags and Git version;
  neither result gives pkg a stable old-vs-remote comparison.  Updating the
  private ref leaves the checkout's local tags untouched and makes the
  refusal deterministic.

**remove**: the entry must be in the lockfile (else "x is not managed by
yew pkg" plus where the directory actually lives); the directory must
`realpath`-resolve inside the plugins dir; the plugin's grants **and** its
`enabled` key are dropped unless `--keep-grants`, so a reinstall re-asks;
then `yew_rmtree(dir, plugins_root)`.

**Pitfall — `yew_rmtree` and symlinks.** Descend with `openat(dirfd, name,
O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)`, remove with `unlinkat(…,
AT_REMOVEDIR)`. A plugin containing `docs -> /` must lose the symlink, not
the filesystem. `must_contain` is checked against `realpath(root)` before
the first `unlinkat`, so a relative-path bug cannot escape.
`system("rm -rf")` is banned twice over.

**list** — porcelain, tab-separated, discovery order, byte-stable:
`NAME\tPIN\tREV12\tSTATE\tURL`; states `ok drift missing unmanaged`.

**doctor** — per plugin, deterministic order; exit 0 iff every row passes:

| Check | Failure state | `--fix` | `--accept` |
|---|---|---|---|
| lockfile parses | hard error, refuses to rewrite | — | — |
| directory exists | `missing` | reinstall at locked rev | — |
| manifest reads + validates (s54) | `error` + the diagnostic | — | — |
| manifest `name` == directory name | `error` | — | — |
| tree hash matches | `drift`, listing added/removed/changed rel paths | `checkout --force <rev>`, re-verify | rewrite `tree` |
| `<dir>/.git` present and a repo | `untracked-tree` | re-clone at locked rev | — |
| `HEAD` == locked `rev` | `rev-mismatch` | checkout locked rev | relock to `HEAD` |
| declared vs granted capabilities | informational listing | — | — |
| directories in `plugins/` absent from the lock | `unmanaged` (informational) | — | — |

Only the last three rows need git. The hash check — the one that matters
for integrity — is bespoke and works on a machine that never had git.

### 6. Offline behaviour and network failures

| Situation | Detected by | Message | Exit |
|---|---|---|---|
| git absent | `PATH` probe | `yew pkg: error: git not found in PATH` + `yew pkg installs plugins over git; you can also copy a plugin directory into <plugins> by hand` | 3 |
| git too old | version parse | names the found version and `2.24` | 3 |
| clone/fetch failed | **exit status only** | `yew pkg: error: git clone failed (exit 128)`, then git's stderr verbatim, indented, under `git said:` | 3 |
| auth would prompt | `GIT_TERMINAL_PROMPT=0` turns the hang into a failure | git's message verbatim + `this remote needs credentials; configure a credential helper or use an ssh:// URL` | 3 |
| timeout | our timer → SIGTERM to the group, SIGKILL after `YEW_JOB_TERM_GRACE_MS` | `yew pkg: error: git fetch timed out after 60s (--timeout SECONDS)` | 3 |
| `update`, some unreachable | per-plugin accumulation | continue to the next; summary `2 updated, 3 up to date, 1 unreachable`; exit 3 iff any failed | 3 |
| `list`, `doctor` hash rows, `remove` | no network at all | work fully offline | 0/1 |

**Pitfall — never classify by parsing git's prose.** Matching English
error text breaks on the user's locale, on a version bump, and on custom
transports. We classify by *exit status* and *our own timer* and show
git's stderr verbatim — which is why `clone` and `fetch` are the two rows
in §5 that keep the user's locale.

**Pinned: nothing on the editor's startup path ever runs git.** Discovery,
manifest reading, and tree verification are pure filesystem. `yew pkg` is
a CLI subcommand; there is no auto-update, no background fetch, no
"updates available" badge. An editor that talks to the network at start is
an editor that hangs at start behind a captive portal.

### 7. No registry, and what that means — honest, in-tree

`pkg.c`'s header comment is the source of truth; §11's docs quote it:

> `yew pkg install gh:someone/thing` runs `git clone` and puts someone
> else's code into your plugin directory. There is no registry, no review,
> no namespace ownership, no signature, no vetting. The only thing between
> you and that code is your judgement about the repository you named — and
> a URL is not a reputation. yew's capability prompts limit what an
> **enabled** plugin can reach (fs, shell, net, clipboard) and nothing
> else; a plugin holding no capabilities at all can still read every
> buffer you have open, because it runs in the same VM and the same
> address space as your editor. The prompts are a blast-radius control,
> not a security boundary. The tree hash tells you the code changed since
> you agreed to it. Together that is honest defence in depth, and it is
> not a substitute for reading a few hundred lines of Fletch.

| 1.0 non-goal | Reason |
|---|---|
| central registry | a registry implies curation, and we would not do the curation |
| signature verification | needs key distribution *and* a crypto dependency (invariant 7); a signature over an unreviewed repo authenticates the author, not the code |
| dependency resolution | s54's manifest rejects `dependencies`; a solver is a project, not a feature |
| post-install / build hooks | arbitrary code execution before any prompt exists (§5) |
| auto-update | §6's startup rule |

### 8. `examples/plugins/trailing-ws/` — hooks, attrs, commands, options

The hello-world example: a `warning`-attr overlay over trailing whitespace
plus an on-save fixer. Declares **no capabilities**, so enabling it never
prompts; exercises `ctx.set`/`opt.get`, `ctx.attr`, `ctx.overlay` (visible
range only, inside s21's frame budget), `ctx.command`, and the
`buf.open`/`buf.save` hooks — the save hook joins the save's transaction,
so the stripped bytes are the bytes that reach disk and one `u` undoes it.

**Full annotated source: `s55_5-example-plugins.md` §2.**

### 9. `examples/plugins/session-notes/` — the fs capability and workspace state

A scratch note per workspace, stored under s25's
`$XDG_STATE_HOME/yew/workspaces/<hash>/` and never in the project,
behind a toggle key read from an option (a plugin that hardcodes a chord
takes it from you: s36 refuses cross-origin `unbind`). It is the smallest
honest use of the `fs` capability — the prompt fires on the first keypress
that needs the filesystem, not at enable — and it degrades when
`ctx.ws.state_dir()` is nil under `--clean`/`--batch`.

**Full annotated source: `s55_5-example-plugins.md` §3.**

Both examples ship a one-screen `README.md` (fixed five-section shape,
specified in s55_5 §4) whose install line is
`yew pkg install ./examples/plugins/<name>` — §2's local-path row is
exactly this workflow, and it is that row's documented first use.

### 10. The cloud-storage preset — `runtime/preset.cloud.fl`

**Why a preset and not a plugin.** It is settings plus two hooks; it runs
under the user's own origin so `fs` is already granted and no prompt ever
appears; it needs no install, no lockfile entry, no capability surface;
`yew_origin_teardown` already unwinds it on a config reload. Shipping it
as a plugin would add a trust decision to something that only changes the
user's own options.

**Reaching it — spec amendment A3.** `.docs/fletch-spec.md` §11's
resolution table gains a fourth row, searched **last** so a user copy
always shadows the shipped file; s33's conformance suite gains one case in
the same commit.

| Date | § | Old → New | Reason |
|---|---|---|---|
| (sprint date) | 11 | 3 resolution rows → 4; row 4 = `$YEW_RUNTIME_DIR/` | a shipped preset the user cannot `import` is not a preset; rows 2–3 still win, so `~/.config/yew/fl/preset.cloud.fl` shadows it and stays editable |

**New options** (added to s36's table, owner sprint 55; each default must
equal the C constant, and the DoD asserts it):

| Option | Type | Scope | Default | Meaning |
|---|---|---|---|---|
| `save.strategy` | enum `auto\|atomic\|inplace` | buffer | `auto` | `auto` = s08's six-row table unchanged; `inplace` forces s08's `inplace_save` even when rename is available; `atomic` forces rows 1/4 and *fails* rather than falling back |
| `save.check_disk` | enum `off\|mtime\|content` | buffer | `mtime` | `mtime` = s08's stat comparison; `content` additionally byte-compares before declaring `YEW_SAVE_CHANGED_ON_DISK` |
| `save.check_disk_max` | int 0–4 GiB | global | 8 MiB | above this, `content` degrades to `mtime`; stated, not silent |
| `save.backup_keep` | int 0–64 | global | 1 | backups retained per file, rotated `<fnv64(realpath)>.<N>.bak`; `1` reproduces s08's behaviour byte-for-byte |
| `save.backup_dir` | str | global | `""` → `yew_xdg_state_dir()/backup/` | s08's location, now nameable |
| `plug.verify_on_load` | bool | global | true | §4 |

```fletch
# preset.cloud.fl — editing inside a directory another process is also
# writing: Syncthing, rclone, Dropbox, Nextcloud, Seafile, iCloud, OneDrive.
#
#     # ~/.config/yew/init.fl
#     import "preset.cloud.fl" as cloud
#     cloud.watch(io.env("HOME") + "/Sync")      # one call per synced root
#
# WHAT THIS PRESET CANNOT DO, first, because everything else depends on it.
# Between the moment yew stats a file to check it has not moved and the
# moment it replaces that file, a sync daemon can write the same path.
# POSIX offers no compare-and-swap on a path name, advisory locks are not
# shared across a sync boundary, and we are one single-threaded process
# among several that each believe they own the file.  That window cannot be
# closed from here and this file will not pretend otherwise.  Instead, in
# descending order of how much it actually helps:
#   1. stop CAUSING conflicts  — keep the file's identity across saves and
#                                put no temp file in the synced tree;
#   2. make a loss VISIBLE     — compare content before overwriting, and
#                                warn when a conflict twin appears;
#   3. make a loss RECOVERABLE — more backups, plus the crash journal.
#
# Ignore patterns your sync client should have (yew cannot install them;
# its temp files are named `.yew-<base>-<pid>-<n>`, s08):
#   Syncthing .stignore : .yew-*        rclone : --exclude '.yew-*'
#   Nextcloud           : add ".yew-*" to the ignored-files list
#   Dropbox             : no pattern support at all — one reason watched
#                         paths below never create a temp file.

let dirs  = []                    # module-private; see watch()
let marks = [                     # substring markers, not regexes: these are
    ".sync-conflict-",            #   literal, and a substring test cannot go
    "(conflicted copy",           #   quadratic on a directory listing
    "_conflict-",                 # Nextcloud / ownCloud
    "(SFConflict ",               # Seafile
    "(conflicted)",               # pCloud
]
# Deliberately NOT matched: Google Drive's "foo (1).c", iCloud's "foo 2.c",
# rclone bisync's bare "foo.c.conflict".  They are indistinguishable from
# files people name by hand, and a warning that cries wolf is a warning
# users switch off — after which the real one never arrives either.  Add
# one yourself with cloud.mark(".conflict") if your setup warrants it.

set({
    save.backup_keep: 10,          # s08 keeps ONE backup on the in-place path.
                                   # In a synced tree the usual loss is "the
                                   # other side won", and that backup is then
                                   # the only copy of what you had.
    save.check_disk_max: 33554432, # 32 MiB: content-compare pays for itself on
                                   # source files, not on a disk image.
})

fn watch(d) {
    # Absolute path, no trailing slash: comparison is literal and there is
    # no ~ expansion anywhere in Fletch (s31).
    list.push(dirs, d)
}
fn watching() { return list.copy(dirs) }
fn mark(s)    { list.push(marks, s) }

fn under_watched(path) {
    if path == nil { return false }
    for d in dirs { if str.starts_with(path, d + "/") { return true } }
    return false
}

# s36 pinned the mechanism: 1.0 ships no declarative per-path option table,
# on purpose, and `on("buf.open", …)` IS the supported form.
on("buf.open", fn(b) {
    if under_watched(b.path()) == false { return nil }

    # STRATEGY: in-place + backup, not tmp+fsync+rename.  The trade in full:
    #   we give up  the rename's all-or-nothing property against kill -9 at
    #               the instant of the swap (s08 table row 4);
    #   we get      (a) the file keeps its inode, so a sync client sees a
    #                   MODIFICATION, not delete+create — that is what
    #                   preserves remote revision history, and what stops a
    #                   rename reaching an inotify watcher as IN_MOVED_FROM
    #                   + IN_MOVED_TO on a name it re-uploads as a new object;
    #               (b) no `.yew-<name>-<pid>-<n>` ever lands in the synced
    #                   tree, so peers never receive one and no ignore rule
    #                   is load-bearing;
    #               (c) no rename across a FUSE or overlay boundary to fail
    #                   with EXDEV — s08 row 5 already falls back to exactly
    #                   this path, so this makes it the plan, not a surprise.
    # What we give up is smaller than it reads: s08's in-place path writes
    # BEFORE it truncates, keeps a byte-exact backup, and the crash journal
    # holds the op log, so a crash leaves the old tail and either the journal
    # or the backup reconstructs the rest — a lane already in the kill -9
    # torture harness.  This preset invents no save path; it selects one that
    # was always there.
    b.opt_set("save.strategy", "inplace")

    # A sync client re-downloading a file writes identical bytes with a new
    # mtime.  Under the default check every later save returns
    # YEW_SAVE_CHANGED_ON_DISK and refuses — a false alarm, every time, until
    # the alarm stops meaning anything.  Compare bytes instead; above
    # save.check_disk_max this degrades back to mtime and says so.
    b.opt_set("save.check_disk", "content")
})

# Sync clients do not merge; they duplicate, under names you have to know to
# notice, in the same directory as the file you are editing.
fn twins(path) {
    let cut  = str.rfind(path, "/")
    let dir  = str.slice(path, 0, cut)
    let self = str.slice(path, cut + 1)
    let out  = []
    for f in io.glob("*", dir) {              # io.glob does not cross '/'
        if f != self {
            for m in marks {
                if str.contains(f, m) { list.push(out, dir + "/" + f) }
            }
        }
    }
    return out
}

fn scan() {
    let out = []
    for b in buf.list() {
        if under_watched(b.path()) {
            for t in twins(b.path()) { list.push(out, t) }
        }
    }
    return out
}

# ed.idle fires once per idle period (>= 500 ms with no input, s34), which
# IS the debounce: one readdir per open watched buffer, never on the
# keypress path, never twice for one pause.
on("ed.idle", fn() {
    if list.len(dirs) == 0 { return nil }
    let found = scan()
    if list.len(found) > 0 {
        msg(fmt.f("sync conflict: {} file(s) beside open buffers — <space> c d",
                  list.len(found)))
    }
})

bind("*", "<space> c d", fn() {
    let found = scan()
    if list.len(found) == 0 { msg("no conflict files found"); return nil }
    # Open the twin beside the original.  yew does not merge it and
    # neither will the sync client: this is a two-buffer diff you do by hand,
    # which is the honest description of the situation you are in.
    win.current().split("v").focus()
    buf.open(found[0])
})
bind("*", "<space> c s", fn() { msg(fmt.f("{} conflict file(s)",
                                          list.len(scan()))) })

on("ws.open", fn(ws) {
    # $XDG_STATE_HOME holds workspace state, crash journals, and backups.  If
    # it is itself inside a synced root, two machines fight over the journal
    # of a file only one of them has open, and the recovery data you are
    # relying on becomes the thing that is conflicted.
    let st = io.env("XDG_STATE_HOME")
    if st == nil { st = io.env("HOME") + "/.local/state" }
    if under_watched(st + "/yew") {
        msg("warning: yew's state directory is inside a synced folder; "
            + "exclude it — journals and backups must not sync")
    }
})
```

One addition to s34's `buf.*` table lands here: `b.opt(name)` →
`ed.opt.get` and `b.opt_set(name, v)` → `ed.opt.set`, with the receiver
buffer as the scope target. Without a buffer-receiver form, s36's
sanctioned "express per-path config with `on("buf.open", …)`" cannot set a
buffer-scoped option on the buffer it was just handed. s54's stability
contract permits additions.

### 11. Docs — the author guide and the `yew pkg` man section

**`docs/plugins-authoring.md`** — the contract boundary s54 §9 names
("anything not in the author guide's reference table may change without
notice"). Outline: (1) what a plugin is, with `plug.h`'s trust text quoted
verbatim; (2) directory layout and every manifest key, with s54's
validation table restated as "why your manifest was rejected"; (3) the
`ctx` reference table below; (4) the capability model — declare in the
manifest, consent at first use, catchable when denied, `--grant NAME:CAP`
under `--batch`; (5) lifecycle and the zero-residue rule, including the
one thing an author can still leak (a closure captured in a value the host
does not own); (6) options and why they go through the one table;
(7) overlays, the frame budget, attributes-are-meanings; (8) testing with
`yew --batch --grant`; (9) publishing — a git repo, a tag, and §7's
honesty; (10) the two examples walked line by line — **chapter 10's text
lands in `s55_5-example-plugins.md` §4**; this sprint lands chapters 1–9
and the `ctx` table below.

**The `ctx` reference table — normative, frozen at api 1:**

| Field | Signature | Ledger | Notes |
|---|---|---|---|
| `ctx.name` | str | — | manifest name; the option and command namespace |
| `ctx.on(event, fn)` | → nil | `REG_HOOK` | event must be in s54's frozen table **and** in the manifest `events` |
| `ctx.command(name, fn, opts?)` | → nil | `REG_CMD` | registers `plug.<ctx.name>.<name>` in the one registry; `opts` carries s13's flags |
| `ctx.bind(mode, seq, target)` | → nil | `REG_BIND` | target = command name or closure; stacks in the plugin layer above the user's |
| `ctx.set(map)` | → nil | `REG_OPTION` | declares `plug.<name>.<key>` with defaults; read with `opt.get` |
| `ctx.attr(name)` | → int | `REG_ATTR` | resolves an s39 attribute name; unknown name = init-time error |
| `ctx.overlay(fn)` | → nil | `REG_OVERLAY` | `fn(win, buf, lo_line, hi_line)` → list of `{lo, hi, attr}`; runs inside the s21 frame budget; out-of-range spans dropped with one log line |
| `ctx.buf` / `ctx.win` | module | — | s34's `buf.*` / `win.*`, re-exported |
| `ctx.ws` | module | — | `ctx.ws.root()` → str; `ctx.ws.state_dir()` → str or **nil** when stateless |
| `ctx.msg(s, level?)` | → nil | — | message line, prefixed `[<name>]` |

**`docs/pkg.md`** — the man section (s59 assembles it into `yew.1`):
SYNOPSIS of all five subcommands · DESCRIPTION carrying §7's paragraph ·
the source-spec table · per-subcommand behaviour including the
fast-forward-only rule and the refusal verbatim · FILES (`plugins.lock`
with its field table, `plugins/`, `trust.fl`) · INTEGRITY including "this
is not a cryptographic hash" · ENVIRONMENT (`YEW_RUNTIME_DIR`,
`XDG_DATA_HOME`, `XDG_STATE_HOME`, §1's git variables) · EXIT STATUS · a
SECURITY paragraph promising nothing it cannot deliver.

### 12. Defer

- Central registry, signing, dependency resolution, install hooks,
  auto-update → **pinned non-goals** (§7), not deferrals; `yew pkg
  publish`/`search` hard-error quoting the reason. Native/binary plugin
  artifacts → never (Fletch-only, locked). Declarative per-path option
  tables → post-1.0, pinned by s36.
- The example plugins' full source, their READMEs, the author guide's
  chapter 10, their script tests, and the API-coverage rot gate →
  **`s55_5-example-plugins.md`**; §8–§9 keep the summaries. A third
  example, and examples covering `shell`/`net`/`clipboard` → post-1.0,
  pinned there.
- Man assembly, user manual, `yew tutor` → **Sprint 59**; budget
  calibration → **Sprint 56** (measured with committed baselines here).
- A conflict *merge* UI (three-way diff on a sync twin) → post-1.0; the
  preset opens both and says plainly that nothing merges them.
- `yew pkg` from inside a running editor (`:PkgInstall`) → post-1.0; a
  networked operation on the event loop is what §6's rule forbids.

## Testing Strategy

- **Unit** (`test_pkg_lock.c`): 20-entry round-trip; bytewise name sort and
  fixed field order against a golden; unknown top-level and per-entry keys
  preserved; absent file → empty + OK; corrupt file → error **and no
  write**; every `pin` form accepted, malformed ones rejected by name.
- **Unit** (`test_pkg_hash.c`): stability across two runs and two
  differently-named temp dirs; sensitivity to a one-byte change, rename,
  `chmod +x`, file added, file removed, symlink retargeted; insensitivity
  to `.git/` churn, directory name, atimes; symlink to `/` not followed;
  5 000-file tree; unreadable file → error, never a quietly different hash.
- **Unit** (`test_pkg_spec.c`): every resolution row plus ≥ 10 rejections
  (bare `user/repo`, leading `-`, `..` in a ref, embedded newline,
  300-byte ref). **Unit** (`test_rmtree.c`): nested dirs, symlink to an
  outside directory, symlink loop, read-only subdirectory, `must_contain`
  violation refused before the first `unlinkat`.
- **Integration** (`tests/pkg/`, bash over `git init` fixtures served via
  `file://`): install · pin to tag · pin to rev · fast-forward update ·
  no-op update · **refused after force-push** · **refused after a moved
  tag** · refused on a dirty worktree · `--dry-run` touching nothing ·
  remove · doctor on a hand-edited tree · `--accept` · `--fix`. Primary
  lane is `MODULES=plugins` with `fuss` excluded. **Offline lane**: the
  same suite with git stripped from `PATH` — `list` and `doctor` hash rows
  pass, networked subcommands exit 3 with §6's messages byte-exact; plus a
  black-holed remote port for the timeout path.
- **Script** (`plug_examples_*.fl`, `--batch --grant`): **owned by
  s55_5**, which specifies all fifteen rows. This sprint asserts only its
  own half — both examples install by local path via §5's `install` flow,
  land in the lockfile with a tree hash, and appear in `yew pkg list` as
  `ok`.
- **Script** (`cloud_*.fl`): the preset imported via the A2 row; a watched
  path gets `inplace` + `content` while an unwatched sibling keeps the
  defaults; identical bytes with a moved mtime **save** under `content` and
  are refused under `mtime`; differing bytes refused under both; each
  conflict marker detected exactly once; the two deliberately-unmatched
  shapes not detected; the state-dir-inside-a-synced-root warning fires.
- **Torture** (extends s08's `make torture`): the full kill -9 sweep re-run
  with `save.strategy=inplace` and `save.backup_keep=10`, asserting s08's
  in-place invariant — dst is old, or new, or restorable from a backup —
  plus rotation (exactly `keep` survive; newest is the pre-save content).
- **Fuzz**: `fl_data_read` corpus extended with mutated lockfiles
  (truncated mid-string, hex fields of every wrong length, deep nesting,
  non-UTF-8 names); a tree fuzzer feeding `yew_pkg_tree_hash` random trees
  (newlines, quotes, invalid UTF-8, 255-byte components, depth 40).
- **Perf**: tree-hash of 20 fixture plugins ≤ 3 ms; lock load+save of 100
  entries ≤ 1 ms; idle conflict scan over 20 open buffers ≤ 2 ms.
  **Determinism**: two clean XDG trees → byte-identical `plugins.lock`;
  `list` and `doctor` byte-identical across the double run.

## Definition of Done

1. `make test` green on gcc and clang at `-Werror`; ASan/UBSan/valgrind/
   determinism lanes clean. The pkg suite passes in a `MODULES=plugins`
   build with `fuss` **excluded**, and `grep -rn 'src/mod/git\|yew_git_'
   src/mod/plug/` prints nothing.
2. `grep -rn '\bsystem(\|\bpopen(\|git pull' src/` prints nothing; every
   git invocation is argv-form and every user string sits behind
   `--end-of-options` or `--` (unit-tested over §5's table).
3. Lockfile determinism: two clean installs of the same three plugins
   produce byte-identical `plugins.lock`; unknown keys survive a rewrite;
   a corrupt lockfile is never overwritten without `--force-relock`.
4. Tree hash: all 6 sensitivity cases and all 3 insensitivity cases assert
   as specified; the tree fuzzer runs 1 h with zero crashes or hangs.
5. **Drift revokes grants** — the sprint's most important test, labelled
   as such: grant `fs` "always" to a fixture plugin, mutate one byte of
   its source, restart, and assert (a) the warning appears, (b) the trust
   db no longer holds the grant, (c) the next capability use re-prompts.
6. Update is fast-forward-only: force-push and moved-tag fixtures both
   refuse with §5's message byte-exact, exit 1, tree and lockfile
   untouched; the changelog delta prints before any checkout, and
   `--dry-run` proves it by leaving `rev` unchanged.
7. Offline lane green: every networked subcommand exits 3 with its exact
   message when git is absent, when the remote is unreachable, and when
   the timeout fires; `list` and `doctor` hash rows still pass; a seeded
   `yew_pkg_git` call on the editor's startup path fails `make test`.
8. `install` executes nothing from the repository: a fixture carrying a
   `Makefile`, an `install.sh`, and a `postinstall:` manifest key installs
   (with s54's unknown-key diagnostic) and runs none of them — proven by a
   sentinel file that must not exist afterwards.
9. A local fixture with the same manifest/tree shape as the deferred
   examples installs by path, lands in `plugins.lock` with a tree hash,
   reports `ok` from `yew pkg list`/`doctor`, and enables. The actual
   `examples/plugins/<name>` trees, their installation, their behaviour,
   their zero-residue teardown, and the API-coverage gate are
   **`s55_5-example-plugins.md`'s DoD**, not this sprint's; §8–§9 carry
   only the summary and the pointer.
10. `runtime/preset.cloud.fl` parses with zero diagnostics, passes the spec
    §15 style checks, imports only via the A2 row, and each of the six new
    options' defaults equal their C constants (s36's rule, unit-asserted).
    Amendment A2 is present in `.docs/fletch-spec.md` §16 and s33's
    conformance count is updated in the same commit.
11. Torture green with `save.strategy=inplace` and `save.backup_keep=10`:
    s08's in-place checker invariant holds in 100 % of iterations, exactly
    10 backups are retained, and `keep = 1` reproduces s08's existing
    on-disk result byte-for-byte.
12. Conflict detection: every marker detected once; the two
    deliberately-unmatched shapes not detected; the idle scan runs at most
    once per idle period (counting hook) within 2 ms at 20 open buffers.
13. Docs: `docs/plugins-authoring.md` carries all ten `ctx` rows and quotes
    `plug.h`'s trust text verbatim (a test diffs the quoted block against
    the header comment); `docs/pkg.md` covers all five subcommands, all
    exit codes, and the "not a cryptographic hash" statement.
14. Non-goal surfaces hard-error naming their reason: `yew pkg publish`/
    `search` quote §7; `--recurse-submodules` quotes §5; `:PkgInstall`
    quotes §6's startup rule; man assembly names Sprint 59.
15. ≥ 140 assertions across the four new unit files; ≥ 18 `tests/pkg/`
    integration cases; perf baselines committed for all three budgets.
