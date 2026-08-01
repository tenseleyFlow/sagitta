#include "text/cursor.h"

#include "util/log.h"

static void cursor_require(const TextBuf *tb, const Cursor *c)
{
    if (tb == NULL)
        SAG_BUG("cursor motion: NULL buffer");
    if (c == NULL)
        SAG_BUG("cursor motion: NULL cursor");
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
    LineNo line = sag_textbuf_line_of(tb, c->pos);
    Span span = sag_textbuf_line_span(tb, line);

    c->goal_col = sag_off_to_gcol(tb, span, c->pos);
}

static ByteOff cursor_line_end(const TextBuf *tb, LineNo line)
{
    Span span = sag_textbuf_line_span(tb, line);
    ByteOff end = BYTEOFF(span.hi);

    if (line.v + 1U < sag_textbuf_line_count(tb)) {
        /* Every non-final line span ends in LF.  GB3 makes a preceding CR
         * part of the same cluster, so one previous step strips CRLF too. */
        end = sag_grapheme_prev(tb, end);
    }
    return end;
}

void sag_cursor_left(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_pos(c, sag_grapheme_prev(tb, c->pos));
    cursor_update_goal(tb, c);
}

void sag_cursor_right(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_pos(c, sag_grapheme_next(tb, c->pos));
    cursor_update_goal(tb, c);
}

void sag_cursor_up(const TextBuf *tb, Cursor *c)
{
    LineNo line;
    Span span;

    cursor_require(tb, c);
    line = sag_textbuf_line_of(tb, c->pos);
    if (line.v == 0U)
        return;
    span = sag_textbuf_line_span(tb, LINENO(line.v - 1U));
    cursor_set_pos(c, sag_gcol_to_off(tb, span, c->goal_col));
}

void sag_cursor_down(const TextBuf *tb, Cursor *c)
{
    LineNo line;
    Span span;

    cursor_require(tb, c);
    line = sag_textbuf_line_of(tb, c->pos);
    if (line.v + 1U >= sag_textbuf_line_count(tb))
        return;
    span = sag_textbuf_line_span(tb, LINENO(line.v + 1U));
    cursor_set_pos(c, sag_gcol_to_off(tb, span, c->goal_col));
}

void sag_cursor_line_home(const TextBuf *tb, Cursor *c)
{
    LineNo line;

    cursor_require(tb, c);
    line = sag_textbuf_line_of(tb, c->pos);
    cursor_set_pos(c, sag_textbuf_line_start(tb, line));
    c->goal_col = (GCol){0U};
}

void sag_cursor_line_end(const TextBuf *tb, Cursor *c)
{
    LineNo line;

    cursor_require(tb, c);
    line = sag_textbuf_line_of(tb, c->pos);
    cursor_set_pos(c, cursor_line_end(tb, line));
    c->goal_col = (GCol){SAG_GCOL_EOL};
}

void sag_cursor_buf_home(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_pos(c, BYTEOFF(0U));
    c->goal_col = (GCol){0U};
}

void sag_cursor_buf_end(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_pos(c, BYTEOFF(sag_textbuf_len(tb)));
    c->goal_col = (GCol){SAG_GCOL_EOL};
}

static ByteOff cursor_clamp_off(const TextBuf *tb, ByteOff pos)
{
    u64 len = sag_textbuf_len(tb);

    if (pos.v > len)
        pos = BYTEOFF(len);
    if (!sag_is_grapheme_boundary(tb, pos))
        pos = sag_grapheme_prev(tb, pos);
    return pos;
}

void sag_cursor_clamp(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    c->pos = cursor_clamp_off(tb, c->pos);
    c->anchor = cursor_clamp_off(tb, c->anchor);
}
