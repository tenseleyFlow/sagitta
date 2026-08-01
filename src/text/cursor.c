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

static void cursor_cache_col(Cursor *c, GCol col)
{
    c->motion_col = col;
    c->motion_col_valid = 1U;
}

static void cursor_forget_col(Cursor *c)
{
    c->motion_col_valid = 0U;
}

static void cursor_update_goal(const TextBuf *tb, Cursor *c)
{
    LineNo line = sag_textbuf_line_of(tb, c->pos);
    Span span = sag_textbuf_line_span(tb, line);

    c->goal_col = sag_off_to_gcol(tb, span, c->pos);
    cursor_cache_col(c, c->goal_col);
}

static void cursor_set_horizontal(const TextBuf *tb, Cursor *c,
                                  ByteOff next, bool right)
{
    ByteOff previous = c->pos;
    LineNo old_line = sag_textbuf_line_of(tb, previous);
    GCol old_actual = c->motion_col_valid == 1U ? c->motion_col : c->goal_col;
    LineNo new_line;

    cursor_set_pos(c, next);
    new_line = sag_textbuf_line_of(tb, c->pos);
    if (new_line.v != old_line.v) {
        /* Crossing right lands at the next line's column zero.  Crossing
         * left lands on the previous line's EOL cluster, the same sticky
         * target produced by line_end. */
        c->goal_col = (GCol){right ? 0U : SAG_GCOL_EOL};
        if (right)
            cursor_cache_col(c, c->goal_col);
        else
            cursor_forget_col(c);
        return;
    }
    if (c->pos.v == previous.v)
        return;
    if (old_actual.v == SAG_GCOL_EOL) {
        cursor_update_goal(tb, c);
        return;
    }
    if (right) {
        if (old_actual.v + 1U == SAG_GCOL_EOL)
            SAG_BUG("cursor grapheme column overflow");
        c->goal_col.v = old_actual.v + 1U;
    } else if (old_actual.v != 0U) {
        c->goal_col.v = old_actual.v - 1U;
    } else {
        /* A finite zero goal away from line start can only have arrived
         * from external cursor construction; repair it once. */
        cursor_update_goal(tb, c);
        return;
    }
    cursor_cache_col(c, c->goal_col);
}

static ByteOff cursor_line_end(const TextBuf *tb, LineNo line)
{
    Span span = sag_textbuf_line_span(tb, line);
    ByteOff end = BYTEOFF(span.hi);

    if (line.v + 1U < sag_textbuf_line_count(tb)) {
        /* Every non-final line span ends in LF.  GB3 makes a preceding CR
         * part of the same cluster, so one previous step strips CRLF too. */
        end = sag_grapheme_prev_boundary(tb, end);
    }
    return end;
}

void sag_cursor_left(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_horizontal(tb, c, sag_grapheme_prev_boundary(tb, c->pos),
                          false);
}

void sag_cursor_right(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_horizontal(tb, c, sag_grapheme_next_boundary(tb, c->pos),
                          true);
}

void sag_cursor_up(const TextBuf *tb, Cursor *c)
{
    LineNo line;
    Span span;
    GCol actual;

    cursor_require(tb, c);
    line = sag_textbuf_line_of(tb, c->pos);
    if (line.v == 0U)
        return;
    span = sag_textbuf_line_span(tb, LINENO(line.v - 1U));
    cursor_set_pos(c, sag_gcol_to_off_resolved(tb, span, c->goal_col,
                                               &actual));
    cursor_cache_col(c, actual);
}

void sag_cursor_down(const TextBuf *tb, Cursor *c)
{
    LineNo line;
    Span span;
    GCol actual;

    cursor_require(tb, c);
    line = sag_textbuf_line_of(tb, c->pos);
    if (line.v + 1U >= sag_textbuf_line_count(tb))
        return;
    span = sag_textbuf_line_span(tb, LINENO(line.v + 1U));
    cursor_set_pos(c, sag_gcol_to_off_resolved(tb, span, c->goal_col,
                                               &actual));
    cursor_cache_col(c, actual);
}

void sag_cursor_line_home(const TextBuf *tb, Cursor *c)
{
    LineNo line;

    cursor_require(tb, c);
    line = sag_textbuf_line_of(tb, c->pos);
    cursor_set_pos(c, sag_textbuf_line_start(tb, line));
    c->goal_col = (GCol){0U};
    cursor_cache_col(c, c->goal_col);
}

void sag_cursor_line_end(const TextBuf *tb, Cursor *c)
{
    LineNo line;
    Span span;

    cursor_require(tb, c);
    line = sag_textbuf_line_of(tb, c->pos);
    span = sag_textbuf_line_span(tb, line);
    cursor_set_pos(c, cursor_line_end(tb, line));
    c->goal_col = (GCol){SAG_GCOL_EOL};
    cursor_cache_col(c, sag_off_to_gcol(tb, span, c->pos));
}

void sag_cursor_buf_home(const TextBuf *tb, Cursor *c)
{
    cursor_require(tb, c);
    cursor_set_pos(c, BYTEOFF(0U));
    c->goal_col = (GCol){0U};
    cursor_cache_col(c, c->goal_col);
}

void sag_cursor_buf_end(const TextBuf *tb, Cursor *c)
{
    LineNo line;
    Span span;

    cursor_require(tb, c);
    cursor_set_pos(c, BYTEOFF(sag_textbuf_len(tb)));
    c->goal_col = (GCol){SAG_GCOL_EOL};
    line = sag_textbuf_line_of(tb, c->pos);
    span = sag_textbuf_line_span(tb, line);
    cursor_cache_col(c, sag_off_to_gcol(tb, span, c->pos));
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
    cursor_forget_col(c);
}
