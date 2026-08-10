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

static void cursor_update_goal(const TextBuf *tb, Cursor *c)
{
    LineNo line = yew_textbuf_line_of(tb, c->pos);
    Span span = yew_textbuf_line_span(tb, line);

    c->goal_col = yew_off_to_gcol(tb, span, c->pos);
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
    cursor_update_goal(tb, c);
}

void yew_cursor_right(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_pos(c, yew_grapheme_next_boundary(tb, c->pos));
    cursor_update_goal(tb, c);
}

void yew_cursor_up(const TextBuf *tb, Cursor *c)
{
    LineNo line;
    Span span;
    cursor_require(tb, c);
    line = yew_textbuf_line_of(tb, c->pos);
    if (line.v == 0U)
        return;
    span = yew_textbuf_line_span(tb, LINENO(line.v - 1U));
    cursor_set_pos(c, yew_gcol_to_off(tb, span, c->goal_col));
}

void yew_cursor_down(const TextBuf *tb, Cursor *c)
{
    LineNo line;
    Span span;
    cursor_require(tb, c);
    line = yew_textbuf_line_of(tb, c->pos);
    if (line.v + 1U >= yew_textbuf_line_count(tb))
        return;
    span = yew_textbuf_line_span(tb, LINENO(line.v + 1U));
    cursor_set_pos(c, yew_gcol_to_off(tb, span, c->goal_col));
}

void yew_cursor_line_home(const TextBuf *tb, Cursor *c)
{
    LineNo line;

    cursor_require(tb, c);
    line = yew_textbuf_line_of(tb, c->pos);
    cursor_set_pos(c, yew_textbuf_line_start(tb, line));
    c->goal_col = (GCol){0U};
}

void yew_cursor_line_end(const TextBuf *tb, Cursor *c)
{
    LineNo line;

    cursor_require(tb, c);
    line = yew_textbuf_line_of(tb, c->pos);
    cursor_set_pos(c, cursor_line_end(tb, line));
    c->goal_col = (GCol){YEW_GCOL_EOL};
}

void yew_cursor_buf_home(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_pos(c, BYTEOFF(0U));
    c->goal_col = (GCol){0U};
}

void yew_cursor_buf_end(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_pos(c, BYTEOFF(yew_textbuf_len(tb)));
    c->goal_col = (GCol){YEW_GCOL_EOL};
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
