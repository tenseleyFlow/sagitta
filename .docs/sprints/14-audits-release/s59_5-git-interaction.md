# Sprint 59.5: Git interaction ownership

## Prerequisites

Sprint 52 supplies FUSS Git actions, pickers, commit cleanup and asynchronous
Git jobs. Sprint 57 supplies stable tab identities, command-output buffers,
prompt editing and modal pickers. Sprint 59's remediation remains intact.
The binding decisions, architecture and Fletch plans apply. This fractional
sprint is the user's 2026-10-20 Git workflow overhaul, including interactive
rebase; it does not advance the numbered campaign or restart its soak.

## Goals

Never display Git or command output by borrowing a document's pane. Give
input-taking Git operations explicit modal ownership, including commit,
amend, destructive confirmations and interactive rebase's editor requests.
Escape must have an unambiguous, safe outcome. Preserve all Git operations,
keyboard access, document bytes, cursor state and undo history.

## Deliverables

1. **Temporary output tabs**, in `src/ui/tabs.[ch]`, `src/edit/shell.c`,
   `src/edit/shell_cmds.c`, `src/edit/mode.c` and
   `src/edit/file_cmds.c`.

   ```c
   int yew_tab_open_transient(Ed *ed, Buffer *buf);
   bool yew_tab_dismiss_transient(Ed *ed);
   ```

   A transient tab is ungrouped, explicitly named, read-only, omitted from
   saved sessions, and remembers its origin by stable tab ID. Reopening an
   existing output buffer selects its tab instead of making duplicates.
   Closing it returns to the surviving origin, otherwise the usual tab
   neighbor. Escape, `:q` and normal tab-close all close the same view.
   Closing a running job's output does not kill its job; `:jobs` can reopen
   it. A command with no output removes its empty temporary view without
   changing a different active tab. `:r !` and filters retain their explicit
   document-editing semantics.

   **Pitfall:** a transient tab's buffer can outlive the view. Never free a
   buffer still owned by a running job. Never use a saved array index after
   tab compaction. Preserve dirty origin buffers and split layouts exactly.

2. **Reusable input modal**, in `src/ui/cmdline.[ch]`, `src/ui/mouse.c`,
   `src/ui/draw.c` and `src/edit/ed.c`.

   ```c
   void yew_cmdline_open_modal(Ed *ed, const char *title,
       const char *help, const char *seed, bool multiline,
       YewCmdlineInputDone done, void *ctx);
   void yew_cmdline_draw_modal(Ed *ed);
   ```

   Reuse the prompt's private TextBuf/Win, editing choke point, undo,
   grapheme motion and clipboard handling. The document Win is never
   replaced. Center and bound the modal to the current grid; size with its
   content, scrolling when the content exceeds the available rectangle.
   Paint after FUSS and other overlays. The modal owns keyboard and mouse
   input; clicks outside cannot edit or close the underlying document.

   | Input | Single line | Multiline |
   |-------|-------------|-----------|
   | Escape | Cancel | Cancel |
   | Enter | Accept | Insert newline |
   | Ctrl-S | Accept | Accept |
   | Arrows/Home/End | Text movement | Text movement |
   | Shift + movement | Extend selection | Extend selection |
   | Ctrl-C/X/V | Existing selection clipboard behavior | Same |

   **Pitfall:** callback teardown precedes opening the next prompt. A
   picker callback must not restore its old mode over a newly opened modal.
   Multiline input must preserve newlines and reject NUL, never flatten a
   commit body or rebase todo list into one command line.

3. **FUSS Git workflow ownership**, in `src/mod/git/fussmode.[ch]`.
   Commit and amend use the modal, keeping the existing comment/cleanup
   rules and `git commit -F - --cleanup=verbatim` transport. Empty or
   cancelled messages do not spawn a commit. Failed submission preserves
   the draft for correction. Remove the commit pane clone/substitution.
   Branch creation/deletion, stash messages, tags, rename, destructive
   operations and rebase confirmation use the same input modal; list
   choices continue to use the existing modal picker. Annotated tags accept
   a multiline annotation. Destructive confirmations state the operation
   and exact target. Git stdout/stderr viewers use titled transient tabs,
   never a floating panel attached to the document. Suspend FUSS while an
   output tab is active and restore it on dismissal when its origin exists.

   **Pitfall:** cancellation is not success, and successful subprocess
   startup is not successful Git completion. Preserve the draft until the
   completion result is known. Display failures without dropping stderr.

4. **Interactive rebase editor bridge**, in `src/mod/git/` and the
   executable's internal-helper dispatch. Git stays asynchronous. Its
   sequence editor and message editor request yew-owned multiline modals;
   accepting writes only the exact requested Git temporary file, then
   releases the waiting helper. Cancellation returns a nonzero editor
   status and reports the repository's resulting rebase state. Continue,
   abort, edit/reword and squash must remain available. No nested terminal
   editor, shell interpolation of user messages, or inherited interactive
   stdin. Bridge resources are private to this editor/job, bounded, and
   cleaned on normal exit, cancellation and failed startup. A dead parent
   cannot leave an editor helper waiting forever.

   **Pitfall:** cancelling a message edit after some commits have replayed
   must not silently reset or discard the partial rebase. Explain that
   state and offer the existing continue/abort actions.

5. **Help and evidence**, in `docs/manual/`, `docs/man/yew.1` and
   `.docs/HANDOFF.md`. Update command-output dismissal and Git input
   instructions. Record exact test commands/results and remaining work;
   never report this overhaul complete while rebase remains on terminal
   handover. No features or stability protections may be removed to meet
   obsolete size gates.

## Testing Strategy

- Unit: stable-ID return after reorder/close, unchanged origin TextBuf,
  cursor/undo and split tree, no duplicate output tabs, empty-output
  cleanup, running-job dismissal/reopen, Escape and ordinary close.
- Unit: modal focus isolation, newline preservation, Unicode movement,
  selection/clipboard, small-terminal and resize geometry, cancellation
  and callback replacement. Exercise both direct action and action picker.
- Script: real temporary Git repositories for commit/amend, empty and
  rejected messages, annotation, confirmations, rebase reorder/reword,
  squash, cancellation, conflicts, continue and abort. Assert file bytes
  and argv rather than only messages. Use existing fixture isolation.
- PTY: reproduce the user's FUSS commit sequence; type and cancel/submit
  without focus escape. Verify titled output tabs and a visible modal
  caret with FUSS behind it. Update affected intentional goldens only.
- Fuzz: extend existing FUSS/prompt state exercises; run bounded corpus
  smoke with one worker, never launch competing long campaigns.
- Regression: warning-clean build, focused unit/script/PTY suites and
  sanitizer coverage. Use at most two build jobs alongside the soak.

## Definition of Done

- [x] `make -j2` is warning-clean under the repository C11 flags.
- [x] `:!` output and `:jobs` have real named tabs; no document pane swap.
- [x] Escape, `:q` and normal tab-close return safely from output views.
- [x] Closing output during a job leaves it running and recoverable.
- [x] Commit and amend are centered editable modals with no scratch swap.
- [x] Cancel/empty commit causes zero Git mutations; failures retain draft.
- [x] All FUSS input flows use modals with exact destructive-action help.
- [x] Git output and errors use independent read-only output tabs.
- [x] Interactive rebase sequence and message edits use the modal bridge.
- [x] Rebase cancel/conflict/continue/abort tests pass without hung helpers.
- [x] Focused unit, Git-script, PTY and sanitizer checks pass.
- [x] Help, handoff and evidence reflect shipped behavior and remaining work.

Local implementation checkpoint, 2026-10-20 (Apple-silicon macOS; unpushed):
default and `MODULES=` builds pass. Full units: 3,051 tests, 73,852,198
assertions, zero failures, two skips. The subsequently extended modal unit
passes 49 assertions in normal and sanitizer builds. Real-Git script suite:
642 assertions in normal and ASan/UBSan builds. Fifty selected FUSS/shell/
output/self-open PTYs and nine context-menu PTYs pass (two overlap). Reviewed
goldens cover the new tabs, centered commit modal/caret and destructive help.
ASan/UBSan `job`, `cmdline`, `fuss`, `git`, `tab` unit filters pass. Bounded
FUSS fuzz: seed 5905, 300 iterations, corpus 5, hash `2fbfa5bb38d353d5`, pass.
Man/manual/docs-sync checks and selftests pass; optional groff lint is skipped
because groff is unavailable. The sanitizer runtime requires
`ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`; this is not LSan evidence.
No fresh Linux/GCC, hosted CI, Valgrind, performance/size gate or soak signoff
is claimed here. See `.docs/HANDOFF.md` for the next pickup.
