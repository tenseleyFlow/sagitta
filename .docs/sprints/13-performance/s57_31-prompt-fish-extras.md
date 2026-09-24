# Sprint 57.31: Prompt Fish Extras — `A-s`, `A-e`, `A-h`

## Prerequisites

- **Sprints 57.28–57.30** — the prompt's readline set, selection and
  clipboard, history and table navigation; `Ed.invoke_seq`; the 57.26
  history snapshot (bang lines).
- **Sprint 57.23** — `yew_shctx_at` / `YewShCtx` (`argv`, `arg_index`,
  wrappers stripped) and `yew_shq_quote`.
- **Sprint 57.18 §4** — `yew_shell_term_run(ed, cmdline, &wait, err, n)`
  (`src/edit/shell.c:671`): hands the REAL terminal to a child and restores
  it on every exit path (invariant 6).
- `yew_ws_scratch_new(ed, name, flags)` / `yew_ws_scratch_find`
  (`src/edit/ed.h:385`) for an unsaved scratch buffer.
- There is NO help system for yew's own `:` commands.
- Binding: invariants 1, 6, 9.

## Goals

Three fish niceties the user asked for by name (2026-09-23):

- **`A-s`** toggles `sudo ` at the front of the command.
- **`A-e`** opens the command line in a yew buffer for full editing; closing
  the buffer returns the text to the prompt.
- **`A-h`** opens the man page for the command under the caret in the
  user's real pager, and returns to the prompt afterwards.

## Deliverables

### 1. `A-s` — new `ed.cmdline.toggle_sudo`

On a bang line (the prompt forms 57.23's `bang_body_start` recognises):

| Body | Result |
|---|---|
| starts with `sudo ` (after leading blanks) | remove that `sudo ` |
| starts with `doas ` | remove it (fish treats the configured prefix; treat both) |
| otherwise, non-empty | insert `sudo ` at the start of the body |
| empty | fill the body with `sudo ` + the most recent bang history entry's body (fish) |

The caret keeps its place relative to the text it was on. On a non-bang
prompt, `A-s` does nothing and says why (`A-s: not a shell command`). One
undo step.

### 2. `A-e` — new `ed.cmdline.edit_in_buffer`

1. Remember the prompt's kind, full text and caret; close the prompt.
2. Open a scratch buffer (`yew_ws_scratch_new`, name `*command-line*`) in a
   new tab holding the text — for a bang line the BODY only, so the user
   edits the command, not the `:!` prefix — with the caret at the same
   place, in Insert mode.
3. When that buffer is closed (`:q`, `:wq`, tab close, `ed.close`), reopen
   the same prompt kind with the buffer's text, caret at the end. Closing
   with `:q!` (discard) reopens the prompt with the ORIGINAL text.
4. Multi-line text must become one line for the prompt. For a bang body,
   use `yew_shctx_at` to decide each newline:
   - newline after a complete command → `; `
   - newline after a trailing `\` continuation → remove both, join with
     a blank
   - newline inside a quote or an unfinished construct → do NOT return; the
     buffer stays open with a message naming the line (`line 3: a one-line
     prompt cannot hold a newline inside quotes`).
   For a non-bang prompt, a newline anywhere → refuse the same way.
5. Only one `*command-line*` buffer at a time: a second `A-e` while one is
   open switches to it.

**Pitfall — the return path must not depend on how the tab closes.** Hook
the buffer's release, not a specific command.

### 3. `A-h` — new `ed.cmdline.man_page`

On a bang line, the command word is `argv[0]` of the simple command under
the caret (wrappers such as `sudo` already stripped by the lexer). If the
caret is on a subcommand of a spec'd command with a man page per
subcommand (`git`), try `man git-<sub>` first, then `man git` — probe with
`man -w` synchronously is FORBIDDEN on the keystroke path; instead run the
fixed script `man "$1" 2>/dev/null || man "$2"` as the child, argv
`["/bin/sh", "-c", SCRIPT, "sh", "git-sub", "git"]` (one name → `man
"$1"`). **The names travel only as `$1`/`$2`, never spliced into the
script** — the same injection rule as 57.26's fish query; a unit test
passes a hostile name and asserts it arrives as one argument.

Run it through `yew_shell_term_run` (real terminal, the user's `$PAGER` /
`$MANPAGER`). When the child exits, reopen the prompt exactly as it was
(kind, text, caret, selection collapsed) and repaint fully. A non-zero exit
(no man page) → message `no man page for NAME`.

On a non-bang prompt, `A-h` says `A-h: man pages are for :! commands`.

### 4. Bindings — `runtime/init.fl`

E mode: `A-s`, `A-e`, `A-h`; also `A-v` as a synonym for `A-e` (fish).
Verify each is free in E; report collisions rather than overwriting.
Mirror in `tests/unit/test_runtime_defaults.c`; RECOUNT
`yew_bind_active_count` by running the test.

### 5. Size

**Do not touch `tests/size/`**; the orchestrator runs the CI rebaseline.
Report an added-bytes estimate.

### 6. Defer

- Help for yew's own `:` commands (there is no help system).
- `A-w` (fish `whatis`), `A-l` (list directory).

## Testing Strategy

- **Unit, `A-s`**: every table row; caret preservation; `doas`; the empty
  body filling from history (fixture history); non-bang prompt message;
  one undo step.
- **Unit, `A-e`**: open with the body; close returns the edited text; `:q!`
  returns the original; each newline rule (complete command → `; `,
  continuation → blank, inside quotes → refusal with the line number and
  the buffer left open); non-bang newline refusal; a second `A-e` switches.
- **Unit, `A-h`**: the child command line built for `git che‸`, `sudo
  make‸`, a quoted name; the prompt restored after the handover (the
  handover is stubbed the way 57.18's handover tests stub it); the
  no-man-page message; the non-bang message.
- **PTY**: `s57_31_toggle_sudo`, `s57_31_edit_in_buffer_roundtrip`. A
  faithful PTY for the man handover may not be achievable — if not, say so
  and cover the restore path by unit test, as 57.18 did.

## Definition of Done

1. `A-s`, `A-e` (and `A-v`), `A-h` behave as specified, each proven by
   real-key unit tests.
2. The terminal is restored after `A-h` on every child exit path.
3. `A-e` never loses text: discard returns the original, refusal keeps the
   buffer open.
4. `yew_bind_active_count` recounted by running the test.
5. The `s57_31_*` goldens pass; full `make test-pty` green.
6. gcc AND clang warning-free: build the WHOLE tree with
   `make -k CC=gcc-16 BUILD=build-gcc FAULTSHIM_ARCH_FLAGS=`; always check
   `snprintf`'s result; never pass a variable by value in the same call
   that writes it through a pointer; `MODULES=""` builds and passes;
   ASan/UBSan filtered runs clean; `scripts/check-sigsafe.sh` green.
7. `test-fletch test-script test-roundtrip test-roundtrip-coverage
   test-audit` and the `scripts/check-*.sh` gates green.
8. `tests/size/` untouched; added-bytes estimate in the report.

## Implementation notes (divergences from this contract)

Where the contract and the code disagreed, or the contract was silent,
the implementation does what the contract intends:

1. **`A-h` does not close the prompt.**  The handover is synchronous, so
   nothing can touch the prompt while `man` runs; it stays open and comes
   back EXACTLY as it was (same `generation`, text, caret, walk), the
   dispatcher collapsing a selection as for any command, and the
   handover's epilogue marks the full repaint.  Every outcome is a message
   on that prompt: `no man page for NAME` (two names: `no man page for
   git-che or git`), `man killed by signal N`, `A-h: cannot run /bin/sh:
   …`.
2. **The script is `man -- "$1" 2>/dev/null || man -- "$2"`** (one name:
   `man -- "$1"`), and a name that starts with `-` is refused (`A-h: no
   command under the caret`).  The names still travel only as `$1`/`$2`;
   the `--` keeps one from being read as man's option -- `-P` names a
   pager man would run.
3. **`yew_shell_term_argv`** is new: `yew_shell_term_run` takes a shell
   command line, and §3's argv needs the argv form of the same handover
   (`shell_term_spec` is the one route both take).  Refused under
   `--batch`.
4. **A handed-over child keeps the user's pagers.**  The job layer forced
   `PAGER=cat`/`GIT_PAGER=cat` on every child, the `inherit_tty` one
   included, and `man` under `PAGER=cat` dumps the page and returns
   before anyone reads it.  A child given the real terminal owns the
   stdin a pager reads, so `job_build_env` now leaves the parent's
   pagers alone for it -- which fixes `:!!git log` too.  A caller's
   explicit `env_set` still wins (the Git module's rebase handover keeps
   its `PAGER=cat`); captured jobs are unchanged.
5. **Which commands get `<cmd>-<sub>` first:** any command whose spec has
   subcommands -- the `||` makes a missing `git-foo`-style page harmless,
   so no per-spec "man per subcommand" key was added.  The subcommand is
   the first-level node the spec walk reached (`git checkout -b fo‸` ->
   `git-checkout`), else the caret's own word in a subcommand slot, as
   fish does (`git che‸` -> `git-che`, then `git`).  Only a plain word
   (`[A-Za-z0-9._+-]`, no leading `-`) is used -- never a path.  The
   command word is the WHOLE shell word under the caret (the lexer reads
   only to the caret it is given), its quote state taken from the lexer,
   wrappers stripped by the spec-aware lexer (`yew_shctx_at_with` +
   `yew_compspec_wrapper`), a path reduced to its basename.
6. **`A-s` details.**  A lone `sudo`/`doas` word is the prefix too;
   `sudoedit` is not.  An empty body whose newest bang entry already
   starts with a prefix gets that entry as it is, not `sudo sudo …`.  A
   caret AT the body start is on the command's first letter and moves
   with it.  Off a bang line: a WARN message and status OK (57.28's A-y
   rule -- an error would abort an open typing transaction).
7. **`A-e` refuses INPUT prompts** (`A-e: this prompt cannot become a
   buffer`): a picker's filter, an LSP rename, a Git or AI question --
   each owner waits on that prompt's answer through its callback.  The
   `:` prompt and both search prompts go and come back; a search reopens
   through `yew_search_open`, so its preview and restore point are a
   fresh `/`'s.  The prompt's own return mode comes back with it, and the
   tab A-e was pressed in is made active again.
8. **"Closed" is the buffer's RELEASE** (`src/ui/cmdedit.[ch]`).  In yew
   a closed tab does not free its buffer, so the release is either
   `yew_ws_scratch_drop` (hooked: `yew_cmdedit_released`) or -- checked
   by `yew_cmdedit_settle` at every event boundary (after a key, a paste,
   any loop event, a click) -- the buffer no longer being shown in any
   window, which the settle then drops.  Tab close (key or close box),
   `:tabonly`, a pane close, another file opened over it: all the same
   release.  The prompt is reopened only at the boundary, because a close
   is usually a command typed into ANOTHER prompt that is still open
   while it runs.
9. **The close commands on that buffer are intercepted**: in yew `:q`
   quits the editor and `:wq` writes a file, neither of which means
   anything for it.  `:q`, `:wq`, `:close` (the contract's `ed.close` is
   `ed.buf.close`/`ed.file.close`) and `:tabclose` return the text;
   `:q!` and `:close!` return the ORIGINAL line with its original caret.
   Each non-discarding close checks the one-line rule first and refuses
   with the message, the buffer untouched.  A route that cannot ask
   first (the close box, `:tabonly`) gets the same answer after the
   fact: the text goes back into a new `*command-line*` buffer with the
   message -- never nowhere.  A release while another prompt is open
   waits for it to close; `A-e` meanwhile says `*command-line* is still
   returning its line` rather than start over on it.
10. **The one-line rule, exactly** (`yew_cmdedit_oneline`): after a
    complete command -> `; `; after an operator (`|`, `&&`, `;`, `&`,
    `(`), a keyword that opens a list (`if then else elif while until do
    { ! time`) or an empty line -> a blank (`; ` there is a syntax
    error); a `\` continuation vanishes as the shell drops it, the next
    line's indent folding to one blank -- or to none when neither side
    had one (`a\<nl>b` is the word `ab`, so "join with a blank" would
    change it).  Newlines inside `$( … )` and `{ … }` join like any
    others (valid shell); inside quotes -> `line N: a one-line prompt
    cannot hold a newline inside quotes`; inside a comment or here-
    document (the lexer's NONE) -> `… inside a comment or an unfinished
    construct`.  Trailing newlines are dropped; CRLF is one newline; a
    NUL is refused with its line; past 64 KiB or 1024 lines, refused
    rather than lexed once per newline.
11. **A second `A-e`** switches to the open buffer and puts the line it
    was pressed on into that prompt's history, where Up finds it.
12. **The `*command-line*` tab** is untitled (a saved session drops it),
    opened by the new `yew_tab_open_buffer`, and labelled with its
    buffer's name (`tab_basename` names an untitled tab after a named
    scratch buffer).  It is laid out before the caret is followed: a
    zero-width view scrolled the line away (caught by the golden).
13. **Batch refusals.**  `ed.cmdline.man_page` is `YEW_CMD_INTERACTIVE`
    (it hands a child the terminal, like `term_run`); `edit_in_buffer`
    is `YEW_CMD_PROMPTS` (it closes and reopens the prompt).  Both are in
    the table and in `tests/script/29_interactive_refusal.fl`, now 50.
    `toggle_sudo` acts only on an open prompt, as `last_arg` does, and is
    not.  All three are internal (no motion word, no `gen_cmds[]` row),
    in the invariant-9 list, verbs in `command_name_valid`, and
    PSEL_COLLAPSE rows of 57.29's collapse table.
14. **Bindings: 389** (385 + `A-s`, `A-h`, `A-e`, `A-v`).  None collided
    in E; `A-s`, `A-h` and `A-v` are Git keys in F, a different mode.
15. **PTY.**  `s57_31_toggle_sudo`, `s57_31_edit_in_buffer_roundtrip` and
    one more, `s57_31_edit_in_buffer_open` (the tab, the body, Insert).
    The man handover has no faithful golden -- the child owns the screen
    -- so, as 57.18 did, the restore is a unit test in a real pty
    (`test_man_page_restores_the_terminal_on_every_exit`: a fake `man`
    that shows a page, has none, is killed, and an exec that fails),
    and the routing is unit-tested with the handover stubbed.
