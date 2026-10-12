# Windows, panes, tabs, groups, and workspace state

A workspace is rooted at the directory yew opened. Buffers may live outside
it, but discovery, relative labels, project configuration, symbol indexing,
and FUSS are workspace-relative.

## Panes and focus

The layout is a binary split tree whose leaves are windows. Horizontal and
vertical split commands divide the focused leaf; closing a leaf collapses its
parent without destroying unrelated windows. Focus commands choose the
nearest pane geometrically, and `focus-next` walks stable tree order.

Each leaf has a minimum usable size. A split or resize that would violate it
is refused rather than constructing an invalid rectangle. When the terminal
shrinks, layout collapses temporarily and restores the saved proportions when
space returns.

## Tabs and groups

Ordinary tabs occupy the top strip. `+` creates an untitled document. A tab
group gives a directory-backed collection one strip position and a member
strip; entering a group remembers its active member. `:group-close` closes
the active group, falling back to the active tab when no group is active.

`Alt-g` in FUSS opens the selected directory as a tab group. The group picker
can select members explicitly. A file opened later—whether from FUSS, an
ordinary command, or `:!yew PATH`—joins the end of the matching directory
group; otherwise it becomes the rightmost ordinary tab. Paths outside the
workspace never appear in FUSS.

Mouse hit regions and keyboard commands share the same registered actions.
Dragging a tab to a pane edge can create a split; Escape cancels an armed
drag. If the pane count or layout changes mid-drag, the drop is discarded
instead of guessing a new target.

## Saved state

Workspace state is a pure Fletch literal below
`$XDG_STATE_HOME/yew/workspaces/`. It records open paths, tab/group order,
layout, focused window, cursors, viewports, and compatible option scopes.
Emission is byte-stable. Restore validates every path and field and skips a
bad entry without executing state as a program.

`--clean` deliberately skips saved workspace state. Untitled dirty bytes are
not treated as workspace metadata; durability belongs to the journal and
normal dirty-buffer quit rules described in [files](03-files.md).

## FUSS as the workspace edge

FUSS is an off-canvas, full-height tree that pushes document and tab content
to the right. Directories collapse by default, expand for an active descendant
or remembered user choice, and persist expansion memory across close/reopen.
The width grows to fit the visible tree within its cap and may take the screen
when a narrow split cannot show a useful document alongside it.

Typing an unmodified filename character performs visible-tree type-to-jump.
Actions use Alt chords, so they never steal filename characters. Ctrl-Shift-/
opens the action list built from the effective keymap.
