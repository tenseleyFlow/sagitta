#ifndef SAG_UI_MOUSE_H
#define SAG_UI_MOUSE_H

/*
 * Sprint 27 §1: THE mouse router.
 *
 * Sprint 4 decoded mouse events and threw them away; Sprint 22 built the
 * region registry; Sprint 18.5 joined the two for the completion menu
 * alone.  This file is the join for everything else, and it is the ONLY
 * place a mouse event turns into an action (DoD 2).
 *
 * THE LAW: THE PRESS CAPTURES ITS REGION.  Motion and release never
 * re-resolve identity.  `press_rgn` is stored when the button goes down
 * and consulted for the rest of the gesture, because the tab strip
 * scrolls, an async job can close a tab, and every index above a closed
 * tab renumbers — so a region re-queried at release can name a
 * different thing than the one the user aimed at.  Identity travels as
 * `tab_id` / `gid` / path, never as an index (Sprint 23's law, applied
 * in the input path).
 *
 * ARMING, NOT DRAGGING.  A press sets SAG_MP_ARMED; the drag begins
 * only once the pointer leaves the pressed CELL.  A click that never
 * moves must stay a click, and nothing is drawn or targeted until then.
 *
 * WHEELS ARE IMPULSES.  A wheel event is dispatched and returns without
 * touching `phase` and without setting `held` — Sprint 4 pinned this:
 * wheel events have no release, so a drag state machine keyed on
 * press-without-release hangs on the first scroll.
 */

#include "edit/cmd.h"
#include "edit/motion.h"
#include "term/input.h"
#include "text/coords.h"
#include "ui/region.h"
#include "util/base.h"

typedef struct Ed Ed;

enum {
    /* A notch is exactly this many rows, forever: no acceleration, no
     * momentum, no fractional accumulation.  Deterministic (invariant
     * 5) and therefore testable. */
    SAG_WHEEL_ROWS = 3,
    /* Shift+wheel, when wrap is off. */
    SAG_WHEEL_COLS = 6,
    SAG_DRAG_DWELL_MS = 400,
    SAG_DRAG_SCROLL_MS = 120,
    SAG_CLICK_MULTI_MS = 400
};

typedef enum {
    SAG_MP_IDLE = 0,
    SAG_MP_ARMED,
    SAG_MP_DRAG_BORDER,
    SAG_MP_DRAG_TAB,
    SAG_MP_DRAG_GROUP,
    SAG_MP_DRAG_SEL
} MousePhase;

typedef struct MouseState {
    MousePhase phase;
    u8 held; /* button bitmask; the wheel never sets a bit */
    u16 press_x, press_y;
    Region press_rgn; /* CAPTURED at press — see the law above */

    /* Identity of what is being dragged; never an index. */
    u32 drag_tab_id;
    u32 drag_gid;
    /*
     * A change here cancels the drag outright.  The array is frozen for
     * the drag's lifetime, so a count that moved means something else
     * mutated it — an async job closing a file, a script — and the
     * target the user aimed at no longer means what it did.
     */
    u32 tab_count_at_press;

    /*
     * §4's preview.  NOTHING in Tabs.v changes until the drop: the drag
     * carries a target slot and the renderer draws the strip as if the
     * held entry were already there.
     */
    int drag_to_slot;
    bool drag_to_valid;
    /*
     * The pointer is past the last entry.  A separate flag rather than
     * "the last slot", because the two mean different things: the last
     * ENTRY is a reorder, the blank TAIL is "move to the end" — and the
     * tail is the only way to carry a tab out of a group when the group
     * is the sole row-1 entry, since there is nothing else to aim at.
     */
    bool drag_to_tail;
    /* Auto-scroll while held over a chevron, on a TIMER (§4).  The
     * pointer's last reported cell is kept because the timer fires when
     * no motion event has arrived — that is the whole point of it. */
    i64 autoscroll_ms;
    u16 at_x, at_y;

    /* Multi-click (§6). */
    i64 last_click_ms;
    u16 last_click_x, last_click_y;
    u8 click_n;

    /* Dwell over a group while dragging (§4). */
    i64 dwell_since_ms;
    u32 dwell_gid;
    u32 preview_gid; /* the member strip a dwell opened; 0 = none */

    /* Selection drag (§6). */
    const UnitOps *sel_unit;
    Span sel_anchor_span;
} MouseState;

void sag_mouse_init(MouseState *m);

/* THE entry point.  Every mouse event in the program arrives here. */
void sag_mouse_event(Ed *ed, const Key *k);

/*
 * Did Sprint 18.5's completion menu claim this event (and act on it)?
 *
 * Exposed because "the click was swallowed so it could not reach the
 * pane underneath" is a claim with no other observable: the handler
 * returns nothing, and a swallowed click by definition changes nothing
 * else.
 */
bool sag_mouse_claimed_by_menu(Ed *ed, Key key);

/*
 * Cancels any gesture in flight, restoring the state it started from
 * (border ratio, tab order, selection anchor).  Esc calls it; so does
 * Sprint 4's FOCUS_OUT — a drag whose release lands in another window
 * would otherwise hang forever with the button logically down.
 */
void sag_mouse_cancel(Ed *ed);

/* True while a gesture is armed or dragging.  The renderer asks so it
 * can draw the preview; Esc asks so it can cancel before any mode sees
 * the key. */
bool sag_mouse_gesture_active(const Ed *ed);

/*
 * §4: the drag's preview, for the strip renderer.  Returns false when
 * no tab/group drag is in flight.  `payload` is the held entry in the
 * row-1 payload convention (index, or −gid); `to_slot` is where it is
 * being drawn.
 */
bool sag_mouse_drag_preview(const Ed *ed, i32 *payload, int *to_slot);

/* §4: the group whose member strip a dwell has opened; 0 when none. */
u32 sag_mouse_preview_group(const Ed *ed);

/*
 * Called from the loop's timer path: dwell and auto-scroll are clocks,
 * not motion counts.  A fast pointer emits far more motion reports than
 * a slow one, and a strip that scrolled per report would fly past the
 * target.
 */
void sag_mouse_tick(Ed *ed, i64 now_ms);
/* When the router next needs the clock, or 0 when it does not. */
i64 sag_mouse_deadline(const Ed *ed, i64 now_ms);

/*
 * §5: the two menus.  Built here rather than in ui/ctxmenu.c because
 * WHICH rows exist and what they mean is editor policy, and ctxmenu.c
 * is deliberately ignorant of the editor.
 */
bool sag_mouse_open_tab_menu(Ed *ed, u32 tab_id, u16 x, u16 y);
bool sag_mouse_open_group_menu(Ed *ed, u32 gid, u16 x, u16 y);
/* The menu's keymap layer, and its draw.  Both no-ops when no menu is
 * open, so the caller does not have to ask first. */
bool sag_mouse_menu_key(Ed *ed, const Key *k);
void sag_mouse_menu_draw(Ed *ed);

/*
 * §9: the runtime toggle.  Sprint 36 owns the option model that makes
 * it persist; this is the session-lifetime half.
 */
bool sag_mouse_enabled(void);
void sag_mouse_set_enabled(bool on);

CmdStatus sag_ui_cmd_context_menu(CmdCtx *cx);
CmdStatus sag_mouse_cmd_enable(CmdCtx *cx);
CmdStatus sag_mouse_cmd_disable(CmdCtx *cx);

#endif
