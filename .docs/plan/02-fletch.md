# Fletch — The yew Language

Fletching is the craft of feathering an arrow so it flies straight; Fletch
steers cursors. One language for config, keybindings, macros, plugins,
syntax definitions, workspace state data, and headless batch editing.
Files: `*.fl`. C prefix: `fl_*`.

Flavor (locked): **hybrid** — first-class terse motion primitives inside a
real structured language. The macro recorder emits valid Fletch source;
any Fletch function can be bound to a key. Macros are programs; programs
are macros.

## The pitch vs vim

| vim | Fletch |
|---|---|
| `q`-registers store keystroke bytes; editing a macro means editing gibberish in a register | recording produces *source code* you can read, refactor, name, and version |
| vimscript is a second, unrelated language bolted beside macros | one language; the terse layer and the structured layer compose |
| no real values in macros (counts only) | buffers, cursors, spans, matches are typed values; loop over them |
| `:g//norm` incantations | query API: `buf.find(re)`, structural spans, then map an action over the results |
| macro errors are silent misfires mid-replay | errors abort the transaction — the buffer rolls back to pre-macro state |
| `vim -es` batch editing is famously hostile | `yew --batch script.fl files…` is a supported, documented product surface |

## Design requirements (bind the s28 design-freeze sprint)

1. **Round-trip law**: recorder output is valid Fletch; running it
   reproduces the recorded edit exactly (tested continuously).
2. **Terse motion layer**: motion literals use the same vocabulary the
   keymap uses, in ASCII (arrow glyphs accepted as aliases). A motion
   block reads like the keystrokes that made it.
3. **Real language**: lexical scoping, first-class functions/closures,
   modules, proper errors with line/col diagnostics (yew-quality carets).
4. **Transactional edits**: a top-level macro/function invocation is one
   undo step; uncaught error = rollback.
5. **Editor values**: buffer, cursor, span, window, regex are first-class
   handles; multi-cursor is "a list of cursors" you can map over.
6. **Data literals**: lists/maps/strings clean enough to serve as the
   config, syntax-definition, and workspace-state format.
7. **Embeddable & small**: VM inside yew; interpreter start cost ~0;
   config load < 1 ms typical; motion-op dispatch ~1 µs.
8. **Sandboxable**: fs/shell/net access are capabilities the host grants
   per script origin (user config = full; downloaded plugin = prompted).

## Sketch (direction, not frozen syntax — s28 freezes it)

```fletch
# -- terse layer: what the recorder emits ------------------------------
# unit prefixes: l w b c (line word block char) · > < ^ v arrows ·
# H( ) highlight · i"…" insert · del · esc — mirrors the keyboard exactly
macro dup_line = @[ l^ H(lv) yank v paste ]

# -- structured layer --------------------------------------------------
fn surround(open, close) {
    for c in win.cursors() {            # multi-cursor = list of cursors
        let s = c.span() or c.word()    # selection, else word under cursor
        edit {                          # one transaction = one undo step
            s.prepend(open)
            s.append(close)
        }
    }
}

# both layers compose
fn sort_block() {
    @[ b^ H(bv) ]                       # block-home, highlight block
    win.selection().replace_lines(sort(win.selection().lines()))
}

bind("H", "s", fn() surround("(", ")"))   # mode H, key s
set { tabwidth: 4, wrap: false, theme: "quiver-dark" }
on("buf.save", fn(b) if b.lang == "c" { fmt.run(b) })
```

## Implementation plan

- **Pipeline**: lexer → recursive-descent parser → AST → single-pass
  bytecode compiler → VM. All arena-allocated per compilation unit;
  interned identifiers.
- **VM**: stack-based bytecode, switch dispatch (computed-goto behind a
  feature probe). No JIT for 1.0.
- **Values**: tagged union (`FlValue`): nil, bool, i64, f64, string
  (interned or heap), list, map (insertion-ordered — determinism, same
  strmap discipline as Cgfried), function/closure, editor handles.
- **GC**: mark-sweep, explicit root set (VM stack, globals, host handles);
  collections only at opcode boundaries; editor handles are weak against
  buffer death (a dead buffer handle errors cleanly, never dangles).
- **Editor API** (`flapi.c`): thin, capability-checked bindings onto the
  same command registry keybindings use — no privileged side doors.
- **Recorder** (`record.c`): consumes normalized key events + resolved
  actions, emits the terse layer; smart enough to fold runs (`>>>>` →
  `4>`) and to name the mode context.
- **Errors**: `file:line:col` + caret rendering reusing the editor's
  diagnostic machinery; stack traces for runtime errors.
- **Testing**: spec conformance suite (every grammar production + semantics
  case), fuzzers (lexer/parser/VM), round-trip law harness, bench suite
  with committed baselines.

## Non-goals for 1.0

No JIT. No FFI / native extensions. No VM threads. No async/await in the
language (the host loop owns async; Fletch sees events via `on(...)`
callbacks). No metatables/operator overloading. Keep the interpreter under
~8 kLOC; small enough to own completely.
