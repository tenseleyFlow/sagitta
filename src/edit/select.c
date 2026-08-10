#include "edit/select.h"

#include <limits.h>

#include "edit/ed.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "unicode/coords.h"
#include "util/log.h"

typedef struct {
    LineNo first;
    LineNo last;
} SelRows;

static const TextBuf *selection_tb(const Win *w, const Cursor *c)
{
    const TextBuf *tb;
    u64 len;

    if (w == NULL || c == NULL)
        YEW_BUG("selection geometry: NULL window or cursor");
    if (w->buf == NULL || w->buf->tb == NULL)
        YEW_BUG("selection geometry: window has no text buffer");
    tb = w->buf->tb;
    len = yew_textbuf_len(tb);
    if (c->pos.v > len || c->anchor.v > len)
        YEW_BUG("selection geometry: cursor outside text buffer");
    return tb;
}

static u32 selection_tabwidth(const Win *w)
{
    return w->buf->tabwidth != 0U ? w->buf->tabwidth : YEW_VP_TABWIDTH;
}

static SelRows selection_rows(const TextBuf *tb, const Cursor *c)
{
    LineNo pos = yew_textbuf_line_of(tb, c->pos);
    LineNo anchor = yew_textbuf_line_of(tb, c->anchor);
    SelRows rows;

    if (pos.v < anchor.v) {
        rows.first = pos;
        rows.last = anchor;
    } else {
        rows.first = anchor;
        rows.last = pos;
    }
    return rows;
}

static void rect_columns(const Win *w, const Cursor *c, const TextBuf *tb,
                         CCol *out0, CCol *out1)
{
    LineNo pos_line = yew_textbuf_line_of(tb, c->pos);
    LineNo anchor_line = yew_textbuf_line_of(tb, c->anchor);
    Span pos_span = yew_textbuf_line_span(tb, pos_line);
    Span anchor_span = yew_textbuf_line_span(tb, anchor_line);
    u32 tabwidth = selection_tabwidth(w);
    CCol pos_col = yew_off_to_ccol(tb, pos_span, c->pos, tabwidth);
    CCol anchor_col = yew_off_to_ccol(tb, anchor_span, c->anchor, tabwidth);

    if (pos_col.v < anchor_col.v) {
        *out0 = pos_col;
        *out1 = anchor_col;
    } else {
        *out0 = anchor_col;
        *out1 = pos_col;
    }
}

static ByteOff line_content_end(const TextBuf *tb, Span line, u32 tabwidth)
{
    return yew_ccol_to_off_padded(tb, line, (CCol){UINT64_MAX}, tabwidth);
}

Span yew_sel_span(const Win *w, const Cursor *c)
{
    const TextBuf *tb = selection_tb(w, c);
    u64 lo = c->pos.v < c->anchor.v ? c->pos.v : c->anchor.v;
    u64 hi = c->pos.v < c->anchor.v ? c->anchor.v : c->pos.v;

    if (w->h.kind == YEW_SEL_CHAR)
        return (Span){lo, hi};
    if (w->h.kind == YEW_SEL_LINE) {
        LineNo first = yew_textbuf_line_of(tb, BYTEOFF(lo));
        LineNo last = yew_textbuf_line_of(tb, BYTEOFF(hi));

        return (Span){yew_textbuf_line_start(tb, first).v,
                      yew_textbuf_line_span(tb, last).hi};
    }
    if (w->h.kind == YEW_SEL_RECT)
        YEW_BUG("yew_sel_span: rectangular selection requires row geometry");
    YEW_BUG("yew_sel_span: invalid selection kind");
}

u32 yew_sel_rows(const Win *w, const Cursor *c)
{
    const TextBuf *tb = selection_tb(w, c);
    SelRows rows = selection_rows(tb, c);
    u64 count = rows.last.v - rows.first.v + 1U;

    if (count > UINT32_MAX)
        YEW_BUG("yew_sel_rows: selection has too many rows");
    return (u32)count;
}

bool yew_sel_rect_row(const Win *w, const Cursor *c, LineNo line,
                      Span *out, CCol *c0, CCol *c1)
{
    const TextBuf *tb = selection_tb(w, c);
    SelRows rows;
    Span line_span;
    ByteOff content_end;
    ByteOff lo;
    ByteOff hi;
    CCol end_col;
    CCol hi_landed;
    u32 tabwidth;

    if (out == NULL || c0 == NULL || c1 == NULL)
        YEW_BUG("yew_sel_rect_row: missing output");
    if (w->h.kind != YEW_SEL_RECT)
        YEW_BUG("yew_sel_rect_row: selection is not rectangular");
    rows = selection_rows(tb, c);
    if (line.v < rows.first.v || line.v > rows.last.v)
        return false;

    rect_columns(w, c, tb, c0, c1);
    line_span = yew_textbuf_line_span(tb, line);
    tabwidth = selection_tabwidth(w);
    content_end = line_content_end(tb, line_span, tabwidth);
    end_col = yew_off_to_ccol(tb, line_span, content_end, tabwidth);

    /* A short row contributes an empty span. Keep the requested columns in
     * c0/c1 so a rectangular insert can pad to the original visual edge. */
    if (end_col.v <= c0->v) {
        *out = (Span){content_end.v, content_end.v};
        return true;
    }

    lo = yew_ccol_to_off_padded(tb, line_span, *c0, tabwidth);
    if (c0->v == c1->v) {
        *out = (Span){lo.v, lo.v};
        return true;
    }

    hi = yew_ccol_to_off_padded(tb, line_span, *c1, tabwidth);
    hi_landed = yew_off_to_ccol(tb, line_span, hi, tabwidth);
    if (hi.v < content_end.v && hi_landed.v < c1->v)
        hi = yew_grapheme_next_boundary(tb, hi);
    if (hi.v > content_end.v)
        hi = content_end;
    *out = (Span){lo.v, hi.v};
    return true;
}

void yew_sel_rect_spans(const Win *w, const Cursor *c, YewSelSpanVec *out)
{
    const TextBuf *tb = selection_tb(w, c);
    SelRows rows;
    CCol c0;
    CCol c1;
    u64 line;

    if (out == NULL)
        YEW_BUG("yew_sel_rect_spans: NULL vector");
    if (w->h.kind != YEW_SEL_RECT)
        YEW_BUG("yew_sel_rect_spans: selection is not rectangular");
    rows = selection_rows(tb, c);
    out->len = 0U;
    YewSelSpanVec_reserve(out, yew_sel_rows(w, c));
    for (line = rows.first.v;; line++) {
        Span span;

        if (!yew_sel_rect_row(w, c, LINENO(line), &span, &c0, &c1))
            YEW_BUG("yew_sel_rect_spans: covered row rejected");
        YewSelSpanVec_push(out, span);
        if (line == rows.last.v)
            break;
    }
}
