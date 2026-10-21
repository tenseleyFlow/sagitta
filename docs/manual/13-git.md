# FUSS mode and Git

FUSS is the workspace tree and Git action surface. It reads repository status
through a bounded porcelain-v2 parser and keeps Git work off the render path.
The visible tree is dirty-first and workspace-relative; repository paths that
resolve above the workspace are filtered rather than leaking a parent row.

Directories use `+` and `-` expansion indicators without connector-line
clutter. Expansion survives close/reopen, and the active document recursively
opens its ancestors. The selected row stays stable across status refresh by
path identity where possible.

## Navigation and discovery

Arrows navigate the visible tree. Sibling navigation is depth-aware. Typing
ordinary filename characters starts or extends type-to-jump over visible,
expanded rows only; the hint expires without stealing the next unrelated key.
Scrolling keeps three tree rows below the selection, including blank space
after the final entry. The margin shrinks on small terminals.

Actions therefore use Alt chords. The defaults include Alt-A for stage,
Alt-D for diff, and Alt-G for opening the selected directory as a tab group.
Ctrl-Shift-/ opens `FUSS actions`, a filterable picker built from the effective
keymap. User bindings replace the advertised row automatically; the palette
does not maintain a second verb table.

## Status and staging

The two porcelain status columns remain distinct through the model, so staged
and unstaged changes do not collapse into a single ambiguous flag. Stage,
unstage, and discard target the captured selected path. Directory actions
apply to the selected subtree. An unknown or vanished path disables actions
instead of applying them to a newly selected row.

Commit (`Alt-m`) opens a centered **Commit message** modal.
Enter inserts a newline; Ctrl-S submits; Escape cancels without running a
commit. Repeating Alt-m also cancels and discards the draft. Other dialogs
likewise cancel on their opening shortcut. Amend uses the previous message.
Arrow movement, Shift selection,
undo and Ctrl-C/X/V edit the modal's private text, never the document behind
it. Empty messages abort. Existing Git comment-character cleanup remains in
effect. yew never invents messages or bypasses hooks. A rejected commit keeps
the draft open for correction; its complete error is in a separate output tab.

Branch creation, stash messages, tags, rename, and confirmations use the same
modal surface. Enter submits single-line prompts. For an annotated tag, put
the tag name on the first line and its annotation on subsequent lines, then
press Ctrl-S. Hard reset from the picker and rebase abort require explicit
confirmation. Escape cancels even after the confirmation word has been typed.
Long help text wraps; Page Up/Down reveals additional details.

## Interactive rebase

Choose a base, review the range in its temporary tab, and confirm `rebase`.
The **Interactive rebase todo** modal contains Git's actual todo list; edit
and reorder its lines with the usual `pick`, `reword`, `edit`, `squash`,
`fixup`, and `drop` commands, then Ctrl-S. Subsequent message edits use the
same modal. Git runs asynchronously and never starts a nested terminal editor.
Human editing time does not consume the Git subprocess timeout.

Escape fails the current Git editor step. A partially replayed rebase is not
silently discarded: inspect the output and repository state, then use
`git.rebase.continue` or `git.rebase.abort` from FUSS as appropriate. Abort
asks for `abort` before returning to the original branch. The bridge accepts
text files smaller than 1 MiB; unreadable, binary, or oversized editor requests fail
explicitly. Authentication continues to use configured Git credential agents;
terminal authentication prompts are disabled and failures appear as output.

## Diff, gutter, and blame

FUSS **Alt-d** opens a named, read-only **file comparison** tab, not a raw
patch transcript. It shows the full file context, red deleted rows, green
added rows, and old/new source line numbers. The gutter's `-` and `+` remain
distinct without color. Alt-Up/Down jumps between change runs; ordinary
navigation, scrolling, selection and copying still work. Editing commands
are refused in these read-only views. In very narrow panes the source
numbers yield to the +/- marker, leaving room for text.

The header names the comparison: **HEAD → saved worktree** includes staged
and unstaged disk changes; it does **not** include unsaved editor changes.
Untracked files (including a repository's first files before any commit)
compare empty → saved worktree. Incoming-only rows compare merge base →
upstream; local changes take precedence if that path also changed upstream.
Missing final newlines are noted. Deleted files retain their old
source lines; binary and metadata-only changes get an explanatory view.
Comparisons are limited to 16 MiB of collected output and 200,000 displayed
source rows; oversized, failed or incomplete output is never shown as a
complete file comparison.

The document **Alt-Shift-d** (`ed.git.diff.view`) command remains the
index-versus-live-buffer side-by-side view, including intraline emphasis.
It is separate from FUSS's saved-file comparison. Interactive merge choices
(ours/incoming/both) are **not implemented yet**; a conflicted file can be
inspected against HEAD, but the diff view cannot resolve or stage it.

FUSS log/status and other command results also use read-only temporary tabs.
Escape, `:q`, or normal tab-close dismisses the view and resumes FUSS without
changing the original document. Closing a loading file comparison discards
its eventual result; it never reopens itself or steals focus. Gutter signs
come from cached hunks and share the fixed priority stack with diagnostics and
other signs. Alt-Up/Down walks hunks from document modes. Blame is requested
asynchronously and cached by file identity/revision; stale results cannot
decorate a changed buffer.

FUSS still works outside a Git repository as a workspace tree. Git-specific
actions then report their unavailable state instead of spawning unrelated
language servers or blocking the UI.
