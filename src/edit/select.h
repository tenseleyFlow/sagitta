#ifndef YEW_EDIT_SELECT_H
#define YEW_EDIT_SELECT_H

#include "edit/motion.h"
#include "text/cursor.h"
#include "unicode/coords.h"
#include "util/vec.h"

typedef struct Win Win;

typedef enum {
    YEW_SEL_CHAR,
    YEW_SEL_LINE,
    YEW_SEL_RECT
} SelKind;

typedef struct HState {
    Mode from;
    const UnitOps *unit;
    SelKind kind;
    bool sticky;
} HState;

VEC_DECL(YewSelSpanVec, Span);

/* CHAR and LINE selections. RECT geometry is row-shaped and must use the
 * row helpers below. */
Span yew_sel_span(const Win *w, const Cursor *c);

/* `c0` and `c1` are the requested visual-cell edges shared by every row.
 * `out` is clipped to line content and widened to whole clusters when an
 * edge lands inside a wide glyph or tab. */
bool yew_sel_rect_row(const Win *w, const Cursor *c, LineNo line,
                      Span *out, CCol *c0, CCol *c1);
u32 yew_sel_rows(const Win *w, const Cursor *c);

/* Replaces `out` with one span per selected row, including empty spans.
 * The caller owns the reusable vector and releases it with
 * YewSelSpanVec_free. */
void yew_sel_rect_spans(const Win *w, const Cursor *c, YewSelSpanVec *out);

#endif
