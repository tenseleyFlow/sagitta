# Motion, selection, and multiple cursors

Motion is built from a small unit interface, not a collection of unrelated
language-specific jumps. Each unit supplies previous, next, home, end, and
selection-span operations. L, W, and B select the active implementation;
Highlight mode composes with it.

## Lines, words, and blocks

- L moves vertically by logical or wrapped display lines and horizontally to
  line boundaries. Home alternates indentation and column zero.
- W follows Unicode word boundaries. Ctrl-Left/Right use subword boundaries;
  `subword = true` makes those the ordinary word boundaries.
- B navigates structural language bodies when settled syntax is available.
  It ignores delimiters in strings and comments, uses enclosing scopes for
  horizontal motion, and falls back to indent/paragraph structure rather than
  guessing when syntax is unsettled.

Vertical motion retains a screen-cell goal column across short lines, tabs,
and wide graphemes. It never places the cursor inside a UTF-8 sequence,
grapheme cluster, tab cell, or the trailing cell of a wide glyph.

## Highlighting

Enter H with `h`, or begin immediately with Shift plus an arrow. The anchor
stays fixed while the active end moves. The selection kind may be character,
whole-line, or rectangular; rectangle edges widen to complete graphemes and
tabs rather than splitting display cells.

Selection actions are transactional. Delete, case conversion, indent,
dedent, join, replacement, and clipboard paste across all selected spans
become one undo step. Escape collapses to the active end and returns to the
borrowed unit mode.

## Multiple cursors

Multi-cursor edits run against original-coordinate spans in a deterministic
order. Overlapping edit ranges merge or suppress the later duplicate instead
of applying the same bytes twice. A failure at any cursor rolls back every
cursor's work and the journal append, so one command is all-or-nothing.

The primary cursor alone owns the terminal hardware cursor. Secondary cursors
and selections are grid styles, which is why rendering stays deterministic
even with hundreds of cursors.

## View movement

Page Up/Down and viewport scroll commands move through display rows. With
wrapping enabled, horizontal offset is zero and one logical line may occupy
several display rows. With wrapping disabled, horizontal following uses cell
width, not byte count. `scrolloff` is a desired margin and degrades safely in
small panes.
