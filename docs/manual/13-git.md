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

Commit opens the editor on Git's message file using the normal buffer and
save path. Abort, save failure, or child failure restores editor and terminal
state. yew never invents a commit message and never bypasses hooks.

## Diff, gutter, and blame

Diff views are read-only editor buffers with intraline spans. Gutter signs
come from cached hunks and share the fixed priority stack with diagnostics and
other signs. Alt-Up/Down walks hunks from document modes. Blame is requested
asynchronously and cached by file identity/revision; stale results cannot
decorate a changed buffer.

FUSS still works outside a Git repository as a workspace tree. Git-specific
actions then report their unavailable state instead of spawning unrelated
language servers or blocking the UI.
