# Sprint 57.11: Shell Self-Open Handoff

## Prerequisites

- Sprint 18 — E-mode parsing and the named-command dispatch surface.
- Sprint 19 — `:!` / `ed.shell.run`, the asynchronous job table, and the
  rule that a range changes `:!` into a filter.
- Sprint 23 — canonical tab identity, append-at-right opening, and active-tab
  focus.
- Sprint 24 — path-bearing tab groups, computed membership, ordinals, and
  lazy hydration.
- Sprint 57.10 — row-aware group/tab presentation and navigation.
- Binding plans and invariants 1–5, 7–9. Self-open may not reinterpret a
  general shell line, create a second buffer for an already-open path, weaken
  file identity, or add a polling cost to the editor loop.

## Goals

Make the exact no-range command `:!yew FILE` open `FILE` in the current Yew
session instead of launching a nested Yew whose terminal output becomes a job
buffer. Relative paths resolve from the workspace root, matching the cwd used
by ordinary `:!` jobs. Absolute paths and `..` paths may leave the workspace;
those files still open as normal ungrouped tabs but remain outside FUSS and
any other workspace-confined service. A newly opened workspace path joins the
most specific open tab group whose root contains it and takes the last group
ordinal; otherwise it is the rightmost row-1 tab. The opened tab becomes
active.

This is an exact editor-aware command handoff, not a second shell parser.
Pipelines, redirects, expansions, command lists, options, directory launches,
and ranged `:!` commands keep Sprint 19 behavior and run through `$SHELL -c`.
Yew has no interactive terminal panel in 1.0, so this sprint introduces no
Facsimile-style session spool, socket, FIFO, filesystem polling, or new event
loop fd.

## Deliverables

### 1. Conservative self-command recognizer — `src/edit/shell.c`,
`src/edit/shell.h`

Add:

```c
typedef enum {
    YEW_SHELL_SELF_NOT_HANDLED,
    YEW_SHELL_SELF_OPENED,
    YEW_SHELL_SELF_ERROR
} YewShellSelfResult;

YewShellSelfResult yew_shell_try_self_open(Ed *ed, const char *cmdline,
                                            char *err, size_t errsz);
```

The recognizer accepts one shell command whose evaluated argv is exactly
`{"yew", "FILE", NULL}` or `{"yew", "--", "FILE", NULL}`. Leading and
trailing horizontal whitespace are allowed. A word may contain literal bytes,
single-quoted bytes, double-quoted bytes without expansion, and backslash
escapes. Quotes are removed and escapes decoded into the path before opening.
The input command remains immutable.

| Input | Result |
|---|---|
| `yew src/main.c` | handle in the current editor |
| `yew 'notes one.md'` | handle; path is `notes one.md` |
| `yew -- '-draft'` | handle; path is `-draft` |
| ` yew ../shared/api.h ` | handle; the resulting tab may be outside the workspace |
| `yew` or `yew ''` | `YEW_SHELL_SELF_ERROR` with a file diagnostic |
| `yew a.c b.c` | not handled; preserve normal CLI/shell behavior |
| `yew --version` | not handled |
| `./build/yew a.c` | not handled |
| `command yew a.c` | not handled |
| `yew "$FILE"` | not handled; the shell owns expansion |
| `yew *.c` | not handled; the shell owns globbing |
| `yew a.c | sed -n 1p` | not handled |
| `yew a.c; echo done` | not handled |
| `yew a.c >out` | not handled |

The conservative rule is semantic: if Yew cannot prove that the shell would
produce exactly the accepted argv, it returns `NOT_HANDLED`. Ordinary shell
execution must remain the fallback. Single-quoted metacharacters are literal
and may be accepted; unquoted or expandable metacharacters are not.

Newlines and NUL are never accepted by this recognizer. E mode already rejects
NUL; a newline falls through to the shell so the recognizer does not invent a
multi-command grammar.

**Pitfall:** do not tokenize with E-mode's argv parser. `:!` deliberately owns
the remainder of the line verbatim, and shell quoting is not E-mode quoting.
The self recognizer is a small finite scan for one proven-safe shape only.

**Pitfall:** do not recognize by prefix. `yew2`, `yew-tool`, an absolute
executable path, or a shell function invocation with preceding assignments is
not the requested builtin spelling.

### 2. Shell command integration — `src/edit/shell_cmds.c`

In `yew_shell_cmd_run()`, after the argument check and before
`yew_shell_run()`, call `yew_shell_try_self_open()` only when no range was
given. Map `OPENED` to `YEW_CMD_OK`, surface `ERROR` through the message line,
and leave `NOT_HANDLED` on the unchanged Sprint 19 path.

The following surfaces remain ordinary external jobs:

- `ed.shell.run_bg`, because background execution explicitly means a job;
- `ed.shell.read`, because it requests child stdout at the cursor;
- `ed.shell.filter` and every ranged `:!`, because their output is the edit;
- any command line not proven to be the exact self-open form in §1.

Successful handoff creates no `YewJob`, no `*job:*` scratch buffer, no child,
and no completion footer. The newly active document is the visible outcome.

**Pitfall:** ranged `:!yew FILE` must never open a tab. The range check has
priority because changing a filter into navigation silently discards the
user's requested edit operation.

### 3. Path resolution — `src/edit/shell.c`

Resolve a recognized path using the same base as `YewJobSpec.cwd == NULL`:
`yew_ws_root(ed)`. Absolute paths remain absolute. Relative paths are joined
to the workspace root before tab lookup/open, so behavior does not depend on
the process cwd from which Yew itself happened to start.

For an existing path, `realpath(3)` supplies canonical identity. For a new
file, canonicalize its existing parent when possible and append the basename;
otherwise keep a bounded absolute spelling. Reject only an empty result or
overflow beyond `PATH_MAX`. A path that currently names a directory is not
the file form and falls through to the existing external launch behavior.
Opening a file outside the workspace is allowed and is not a warning.

This sprint does not change FUSS. Its existing workspace confinement decides
whether the new tab appears there. It does not change LSP root selection,
syntax detection, save policy, recovery prompts, or binary-file prompting;
the normal `yew_tab_open()` / `yew_tab_switch()` path owns those behaviors.

**Pitfall:** never temporarily `chdir()` the editor process. Every async job
and relative-path consumer shares that process; changing cwd around a tab open
would make unrelated callbacks nondeterministic.

**Pitfall:** `/work/src2/x.c` is not below `/work/src`. Every containment
test is component-boundary aware, including the root directory special case.

### 4. Group destination policy — `src/ui/groups.c`, `src/ui/groups.h`

Add:

```c
u32 yew_group_for_path(const Ed *ed, const char *path);
```

`path` is the absolute/canonical candidate from §3. Inspect group
`dir_path` values in insertion order and return the most specific root that
contains the path. Groups may overlap. A longer matching root wins; equal
roots prefer `yew_active_group_id(ed)`; remaining equal ties retain insertion
order for deterministic behavior. A path equal to the directory itself is
not a file below that directory and does not match.

This helper suggests a destination only. `TabGroup.dir_path` remains a label
of origin, not a membership constraint: users may still add arbitrary files
to a group through the picker or existing APIs.

After resolving the destination, self-open records whether the file already
has a tab, then calls `yew_tab_open()`:

- new tab plus matching group: call `yew_group_add_member()` before the final
  `yew_tab_switch()`; its computed max ordinal puts the tab last;
- new tab without a match: retain the append-at-right ungrouped tab;
- existing tab: focus it without moving it or rewriting membership.

Compute the destination before opening, while the command's originating
active group still exists as the equal-root tiebreaker. Check the returned tab
index at every step. A tab-cap, buffer-cap, hydration, or file-open failure is
an error; never attach whichever tab happened to remain active.

**Pitfall:** groups own no member list. Do not cache a group count or mutate an
ordinal directly. `yew_group_add_member()` is the only membership operation
used here.

**Pitfall:** a newly appended group member need not be made array-contiguous
with its siblings. Row 1 is derived from the first member and row 2 is sorted
by ordinal; forcing a block reorder would move unrelated row-1 tabs.

### 5. User-facing contract and deferrals

The help/man surface for `:!` gains one concise note when Sprint 59 authors the
manual. Until then the command registry help remains accurate: this is still
`ed.shell.run`, with one editor-aware command form.

Directory self-handoff is deliberately outside this sprint. `:!yew DIR`,
multiple files, explicit Yew options other than the path separator `--`, and
self-invocation through aliases or executable paths retain external job
semantics. A future request may extend the recognizer or justify session IPC;
there is no silent stub and no new named command reserved here.

## Testing Strategy

- Unit recognizer table: whitespace, plain/quoted/escaped filenames,
  `--`, empty operands, unterminated quotes, expansion/glob/operator cases,
  prefixes, options, multiple files, and trailing commands.
- Unit integration: a no-range `CmdCtx` opens without growing `ed.jobs` or
  creating a scratch buffer; a ranged call and every non-matching form still
  take or select the established shell path.
- Unit path matrix: workspace-relative existing and new files, absolute
  outside-workspace files, `../` escape, spaces, symlink aliases, directory
  refusal, and `PATH_MAX` overflow.
- Unit group matrix: no match, one root, nested roots, equal roots with and
  without an active match, component-prefix collision, new member last by
  ordinal, and existing-tab membership preservation.
- PTY: type the literal `:!yew FILE` flow, prove the file text and active tab
  appear, and prove no `*job:*` buffer/footer appears.
- PTY/regression: retain one ordinary `:!printf` job and the existing ranged
  filter goldens unchanged.
- Build: warning-clean GCC and Clang with default modules and `MODULES=""`;
  focused ASan/UBSan unit and PTY coverage.
- Performance: no new steady-state work. A focused loop of 10,000 rejected
  recognizer calls and 1,000 accepted existing-tab calls must remain below the
  existing 5 ms keypress gate per invocation; no new baseline is warranted
  unless measurement shows a material regression.

## Definition of Done

1. `:!yew FILE` opens exactly one active document tab and creates zero jobs
   and zero job buffers.
2. Relative paths resolve from the workspace root; absolute and parent paths
   may open outside it without being added to FUSS.
3. New files under overlapping groups join the most specific group at its last
   ordinal; equal roots prefer the active group deterministically.
4. An unmatched or outside-workspace new file is the rightmost ungrouped
   row-1 tab and is active.
5. Reopening an existing path focuses its stable tab without duplication,
   reordering, or membership mutation.
6. Quoted spaces and the `--` separator work; ambiguous shell syntax always
   falls through unchanged.
7. Ranged/filter, background, read-output, directory, option, multi-file,
   pipeline, redirection, and command-list forms retain Sprint 19 semantics.
8. Tab caps and invalid/overflowing paths report an error
   without changing tabs, groups, buffers, jobs, or focus.
9. The existing job, tab, group, FUSS, state, LSP-root, binary-file, and save
   suites remain green without contract changes.
10. Default/core-only GCC and Clang builds, focused ASan/UBSan, unit/script,
    and the exact PTY are green on the committed SHA.
