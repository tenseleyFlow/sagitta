# Installing, trusting, and writing plugins

Plugins are Fletch source loaded into yew's existing VM. They are not native
libraries, child processes, or isolated sandboxes. An enabled plugin can read
every open buffer, consume CPU, and allocate memory even when it declares no
I/O capability. Read its source before enabling it.

The full API contract and worked examples are in
[Writing yew plugins](../plugins-authoring.md). Package-manager behavior,
integrity records, and recovery are in [yew pkg](../pkg.md) and `yew-pkg(1)`.

## Manifest and discovery

Each plugin directory contains a pure-literal `plugin.fl` manifest naming its
entry module, API major, capabilities, and event subscriptions. Pure-literal
loading cannot import or call code. The directory name and manifest name must
match, entry paths cannot escape through `..` or symlinks, and unknown keys,
capabilities, or events are rejected.

Discovery layers data, user configuration, and trusted workspace locations in
deterministic byte order. A higher row shadows a lower row by name. `--clean`
scans no plugins.

## Capabilities are blast-radius controls

The four capabilities are `fs`, `shell`, `net`, and `clipboard`. Declaration
is not consent. Enable settles allow-once, allow-always, or deny; batch mode
never prompts and requires explicit `--grant NAME:CAP`. A plugin cannot invoke
an undeclared capability even if trust data contains an old grant.

These gates cover host I/O only. They do not isolate memory, buffer reads,
CPU, or allocation. Calling them a sandbox would be misleading.

## Lifecycle

Plugins register commands, hooks, options, bindings, and overlays through the
host-owned `ctx`. Disable removes ledger entries in reverse order, drops the
module root, and collects. A failed `init` rolls back its partial
registrations. Repeated uncaught callback errors disable the plugin at
`plug.error_limit` rather than aborting unrelated subscribers.

```fletch
let manifest = {
  name: "example",
  version: "1.0.0",
  api: 1,
  entry: "main.fl",
  capabilities: [],
  events: ["buf.open"],
}
```

## Packages and integrity

`yew pkg install` clones without executing repository code, validates the
manifest, checks out a detached revision, hashes the content tree, and writes
a pure-literal lock row. There is no registry, signature, dependency resolver,
post-install hook, or automatic update. `doctor` reports drift; `--fix`
restores locked reality while `--accept` deliberately records local reality.
