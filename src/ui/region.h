#ifndef YEW_UI_REGION_H
#define YEW_UI_REGION_H

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
    YEW_REGION_NONE = 0,
    YEW_REGION_PANE,        /* payload: leaf table index            */
    YEW_REGION_PANE_BORDER, /* payload: split table index           */
    /*
     * Sprint 23/24.  The payload sign convention is documented HERE,
     * now, although groups do not land until Sprint 24: the click
     * router reads it and the renderer writes it, so inventing it twice
     * in two sprints is exactly how the two ends disagree.
     *
     *   payload >= 0   tab index
     *   payload <  0   group id, negated
     */
    YEW_REGION_TAB,
    YEW_REGION_TAB_SCROLL, /* payload: +1 scroll right, -1 left     */
    /*
     * Inert: owns its rectangle and means nothing.  A dialog registers
     * one so clicks on it never fall through to the pane underneath.
     */
    YEW_REGION_BLOCK,
    /* Sprint 24 §4: the group picker's name field and its listing rows
     * (payload = index into the listing). */
    YEW_REGION_GP_NAME,
    YEW_REGION_GP_ROW,
    /*
     * Sprint 18.5 §5.  Payload is the row's index within the menu's
     * CURRENT item vector -- not a stable id, because the menu has none
     * and the table is rebuilt every frame.  That is sound precisely
     * because a region can only be hit during the frame it was
     * registered in; see region.c on clearing at frame BEGIN.
     */
    YEW_REGION_MENU_ROW,
    /* Sprint 44: item index within the focused window's open completion. */
    YEW_REGION_COMPL_ROW,
    /*
     * Sprint 26 §5.  Payload is the item's PAYLOAD, never the row
     * index — the same law the picker's selection follows, so a click
     * lands on what was pointed at even if the list reordered between
     * the paint and the press.
     */
    YEW_REGION_PICK_ROW,
    /*
     * Sprint 27 §5.  Payload is the row's index within the OPEN menu,
     * which is sound for the same reason YEW_REGION_MENU_ROW's is: a
     * region can only be hit during the frame it was registered in, and
     * the menu's row list is fixed for its lifetime.
     *
     * What the payload is NOT is the menu's TARGET.  The menu captured
     * that at open time — see ui/ctxmenu.h — because the strip can
     * scroll and tabs can close under an open menu, and a row that
     * re-resolved its target from the cells beneath it would act on a
     * different file than the one the user right-clicked.
     */
    YEW_REGION_CTX_ROW,
    /* Sprint 52: payload is the flattened FUSS row for this frame. */
    YEW_REGION_FUSS_ROW
} RegionKind;

typedef struct Region {
    RegionKind kind;
    Rect rect;
    i32 payload;
} Region;

enum {
    /* A frame with this many regions is a rendering bug, not a growth
     * problem, so the table is fixed and overflow drops. */
    YEW_REGION_MAX = 256
};

/* Clears the table.  Called at frame BEGIN — see region.c for why the
 * obvious alternative is wrong. */
void yew_region_frame_begin(void);
void yew_region_add(RegionKind kind, Rect rect, i32 payload);
/* Last-added wins, so overlays drawn after the document shadow it. */
Region yew_region_hit(u16 x, u16 y);
u32 yew_region_count(void);

/*
 * Sprint 27 §5: freezes the table.  A hit-test while frozen is a BUG
 * and aborts — see region.c and ui/ctxmenu.h for the rule it enforces.
 * The context menu is the only caller; it wraps a row's action.
 */
void yew_region_freeze(bool on);
bool yew_region_frozen(void);

/*
 * DoD 10: in a debug build, querying between frame_begin and the first
 * add is a bug — it means the input path ran against a frame that never
 * drew anything.  Returns the number of times that happened.
 */
u32 yew_region_empty_queries(void);

#endif
