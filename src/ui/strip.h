#ifndef SAG_UI_STRIP_H
#define SAG_UI_STRIP_H

/*
 * Sprint 23 §3: the strip layout engine.
 *
 * ONE engine, reused verbatim by Sprint 24's row-2 member strip.  That
 * reuse is the whole point: two rows that each compute their own cell
 * spans will eventually disagree about where a click landed, and the
 * disagreement shows up as "clicking the wrong tab sometimes" rather
 * than as the duplicated arithmetic it is.
 *
 * The engine returns spans in CELLS.  The renderer draws with them and
 * registers regions with them — never recomputing a position from a
 * label's length, which is the Sprint 22 law and the reason a multibyte
 * tab name cannot shift every click to its right.
 */

#include "ui/layout.h"
#include "util/base.h"

enum {
    /* Labels clip here; a long path should not eat the strip. */
    SAG_STRIP_LABEL_CELLS = 24
};

typedef struct StripEntry {
    char label[64];
    i32 payload;
    bool dim;
} StripEntry;

typedef struct StripSpan {
    int idx;        /* index into the entry array */
    u16 col0, col1; /* half-open cell range */
} StripSpan;

/*
 * Lays entries left to right into `width` cells.
 *
 * `scroll` is in-out: the first visible entry.  The engine adjusts it
 * MINIMALLY to keep `active` visible — scrolling further than needed
 * makes the strip jump under the user for no reason.
 *
 * `more_left` / `more_right` report whether entries fall outside, so
 * the renderer can draw the `<` and `>N` indicators.
 */
void sag_strip_layout(const StripEntry *entries, int n, u16 width,
                      int active, int *scroll, StripSpan *spans,
                      int *n_spans, bool *more_left, bool *more_right);

/* Cells a label occupies once clipped — the same measurement the
 * layout used, exposed so a renderer never re-derives it. */
u16 sag_strip_label_cells(const char *label);

#endif
