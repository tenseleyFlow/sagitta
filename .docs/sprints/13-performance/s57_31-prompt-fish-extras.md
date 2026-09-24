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
