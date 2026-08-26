# trailing-ws

## What it does

Highlights trailing spaces and tabs in open buffers. It can remove every run
on demand or immediately before a file is saved.

## Install

```text
mkdir -p "$XDG_CONFIG_HOME/yew/plugins"
cp -R ./examples/plugins/trailing-ws "$XDG_CONFIG_HOME/yew/plugins/"
yew plug enable trailing-ws
```

`yew pkg install /path/to/trailing-ws` is the equivalent command after this
directory is published or copied as the root of its own Git repository.

Run `:ed.plug.trailing_ws.strip` to strip the current buffer.

## Options

- `plug.trailing-ws.highlight` — bool, default `true`; show the overlay.
- `plug.trailing-ws.strip_on_save` — bool, default `true`; strip before save.
- `plug.trailing-ws.skip_langs` — str, default `"markdown,diff"`; comma-separated
  language names whose meaningful trailing whitespace is left untouched.

## Capabilities

None. The plugin only reads and edits buffers already open in yew, so enabling
it never presents a capability prompt.

## The tradeoff

The save hook changes the buffer as part of saving it. That makes the bytes on
disk predictable and groups all deletions into one undo step, but it can make a
buffer dirty again if you undo immediately after a save.
