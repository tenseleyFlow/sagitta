# Writing yew plugins

Plugin API 1 is intentionally small. This guide is its contract boundary:
the manifest keys, event names, capability names, and `ctx` reference below
are stable for yew 1.x. Host functions not documented here may change without
notice.

## 1. What a plugin is

A plugin is a directory containing a pure-literal `plugin.fl` manifest and
Fletch source. yew loads the entry module into the editor's existing Fletch
VM. Plugins are not native libraries or separate processes.

The trust model is part of the API. The following block is quoted verbatim
from `src/mod/plug/plug.h`:

> Plugins are Fletch running in the same VM, same process, same address
> space as your editor.  There is no memory isolation and no resource
> isolation: an enabled plugin can read any open buffer, burn CPU, or
> allocate until the GC sweats.  Capability gates cover exactly the flapi
> I/O surface -- fs, shell, net, clipboard -- nothing else.  Enabling a
> plugin is an act of trust in its author; capabilities limit blast radius,
> they do not create a sandbox.  Read the source: it is Fletch, and it is
> short.

Keep a plugin short enough to audit. Declare only the capabilities and events
it uses.

## 2. Layout and manifest

The directory basename and manifest name are the plugin's identity:

```text
trailing-ws/
  plugin.fl
  src/
    main.fl
  README.md
  LICENSE
```

`plugin.fl` must be one pure-literal map. It cannot import, call a function,
or evaluate an expression, so yew can inspect it before plugin code runs.

```fletch
{
  name: "trailing-ws",
  version: "1.0.0",
  api: 1,
  entry: "src/main.fl",
  capabilities: [],
  events: ["buf.open", "buf.save"],
  description: "Highlights and removes trailing whitespace.",
}
```

| Key | Required | Value |
|---|---:|---|
| `name` | yes | `[a-z0-9-]{1,32}`; must equal the directory basename |
| `version` | yes | Semantic version; informational in API 1 |
| `api` | yes | Plugin API major; yew 1.x accepts `1` |
| `entry` | yes | Relative Fletch path that resolves inside the plugin directory |
| `capabilities` | yes | List containing only `fs`, `shell`, `net`, or `clipboard` |
| `events` | yes | List of frozen API 1 event names; every `ctx.on` event must appear here |
| `description` | no | One line shown by plugin information surfaces |

The manifest is strict. yew rejects it for the following reasons:

| Why your manifest was rejected | Diagnostic identifies |
|---|---|
| It contains an unknown key | The key, with a suggestion for a one-edit typo |
| `name` differs from the directory basename | Both names |
| `entry` is absolute, escapes with `..`, or resolves through a symlink outside the directory | The resolved path |
| A capability is not one of the four API 1 names | The capability |
| An event is not in the frozen event table | The event |
| `api` is newer than the host | `requires plugin API N; this yew speaks 1` |
| It contains `dependencies` | `plugins have no dependencies at 1.0` |
| A required key is absent or has the wrong type | The manifest rule that failed |

The frozen event names and payloads are:

| Event | Payload | When it fires |
|---|---|---|
| `buf.open` | buffer | After load, before first render |
| `buf.change` | buffer, damaged span | Coalesced per input burst |
| `buf.save` | buffer | Before write; the hook may edit in the save transaction |
| `buf.saved` | buffer | After a successful save |
| `buf.close` | buffer | Before buffer teardown |
| `win.focus` | window | On focus change |
| `mode.enter`, `mode.leave` | mode name | On modal transitions |
| `ws.open`, `ws.close` | workspace | At session bounds |
| `plug.enable`, `plug.disable` | plugin name | For other plugin origins |
| `ed.idle` | none | Once after at least 500 ms without input |

## 3. The `ctx` API

The entry module must export `fn init(ctx)`. Register host-owned state through
that context so yew can attribute it to the plugin and remove it later.

This table is normative and frozen at plugin API 1:

| Field | Signature | Ledger | Notes |
|---|---|---|---|
| `ctx.name` | str | — | manifest name; the option and command namespace |
| `ctx.on(event, fn)` | → nil | `REG_HOOK` | event must be in API 1's frozen table **and** in the manifest `events` |
| `ctx.command(name, fn, opts?)` | → nil | `REG_CMD` | registers `plug.<ctx.name>.<name>` in the one registry; `opts` carries command flags |
| `ctx.bind(mode, seq, target)` | → nil | `REG_BIND` | target = command name or closure; stacks in the plugin layer above the user's |
| `ctx.set(map)` | → nil | `REG_OPTION` | declares `plug.<name>.<key>` with defaults; read with `opt.get` |
| `ctx.attr(name)` | → int | `REG_ATTR` | resolves a syntax attribute name; unknown name = init-time error |
| `ctx.overlay(fn)` | → nil | `REG_OVERLAY` | `fn(win, buf, lo_line, hi_line)` → list of `{lo, hi, attr}`; runs inside the frame budget; out-of-range spans are dropped with one log line |
| `ctx.buf` / `ctx.win` | module | — | the `buf.*` / `win.*` editor modules, re-exported |
| `ctx.ws` | module | — | `ctx.ws.root()` → str; `ctx.ws.state_dir()` → str or **nil** when stateless |
| `ctx.msg(s, level?)` | → nil | — | message line, prefixed `[<name>]`; level is `info`, `warn`, or `error` |

`ctx.command` accepts the boolean flags `repeatable`, `takes_count`,
`needs_win`, `changes_buffer`, and `prompts`. `repeatable` and `takes_count`
cannot both be true. `recordable` and `deferred` are host-only.

## 4. Capabilities and consent

Capabilities cover four host I/O surfaces:

| Capability | Authority |
|---|---|
| `fs` | Read and write files outside open editor buffers |
| `shell` | Run shell commands |
| `net` | Access the network |
| `clipboard` | Read and write the system clipboard |

Declaration is not permission. Put every required capability in
`capabilities`; yew settles consent when it first enables the plugin. A
persisted allow or deny is reused, while a once decision lasts only for the
editor session. A capability that was not declared fails immediately and
never prompts.

Denial raises a catchable Fletch error with kind `"capability"`. Handle that
error when the plugin has a useful reduced mode. Do not turn a denial into a
loop of repeated attempts.

Batch mode never prompts. Pre-grant each required capability explicitly and
repeat `--grant` when a test needs more than one:

```text
yew --clean --batch --grant session-notes:fs tests/session-notes.fl
```

The name and capability must match the discovered plugin and its manifest.
A batch grant lasts for that process; it does not write an `allow always`
decision.

## 5. Lifecycle and zero residue

Discovery reads the manifest. Enabling compiles the entry module, calls
`init(ctx)`, and records every registration. Disabling masks the plugin's
origin, removes its ledger entries in reverse order, drops the module root,
and runs garbage collection. Reload is disable, rescan, then enable.

An uncaught error during `init` rolls back every registration made by that
attempt. An uncaught hook, command, or bound-closure error rolls back the
current edit, logs the trace, increments the plugin error count, and does not
stop later event subscribers. At `plug.error_limit` errors, yew disables the
plugin through the normal teardown path.

The zero-residue rule is strict: after disable, the plugin must leave no
bindings, hooks, commands, options, overlay attributes, timers, or closures
reachable from a host registry. Register through `ctx`; do not retain raw host
state through undocumented APIs.

One leak remains possible because the host cannot own every Fletch value: a
closure captured in a value stored by user code or another origin can keep the
plugin's objects reachable. Do not export callbacks for other origins to
retain. Prefer named commands, events, and values with no plugin closure.

## 6. Options

Declare plugin options once in `init`:

```fletch
fn init(ctx) {
    ctx.set({enabled: true, limit: 80})
}
```

The host registers these as `plug.<manifest-name>.<key>`. Defaults may be
bools, integers, strings, or lists of strings; the default fixes the option's
type. Read a value through `opt.get("plug.trailing-ws.enabled")` and let users
change it through the same option system as every built-in setting.

The one option table is deliberate. It gives configuration, completion,
validation, persistence, and inspection one authoritative descriptor instead
of letting each plugin invent a settings store. `ctx.set` registrations also
participate in zero-residue teardown.

## 7. Overlays and attributes

Resolve an attribute by meaning, then return byte spans only for the visible
line range passed to the overlay:

```fletch
fn init(ctx) {
    let warning = ctx.attr("warning")
    ctx.overlay(fn(win, buf, lo_line, hi_line) [
        {lo: 0, hi: 1, attr: warning},
    ])
}
```

An overlay result is a list of maps with integer `lo`, `hi`, and `attr`.
Offsets are absolute buffer-byte offsets and `hi` is exclusive. yew clips
spans to the visible range; it drops invalid, empty, out-of-range, or
non-grapheme-boundary spans and logs the contract failure.

The entire viewport render, including plugin overlays, shares a 1000 µs frame
budget. Work from `lo_line` through `hi_line`; do not scan the whole buffer,
walk the filesystem, or run a job on the render path.

Attribute names describe meanings such as `warning`, not colors. Themes own
the rendering. A plugin that assumes `warning` is yellow breaks user themes
and terminal capabilities.

## 8. Testing

Test the entry module and its teardown under a temporary XDG tree. Use
`--clean` for deterministic host configuration, `--batch` to avoid a tty,
and one repeatable `--grant NAME:CAP` per required capability. Assert both the
normal path and the catchable-denial path.

At minimum, cover:

- a valid manifest and each validation failure relevant to the plugin;
- enable, disable, re-enable, and a failing `init`;
- every declared event and its payload shape;
- denied, session-granted, and persisted capability decisions;
- registry counts before enable and after disable;
- overlay clipping and a buffer larger than the visible viewport;
- an uncaught callback error and automatic disable at `plug.error_limit`.

Installation is a separate test surface. A local repository path exercises
the same clone, manifest, lockfile, and tree-hash flow as a remote:

```text
yew pkg install ./path/to/plugin
yew pkg doctor <name>
```

Installation must not execute a repository's `Makefile`, scripts, or Fletch
entry module.

## 9. Publishing without a registry

Publish a plugin as a Git repository containing `plugin.fl`, the Fletch
source, a license, and a short README. Tag releases with semantic versions so
users can install a stable ref:

```text
yew pkg install gh:author/repository --tag v1.0.0
```

yew 1.0 has no central registry, namespace ownership, review, signature
verification, dependency resolver, install hooks, or auto-update. A repository
URL is not an endorsement. Tell users who maintains the code, what every
declared capability enables, and which tag or revision they should inspect.

The two in-tree examples and their line-by-line walkthrough are owned by
Sprint 55.5. They use the same local-path installation flow documented above.
