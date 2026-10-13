# yew — Editor Architecture

Companion to `00-decisions.md` (which binds this file). This is the shape of
the program; sprint files carry the implementation detail.

## Repository layout

```
Makefile  LICENSE  README.md  .gitignore
src/main.c
src/util/        base.h arena vec strmap intern buf sort  (Cgfried-proven set)
src/unicode/     utf8.c|h grapheme.c|h width.c|h tables.c (generated, checked in)
src/term/        tty.c (raw mode, restore) input.c (escape parser, kitty proto)
                 grid.c (cell grid, damage) render.c (diff → escape stream)
src/text/        piece.c|h (piece tree) lineidx.c file.c (load/atomic save/journal)
                 cursor.c mark.c undo.c register.c
src/edit/        keymap.c mode.c dispatch.c  motion.c (unit engines: line/word/block)
                 select.c multicursor.c
src/ui/          layout.c (pane tree, cells) tabs.c groups.c statusline.c
                 cmdline.c region.c (clickable registry) draw.c
src/ws/          workspace.c state.c (persist/resume) finder.c (fuzzy)
src/fl/          lex.c parse.c ast.h compile.c vm.c value.c gc.c std*.c
                 flapi.c (editor bindings) record.c (macro→source)
src/syn/         engine.c defs.c theme.c
src/search/      regex.c literal.c searchui.c
src/mod/lsp/     json.c jsonrpc.c client.c features.c
src/mod/ai/      http.c backend_local.c backend_curl.c shadow_ai.c
src/mod/git/     git.c porcelain.c fusstree.c fussmode.c gutter.c blame.c
src/mod/plug/    pkg.c hooks.c sandbox.c
runtime/         default init.fl, syntax defs (*.fl), themes, tutor
tests/           unit/ script/ pty/ fuzz/ perf/
scripts/         gen-unicode-tables, smoke, perf harness
```

Directories are created by the sprint that first puts a file in them.
`src/mod/*` are the compile-time modules (`MODULES=` in the Makefile);
everything else is core.

## Process model

Single-threaded event loop over `poll(2)`:

```
fds: tty stdin · signal self-pipe (SIGWINCH/SIGCHLD/SIGTERM) ·
     per-job pipes (shell, LSP, AI/curl, git) · timer deadline as poll timeout
loop:
  poll → drain ALL ready input first (typeahead coalescing: never render
  between queued keys) → dispatch through keymap → actions mutate model →
  pump async jobs (parse LSP/AI frames, reap children) → layout if dirty →
  render damage → compute next timer deadline
```

Rules:
- Everything nonblocking. A slow LSP server or shell job can never freeze a
  keystroke (invariant 4).
- One render per input burst, max. Damage tracking makes renders O(changed
  cells).
- Timers form a small binary heap (cursor blink, group-jump digit window,
  AI debounce, status messages).
- `SIGSEGV/SIGBUS/SIGABRT` handler: restore terminal, write crash journal,
  re-raise (invariant 6 + 1).

## Data model

```
Editor
 └─ Workspace (one per process; dir = workspace)
     ├─ BufferList        (buffers are shared, views are not)
     │    └─ Buffer: piece tree + line index + undo tree + marks + syn state
     ├─ Tabs[]            (stable tab_id; group_id/group_ordinal per facsimile)
     ├─ TabGroups[]       (id, label, dir_path, last_active_member — no member lists)
     └─ per Tab: PaneTree (splits) → Window (view): buffer ref, cursors[],
                 viewport, local options
```

- Multi-cursor lives on the *window* (view), not the buffer.
- Deferred tabs: group members restore as unhydrated tabs (no buffer read)
  until first viewed — a 40-file group costs one file read.
- Coordinates are typed and never mixed: `ByteOff`, `CharCol`, `Grapheme
  Col`, `Cell` (screen). Conversions live in `src/unicode/` and are the only
  place allowed to do width math.

## Modal engine

- Mode = keymap layer + unit engine + statusline identity (name, color —
  the whole footer recolors, fuss-style).
- Unit engines (`motion.c`) implement one interface:
  `unit_next / unit_prev / unit_home / unit_end / unit_span(alt variant)`
  over (buffer, position). L/W/B are instances; H borrows the previous unit
  mode's engine for extension; I uses the char engine.
- B mode's structural units are provider-layered: syntax spans when the
  highlighter knows the language → bracket/indent/paragraph heuristics
  otherwise. Same interface, so B works in a plain .txt.
- Keymap: trie of key events → action ids; layers stack (mode → user →
  plugin); all bindings data-driven so Fletch can rebind anything.
  Key events are normalized once in `input.c` (kitty protocol when
  available, legacy decoding otherwise) — the keymap never sees raw bytes.
- Every action is a named editor command (`ed.move`, `ed.ins`, …) — the
  same registry serves keybindings, E-mode commands, Fletch calls, and the
  macro recorder. One dispatch surface, no parallel paths.

## Rendering

- `grid.c`: front/back cell buffers; a cell = grapheme (small inline buf or
  intern id), fg/bg/attrs, width flag. Wide glyphs occupy 2 cells with a
  continuation sentinel.
- `render.c` diffs back→front, emits minimal cursor-motion + SGR runs,
  wrapped in synchronized-output (2026) when supported.
- Layout tree computes pane rectangles in cells; each window draws its
  viewport into its rect; `region.c` records clickable spans during the same
  pass (payload convention from facsimile: positive = tab index, negative =
  −group id).
- Statusline: mode chip (colored), file, position, encoding/CRLF indicator,
  branch (git module), LSP/AI badges.

## Text engine

- Piece tree: pieces reference original buf or append-only add buf; tree
  nodes carry subtree byte and newline counts → O(log n) line↔offset both
  directions.
- Edits produce op-log entries; undo tree nodes group ops into transactions
  (one multi-cursor edit or macro run = one undo step).
- Line index and syntax line-state cache invalidate by damaged line range.
- `file.c` owns the no-corruption contract (decision table) and the crash
  journal: append-only op log per dirty buffer under XDG state, replayable
  on next open, deleted on clean save.

## Async subsystems (modules)

- All speak over pipes to subprocesses; all register fds with the loop.
- LSP: framed JSON-RPC; requests carry generation counters so stale
  responses (post-edit) are dropped, never applied.
- AI: debounce → context assembly → stream tokens into ghost text;
  cancellation on any keystroke that invalidates the prefix.
- Shell (E mode): jobs run under `$SHELL -c`; output streams into a job
  buffer; foreground filters (`|` region through command) are synchronous
  with a timeout + cancel.
- git: never blocks the loop; status cache with wall-clock TTL, force
  refresh after mutating verbs (fuss lesson: use wall clock, not cpu_time).

## Testing architecture (first-class, from Sprint 1)

- **Unit**: bespoke registry-based harness (no `__attribute__((constructor))`),
  `make test`.
- **Script tests**: headless editor driven by Fletch — open fixture, run
  edits, assert buffer/cursor/undo state. The bulk of feature coverage.
- **Pty tests**: fork yew under a pty, feed key sequences, snapshot the
  cell grid, byte-compare against goldens (deterministic rendering makes
  this exact). The TUI is tested like a compiler tests codegen.
- **Fuzz**: utf8 decoder, input escape parser, regex, Fletch lexer/parser/VM,
  piece tree (differential vs a naive array-of-lines oracle), undo.
- **Torture**: kill -9 during save loops; power-loss simulation via
  fs faults; 1 GB file open/edit/save.
- **Perf**: latency/startup/scroll harness with committed baselines; CI
  gates per `00-decisions.md`.
- CI: gcc + clang lanes, ASan/UBSan lane, valgrind lane, perf lane
  (designated runner), determinism lane (double-run diff).
