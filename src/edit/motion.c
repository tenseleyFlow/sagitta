#include "edit/motion.h"

#include "edit/ed.h"
#include "ui/viewport.h"
#include "unicode/coords.h"
#include "unicode/wordbreak.h"

static ByteOff clamp_pos(const TextBuf *tb, ByteOff p)
{
    u64 len = yew_textbuf_len(tb);

    if (p.v > len)
        p = BYTEOFF(len);
    if (!yew_is_grapheme_boundary(tb, p))
        p = yew_grapheme_prev(tb, p);
    return p;
}

static ByteOff line_content_end(const TextBuf *tb, LineNo line)
{
    Span span = yew_textbuf_line_span(tb, line);
    ByteOff end = BYTEOFF(span.hi);

    if (line.v + 1U < yew_textbuf_line_count(tb))
        end = yew_grapheme_prev_boundary(tb, end);
    return end;
}

static ByteOff line_at_col(const TextBuf *tb, LineNo line, GCol goal)
{
    Span span = yew_textbuf_line_span(tb, line);

    return yew_gcol_to_off(tb, span, goal);
}

static GCol line_goal(const UnitCtx *u, ByteOff p)
{
    if (u->win != NULL && u->win->cs.curs.len != 0U &&
        (size_t)u->win->cs.primary < u->win->cs.curs.len) {
        const Cursor *cursor =
            &u->win->cs.curs.data[u->win->cs.primary];

        if (cursor->pos.v == p.v)
            return cursor->goal_col;
    }
    return yew_off_to_gcol(u->tb,
                           yew_textbuf_line_span(
                               u->tb, yew_textbuf_line_of(u->tb, p)),
                           p);
}

static u64 line_step(const UnitCtx *u, bool alt)
{
    u64 rows;

    if (!alt)
        return 1U;
    rows = u->win == NULL ? 1U : (u64)u->win->vp.rows / 2U;
    return rows == 0U ? 1U : rows;
}

static ByteOff line_next(UnitCtx *u, ByteOff p, bool alt)
{
    u64 count;
    LineNo line;
    GCol goal;
    u64 target;

    p = clamp_pos(u->tb, p);
    if (u->win != NULL && u->win->vp.wrap) {
        ByteOff target = yew_vp_display_target(
            u->win, p, (i32)line_step(u, alt));

        return target.v > p.v ? target
                              : BYTEOFF(yew_textbuf_len(u->tb));
    }
    count = yew_textbuf_line_count(u->tb);
    line = yew_textbuf_line_of(u->tb, p);
    if (line.v + 1U >= count)
        return BYTEOFF(yew_textbuf_len(u->tb));
    goal = line_goal(u, p);
    target = line.v + line_step(u, alt);
    if (target >= count)
        target = count - 1U;
    return line_at_col(u->tb, LINENO(target), goal);
}

static ByteOff line_prev(UnitCtx *u, ByteOff p, bool alt)
{
    LineNo line;
    GCol goal;
    u64 step;

    p = clamp_pos(u->tb, p);
    if (p.v == 0U)
        return p;
    if (u->win != NULL && u->win->vp.wrap) {
        ByteOff target = yew_vp_display_target(
            u->win, p, -(i32)line_step(u, alt));

        return target.v < p.v ? target : BYTEOFF(0U);
    }
    line = yew_textbuf_line_of(u->tb, p);
    if (line.v == 0U)
        return BYTEOFF(0U);
    goal = line_goal(u, p);
    step = line_step(u, alt);
    return line_at_col(u->tb, LINENO(line.v > step ? line.v - step : 0U),
                       goal);
}

static bool cluster_is_blank(const TextBuf *tb, Span line, ByteOff p)
{
    YewTextCluster cluster;

    if (!yew_text_cluster_next(tb, line, p, &cluster))
        return false;
    return yew_unicode_is_white_space(cluster.base_cp) &&
           cluster.base_cp != (u32)'\n' && cluster.base_cp != (u32)'\r';
}

static ByteOff line_home(UnitCtx *u, ByteOff p, bool alt)
{
    LineNo line;
    Span span;
    ByteOff at;
    ByteOff end;

    p = clamp_pos(u->tb, p);
    line = yew_textbuf_line_of(u->tb, p);
    span = yew_textbuf_line_span(u->tb, line);
    at = BYTEOFF(span.lo);
    if (!alt)
        return at;
    end = line_content_end(u->tb, line);
    while (at.v < end.v && cluster_is_blank(u->tb, span, at))
        at = yew_grapheme_next_boundary(u->tb, at);
    return at.v <= p.v ? at : BYTEOFF(span.lo);
}

static ByteOff line_end(UnitCtx *u, ByteOff p, bool alt)
{
    LineNo line;
    Span span;
    ByteOff end;
    ByteOff at;
    ByteOff last;

    p = clamp_pos(u->tb, p);
    line = yew_textbuf_line_of(u->tb, p);
    end = line_content_end(u->tb, line);
    if (!alt)
        return end;
    span = yew_textbuf_line_span(u->tb, line);
    at = BYTEOFF(span.lo);
    last = at;
    while (at.v < end.v) {
        ByteOff next = yew_grapheme_next_boundary(u->tb, at);

        if (!cluster_is_blank(u->tb, span, at))
            last = next;
        at = next;
    }
    return last.v >= p.v ? last : end;
}

static Span line_span(UnitCtx *u, ByteOff p, bool alt)
{
    return (Span){line_home(u, p, alt).v, line_end(u, p, alt).v};
}

static ByteOff char_next(UnitCtx *u, ByteOff p, bool alt)
{
    /* UnitOps returns cursor positions, and every cursor position must be an
     * extended-grapheme boundary.  The alternate's decoded-codepoint view is
     * therefore projected onto legal cursor positions: valid codepoints
     * inside one grapheme are skipped, while each escaped invalid byte remains
     * its own codepoint and singleton grapheme. */
    (void)alt;
    return yew_grapheme_next_boundary(u->tb, clamp_pos(u->tb, p));
}

static ByteOff char_prev(UnitCtx *u, ByteOff p, bool alt)
{
    (void)alt;
    return yew_grapheme_prev_boundary(u->tb, clamp_pos(u->tb, p));
}

static Span char_at(UnitCtx *u, ByteOff p, bool alt)
{
    u64 len = yew_textbuf_len(u->tb);
    Span span;

    (void)alt;
    p = clamp_pos(u->tb, p);
    if (p.v == len && p.v != 0U) {
        span.hi = p.v;
        span.lo = yew_grapheme_prev_boundary(u->tb, p).v;
    } else {
        span.lo = p.v;
        span.hi = yew_grapheme_next_boundary(u->tb, p).v;
    }
    return span;
}

static ByteOff char_home(UnitCtx *u, ByteOff p, bool alt)
{
    return BYTEOFF(char_at(u, p, alt).lo);
}

static ByteOff char_end(UnitCtx *u, ByteOff p, bool alt)
{
    return BYTEOFF(char_at(u, p, alt).hi);
}

const UnitOps yew_unit_line = {
    "line", line_next, line_prev, line_home, line_end, line_span,
};

const UnitOps yew_unit_char = {
    "char", char_next, char_prev, char_home, char_end, char_at,
};

const UnitOps *yew_unit_of_mode(Mode mode)
{
    switch (mode) {
    case YEW_MODE_L:
        return &yew_unit_line;
    case YEW_MODE_W:
        return &yew_unit_word;
    case YEW_MODE_B:
        return &yew_unit_block;
    case YEW_MODE_I:
        return &yew_unit_char;
    case YEW_MODE_H:
    case YEW_MODE_E:
    case YEW_MODE_F:
    case YEW_MODE__N:
        return NULL;
    }
    return NULL;
}

void yew_selstack_clear(Win *win)
{
    size_t i;

    if (win == NULL)
        return;
    for (i = 0U; i < win->cs.selstacks.len; i++)
        win->cs.selstacks.data[i].n = 0U;
}
