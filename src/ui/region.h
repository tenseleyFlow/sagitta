#ifndef SAG_UI_REGION_H
#define SAG_UI_REGION_H

/*
 * Sprint 22 §6: the clickable-region registry.
 *
 * ONE SOURCE OF TRUTH for mapping screen cells to meaning.  Render
 * passes populate it; the input path queries it; nothing else may map a
 * cell to a thing.
 *
 * The rule exists because the alternative is deriving placement twice —
 * once while drawing and once while hit-testing — and the two
 * derivations drift the moment text stops being one cell per byte.  A
 * multibyte filename in a tab strip shifts every click to its right,
 * and the bug looks like "clicks are off by a bit" rather than like the
 * duplicated arithmetic it is.  So a renderer registers its region in
 * the same statement block that draws it, using the SAME Rect.
 */

#include "ui/layout.h"
#include "util/base.h"

typedef enum {
    SAG_REGION_NONE = 0,
    SAG_REGION_PANE,        /* payload: leaf table index            */
    SAG_REGION_PANE_BORDER, /* payload: split table index           */
    /*
     * Sprint 23/24.  The payload sign convention is documented HERE,
     * now, although groups do not land until Sprint 24: the click
     * router reads it and the renderer writes it, so inventing it twice
     * in two sprints is exactly how the two ends disagree.
     *
     *   payload >= 0   tab index
     *   payload <  0   group id, negated
     */
    SAG_REGION_TAB,
    SAG_REGION_TAB_SCROLL, /* payload: +1 scroll right, -1 left     */
    /*
     * Inert: owns its rectangle and means nothing.  A dialog registers
     * one so clicks on it never fall through to the pane underneath.
     */
    SAG_REGION_BLOCK
} RegionKind;

typedef struct Region {
    RegionKind kind;
    Rect rect;
    i32 payload;
} Region;

enum {
    /* A frame with this many regions is a rendering bug, not a growth
     * problem, so the table is fixed and overflow drops. */
    SAG_REGION_MAX = 256
};

/* Clears the table.  Called at frame BEGIN — see region.c for why the
 * obvious alternative is wrong. */
void sag_region_frame_begin(void);
void sag_region_add(RegionKind kind, Rect rect, i32 payload);
/* Last-added wins, so overlays drawn after the document shadow it. */
Region sag_region_hit(u16 x, u16 y);
u32 sag_region_count(void);

/*
 * DoD 10: in a debug build, querying between frame_begin and the first
 * add is a bug — it means the input path ran against a frame that never
 * drew anything.  Returns the number of times that happened.
 */
u32 sag_region_empty_queries(void);

#endif
