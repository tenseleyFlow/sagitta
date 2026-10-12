# Recording, reading, editing, and replaying macros

yew records command words and motion literals into Fletch source. It does not
store terminal byte sequences. A recording therefore survives a keyboard
rebind: replay invokes the meaning that was recorded, not the old key that
once reached it.

Start recording with `q` and a one-letter register, perform commands, then
press `q` again. The statusline shows `REC` and the register while recording.
Replay with `@` plus the register; a count such as `3@a` performs three runs
and creates one undo step per run. `@@` repeats the last macro.

## Source and provenance

Pasting a macro register into a scratch buffer reveals ordinary source with a
small provenance header and a `macro` declaration. The emitter annotates unit
mode, combines adjacent insert text where semantics permit it, escapes string
bytes, and keeps intervening motions explicit.

```fletch
macro quote_line = @[ l ^ i"\"" esc end i"\"," esc ]
```

The motion literal is only syntax sugar for a zero-argument callable value.
Rename it, remove historical navigation that no longer belongs to the intent,
or call structured Fletch functions from it. Store validates and compiles the
entire replacement first; an error leaves the old macro live.

## Library

Macro source files live in `macro.dir`, or the XDG macro directory when that
option is empty. Public zero-argument functions and `macro` declarations are
indexed in bytewise file order. Broken files are isolated and reported; they
do not hide valid macros from other files. Reload replaces entries
transactionally and releases the old VM roots.

The macro browser is an ordinary editable buffer. Store establishes an undo
boundary and marks it clean only after successful compilation. Recording is
unavailable in batch mode because it requires interactive keys, but replay is
available through `--replay REG`.

See [Fletch book chapter 1](../fletch-book/01-arrows-to-programs.md) for the
full path from a recording to a parameterized function.
