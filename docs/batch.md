# Headless batch editing

yew can open editor buffers and run a Fletch program without opening a
terminal:

```text
yew [OPTION...] --batch SCRIPT.fl [FILE...] [-- ARG...]
```

Batch mode does not initialize a tty, input decoder, terminal grid, or event
loop. It loads configuration, opens every `FILE` in order, runs `SCRIPT.fl`,
then exits. `buf.list()` uses the same file order and `buf.current()` is the
first buffer. With no file operands, yew opens an empty scratch buffer.

Put options before the first `FILE`. The first `--` ends yew's operands;
everything after it belongs to the script's `args` list, unchanged. For
example:

```sh
yew --clean --batch tools/check.fl one.c two.c -- --warnings-as-errors src/
```

This opens `one.c` and `two.c`. The script receives
`["--warnings-as-errors", "src/"]` as `args`.

## Configuration

Configuration finishes before files are opened and before the batch script
runs. The normal order is the shipped runtime `init.fl`, the user `init.fl`,
then the workspace `.yew.fl` after its trust check.

| Option | Effect |
|---|---|
| `--clean` | Skip all file-based configuration and workspace state. Use this for reproducible automation and script tests. |
| `--config PATH` | Load `PATH` in place of the user's `init.fl`. |
| `--no-workspace-config` | Do not load the workspace `.yew.fl`. |
| `--trust-workspace` | Pre-grant the current workspace configuration. Batch mode never opens a trust prompt. |
| `--quiet` | Suppress batch warnings and mirrored warning logs. Errors still go to stderr. |

`--grant NAME:CAP` has reserved syntax but is not usable yet. Supplying it
exits with an error naming Sprint 54. There is no `--batch-strict` option.

## Script globals

The batch host adds these globals before compiling the script:

| Global | Type | Value |
|---|---|---|
| `args` | list of str | Every value after `--`, byte-for-byte and in order. |
| `script_path` | str | The script's resolved absolute path. |
| `files` | list of str | The file operands exactly as supplied, including `-`. |
| `batch` | bool | Always `true`. |

Imported editor modules such as `buf`, `span`, `win`, and `ed` operate on the
same editor instance that owns the opened buffers.

## Standard input and output

| Stream | Carries | Never carries |
|---|---|---|
| stdout | Only script output from functions such as `io.print`. | Editor diagnostics, logs, warnings, and error traces. |
| stderr | Editor diagnostics, compile diagnostics, runtime traces, batch warnings, and WARN-or-higher log mirrors. | Script output intended for stdout. |
| stdin | Either the result of `io.stdin()` or one `-` file buffer. | Prompts. Batch mode has none. |

`-` reads standard input to EOF before the script starts and creates a buffer
named `[stdin]`. It may appear only once. If a `-` file already consumed
stdin, `io.stdin()` raises an `io` error. `io.stdin()` likewise claims stdin
for the run.

Stdout is flushed on success, script failure, I/O failure, and internal bug
paths. Redirection is therefore a supported interface:

```sh
yew --clean --batch tools/render.fl input.txt >output.txt
```

Nothing except the script's stdout can enter `output.txt`.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | The script completed. Modified but unsaved buffers do not change this code. |
| 1 | Command-line error, unreadable script, or unsupported reserved option. |
| 2 | The script did not compile or ended with an uncaught Fletch error. |
| 3 | yew could not open a file operand or another host-side I/O operation failed before the script could handle it. |
| 4 | Internal yew bug, including any attempted terminal access from batch mode. |

An uncaught runtime error starts its stderr report with
`yew: script failed:` and includes the Fletch trace and source location.
A compile failure has a caret diagnostic but no runtime frames. Both are exit
2; neither is a crash.

## Saving and dirty buffers

Batch mode writes nothing automatically. A script must call `buf.save`,
`ed.run("ed.file.save", {})`, or an explicit filesystem function such as
`io.write`. `buf.save` accepts these forms:

```fletch
buf.save(b)                  # save to the buffer's current path
buf.save(b, "other.txt")     # write this buffer to another path
buf.save(b, {force: true})   # accept a changed-on-disk conflict
```

Without `{force: true}`, an mtime or overwrite conflict raises a catchable
Fletch error with kind `"io"`; batch mode never prompts. Saves use yew's
durable file path.

If the script exits normally with dirty buffers, yew still returns 0 and
writes one warning to stderr, for example:

```text
warning: 2 buffers modified and not saved: a.c, b.c
```

`--quiet` suppresses this warning. On clean process exit, yew discards
the run's crash journals. An interrupted run retains recovery information.

## Commands that require a terminal

Every command marked interactive refuses under `--batch` with a catchable
`"capability"` error. The error names the command and the available scripted
alternative.

| Interactive command | Batch alternative named by yew |
|---|---|
| `ed.ui.message_expand` | No batch alternative. |
| `ed.cmdline.accept`, `ed.cmdline.cancel` | Call the intended command directly with `ed.run(name, args)`. |
| `ed.file.open` | `buf.open(path)` |
| `ed.group.rename` | Pass a name to a non-interactive group command. |
| `ed.group.new` | Construct the group with explicit arguments. |
| `ed.group.edit` | No batch alternative. |
| `ed.find.file` | `io.glob(pattern)` |
| `ed.find.buffer` | `buf.list()` |
| `ed.undo.branches` | `ed.run("ed.edit.undo", {})` or redo. |
| `ed.search.open`, `ed.search.open_back` | `b.find(re)` / `buf.find(b, re)` |
| `ed.macro.record` | No alternative: recording requires keys. Replay is available. |
| `ed.ai.open` | No batch alternative. |
| `ed.mode.enter` with mode `E` | Call the intended command directly with `ed.run(name, args)`. |

Scripts may catch these refusals with `try`/`catch`. A newly registered
interactive command without a refusal-table entry is an internal bug rather
than an accidental prompt.

## Worked examples

### Format, edit, and save one file

This formatter expands every tab to four spaces, edits the current buffer,
and saves it atomically:

```fletch
# tools/format.fl
import buf
import span
import str

let b = buf.current()
let whole = buf.span(b, 0, buf.len(b))
span.replace(whole, str.replace(buf.text(b), "\t", "    "))
buf.save(b)
```

```sh
yew --clean --batch tools/format.fl source.c
```

Omitting `buf.save(b)` leaves `source.c` unchanged and produces the dirty
buffer warning.

### Lint files and fail the job

This linter writes findings to stdout and raises an error after checking all
buffers. Findings remain available to a pipeline, while the trace stays on
stderr and the process exits 2.

```fletch
# tools/no-tabs.fl
import buf
import io
import str

let failed = false
for b in buf.list() {
    if str.contains(buf.text(b), "\t") {
        io.print(buf.path(b) + ": tab character")
        failed = true
    }
}
if failed {
    error({kind: "lint", msg: "tab characters found"})
}
```

```sh
yew --clean --batch tools/no-tabs.fl src/*.c >findings.txt
```

### Rename across a file list

This script requires an old and new spelling after `--`. It reads buffers in
file-list order and performs an explicit atomic write for every changed file.

```fletch
# tools/rename.fl
import buf
import io
import list
import str

if list.len(args) != 2 {
    error({kind: "user", msg: "usage: rename.fl -- OLD NEW"})
}

for b in buf.list() {
    let before = buf.text(b)
    let after = str.replace(before, args[0], args[1])
    if after != before {
        io.write(buf.path(b), after)
    }
}
```

```sh
yew --clean --batch tools/rename.fl one.c two.c -- old_name new_name
```

## Script tests

`--test` is valid only with `--batch`. It installs a frozen `t` global; test
files do not import it. Assertion failures are recorded and execution
continues so one file can report multiple failures.

| Function | Check or result |
|---|---|
| `t.eq(a, b, note?)` | Fletch equality; the optional note prefixes a mismatch. |
| `t.ne(a, b)` | Fletch inequality. |
| `t.text(b, text)` | Whole-buffer bytes equal `text`, without normalization. |
| `t.line(b, n, text)` | One-based line `n`, excluding CR/LF bytes, equals `text`. |
| `t.cursor(w, line, col)` | Primary cursor is at the one-based line and zero-based grapheme column. |
| `t.cursors(w, positions)` | The complete cursor set equals `[[line, col], ...]`. |
| `t.sel(w, text)` | The primary selection's bytes equal `text`. |
| `t.reg(name, text, type?)` | Register bytes and optional type match. Types accept `char`, `line`, or `block` and their `*wise` forms. |
| `t.undo(b, n)` | The live undo-tree node count, including its root, equals `n`. |
| `t.file(path, text)` | On-disk bytes equal `text`, without normalization. |
| `t.raises(kind, fn)` | Calling `fn()` raises an error of `kind`. |
| `t.log(level, substring)` | A structural log record at `debug`, `info`, `warn`, or `error` contains the substring. |
| `t.fixture(name)` | Copy `tests/script/fixtures/name` into the sandbox cwd and return its path. |
| `t.tmpdir()` | Return the isolated test root. |
| `t.skip(reason)` | Mark the file skipped. A skipped file may have zero assertions. |

A non-skipped test with no assertions fails. `t.fixture`, `t.tmpdir`, and
`t.skip` are helpers and do not increment the assertion count.

Add test programs as `tests/script/*.fl` and shared inputs under
`tests/script/fixtures/`. The runner discovers regular `.fl` files,
byte-sorts their names, and gives each one a fresh process and filesystem
sandbox. It runs the equivalent of:

```sh
build/yew --batch --test --clean tests/script/example.fl
```

Use the Make target for the normal suite:

```sh
make test-script
```

The runner also supports deterministic listing and substring filtering:

```sh
build/script_runner --list
build/script_runner --filter search_replace
build/script_runner --yew /path/to/yew --filter api_
```

`YEW_SCRIPT_BUDGET_MS` changes the default 10,000 ms wall-clock budget per
test. Passing sandboxes are removed. A failing, crashing, or timed-out test
preserves its sandbox and prints its path. The runner exits 0 only when every
selected test passes or skips; a filter matching no tests exits 1.
