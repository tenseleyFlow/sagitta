#include "text/cursor.h"

#include "util/log.h"

static void cursor_require(const TextBuf *tb, const Cursor *c)
{
    if (tb == NULL)
        YEW_BUG("cursor motion: NULL buffer");
    if (c == NULL)
        YEW_BUG("cursor motion: NULL cursor");
}

static void cursor_set_pos(Cursor *c, ByteOff pos)
{
    bool unselected = c->anchor.v == c->pos.v;

    c->pos = pos;
    if (unselected)
        c->anchor = pos;
}

/*
 * Horizontal motion does not MEASURE the new column, it just says "the
 * goal is wherever I am now".  Measuring a cell column walks the line --
 * cursor.h says why -- and nothing reads the goal until a vertical
 * motion, which is already walking.
 */
static void cursor_update_goal(Cursor *c)
{
    c->goal_col = (CCol){YEW_CCOL_HERE};
}

CCol yew_cursor_goal(const TextBuf *tb, const Cursor *c, u32 tabw)
{
    LineNo line;
    Span span;

    cursor_require(tb, c);
    if (c->goal_col.v != YEW_CCOL_HERE)
        return c->goal_col;
    line = yew_textbuf_line_of(tb, c->pos);
    span = yew_textbuf_line_span(tb, line);
    return yew_off_to_ccol(tb, span, c->pos, tabw);
}

static ByteOff cursor_line_end(const TextBuf *tb, LineNo line)
{
    Span span = yew_textbuf_line_span(tb, line);
    ByteOff end = BYTEOFF(span.hi);

    if (line.v + 1U < yew_textbuf_line_count(tb)) {
        /* Every non-final line span ends in LF.  GB3 makes a preceding CR
         * part of the same cluster, so one previous step strips CRLF too. */
        end = yew_grapheme_prev_boundary(tb, end);
    }
    return end;
}

void yew_cursor_left(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_pos(c, yew_grapheme_prev_boundary(tb, c->pos));
    cursor_update_goal(c);
}

void yew_cursor_right(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_pos(c, yew_grapheme_next_boundary(tb, c->pos));
    cursor_update_goal(c);
}

/*
 * The PADDED cell lookup, because this is a caret and not a character:
 * a goal past the line's end rests AFTER the last character, on every
 * line including the buffer's final one, which is the only line without
 * a trailing newline to stand on.  `line_at_col` in edit/motion.c owes
 * the caret the same answer; the two vertical paths are the same key.
 */
void yew_cursor_up(const TextBuf *tb, Cursor *c, u32 tabw)
{
    LineNo line;
    Span span;
    cursor_require(tb, c);
    line = yew_textbuf_line_of(tb, c->pos);
    if (line.v == 0U)
        return;
    span = yew_textbuf_line_span(tb, LINENO(line.v - 1U));
    c->goal_col = yew_cursor_goal(tb, c, tabw);
    cursor_set_pos(c, yew_ccol_to_off_padded(tb, span, c->goal_col, tabw));
}

void yew_cursor_down(const TextBuf *tb, Cursor *c, u32 tabw)
{
    LineNo line;
    Span span;
    cursor_require(tb, c);
    line = yew_textbuf_line_of(tb, c->pos);
    if (line.v + 1U >= yew_textbuf_line_count(tb))
        return;
    span = yew_textbuf_line_span(tb, LINENO(line.v + 1U));
    c->goal_col = yew_cursor_goal(tb, c, tabw);
    cursor_set_pos(c, yew_ccol_to_off_padded(tb, span, c->goal_col, tabw));
}

void yew_cursor_line_home(const TextBuf *tb, Cursor *c)
{
    LineNo line;

    cursor_require(tb, c);
    line = yew_textbuf_line_of(tb, c->pos);
    cursor_set_pos(c, yew_textbuf_line_start(tb, line));
    c->goal_col = (CCol){0U};
}

void yew_cursor_line_end(const TextBuf *tb, Cursor *c)
{
    LineNo line;

    cursor_require(tb, c);
    line = yew_textbuf_line_of(tb, c->pos);
    cursor_set_pos(c, cursor_line_end(tb, line));
    c->goal_col = (CCol){YEW_CCOL_EOL};
}

void yew_cursor_buf_home(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_pos(c, BYTEOFF(0U));
    c->goal_col = (CCol){0U};
}

void yew_cursor_buf_end(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_pos(c, BYTEOFF(yew_textbuf_len(tb)));
    c->goal_col = (CCol){YEW_CCOL_EOL};
}

static ByteOff cursor_clamp_off(const TextBuf *tb, ByteOff pos)
{
    u64 len = yew_textbuf_len(tb);

    if (pos.v > len)
        pos = BYTEOFF(len);
    if (!yew_is_grapheme_boundary(tb, pos))
        pos = yew_grapheme_prev(tb, pos);
    return pos;
}

void yew_cursor_clamp(const TextBuf *tb, Cursor *c)
{
    bool unselected;

    cursor_require(tb, c);
    unselected = c->anchor.v == c->pos.v;
    c->pos = cursor_clamp_off(tb, c->pos);
    c->anchor = unselected ? c->pos : cursor_clamp_off(tb, c->anchor);
}
