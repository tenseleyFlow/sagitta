#define _POSIX_C_SOURCE 200809L

#include "ui/mouse.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/pane_cmds.h"
#include "text/piece.h"
#include "ui/cmdline.h"
#include "ui/groupnav.h"
#include "ui/grouppicker.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/picker.h"
#include "ui/tabs.h"
#include "ui/viewport.h"
#include "ui/win.h"

/* ---------------------------------------------------------------- */
/* Small predicates                                                 */
/* ---------------------------------------------------------------- */

static bool is_wheel(u8 button)
{
    return button == (u8)SAG_MB_WHEEL_UP ||
           button == (u8)SAG_MB_WHEEL_DOWN ||
           button == (u8)SAG_MB_WHEEL_LEFT ||
           button == (u8)SAG_MB_WHEEL_RIGHT;
}

static i32 wheel_dir(u8 button)
{
    return (button == (u8)SAG_MB_WHEEL_UP ||
            button == (u8)SAG_MB_WHEEL_LEFT)
               ? -1
               : 1;
}

void sag_mouse_init(MouseState *m)
{
    if (m == NULL)
        return;
    (void)memset(m, 0, sizeof(*m));
    m->drag_to_slot = -1;
}

bool sag_mouse_gesture_active(const Ed *ed)
{
    return ed != NULL && ed->mouse.phase != SAG_MP_IDLE;
}

/* ---------------------------------------------------------------- */
/* Sprint 18.5 §8: the completion menu's first refusal              */
/* ---------------------------------------------------------------- */

/*
 * Gated on cmdline.active because SAG_REGION_BLOCK is not the menu's
 * alone — Sprint 24's group picker registers one over its own rectangle
 * for the same swallow-the-gap reason.  The two overlays are never open
 * at once, so the prompt's own state is what disambiguates them.
 *
 * Sprint 27 moved this out of ed.c: DoD 2 is that no mouse event
 * becomes an action anywhere but this file.
 */
bool sag_mouse_claimed_by_menu(Ed *ed, Key key)
{
    Region hit;

    if (ed == NULL || key.kind != (u16)SAG_EV_MOUSE || !ed->cmdline.active)
        return false;
    hit = sag_region_hit(key.col, key.row);
    if (hit.kind != SAG_REGION_MENU_ROW && hit.kind != SAG_REGION_BLOCK)
        return false;
    if (key.button == (u8)SAG_MB_WHEEL_UP ||
        key.button == (u8)SAG_MB_WHEEL_DOWN) {
        /* Wheeling over the menu scrolls the LIST and deliberately does
         * not move the selection: looking is not choosing. */
        (void)sag_cmdline_menu_scroll(
            ed, key.button == (u8)SAG_MB_WHEEL_UP ? -SAG_WHEEL_ROWS
                                                  : SAG_WHEEL_ROWS);
        return true;
    }
    if (key.button != (u8)SAG_MB_LEFT)
        return false;
    /* Swallow the release of a press we handled, so it cannot fall
     * through to whatever is underneath. */
    if (key.ev != SAG_KEY_PRESS)
        return true;
    if (hit.kind == SAG_REGION_MENU_ROW)
        (void)sag_cmdline_menu_click(ed, hit.payload);
    /* A BLOCK hit is inert by construction: it exists so a click on a
     * gap in the menu does not land in the pane underneath it. */
    return true;
}

/* ---------------------------------------------------------------- */
/* §3: the wheel                                                    */
/* ---------------------------------------------------------------- */

/*
 * How far right the view may travel.  There is no global "longest
 * line" — computing one would be a full-buffer scan per notch — so the
 * bound is the widest line CURRENTLY IN VIEW.  Scrolling into empty
 * space past the end of every visible line is the one horizontal-scroll
 * misbehaviour users notice, because the text simply vanishes and no
 * key brings it back except Home.
 */
static u64 hscroll_limit(Win *w)
{
    TextBuf *tb;
    u64 widest = 0U;
    u16 row;

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL)
        return 0U;
    tb = w->buf->tb;
    for (row = 0U; row < w->vp.rows; row++) {
        LineNo line;
        u32 sub = 0U;
        Span span;
        CCol end;

        if (!sag_vp_line_of_row(w, row, &line, &sub))
            break;
        span = sag_textbuf_line_span(tb, line);
        end = sag_off_to_ccol(tb, span, BYTEOFF(span.hi), SAG_VP_TABWIDTH);
        if (end.v > widest)
            widest = end.v;
    }
    return widest;
}

/*
 * Shift+wheel.  A no-op when wrap is on — there is nowhere to go — and
 * silent, because a wheel is not a command that can fail and a message
 * per notch would be noise.
 */
static void wheel_horizontal(Win *w, i32 cells)
{
    i64 want;
    u64 limit;

    if (w == NULL || w->vp.wrap)
        return;
    want = (i64)w->vp.left.v + cells;
    if (want < 0)
        want = 0;
    limit = hscroll_limit(w);
    if ((u64)want > limit)
        want = (i64)limit;
    w->vp.left = (CCol){(u64)want};
}

/*
 * Wheel over a pane scrolls THAT pane, whether or not it is focused,
 * and moves neither the focus nor the cursor.
 *
 * Two panes side by side exist to be compared.  If the wheel followed
 * focus you would have to click the other pane first — and clicking
 * moves the cursor, so reading a second file would mutate your editing
 * position.  A scroll must never change document state.
 *
 * PITFALL, and the reason sag_vp_follow appears nowhere in this file:
 * follow drags the viewport back to satisfy scrolloff around the
 * cursor, which is exactly where it already was.  Called after a wheel
 * it would make an unfocused pane appear not to scroll at all, and a
 * focused one snap back.  sag_vp_scroll already clamps.  Follow resumes
 * at the next cursor motion, as always.
 */
static void wheel_pane(Ed *ed, const Region *hit, const Key *k)
{
    Pane *leaf = sag_pane_leaf_by_index(ed, hit->payload);
    Win *w = leaf != NULL ? leaf->win : NULL;
    i32 dir = wheel_dir(k->button);

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL)
        return;
    if ((k->mods & (u16)SAG_MOD_SHIFT) != 0U ||
        k->button == (u8)SAG_MB_WHEEL_LEFT ||
        k->button == (u8)SAG_MB_WHEEL_RIGHT)
        wheel_horizontal(w, dir * SAG_WHEEL_COLS);
    else
        sag_vp_scroll(w, dir * SAG_WHEEL_ROWS);
    ed->full_damage = true;
}

/* Row 1 or row 2?  The strip rect is registered state, not a
 * re-derivation of the layout — see region.h's law. */
static bool region_is_member_row(const Ed *ed, const Region *hit)
{
    return ed->tab_strip_rect.h >= 2U &&
           hit->rect.y == (u16)(ed->tab_strip_rect.y + 1U);
}

static void strip_scroll(Ed *ed, bool row2, i32 delta)
{
    int *scroll = row2 ? &ed->tabs.member_scroll : &ed->tabs.scroll;
    int limit = (int)ed->tabs.v.len;
    int to = *scroll + delta;

    if (row2)
        limit = sag_group_member_count(ed, sag_active_group_id(ed));
    if (to < 0)
        to = 0;
    if (to >= limit)
        to = limit > 0 ? limit - 1 : 0;
    if (*scroll == to)
        return;
    *scroll = to;
    ed->full_damage = true;
}

static void mouse_wheel(Ed *ed, const Key *k)
{
    Region hit;

    /*
     * Ctrl+wheel is DELIBERATELY unbound: it is the terminal emulator's
     * font-size gesture, and stealing it would fight the host
     * application over a key the user does not think of as ours.
     */
    if ((k->mods & (u16)SAG_MOD_CTRL) != 0U)
        return;
    if (sag_mouse_claimed_by_menu(ed, *k))
        return;
    hit = sag_region_hit(k->col, k->row);
    switch (hit.kind) {
    case SAG_REGION_PANE:
        wheel_pane(ed, &hit, k);
        break;
    case SAG_REGION_TAB:
        strip_scroll(ed, region_is_member_row(ed, &hit), wheel_dir(k->button));
        break;
    case SAG_REGION_TAB_SCROLL:
        strip_scroll(ed, hit.payload == 2 || hit.payload == -2,
                     wheel_dir(k->button));
        break;
    case SAG_REGION_PICK_ROW:
        sag_picker_scroll(ed, wheel_dir(k->button) * SAG_WHEEL_ROWS);
        break;
    case SAG_REGION_GP_ROW:
    case SAG_REGION_GP_NAME:
        sag_gp_scroll(ed, wheel_dir(k->button) * SAG_WHEEL_ROWS);
        break;
    /*
     * A wheel over a pane BORDER, a context menu, or a modal's own
     * BLOCK does nothing.  Swallowed rather than passed through: the
     * whole point of BLOCK is that the document underneath is inert.
     */
    case SAG_REGION_PANE_BORDER:
    case SAG_REGION_CTX_ROW:
    case SAG_REGION_BLOCK:
    case SAG_REGION_MENU_ROW:
    case SAG_REGION_NONE:
    default:
        break;
    }
}

/* ---------------------------------------------------------------- */
/* §2: press                                                        */
/* ---------------------------------------------------------------- */

static void press_pane(Ed *ed, const Region *hit, const Key *k)
{
    Pane *leaf = sag_pane_leaf_by_index(ed, hit->payload);

    if (leaf == NULL)
        return;
    (void)sag_pane_click(ed, k->col, k->row);
    /*
     * The anchor for a drag-select is the grapheme just clicked.  It is
     * read back from the cursor rather than recomputed, so the
     * selection can never start one cluster away from where the caret
     * visibly landed.
     */
    ed->mouse.sel_unit = &sag_unit_char;
    if (leaf->win != NULL && leaf->win->cs.curs.len != 0U) {
        ByteOff at = leaf->win->cs.curs.data[leaf->win->cs.primary].pos;

        ed->mouse.sel_anchor_span = (Span){at.v, at.v};
    }
}

static void press_border(Ed *ed, const Region *hit)
{
    Pane *split = sag_pane_split_by_index(ed, hit->payload);

    if (split == NULL)
        return;
    sag_pane_drag_begin(ed, split, ed->mouse.press_x, ed->mouse.press_y);
    ed->mouse.phase = SAG_MP_DRAG_BORDER;
}

/*
 * A press on a tab entry ARMS a drag and does nothing else visible.
 * The switch happens at release, when we know the gesture was a click:
 * switching on press and then dragging would leave a tab activated that
 * the user only meant to move.
 */
static void press_tab(Ed *ed, const Region *hit)
{
    ed->mouse.tab_count_at_press = sag_tab_count(ed);
    if (hit->payload < 0) {
        ed->mouse.drag_gid = (u32)(-hit->payload);
        ed->mouse.drag_tab_id = 0U;
    } else {
        Tab *t = sag_tab_at(ed, hit->payload);

        ed->mouse.drag_gid = 0U;
        ed->mouse.drag_tab_id = t != NULL ? t->tab_id : 0U;
    }
}

static void press_pick_row(Ed *ed, const Region *hit)
{
    /* Selecting is not accepting.  The accept happens at release, and
     * only when the release lands on the same row — a press that slid
     * off the row was a mis-aim, not a choice. */
    sag_picker_select_payload(ed, hit->payload);
}

static void mouse_press(Ed *ed, const Key *k)
{
    MouseState *m = &ed->mouse;
    Region hit = sag_region_hit(k->col, k->row);

    if (k->button == (u8)SAG_MB_RIGHT) {
        /* §5 owns the menus; §9 pins that a right-click inside a pane
         * is unbound and does nothing rather than opening a stub. */
        return;
    }
    if (k->button != (u8)SAG_MB_LEFT)
        return;
    if (sag_mouse_claimed_by_menu(ed, *k))
        return;

    m->held = (u8)(1U << (k->button - 1U));
    m->press_x = k->col;
    m->press_y = k->row;
    m->press_rgn = hit; /* CAPTURED — see mouse.h */
    m->phase = SAG_MP_ARMED;
    m->drag_tab_id = 0U;
    m->drag_gid = 0U;
    m->drag_to_slot = -1;
    m->drag_to_valid = false;
    m->dwell_gid = 0U;
    m->dwell_since_ms = 0;

    switch (hit.kind) {
    case SAG_REGION_PANE:
        press_pane(ed, &hit, k);
        break;
    case SAG_REGION_PANE_BORDER:
        press_border(ed, &hit);
        break;
    case SAG_REGION_TAB:
        press_tab(ed, &hit);
        break;
    case SAG_REGION_TAB_SCROLL:
        strip_scroll(ed, hit.payload == 2 || hit.payload == -2,
                     hit.payload < 0 ? -1 : 1);
        break;
    case SAG_REGION_PICK_ROW:
        press_pick_row(ed, &hit);
        break;
    case SAG_REGION_GP_ROW:
    case SAG_REGION_GP_NAME:
        if (sag_gp_click(ed, k->col, k->row))
            sag_gp_apply(ed);
        break;
    case SAG_REGION_BLOCK:
        /* Swallowed by construction.  This is what makes a dialog modal
         * to the mouse without any dialog knowing the router exists. */
        break;
    case SAG_REGION_CTX_ROW:
    case SAG_REGION_MENU_ROW:
    case SAG_REGION_NONE:
    default:
        break;
    }
}

/* ---------------------------------------------------------------- */
/* §2: motion                                                       */
/* ---------------------------------------------------------------- */

/*
 * Extends the selection to the pointer.  The anchor is whatever the
 * press established; §6 replaces the char unit with a word or line
 * engine after a multi-click.
 */
static void drag_select(Ed *ed, const Key *k)
{
    Pane *leaf = sag_pane_leaf_by_index(ed, ed->mouse.press_rgn.payload);
    Win *w = leaf != NULL ? leaf->win : NULL;
    Cursor *c;

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL ||
        w->cs.curs.len == 0U)
        return;
    sag_win_click_to_cursor(w, k->col, k->row);
    c = &w->cs.curs.data[w->cs.primary];
    /* click_to_cursor collapses the anchor onto the caret; the drag's
     * anchor is the press, so it is written back after. */
    c->anchor = BYTEOFF(ed->mouse.sel_anchor_span.lo);
    ed->full_damage = true;
}

static void begin_drag(Ed *ed)
{
    MouseState *m = &ed->mouse;

    switch (m->press_rgn.kind) {
    case SAG_REGION_PANE:
        /*
         * A drag in a pane is a selection, and a selection in this
         * editor IS H mode.  Entering it here rather than at press is
         * the arming law: a plain click must not change the mode.
         */
        (void)sag_mode_enter_highlight(ed, SAG_MODE_I, false);
        m->phase = SAG_MP_DRAG_SEL;
        break;
    case SAG_REGION_TAB:
        m->phase = m->press_rgn.payload < 0 ? SAG_MP_DRAG_GROUP
                                            : SAG_MP_DRAG_TAB;
        break;
    default:
        /* Nothing else has a drag; the gesture stays armed and the
         * release is still a click on the captured region. */
        break;
    }
}

static void mouse_motion(Ed *ed, const Key *k)
{
    MouseState *m = &ed->mouse;

    if (m->phase == SAG_MP_IDLE)
        return;
    if (m->phase == SAG_MP_ARMED) {
        /* The pointer has to leave the pressed CELL.  Cells are the
         * unit of everything here, so there is no pixel radius to
         * tune. */
        if (k->col == m->press_x && k->row == m->press_y)
            return;
        begin_drag(ed);
        if (m->phase == SAG_MP_ARMED)
            return;
    }
    switch (m->phase) {
    case SAG_MP_DRAG_BORDER:
        sag_pane_drag_motion(ed, k->col, k->row);
        break;
    case SAG_MP_DRAG_SEL:
        drag_select(ed, k);
        break;
    case SAG_MP_DRAG_TAB:
    case SAG_MP_DRAG_GROUP:
        /* §4 owns the preview, the dwell and the auto-scroll. */
        break;
    case SAG_MP_IDLE:
    case SAG_MP_ARMED:
    default:
        break;
    }
}

/* ---------------------------------------------------------------- */
/* §2: release                                                      */
/* ---------------------------------------------------------------- */

/*
 * A click on a tab entry, resolved from the identity captured at press.
 *
 * Re-reading the region here is exactly what the law forbids: the strip
 * can have scrolled between press and release (a chevron auto-scroll,
 * a job closing a tab), and the cells the pointer is over may now
 * belong to a different file.
 */
static void click_tab(Ed *ed)
{
    MouseState *m = &ed->mouse;

    if (m->drag_gid != 0U) {
        sag_group_note_position(ed);
        /* An EXPLICIT entry resumes where the user left off, unlike a
         * mid-walk arrival which enters from the side it came from. */
        sag_group_enter(ed, m->drag_gid);
        return;
    }
    if (m->drag_tab_id != 0U) {
        int idx = sag_tab_index_of_id(ed, m->drag_tab_id);

        if (idx >= 0)
            sag_tab_switch(ed, idx);
    }
}

static void mouse_release(Ed *ed, const Key *k)
{
    MouseState *m = &ed->mouse;
    MousePhase phase = m->phase;

    if (k->button != (u8)SAG_MB_LEFT)
        return;
    if (sag_mouse_claimed_by_menu(ed, *k)) {
        sag_mouse_init(m);
        return;
    }
    if (phase == SAG_MP_IDLE)
        return;
    switch (phase) {
    case SAG_MP_DRAG_BORDER:
        /* Cells become a ratio ONCE, here — a round-trip per motion
         * event accumulates float error and the border stutters against
         * the pointer (s22's pitfall). */
        sag_pane_drag_end(ed);
        break;
    case SAG_MP_ARMED:
        switch (m->press_rgn.kind) {
        case SAG_REGION_TAB:
            click_tab(ed);
            break;
        case SAG_REGION_PICK_ROW:
            /*
             * Accept only when the release is in the SAME row.  A press
             * that slid onto a neighbour before coming up was a mis-aim
             * and opening the neighbour is the worst possible reading
             * of it.
             */
            if (sag_region_hit(k->col, k->row).kind ==
                    SAG_REGION_PICK_ROW &&
                sag_region_hit(k->col, k->row).payload ==
                    m->press_rgn.payload)
                (void)sag_picker_accept(ed);
            break;
        default:
            break;
        }
        break;
    case SAG_MP_DRAG_SEL:
        /* The selection is already live; release only ends the
         * gesture.  H mode keeps it, which is what makes every H key,
         * the multi-cursor lift and the recorder work on it. */
        break;
    case SAG_MP_DRAG_TAB:
    case SAG_MP_DRAG_GROUP:
        /* §4 owns the drop. */
        break;
    case SAG_MP_IDLE:
    default:
        break;
    }
    sag_mouse_init(m);
}

/* ---------------------------------------------------------------- */
/* Cancellation                                                     */
/* ---------------------------------------------------------------- */

void sag_mouse_cancel(Ed *ed)
{
    if (ed == NULL || ed->mouse.phase == SAG_MP_IDLE)
        return;
    if (ed->mouse.phase == SAG_MP_DRAG_BORDER)
        sag_pane_drag_cancel(ed);
    if (ed->mouse.preview_gid != 0U)
        ed->full_damage = true;
    sag_mouse_init(&ed->mouse);
}

/* ---------------------------------------------------------------- */
/* Timers                                                           */
/* ---------------------------------------------------------------- */

void sag_mouse_tick(Ed *ed, i64 now_ms)
{
    /* §4 owns dwell and auto-scroll; both are clocks, not motion
     * counts. */
    (void)ed;
    (void)now_ms;
}

i64 sag_mouse_deadline(const Ed *ed, i64 now_ms)
{
    (void)ed;
    (void)now_ms;
    return 0;
}

bool sag_mouse_drag_preview(const Ed *ed, i32 *payload, int *to_slot)
{
    if (ed == NULL || payload == NULL || to_slot == NULL)
        return false;
    if (ed->mouse.phase != SAG_MP_DRAG_TAB &&
        ed->mouse.phase != SAG_MP_DRAG_GROUP)
        return false;
    if (!ed->mouse.drag_to_valid)
        return false;
    *payload = ed->mouse.press_rgn.payload;
    *to_slot = ed->mouse.drag_to_slot;
    return true;
}

u32 sag_mouse_preview_group(const Ed *ed)
{
    return ed != NULL ? ed->mouse.preview_gid : 0U;
}

/* ---------------------------------------------------------------- */
/* THE entry point                                                  */
/* ---------------------------------------------------------------- */

void sag_mouse_event(Ed *ed, const Key *k)
{
    if (ed == NULL || k == NULL || k->kind != (u16)SAG_EV_MOUSE)
        return;
    /*
     * BEFORE the phase machine, and never touching it.  A wheel event
     * has no release, so a state machine keyed on press-without-release
     * hangs on the first scroll — Sprint 4 pinned this and named this
     * sprint as the place it had to be honoured.
     */
    if (is_wheel(k->button)) {
        mouse_wheel(ed, k);
        return;
    }
    switch (k->ev) {
    case SAG_KEY_PRESS:
        mouse_press(ed, k);
        break;
    case SAG_KEY_REPEAT:
        mouse_motion(ed, k);
        break;
    case SAG_KEY_RELEASE:
        mouse_release(ed, k);
        break;
    default:
        break;
    }
}
