# session-notes

## What it does

Opens a private Markdown scratchpad for the current workspace with `<space> n`.
The note lives in yew's workspace state directory, outside the project tree.

## Install

```text
mkdir -p "$XDG_CONFIG_HOME/yew/plugins"
cp -R ./examples/plugins/session-notes "$XDG_CONFIG_HOME/yew/plugins/"
yew plug enable session-notes
```

`yew pkg install /path/to/session-notes` is the equivalent command after this
directory is published or copied as the root of its own Git repository.

## Options

- `plug.session-notes.key` — str, default `"<space> n"`; chord bound when the
  plugin is enabled. Reload the plugin after changing it.

## Capabilities

`fs`. yew asks when the plugin is enabled, before its entry module runs. A
denial leaves the plugin usable only in its stateless no-note path; batch tests
must pass `--grant session-notes:fs` to exercise filesystem calls.

## The tradeoff

The `ws.open` hook checks for an existing note, so interactive enable may ask
for filesystem access before you press the note key. Remove that hook if a
first-use prompt matters more than announcing an existing note at startup.
