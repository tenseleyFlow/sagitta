#define _POSIX_C_SOURCE 200809L

#include "ui/mouse.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/pane_cmds.h"
#include "text/piece.h"
#include "ui/cmdline.h"
#include "ui/ctxmenu.h"
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
/* §5: the context menus                                            */
/* ---------------------------------------------------------------- */

/*
 * The rows are OPAQUE ACTIONS to ctxmenu.c, which is what lets that
 * module stay below the editor in the dependency graph.  The meaning
 * lives here, with the caller, exactly as §5 requires.
 */
enum {
    CTXA_NONE = 0,
    CTXA_TAB_CLOSE,
    CTXA_TAB_CLOSE_OTHERS,
    CTXA_TAB_COPY_PATH,
    CTXA_TAB_LEAVE_GROUP,
    CTXA_GROUP_EDIT,
    CTXA_GROUP_RENAME,
    CTXA_GROUP_DISSOLVE
};

/* Where a menu may be placed: everything above the footer. */
static Rect menu_allowed(const Ed *ed)
{
    u16 rows = ed->grid.rows;

    if (rows > 1U)
        rows = (u16)(rows - 1U); /* the footer keeps its row */
    return (Rect){0U, 0U, ed->grid.cols, rows};
}

bool sag_mouse_open_tab_menu(Ed *ed, u32 tab_id, u16 x, u16 y)
{
    int idx = sag_tab_index_of_id(ed, tab_id);
    Tab *t = sag_tab_at(ed, idx);

    if (t == NULL)
        return false;
    sag_ctx_begin((u32)SAG_CTX_KIND_TAB);
    /*
     * The target, captured NOW: the tab_id and the canonical path.
     * Every row re-finds the tab from the id when it is invoked,
     * because the strip can scroll and tabs can close while the menu is
     * up — and then the entry at those cells is a different file.
     */
    sag_ctx_target(tab_id, t->path);
    sag_ctx_item("Close Tab", "C-w", CTXA_TAB_CLOSE,
                 sag_tab_count(ed) > 1U);
    /* Disabled rows are GREYED, never hidden, so the menu keeps its
     * shape and a row does not move under the pointer between one
     * right-click and the next. */
    sag_ctx_item("Close Other Tabs", NULL, CTXA_TAB_CLOSE_OTHERS,
                 sag_tab_count(ed) > 1U);
    sag_ctx_sep();
    sag_ctx_item("Copy Path", NULL, CTXA_TAB_COPY_PATH, t->path != NULL);
    sag_ctx_item("Remove from Group", NULL, CTXA_TAB_LEAVE_GROUP,
                 t->group_id != 0U);
    return sag_ctx_show(x, y, menu_allowed(ed));
}

bool sag_mouse_open_group_menu(Ed *ed, u32 gid, u16 x, u16 y)
{
    if (sag_group_at(ed, gid) == NULL)
        return false;
    sag_ctx_begin((u32)SAG_CTX_KIND_GROUP);
    sag_ctx_target(gid, NULL);
    sag_ctx_item("Edit Group...", NULL, CTXA_GROUP_EDIT, true);
    sag_ctx_item("Rename Group...", NULL, CTXA_GROUP_RENAME, true);
    sag_ctx_sep();
    sag_ctx_item("Dissolve Group", NULL, CTXA_GROUP_DISSOLVE, true);
    return sag_ctx_show(x, y, menu_allowed(ed));
}

static void invoke_named(Ed *ed, const char *name)
{
    CmdCtx cx = {0};
    CmdId id = sag_cmd_lookup(name, strlen(name));

    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.source = SAG_SRC_KEY;
    (void)sag_ed_invoke(ed, id, &cx);
}

/*
 * Runs whatever row was chosen, against the target the menu captured.
 *
 * The region table is FROZEN for the duration: a row handler that
 * reached for a payload would be re-resolving the target from cells
 * that may since have come to mean a different file, and freezing turns
 * that from a rule into an abort (ui/region.h).
 */
static void apply_menu_action(Ed *ed)
{
    u32 action = sag_ctx_take();
    u32 kind = sag_ctx_kind();
    u32 target = sag_ctx_target_id();

    if (action == CTXA_NONE)
        return;
    sag_region_freeze(true);
    if (kind == (u32)SAG_CTX_KIND_TAB) {
        /*
         * The target becomes active first, resolved from its ID.  A
         * right-click on a tab is an act of pointing at it, so acting
         * on it is what the user asked for — and it means the rows can
         * be the ordinary registry commands rather than a second
         * implementation that takes a tab argument.
         */
        int idx = sag_tab_index_of_id(ed, target);

        if (idx >= 0) {
            sag_tab_switch(ed, idx);
            switch (action) {
            case CTXA_TAB_CLOSE:
                invoke_named(ed, "ed.tab.close");
                break;
            case CTXA_TAB_CLOSE_OTHERS:
                invoke_named(ed, "ed.tab.close_others");
                break;
            case CTXA_TAB_COPY_PATH:
                invoke_named(ed, "ed.tab.copy_path");
                break;
            case CTXA_TAB_LEAVE_GROUP:
                invoke_named(ed, "ed.group.remove_tab");
                break;
            default:
                break;
            }
        }
    } else if (kind == (u32)SAG_CTX_KIND_GROUP) {
        u32 was = sag_active_group_id(ed);

        if (was != target)
            sag_group_enter(ed, target);
        switch (action) {
        case CTXA_GROUP_EDIT:
            invoke_named(ed, "ed.group.edit");
            break;
        case CTXA_GROUP_RENAME:
            invoke_named(ed, "ed.group.rename");
            break;
        case CTXA_GROUP_DISSOLVE:
            invoke_named(ed, "ed.group.dissolve");
            break;
        default:
            break;
        }
    }
    sag_region_freeze(false);
    ed->layout_dirty = true;
    ed->full_damage = true;
}

/* The menu is a keymap layer: it takes the key before any mode does,
 * and swallows what it does not use. */
bool sag_mouse_menu_key(Ed *ed, const Key *k)
{
    if (ed == NULL || !sag_ctx_active())
        return false;
    if (!sag_ctx_key(k))
        return false;
    apply_menu_action(ed);
    ed->full_damage = true;
    return true;
}

void sag_mouse_menu_draw(Ed *ed)
{
    if (ed != NULL)
        sag_ctx_draw(&ed->grid);
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
        /*
         * §9: a right-click inside a pane is UNBOUND and does nothing.
         * The document context menu is post-1.0 and is named here so
         * nobody invents one; a stub menu would be worse than none.
         */
        if (sag_ctx_active()) {
            /* A right-click anywhere closes an open menu — including on
             * the menu itself, which is how every menu everywhere
             * behaves. */
            sag_ctx_close();
            ed->full_damage = true;
            return;
        }
        if (hit.kind != SAG_REGION_TAB)
            return;
        if (hit.payload < 0) {
            (void)sag_mouse_open_group_menu(ed, (u32)(-hit.payload),
                                            k->col, (u16)(k->row + 1U));
        } else {
            Tab *t = sag_tab_at(ed, hit.payload);

            if (t != NULL) {
                (void)sag_mouse_open_tab_menu(ed, t->tab_id, k->col,
                                              (u16)(k->row + 1U));
            }
        }
        ed->full_damage = true;
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
        /* Highlighting, not invoking: the action fires at release, and
         * only when the release lands on the same row. */
        sag_ctx_hover(hit.payload);
        ed->full_damage = true;
        break;
    case SAG_REGION_MENU_ROW:
    case SAG_REGION_NONE:
        /* A left-click outside an open menu closes it, and is consumed
         * doing so — the click that dismisses a menu must not also do
         * whatever is underneath. */
        if (sag_ctx_active()) {
            sag_ctx_close();
            m->phase = SAG_MP_IDLE;
            m->held = 0U;
            ed->full_damage = true;
        }
        break;
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

/* ---------------------------------------------------------------- */
/* §4: tab and group drag-reorder                                   */
/* ---------------------------------------------------------------- */

/* The group the held tab is currently a member of; 0 when ungrouped.
 * Resolved from the id, never from the press's index. */
static u32 held_tab_group(Ed *ed)
{
    int idx = sag_tab_index_of_id(ed, ed->mouse.drag_tab_id);
    Tab *t = sag_tab_at(ed, idx);

    return t != NULL ? t->group_id : 0U;
}

/*
 * PITFALL — the dwell target is not sag_region_hit.
 *
 * The region table describes the PREVIEWED strip, where the held entry
 * has been moved under the pointer, so hit-testing would always answer
 * "you are hovering the thing you are holding".  The pre-drag slot
 * table (s27 §4, ui/tabs.h) is the only thing that can answer what was
 * here before the drag started.
 *
 * Arming rather than opening: a drag that merely PASSES over a group on
 * its way somewhere else must not make that group's members flash open,
 * so the clock restarts every time the hovered group changes and the
 * open happens in sag_mouse_tick.
 */
static void drag_dwell(Ed *ed, int slot)
{
    MouseState *m = &ed->mouse;
    i32 pre = 0;
    u32 gid = 0U;

    /* Only a TAB dwells into a group.  A group dragged into another
     * group is not a thing this model has — groups do not nest. */
    if (m->phase == SAG_MP_DRAG_TAB && slot >= 0 &&
        sag_strip_pre_payload(slot, &pre) && pre < 0)
        gid = (u32)(-pre);
    /* A tab never dwells into the group it is already a member of:
     * there is nothing to join, and opening the strip would offer a
     * drop that means nothing. */
    if (gid != 0U && gid == held_tab_group(ed))
        gid = 0U;
    if (gid != m->dwell_gid) {
        m->dwell_gid = gid;
        m->dwell_since_ms = gid != 0U ? ed->now_ms : 0;
    }
}

static void drag_strip_motion(Ed *ed, const Key *k)
{
    MouseState *m = &ed->mouse;
    int slot;

    /*
     * The array is frozen for the drag's lifetime, so a changed count
     * means something ELSE mutated it — an async job closing a file, a
     * script — and the target the user aimed at no longer means what it
     * did.  Cancel outright rather than commit against a moved target.
     */
    if (sag_tab_count(ed) != m->tab_count_at_press) {
        sag_mouse_cancel(ed);
        return;
    }
    slot = sag_strip_slot_at(k->col, k->row);
    if (slot >= 0) {
        if (!m->drag_to_valid || m->drag_to_slot != slot || m->drag_to_tail) {
            m->drag_to_slot = slot;
            m->drag_to_valid = true;
            m->drag_to_tail = false;
            ed->full_damage = true;
        }
    } else if (sag_strip_slot_count() > 0 &&
               k->row == ed->tab_strip_rect.y &&
               k->col >= sag_strip_tail_x()) {
        /*
         * The blank tail past the last entry — row 1's, whatever row
         * the press came from: a member dragged UP out of row 2 aims at
         * row 1's empty space, and that gesture is the whole reason the
         * tail is a drop target.
         */
        if (!m->drag_to_tail) {
            m->drag_to_slot = sag_strip_slot_count() - 1;
            m->drag_to_valid = true;
            m->drag_to_tail = true;
            ed->full_damage = true;
        }
    }
    drag_dwell(ed, slot);
}

/* The tab-array index a row-1 slot names, resolved against the PRE-DRAG
 * list.  A group entry answers with its first member's index, which is
 * where the group's block starts. */
static int slot_to_tab_index(Ed *ed, int slot)
{
    i32 pre = 0;

    if (!sag_strip_pre_payload(slot, &pre))
        return -1;
    if (pre >= 0)
        return (int)pre;
    {
        int members[SAG_TAB_MAX];
        int n = sag_group_members(ed, (u32)(-pre), members,
                                  (int)SAG_ARRAY_LEN(members));
        int lowest = -1;
        int i;

        for (i = 0; i < n; i++) {
            if (lowest < 0 || members[i] < lowest)
                lowest = members[i];
        }
        return lowest;
    }
}

/* Where a group's members begin in the tab array. */
static int group_block_start(Ed *ed, u32 gid)
{
    int members[SAG_TAB_MAX];
    int n = sag_group_members(ed, gid, members, (int)SAG_ARRAY_LEN(members));
    int lowest = -1;
    int i;

    for (i = 0; i < n; i++) {
        if (lowest < 0 || members[i] < lowest)
            lowest = members[i];
    }
    return lowest;
}

/*
 * Joining a group is Sprint 24's exact sequence, called and never
 * re-derived: the ordinal off-by-one is s24's pinned pitfall and
 * reinventing it here would put a second, subtly different answer in
 * the program.
 */
static void drop_into_group(Ed *ed, u32 gid, int pos)
{
    int tab_idx = sag_tab_index_of_id(ed, ed->mouse.drag_tab_id);
    Tab *t = sag_tab_at(ed, tab_idx);

    if (t == NULL || gid == 0U)
        return;
    if (t->group_id != 0U)
        sag_group_remove_member(ed, tab_idx); /* FIRST */
    /* The removal can dissolve an emptied group and does not move
     * anything, so the index still names this tab. */
    sag_group_add_member(ed, gid, tab_idx);
    sag_group_set_ordinal(ed, tab_idx, pos); /* pos counts in the FINAL list */
    {
        int start = group_block_start(ed, gid);

        if (start >= 0)
            sag_group_reorder_block(ed, gid, start); /* keep contiguous */
    }
    sag_state_mark_dirty(ed);
}

/*
 * Row 2 under the pointer: which group, and at which ordinal.
 *
 * The group is the one row 2 is SHOWING — the dwell's preview when
 * there is one, otherwise the pinned member strip.  Resolving it from
 * the member under the pointer instead would fail on the blank tail,
 * which is where "put it last" has to be expressible.
 */
static bool drop_target_row2(Ed *ed, const Key *k, u32 *gid, int *pos)
{
    Region hit;

    if (ed->tab_strip_rect.h < 2U ||
        k->row != (u16)(ed->tab_strip_rect.y + 1U))
        return false;
    *gid = ed->mouse.preview_gid != 0U ? ed->mouse.preview_gid
                                       : sag_active_group_id(ed);
    if (*gid == 0U)
        return false;
    hit = sag_region_hit(k->col, k->row);
    if (hit.kind == SAG_REGION_TAB && hit.payload >= 0) {
        Tab *t = sag_tab_at(ed, hit.payload);

        if (t != NULL && t->group_id == *gid) {
            *pos = (int)t->group_ordinal;
            return true;
        }
    }
    /* The blank tail of row 2: append. */
    *pos = sag_group_member_count(ed, *gid) + 1;
    return true;
}

static void drag_strip_drop(Ed *ed, const Key *k)
{
    MouseState *m = &ed->mouse;
    u32 gid = 0U;
    int pos = 0;
    int to;

    if (sag_tab_count(ed) != m->tab_count_at_press)
        return; /* cancelled; nothing was mutated on the way */
    if (m->phase == SAG_MP_DRAG_TAB && drop_target_row2(ed, k, &gid, &pos)) {
        drop_into_group(ed, gid, pos);
        ed->full_damage = true;
        return;
    }
    if (!m->drag_to_valid)
        return; /* released somewhere with no target: nothing changes */
    to = m->drag_to_tail ? (int)sag_tab_count(ed) - 1
                         : slot_to_tab_index(ed, m->drag_to_slot);
    if (to < 0)
        return;
    if (m->phase == SAG_MP_DRAG_GROUP) {
        sag_group_reorder_block(ed, m->drag_gid, to);
    } else {
        int from = sag_tab_index_of_id(ed, m->drag_tab_id);

        if (from < 0)
            return;
        /*
         * Dropping on the blank tail carries the tab OUT of its group —
         * the one gesture that can, when the group is the only row-1
         * entry left to aim at.
         */
        if (m->drag_to_tail && held_tab_group(ed) != 0U)
            sag_group_remove_member(ed, from);
        sag_tab_reorder(ed, from, to);
    }
    sag_state_mark_dirty(ed);
    ed->full_damage = true;
}

static void mouse_motion(Ed *ed, const Key *k)
{
    MouseState *m = &ed->mouse;

    if (m->phase == SAG_MP_IDLE)
        return;
    m->at_x = k->col;
    m->at_y = k->row;
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
        drag_strip_motion(ed, k);
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
        case SAG_REGION_CTX_ROW: {
            Region up = sag_region_hit(k->col, k->row);

            /* Same row, or nothing: a press that slid onto a neighbour
             * before coming up was a mis-aim, and invoking the
             * neighbour is the worst possible reading of it. */
            if (up.kind == SAG_REGION_CTX_ROW &&
                up.payload == m->press_rgn.payload) {
                sag_ctx_invoke(up.payload);
                apply_menu_action(ed);
            }
            ed->full_damage = true;
            break;
        }
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
        drag_strip_drop(ed, k);
        break;
    case SAG_MP_IDLE:
    default:
        break;
    }
    if (m->preview_gid != 0U) {
        /* The dwell-opened strip goes away with the gesture that opened
         * it, and it changed the strip's row count, so the layout has to
         * be recomputed rather than merely repainted. */
        ed->layout_dirty = true;
        ed->full_damage = true;
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
    /*
     * A tab or group drag needs nothing undone: Tabs.v was never
     * touched.  That is the whole reason the preview is a picture and
     * not a live mutation — cancelling here would otherwise mean
     * undoing an arbitrary number of moves.
     */
    if (ed->mouse.preview_gid != 0U) {
        ed->layout_dirty = true;
        ed->full_damage = true;
    } else if (ed->mouse.drag_to_valid) {
        ed->full_damage = true;
    }
    sag_mouse_init(&ed->mouse);
}

/* ---------------------------------------------------------------- */
/* §4: the clocks                                                   */
/* ---------------------------------------------------------------- */

static bool drag_over_chevron(Ed *ed, i32 *delta)
{
    Region hit = sag_region_hit(ed->mouse.at_x, ed->mouse.at_y);

    if (hit.kind != SAG_REGION_TAB_SCROLL)
        return false;
    *delta = hit.payload < 0 ? -1 : 1;
    return true;
}

void sag_mouse_tick(Ed *ed, i64 now_ms)
{
    MouseState *m;
    i32 delta = 0;

    if (ed == NULL)
        return;
    m = &ed->mouse;
    if (m->phase != SAG_MP_DRAG_TAB && m->phase != SAG_MP_DRAG_GROUP)
        return;
    ed->now_ms = now_ms;
    if (m->dwell_gid != 0U && m->preview_gid != m->dwell_gid &&
        now_ms - m->dwell_since_ms >= SAG_DRAG_DWELL_MS) {
        m->preview_gid = m->dwell_gid;
        /* The strip grew a row, so this is a layout change and not a
         * repaint — the pane tree below it has to give the row back. */
        ed->layout_dirty = true;
        ed->full_damage = true;
    }
    /*
     * Auto-scroll runs on THIS clock rather than per motion event: a
     * fast pointer emits far more motion reports than a slow one, and
     * a strip that scrolled per report would fly past the target at a
     * speed that depends on how the terminal batches its reports.
     */
    if (drag_over_chevron(ed, &delta) &&
        now_ms - m->autoscroll_ms >= SAG_DRAG_SCROLL_MS) {
        m->autoscroll_ms = now_ms;
        strip_scroll(ed, false, delta);
    }
}

i64 sag_mouse_deadline(const Ed *ed, i64 now_ms)
{
    const MouseState *m;
    i64 next = -1;

    if (ed == NULL)
        return -1;
    m = &ed->mouse;
    if (m->phase != SAG_MP_DRAG_TAB && m->phase != SAG_MP_DRAG_GROUP)
        return -1;
    if (m->dwell_gid != 0U && m->preview_gid != m->dwell_gid)
        next = m->dwell_since_ms + SAG_DRAG_DWELL_MS;
    if (sag_region_hit(m->at_x, m->at_y).kind == SAG_REGION_TAB_SCROLL) {
        i64 at = m->autoscroll_ms + SAG_DRAG_SCROLL_MS;

        if (next < 0 || at < next)
            next = at;
    }
    if (next < 0)
        return -1;
    return next <= now_ms ? 0 : next - now_ms;
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
     * §9: with the mouse off, events are DROPPED here rather than at
     * the terminal.  A terminal that keeps reporting after the disable
     * sequence — or one that never honoured it — must not be able to
     * move the cursor, and every action still has its keyboard path.
     */
    if (!sag_mouse_enabled())
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

/* ---------------------------------------------------------------- */
/* §5/§9: the registry commands this file owns                      */
/* ---------------------------------------------------------------- */

/*
 * Invariant 9's entry into the menu: opens it for the FOCUSED tab or
 * group, anchored at the strip rather than at a pointer that may not
 * exist.  Without this the menu rows would be mouse-only, and every one
 * of them would be a feature the keyboard could not reach.
 */
CmdStatus sag_ui_cmd_context_menu(CmdCtx *cx)
{
    Ed *ed;
    u32 gid;
    u16 y;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    ed = cx->ed;
    if (ed->tabs.active < 0)
        return SAG_CMD_ERR_STATE;
    y = (u16)(ed->tab_strip_rect.y + ed->tab_strip_rect.h);
    gid = sag_active_group_id(ed);
    if (gid != 0U) {
        if (!sag_mouse_open_group_menu(ed, gid, ed->tab_strip_rect.x, y))
            return SAG_CMD_ERR_STATE;
    } else {
        Tab *t = sag_tab_at(ed, ed->tabs.active);

        if (t == NULL ||
            !sag_mouse_open_tab_menu(ed, t->tab_id, ed->tab_strip_rect.x,
                                     y))
            return SAG_CMD_ERR_STATE;
    }
    ed->full_damage = true;
    return SAG_CMD_OK;
}

/*
 * §9: the runtime toggle.  The option model that PERSISTS it is Sprint
 * 36; until then it lives for the session, which is what makes it
 * usable for "this terminal's mouse reporting is fighting me right
 * now".
 */
static bool mouse_enabled = true;

bool sag_mouse_enabled(void)
{
    return mouse_enabled;
}

void sag_mouse_set_enabled(bool on)
{
    mouse_enabled = on;
}

CmdStatus sag_mouse_cmd_enable(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    mouse_enabled = true;
    sag_msg(cx->ed, SAG_MSG_INFO, "mouse on");
    return SAG_CMD_OK;
}

CmdStatus sag_mouse_cmd_disable(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    /* Any gesture in flight goes with it: a router that stopped
     * receiving events mid-drag would sit with the button logically
     * down forever. */
    sag_mouse_cancel(cx->ed);
    sag_ctx_close();
    mouse_enabled = false;
    sag_msg(cx->ed, SAG_MSG_INFO, "mouse off");
    return SAG_CMD_OK;
}
