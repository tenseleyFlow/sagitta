# yew pkg

`yew pkg` installs and maintains Fletch plugins from Git repositories. It is
a command-line operation, not an editor command; editor startup never runs
Git or checks for updates.

This document is the `yew pkg` man section. Sprint 59 assembles it into the
complete `yew(1)` manual.

## Synopsis

```text
yew pkg install <spec> [--rev R | --tag T | --branch B] [--enable]
yew pkg update [<name>...] [--dry-run] [--discard-local]
yew pkg remove <name> [--keep-grants]
yew pkg list [--long]
yew pkg doctor [<name>...] [--fix | --accept]
yew pkg verify [<name>...] [--fix | --accept]
```

`verify` is an alias for `doctor`. Network operations use a 60-second
timeout by default; `--timeout SECONDS` overrides it.

## Description

`yew pkg` clones a repository into the user plugin directory, validates its
pure-literal manifest, checks out a detached revision, records that revision
and a content tree hash, and leaves the plugin disabled unless installation
used `--enable`.

The distribution model has deliberately narrow guarantees:

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

yew 1.0 has no central registry, signing, dependency resolution,
post-install or build hooks, submodule recursion, or automatic updates.
`publish`, `search`, and `--recurse-submodules` refuse and name these limits.

Git 2.24 or newer is required for networked package operations. `list`,
`remove`, and the filesystem-integrity rows of `doctor` work without Git and
without a network.

## Source specifications

The accepted source forms are closed:

| Input | Resolved source |
|---|---|
| `gh:user/repo` | `https://github.com/user/repo.git` |
| `gl:user/repo` | `https://gitlab.com/user/repo.git` |
| `cb:user/repo` | `https://codeberg.org/user/repo.git` |
| `sr:~user/repo` | `https://git.sr.ht/~user/repo` |
| `https://...`, `http://...`, `ssh://...`, `git://...` | Used verbatim |
| `git@host:path/repo.git` | Used verbatim as an scp-like source |
| `/absolute`, `./relative`, `file://...` | Used verbatim as a local source |

A bare `user/repo` is rejected. It is ambiguous with a relative path, and
assuming GitHub would create an implicit registry.

Revision, tag, and branch arguments must match
`[A-Za-z0-9._/-]{1,255}`. A ref may not begin with `-` or contain a `..`
component. yew passes user-controlled Git arguments after `--end-of-options`
or `--`.

## Commands

### install

```text
yew pkg install <spec> [--rev R | --tag T | --branch B] [--enable]
```

`install` performs a full clone in a temporary directory beside the final
plugin directory. It resolves the requested ref, checks out the commit in
detached-head state, reads `plugin.fl` in pure-literal mode, hashes the
content, then atomically renames the directory into place. It keeps `.git`
for future updates but excludes `.git` from the content hash.

The destination name comes from the manifest, never the URL. The final
directory basename must equal that manifest name. There is no `--name`
override.

The three pin options are mutually exclusive:

| Option | Lock pin |
|---|---|
| `--rev R` | `rev:<resolved-40-hex-commit>`; later `update` runs no network operation for this entry |
| `--tag T` | `tag:T` |
| `--branch B` | `branch:B` |
| none | `head`, following the remote default branch |

By default installation writes `enabled: false` to the trust database and
prints `yew plug enable <name>`. `--enable` records the plugin as enabled, so
the normal capability consent flow begins when yew next loads it.

Installation executes nothing from the repository. It does not run a
`Makefile`, `install.sh`, manifest hook, or entry module. A repository with
`.gitmodules` produces a warning; yew never recurses into its submodules.
Any failure removes the temporary clone and leaves the destination
unchanged.

### update

```text
yew pkg update [<name>...] [--dry-run] [--discard-local]
```

With no names, `update` visits every lock entry in lockfile order. Revision
pins perform no network operation. Branch and head pins fetch tags and remote
refs. A tag pin fetches that tag into yew's private `refs/yew-pkg/tags/`
namespace so a moved remote tag can be compared without rewriting the clone's
local tag. All non-revision pins resolve the target and update only when the
current commit is an ancestor of that target.

yew never merges and never runs `git pull`. Before checkout it prints up to
50 changelog rows as `<short-rev><TAB><subject>` and reports any remaining
count. `--dry-run` stops after this report and changes neither the checkout
nor lockfile.

A dirty tracked worktree is refused. `--discard-local` explicitly permits yew
to discard those local changes before checking out the target. It does not
weaken the ancestry rule.

A force-pushed branch or moved tag produces this refusal verbatim:

```text
yew pkg: refusing to update "trailing-ws": 0f4d8a1177c3 is not an ancestor
  of 91b2ee40cd77 — the branch was force-pushed or history was rewritten.
  Inspect it (git -C <dir> log --oneline --all), then either
  `yew pkg install --rev <sha>` deliberately, or pin a rev you trust.
```

If one remote is unreachable, `update` continues with later plugins and
prints a summary such as `2 updated, 3 up to date, 1 unreachable`. It exits 3
when any network operation failed.

### remove

```text
yew pkg remove <name> [--keep-grants]
```

`remove` accepts only a plugin recorded in `plugins.lock`. It verifies that
the resolved directory is inside the managed plugin root before deleting it.
Symlinks inside the plugin are removed as links and are never followed.

The default also removes the plugin's persisted capability grants and
`enabled` setting, so a later installation asks again. `--keep-grants`
retains those trust records. Removal requires no Git or network access.

### list

```text
yew pkg list [--long]
```

The compact form is deterministic, tab-separated output in discovery order:

```text
NAME<TAB>PIN<TAB>REV12<TAB>STATE<TAB>URL
```

States are `ok`, `drift`, `missing`, and `unmanaged`. `--long` requests the
detailed form while preserving deterministic ordering. Listing hashes local
trees and performs no network operation.

### doctor and verify

```text
yew pkg doctor [<name>...] [--fix | --accept]
yew pkg verify [<name>...] [--fix | --accept]
```

`doctor` checks named plugins, or every managed plugin in deterministic
order. It exits 0 only when every required check passes.

| Check | Report | `--fix` | `--accept` |
|---|---|---|---|
| Lockfile parses | Hard error; refuses to rewrite | — | — |
| Directory exists | `missing` | Reinstall the locked revision | — |
| Manifest reads and validates | `error` plus the diagnostic | — | — |
| Manifest name equals directory name | `error` | — | — |
| Tree hash matches | `drift`, with added, removed, and changed paths | Restore the locked revision, then verify | Record the current tree hash |
| `.git` is present and valid | `untracked-tree` | Re-clone the locked revision | — |
| `HEAD` equals locked `rev` | `rev-mismatch` | Check out the locked revision | Relock to `HEAD` |
| Declared and granted capabilities | Informational | — | — |
| Plugin directory absent from lockfile | `unmanaged`, informational | — | — |

`--fix` and `--accept` are opposites. `--fix` discards drift and restores the
recorded checkout. `--accept` keeps local reality and changes the lock record.
Inspect a drift report before choosing either.

The tree-hash, manifest, missing, and unmanaged checks work without Git.
Repository repair and revision comparison require Git.

## Files

Paths use the XDG base-directory fallbacks shown below.

| Path | Purpose |
|---|---|
| `$XDG_DATA_HOME/yew/plugins/` | Managed plugin checkouts; defaults to `$HOME/.local/share/yew/plugins/` |
| `$XDG_DATA_HOME/yew/plugins.lock` | Pure-literal package lockfile; defaults to `$HOME/.local/share/yew/plugins.lock` |
| `$XDG_STATE_HOME/yew/trust.fl` | Workspace trust, plugin enable state, and persisted capability decisions; defaults to `$HOME/.local/state/yew/trust.fl` |

An absent lockfile means there are no managed plugins. A corrupt lockfile is
an error and is never overwritten implicitly. `--force-relock` is the
explicit recovery path; it names the tree-hash records it will destroy.

The lockfile uses schema 1. yew preserves unknown top-level and per-plugin
keys across rewrites.

| Field | Type | Meaning |
|---|---|---|
| `schema` | int | `1` |
| `url` | str | Resolved absolute source passed to Git |
| `shorthand` | str | Original source spelling, or `""` |
| `rev` | str | Exactly 40 lowercase hexadecimal digits |
| `pin` | str | `rev:<40hex>`, `tag:<name>`, `branch:<name>`, or `head` |
| `tree` | str | 16 lowercase hexadecimal digits from the content tree hash |
| `installed_at` | int | Installation time in Unix seconds |
| `updated_at` | int | Last update time in Unix seconds |

Serialization is stable: top-level keys are `schema`, then `plugins`;
plugin names sort bytewise; entry fields use the table order above.

## Integrity

yew hashes every regular file and symlink below the plugin directory except
the top-level `.git/` subtree. Paths sort bytewise. A regular file contributes
its relative path, executable-bit class, length, and bytes. A symlink
contributes its relative path and link target; yew never follows it. Other
permission bits, access times, empty directories, and `.git` state do not
affect the result.

The tree value is an FNV-1a-64 content hash. **This is not a cryptographic
hash.** It detects accidental and casual drift; it does not resist a crafted
collision. The lockfile is user-writable beside the plugin tree, so replacing
the hash algorithm would not make it a signature or security boundary.

With `plug.verify_on_load = true`, the default, yew hashes managed plugins
before executing their entry modules. Drift emits
`plugin "x" changed on disk since install (yew pkg doctor x)`. If the plugin
has persisted capability grants, yew removes those grants and the next enable
requires fresh consent. It never silently updates the recorded tree.

Use `doctor --accept` to record an intentional local change. Use
`doctor --fix` to restore the locked revision.

## Environment

| Variable | Effect |
|---|---|
| `XDG_DATA_HOME` | Selects the managed plugin directory and lockfile root |
| `XDG_STATE_HOME` | Selects the trust database root |
| `YEW_RUNTIME_DIR` | Selects shipped runtime files; Fletch imports search this directory last |

Git children receive `GIT_TERMINAL_PROMPT=0`, empty `GIT_ASKPASS`,
`SSH_ASKPASS_REQUIRE=never`, and `PAGER=cat` / `GIT_PAGER=cat`. yew removes
`GIT_DIR`, `GIT_WORK_TREE`, `GIT_INDEX_FILE`, `GIT_OBJECT_DIRECTORY`, and
`GIT_COMMON_DIR` so an inherited environment cannot redirect operations to
another repository. Commands whose output yew parses receive `LC_ALL=C`;
clone and fetch keep the user's locale because yew displays, but does not
classify, their stderr.

yew keeps the user's Git configuration. Proxies, `insteadOf` rewrites, and
credential helpers continue to apply.

## Exit status

| Code | Meaning |
|---:|---|
| 0 | Requested operation completed; every required doctor check passed |
| 1 | Usage error, refusal, dirty or non-fast-forward update, or lockfile integrity error |
| 3 | Filesystem I/O, Git missing or too old, network, authentication, or timeout failure |
| 4 | Internal yew bug |

Git absence reports:

```text
yew pkg: error: git not found in PATH
yew pkg installs plugins over git; you can also copy a plugin directory into <plugins> by hand
```

yew classifies Git failures by exit status and its own timeout, never by
parsing localized Git prose. On clone or fetch failure it reports the status,
then preserves Git's stderr under `git said:`. Authentication prompts are
disabled; configure a credential helper or use an SSH URL.

## Security

Installing copies untrusted code but executes none of it. Enabling executes
Fletch in the editor process, with access to every open buffer and without
memory or resource isolation. Capability prompts limit only `fs`, `shell`,
`net`, and `clipboard` host I/O.

The content hash detects local drift and revokes persisted grants when code
changes. It does not authenticate an author, repository, tag, commit, or
lockfile. yew does not provide registry review, namespace ownership,
signatures, dependency vetting, or automatic security updates. Read the
source and inspect the pinned revision before enabling a plugin.
