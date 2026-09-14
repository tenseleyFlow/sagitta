#ifndef YEW_TEXT_CURSOR_H
#define YEW_TEXT_CURSOR_H

#include <stdint.h>

#include "text/piece.h"
#include "unicode/coords.h"

#define YEW_CCOL_EOL UINT64_MAX
/*
 * "Wherever the caret is": an UNRESOLVED goal, standing for the cell
 * column of `pos` on whatever line `pos` is on right now.  Horizontal
 * motion sets this instead of measuring; the next vertical motion
 * measures once and stores the answer, after which the goal is a real
 * column and stays sticky across short lines.  `yew_cursor_goal` is the
 * one place that resolves it.
 */
#define YEW_CCOL_HERE (UINT64_MAX - 1U)

/*
 * `goal_col` is a CELL column: the screen column the caret is trying to
 * hold while it moves between lines.
 *
 * FIELD REPORT: "I have a goal column on the v in `var acc = zero`, and I
 * arrow up to `fn total`, but the cursor lands before the goal column
 * should be."  The goal was a GCol.  Grapheme columns count a tab as ONE,
 * so a tab-indented line and a space-indented line disagreed about where
 * a given screen column is, and crossing between them slid the caret left
 * by tabwidth-1 cells for every tab in the indent.  The SAME arrow key
 * got it right with wrap on, because the wrapped path has always carried
 * its goal as a CCol in `Win.wrap_goal`.  Cells is the answer the
 * codebase already had; this is the rest of it.
 *
 * The TYPE changes rather than motion converting at the boundary, because
 * at the boundary the conversion is not available: a goal outlives the
 * line it was taken from.  A caret that passes over a short line keeps the
 * goal it had, so the only line a motion could convert through is the one
 * the caret stands on now -- the wrong line, and the further the caret has
 * travelled the wronger it gets.  A remembered column has to be remembered
 * in the units it will later be compared in.
 *
 * It is RESOLVED LAZILY because cells, unlike graphemes, have no index
 * behind them: `yew_off_to_gcol` reaches a checkpoint and walks a few
 * clusters, while `yew_off_to_ccol` walks the line.  On an 8 MiB single
 * line that is 6 microseconds against 244 milliseconds, so measuring a
 * cell column on every LEFT and RIGHT would put a quarter-second scan on
 * every arrow key (perf-cursor is the gate that says so).  Only vertical
 * motion needs the number, and it is already paying a line walk to turn
 * the goal back into an offset, so that is where the measuring belongs.
 * Horizontal motion writes YEW_CCOL_HERE and moves on.
 *
 * A tab's cell width belongs to the buffer, so every conversion takes the
 * width the renderer uses for that buffer -- `Buffer.tabwidth`, and
 * YEW_VP_TABWIDTH when that is zero.  The motions below are the text
 * layer's, below Buffer, so the vertical pair takes the width as a
 * parameter.  Callers that hold a Win take it from `win->buf`.
 *
 * YEW_CCOL_EOL is "past the end of any line": it resolves to each line's
 * content end.  Zero still means column zero under either representation,
 * which is why every goal reset in the tree is untouched by the change.
 */
typedef struct Cursor {
    ByteOff pos;
    CCol goal_col;
    ByteOff anchor;
} Cursor;

_Static_assert(sizeof(Cursor) == 24U, "cursor layout changed");

/* The cell column `c` is aiming at, resolving YEW_CCOL_HERE against the
 * line `c->pos` is on.  Every reader of `goal_col` goes through this. */
CCol yew_cursor_goal(const TextBuf *tb, const Cursor *c, u32 tabw);

void yew_cursor_left(const TextBuf *tb, Cursor *c);
void yew_cursor_right(const TextBuf *tb, Cursor *c);
void yew_cursor_up(const TextBuf *tb, Cursor *c, u32 tabw);
void yew_cursor_down(const TextBuf *tb, Cursor *c, u32 tabw);
void yew_cursor_line_home(const TextBuf *tb, Cursor *c);
void yew_cursor_line_end(const TextBuf *tb, Cursor *c);
void yew_cursor_buf_home(const TextBuf *tb, Cursor *c);
void yew_cursor_buf_end(const TextBuf *tb, Cursor *c);
void yew_cursor_clamp(const TextBuf *tb, Cursor *c);

#endif
