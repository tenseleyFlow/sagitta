# Installing and starting yew

yew is a C11 program with no runtime dependency beyond libc and the POSIX
interfaces supplied by the host. A normal development build uses the native
compiler and places the editor at `build/yew`:

```sh
make >/dev/null
test -x build/yew
```
```output
```

Both GCC and Clang are supported. The build is deliberately strict:
`-std=c11 -pedantic -Wall -Wextra -Werror` is part of the ordinary profile,
not an optional maintainer check. Run `make check` before submitting a change.

## Build profiles and modules

All optional modules are present unless `MODULES` says otherwise:

```text
make MODULES="lsp ai fuss plugins"
make MODULES=""
```

The second command builds the small core editor. It does not remove core text,
modal, terminal, workspace, or Fletch behavior. Commands owned by an omitted
module fail with a diagnostic instead of silently doing nothing.

`make SHIPPING=1` selects the optimized shipping profile: section garbage
collection, PIE where the target supports it, and `NDEBUG`. It does not enable
undefined C behavior or unsafe optimizer promises. Sprint 60 owns the signed,
reproducible distribution artifacts; for ordinary local use, the default
`-O2` build is already optimized.

Supported target profiles are `x86_64-linux-gnu`, `arm64-linux`,
`x86_64-linux-musl`, and `arm64-macos`. Native builds infer `TARGET` from the
host. Cross builds must provide the matching `CROSS` tool prefix.

## Install

`make install` installs the binary, runtime Fletch files, syntax definitions,
themes, completion specifications, privacy documentation, and man pages below
`PREFIX` (default `/usr/local`). Packagers stage with `DESTDIR`:

```text
make DESTDIR="$pkgdir" PREFIX=/usr install
```

The runtime directory compiled into the binary follows `PREFIX`. Set
`YEW_RUNTIME_DIR` only to test or deliberately use a different runtime tree.

## First run

Pass files or a directory. A directory becomes the workspace root and opens
the FUSS tree; files outside that root may still be opened as ordinary tabs.

```text
yew .
yew src/main.c include/yew.h
```

Use `--clean` when diagnosing configuration: it loads the shipped defaults
but skips file-based user/workspace configuration and saved workspace state.
It does not make the editor read-only.

## Where state lives

yew follows the XDG base-directory convention, falling back beneath `HOME`:

| Kind | Default root | Examples |
|---|---|---|
| configuration | `~/.config/yew/` | `init.fl`, themes, plugins, macros |
| data | `~/.local/share/yew/` | managed plugins and `plugins.lock` |
| state | `~/.local/state/yew/` | log, journal, backups, history, trust, workspaces |
| cache | `~/.cache/yew/` | compiled syntax cache |

See [configuring yew](08-fletch.md) for load order and trust, and
[files and durability](03-files.md) before changing save policy.
