# Commands, shell execution, filters, and jobs

E mode is yew's command line. Press `:` from a unit mode, or press bare `!`
to open the same prompt already prefixed with `!`. Completion, history,
selection, Ctrl-C/Ctrl-X/Ctrl-V, and ordinary prompt editing share the editor's
grapheme-safe text machinery.

Commands have one registry and explicit range policies. `:help-cmds` is not a
separate truth source: `yew --help-cmds` prints the same registered inventory.
Abbreviations such as `:q`, `:w`, and `:e` resolve only where the registry says
they do. Ambiguous prefixes are errors.

## Ranges

An address may name the current line (`.`), last line (`$`), an absolute line,
a mark, or a search. Two addresses form a range; `%` means the whole buffer.
The command descriptor decides whether a range is forbidden, optional,
required, current-line-by-default, or whole-buffer-by-default. Supplying a
range to a command that forbids one is an error before any mutation.

## Shell commands

`:!COMMAND` runs through the user's shell and opens stdout/stderr in a
dismissible output buffer. Escape or `:q` returns to the source document.
The source buffer is not replaced and the output buffer is not saved over it.
`:!!COMMAND` is the explicit terminal-handover form for interactive programs;
it temporarily restores the terminal and repaints the editor afterward.

Plain `:!` commands default to one persistent shell session. `cd`, exported
variables, shell functions, and the working directory therefore survive into
the next command. Set `shell.session = "fresh"` for one `$SHELL -c` per run,
or use the reset command to return the persistent session to the workspace
root.

The prompt completion stack is deterministic: shipped specifications first,
then optional fish queries, native `--help` parsing under policy, and finally
paths. Generators run in the effective directory at the caret, including a
pending `cd`. History suggestions are read-only and refuse secret-shaped,
multiline, and control-bearing rows.

## Filters and background work

A ranged shell command may consume the selected bytes as stdin and replace
the range with successful stdout. The original bytes remain unchanged when
the child cannot start, exits nonzero, times out, or the editor cannot commit
the journal record. A successful multi-cursor filter is one transaction.

Jobs are subprocesses integrated into the single-threaded poll loop. Pipes
are nonblocking, reads are budgeted, process groups receive cancellation, and
the editor waits for both output streams and child reap before releasing a
job. No worker thread mutates editor state.

For automation, prefer headless Fletch instead of scraping a terminal:

```sh
yew --help-cmds >/dev/null
```
```output
```

The complete batch contract is in [Headless batch editing](../batch.md).
