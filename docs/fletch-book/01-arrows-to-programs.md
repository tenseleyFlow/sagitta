# From arrows to a Fletch program

Fletch begins with a practical claim: recordings are source. This chapter
takes one repetitive edit from arrow keys, through a recorded motion literal,
to a named function that runs without a terminal.

Start with three bare field names:

```text
id
name
created_at
```

The goal is one quoted, comma-terminated string per line:

```text
"id",
"name",
"created_at",
```

## 1. Make the edit with keys

Begin in L mode on line 1. The sequence, in yew's canonical key spelling, is:

```text
l ^ i " <esc> <end> i " , <esc> <down>
```

`l` makes the unit explicit. `^` moves to line home. `i` enters Insert and
typing `"` changes the complete buffer to:

```text
"id
name
created_at
```

Escape closes that typing transaction. `<end>` moves to the end of `id`, `i`
re-enters Insert, and typing `",` produces:

```text
"id",
name
created_at
```

The second Escape commits the suffix. Down moves to the next line. Repeat the
same content-changing keys there:

```text
"id",
"name",
created_at
```

and once more:

```text
"id",
"name",
"created_at",
```

Nothing in those snapshots is implied: mode changes and motions leave bytes
unchanged; the two Insert runs make the shown prefix and suffix; each Escape
closes the current undo group.

## 2. Record it

Undo or reopen the starting text. Press `q a`, perform the line edit, move
down, then press `q` to stop. The statusline shows `REC a` while recording.
Paste register `a` into a scratch buffer with `"ap`. The complete artifact is
readable Fletch:

```text
# yew-macro: 1
# recorded-with: yew 1.0.0
# keymap: default
macro rec_a = @[ l ^ i"\"" esc end i"\"," esc v ]
```

The header is provenance. Execution comes from the declaration. The same
body, without comments, parses as ordinary Fletch:

```fletch
macro rec_a = @[ l ^ i"\"" esc end i"\"," esc v ]
```

## 3. Read what was recorded

`@[ ... ]` is a motion literal: a sequence of registered command words and
literal insertion nodes.

- `l` records the unit context. Replaying from W or B still performs the line
  edit the author recorded.
- `^` and `end` are motions, not raw keyboard bytes. Rebinding Home or End
  does not change the macro's meaning.
- `i"\""` and `i"\","` are two inserts because a motion occurs between them.
  The backslashes are normal string escaping: the inserted byte is `"`.
- `esc` ends each insert action explicitly.
- `v` is present only because the recording moved down after editing.

There is no key code in the body. That is the useful distinction from a
keystroke register: the source names intent and survives a keyboard layout or
binding change.

## 4. Edit intent instead of history

First rename the declaration:

```fletch
macro quote_line = @[ l ^ i"\"" esc end i"\"," esc v ]
```

The name is an ordinary binding. Next delete the historical Down motion so
the caller—not the macro—decides which line comes next:

```fletch
macro quote_line = @[ l ^ i"\"" esc end i"\"," esc ]
```

Store the buffer. yew compiles the replacement before changing the live
register; a bad command word leaves the old macro intact.

Run it three times with `3@a`. The result is the three-line target shown at
the start. The count means three calls, so it creates three undo nodes. Press
`u` once and line 3 returns to `created_at`; again restores `name`; a third
restores `id`. This boundary is deliberate: a count repeats a command rather
than merging independent calls.

## 5. Generalize it

The recorded version fixes both delimiters and acts on one current line. A
function can accept delimiters and operate on every cursor-owned line in one
transaction:

```fletch
import win
import cur
import span

# Wrap every cursor-owned line in open/close.
# One edit{} is one undo step, no matter how many lines.
fn wrap_lines(open, close) {
    edit {
        let w = win.current()
        for c in win.cursors(w) {
            let s = cur.line_span(c)
            span.prepend(s, open)
            span.append(s, close)
        }
    }
}

bind("H", "q", fn() { wrap_lines("\"", "\",") })
```

This adds four things that the recording did not have: parameters, iteration
over real cursor handles, an explicit all-or-nothing edit transaction, and a
binding. The layers compose: a later motion literal may call `wrap_lines`
directly, and structured code may call a recorded macro.

## 6. Run it headlessly

Move the transformation into `tools/quote.fl`. This version preserves a final
newline, saves every file operand, and prints a self-demonstration when no
files were supplied—the mode used by the documentation checker.

```batch
import buf
import io
import list
import span
import str

fn quote_text(text) {
    let quoted = []
    for line in str.split_lines(text) {
        list.push(quoted, "\"" + line + "\",")
    }
    let result = str.join(quoted, "\n")
    if str.ends_with(text, "\n") { result = result + "\n" }
    return result
}

if list.len(files) == 0 {
    io.print(quote_text("id\nname\ncreated_at"))
} else {
    for b in buf.list() {
        edit {
            span.replace(buf.span(b, 0, buf.len(b)), quote_text(buf.text(b)))
        }
        buf.save(b)
    }
}
```
```output
"id",
"name",
"created_at",
```

Run the saved program over as many files as needed:

```text
yew --clean --batch tools/quote.fl fields.txt more.txt
```

The terminal is gone, but the same buffer, span, transaction, durable-save,
and error machinery remains. That is the route from one arrow-key edit to a
reviewable program.
