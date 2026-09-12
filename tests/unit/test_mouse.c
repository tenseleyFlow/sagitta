/*
 * Sprint 27 §1/§2/§3: the router, the region table, and the wheel.
 *
 * WHAT THESE TESTS ARE ABOUT.  Almost every row here defends one of
 * three laws, and each law is a bug that has shipped in every mouse
 * implementation that did not have it written down:
 *
 *   THE PRESS CAPTURES ITS REGION.  Between a press and its release the
 *   tab strip can scroll, a job can close a file, and every index above
 *   a closed tab renumbers.  A gesture that re-queries the region at
 *   release acts on whatever now occupies those cells.  So the tests
 *   deliberately MUTATE the region table mid-gesture and assert the
 *   commit still used the captured identity.
 *
 *   WHEELS ARE IMPULSES.  A wheel event has no release.  A phase
 *   machine that treats one as a press hangs on the first scroll, with
 *   the button logically down forever — Sprint 4 pinned this and named
 *   this sprint as the place to honour it.
 *
 *   ARMING IS NOT DRAGGING.  A press that never leaves its cell is a
 *   click.  Nothing may be drawn, targeted, or mutated until the
 *   pointer moves.
 */
#include "harness.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/pane_cmds.h"
#include "mod/git/fussmode.h"
#include "term/tty.h"
#include "text/register.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/ctxmenu.h"
#include "ui/ctxrows.h"
#include "ui/grouppicker.h"
#include "ui/mouse.h"
#include "ui/picker.h"
#include "ui/region.h"
#include "ui/tabs.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "util/intern.h"

/* ---------------------------------------------------------------- */
/* Fixture                                                          */
/* ---------------------------------------------------------------- */

static void ms_fixture(Ed *ed)
{
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    YEW_ASSERT(yew_grid_init(&ed->grid, &ed->interner, 24U, 80U));
    ed->grid_ready = true;
    /* The real layout, not a hand-placed rect: the viewport's row and
     * column counts come from it, and a zero-row viewport makes
     * yew_vp_clamp a no-op — which would let a wheel test pass while
     * proving nothing. */
    yew_ed_layout(ed);
    ed->now_ms = 1000;
}

static Key ms_ev(u8 button, u8 ev, u16 x, u16 y)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.kind = (u16)YEW_EV_MOUSE;
    k.button = button;
    k.ev = ev;
    k.col = x;
    k.row = y;
    return k;
}

static Key ms_wheel(u8 button, u16 x, u16 y, u16 mods)
{
    Key k = ms_ev(button, (u8)YEW_KEY_PRESS, x, y);

    k.mods = mods;
    return k;
}

static bool ms_find_region(RegionKind kind, Rect within, Region *out)
{
    u16 y;

    for (y = within.y; y < (u16)(within.y + within.h); y++) {
        u16 x;

        for (x = within.x; x < (u16)(within.x + within.w); x++) {
            Region hit = yew_region_hit(x, y);

            if (hit.kind == kind) {
                if (out != NULL)
                    *out = hit;
                return true;
            }
        }
    }
    return false;
}

/* A frame whose only region is the pane, registered with the rect the
 * layout actually gave it. */
static void ms_frame_pane(const Pane *leaf, i32 leaf_payload)
{
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, leaf->rect, leaf_payload);
}

static void ms_fill_lines(Ed *ed, u32 n)
{
    u32 i;

    for (i = 0U; i < n; i++) {
        EditCtx ec = yew_ed_edit_ctx(ed);
        char line[64];

        (void)snprintf(line, sizeof(line), "line %u padding padding\n",
                       (unsigned)i);
        yew_edit_insert(&ec, yew_ed_cursor(ed)->pos, (const u8 *)line,
                        strlen(line));
        yew_ed_finish_edit(ed, &ec);
    }
}

/* ---------------------------------------------------------------- */
/* §1: the phase machine                                            */
/* ---------------------------------------------------------------- */

void test_mouse_press_arms_and_does_not_drag(void)
{
    Ed ed;
    i32 leaf;

    ms_fixture(&ed);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);

    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
    {
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, 10U, 5U);

        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_ARMED);
    YEW_ASSERT(ed.mouse.held != 0U);
    YEW_ASSERT_EQ_U64(ed.mouse.press_x, 10U);
    YEW_ASSERT_EQ_U64(ed.mouse.press_y, 5U);
    /* The region was CAPTURED, not merely hit-tested and thrown away. */
    YEW_ASSERT_EQ_U64((u64)ed.mouse.press_rgn.kind,
                      (u64)YEW_REGION_PANE);

    /* Motion within the pressed cell is still a click, not a drag. */
    {
        Key motion = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_REPEAT, 10U, 5U);

        yew_mouse_event(&ed, &motion);
    }
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_ARMED);

    /* One cell away, and it becomes a drag. */
    {
        Key motion = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_REPEAT, 11U, 5U);

        yew_mouse_event(&ed, &motion);
    }
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_DRAG_SEL);

    {
        Key up = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE, 11U, 5U);

        yew_mouse_event(&ed, &up);
    }
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
    YEW_ASSERT_EQ_U64(ed.mouse.held, 0U);
    yew_ed_free(&ed);
}

void test_mouse_new_tab_control_invokes_the_named_command_once(void)
{
    Ed ed;
    Region add;
    Key press;
    Key release;
    u32 old_id;

    ms_fixture(&ed);
    yew_region_frame_begin();
    yew_tab_strip_draw(&ed, ed.tab_strip_rect);
    YEW_ASSERT(ms_find_region(YEW_REGION_TAB_NEW, ed.tab_strip_rect, &add));
    old_id = yew_tab_at(&ed, 0)->tab_id;
    press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS,
                  (u16)(add.rect.x + 1U), add.rect.y);
    release = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE,
                    (u16)(add.rect.x + 1U), add.rect.y);

    yew_mouse_event(&ed, &press);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 1U);
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_ARMED);
    yew_mouse_event(&ed, &release);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 2U);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 1);
    YEW_ASSERT(yew_tab_at(&ed, 1)->path == NULL);
    YEW_ASSERT(yew_tab_at(&ed, 1)->tab_id > old_id);
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
    yew_ed_free(&ed);
}

void test_mouse_new_tab_control_cancels_off_region_and_drag(void)
{
    Ed ed;
    Region add;
    Key press;
    Key away;
    Key release;

    ms_fixture(&ed);
    yew_region_frame_begin();
    yew_tab_strip_draw(&ed, ed.tab_strip_rect);
    YEW_ASSERT(ms_find_region(YEW_REGION_TAB_NEW, ed.tab_strip_rect, &add));
    press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS,
                  (u16)(add.rect.x + 1U), add.rect.y);
    away = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE,
                 (u16)(add.rect.x + add.rect.w + 1U), add.rect.y);
    yew_mouse_event(&ed, &press);
    yew_mouse_event(&ed, &away);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 1U);

    yew_mouse_event(&ed, &press);
    away.ev = (u8)YEW_KEY_REPEAT;
    yew_mouse_event(&ed, &away);
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_ARMED);
    away.ev = (u8)YEW_KEY_RELEASE;
    yew_mouse_event(&ed, &away);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 1U);
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);

    /* A repaint can move the tail action between press and release.
     * Landing on the newly placed action is not the same click. */
    yew_mouse_event(&ed, &press);
    yew_region_frame_begin();
    yew_tab_strip_draw(&ed, (Rect){4U, ed.tab_strip_rect.y,
                                   (u16)(ed.tab_strip_rect.w - 4U), 1U});
    release = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE,
                    (u16)(add.rect.x + 5U), add.rect.y);
    YEW_ASSERT_EQ_I64(yew_region_hit(release.col, release.row).kind,
                      YEW_REGION_TAB_NEW);
    yew_mouse_event(&ed, &release);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 1U);
    yew_ed_free(&ed);
}

/*
 * Sprint 4's pinned pitfall, made executable.  A wheel notch must leave
 * the phase machine exactly as it found it — including in the middle of
 * a live drag, which is when a wheel-as-press would do the most damage.
 */
void test_mouse_wheel_never_touches_the_phase_machine(void)
{
    Ed ed;
    i32 leaf;
    u32 i;

    ms_fixture(&ed);
    ms_fill_lines(&ed, 60U);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);

    for (i = 0U; i < 8U; i++) {
        Key w = ms_wheel(i % 2U == 0U ? (u8)YEW_MB_WHEEL_DOWN
                                      : (u8)YEW_MB_WHEEL_UP,
                         10U, 5U, 0U);

        yew_mouse_event(&ed, &w);
        YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
        YEW_ASSERT_EQ_U64(ed.mouse.held, 0U);
    }

    /* And mid-drag: the drag survives, the wheel changes no phase. */
    {
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, 10U, 5U);
        Key motion = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_REPEAT, 14U, 5U);
        Key w = ms_wheel((u8)YEW_MB_WHEEL_DOWN, 10U, 5U, 0U);

        yew_mouse_event(&ed, &press);
        yew_mouse_event(&ed, &motion);
        YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_DRAG_SEL);
        yew_mouse_event(&ed, &w);
        YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_DRAG_SEL);
        YEW_ASSERT(ed.mouse.held != 0U);
    }
    yew_ed_free(&ed);
}

void test_mouse_escape_and_focus_out_cancel_a_drag(void)
{
    Ed ed;
    i32 leaf;

    ms_fixture(&ed);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);
    {
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, 10U, 5U);
        Key motion = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_REPEAT, 14U, 5U);

        yew_mouse_event(&ed, &press);
        yew_mouse_event(&ed, &motion);
    }
    YEW_ASSERT(yew_mouse_gesture_active(&ed));
    yew_mouse_cancel(&ed);
    YEW_ASSERT(!yew_mouse_gesture_active(&ed));
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);

    /*
     * Cancelling an idle router is a no-op rather than a crash: a
     * FOCUS_OUT arrives whenever the user alt-tabs, drag or no drag.
     */
    yew_mouse_cancel(&ed);
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
    yew_ed_free(&ed);
}

/*
 * A release with no press is not a gesture.  Terminals emit them after
 * a focus change, and a router that treated one as the end of
 * something would commit whatever the last press happened to capture.
 */
void test_mouse_release_without_press_is_inert(void)
{
    Ed ed;
    i32 leaf;

    ms_fixture(&ed);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);
    {
        Key up = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE, 10U, 5U);

        yew_mouse_event(&ed, &up);
    }
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
    YEW_ASSERT_EQ_U64(ed.mouse.held, 0U);
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* §2: the region table                                             */
/* ---------------------------------------------------------------- */

void test_mouse_click_a_pane_focuses_and_places_the_cursor(void)
{
    Ed ed;
    Pane *right;
    i32 left_id;
    i32 right_id;

    ms_fixture(&ed);
    ms_fill_lines(&ed, 20U);
    right = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_V);
    YEW_ASSERT_NOT_NULL(right);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 23U});
    yew_pane_tables_reset(&ed);
    left_id = yew_pane_table_add_leaf(&ed, ed.focus == right
                                               ? ed.pane_root->a
                                               : ed.focus);
    right_id = yew_pane_table_add_leaf(&ed, right);
    YEW_ASSERT(left_id >= 0 && right_id >= 0);

    ed.focus = yew_pane_leaf_by_index(&ed, left_id);
    ed.win = ed.focus->win;
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, right->rect, right_id);
    {
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS,
                          (u16)(right->rect.x + 2U),
                          (u16)(right->rect.y + 1U));
        Key up = press;

        up.ev = (u8)YEW_KEY_RELEASE;
        yew_mouse_event(&ed, &press);
        yew_mouse_event(&ed, &up);
    }
    YEW_ASSERT(ed.focus == right);
    yew_ed_free(&ed);
}

/*
 * YEW_REGION_BLOCK is what makes a dialog modal to the mouse without
 * any dialog knowing the router exists.  A press on it changes nothing
 * and, crucially, does not reach whatever is underneath.
 */
void test_mouse_block_region_swallows_everything(void)
{
    Ed ed;
    i32 leaf;
    Pane *before;

    ms_fixture(&ed);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    before = ed.focus;
    yew_region_frame_begin();
    /* The pane first, the block on top: last-added wins (s22). */
    yew_region_add(YEW_REGION_PANE, ed.pane_root->rect, leaf);
    yew_region_add(YEW_REGION_BLOCK, ed.pane_root->rect, 0);
    {
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, 10U, 5U);
        Key motion = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_REPEAT, 20U, 9U);
        Key up = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE, 20U, 9U);

        yew_mouse_event(&ed, &press);
        YEW_ASSERT_EQ_U64((u64)ed.mouse.press_rgn.kind,
                          (u64)YEW_REGION_BLOCK);
        yew_mouse_event(&ed, &motion);
        /* No drag begins on a block: there is nothing there to drag. */
        YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_ARMED);
        yew_mouse_event(&ed, &up);
    }
    YEW_ASSERT(ed.focus == before);
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
    yew_ed_free(&ed);
}

/*
 * Sprint 57.13 supersedes Sprint 27 §9.  A right-click inside a pane
 * opens the DOCUMENT menu — and, just as importantly, it still does not
 * touch the phase machine: the menu gesture is resolved before arming,
 * so no drag is pending and no selection is live behind the pop-up.
 */
void test_mouse_right_click_in_a_pane_opens_the_document_menu(void)
{
    Ed ed;
    i32 leaf;
    Pane *before;

    ms_fixture(&ed);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    before = ed.focus;
    ms_frame_pane(ed.pane_root, leaf);
    {
        Key press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS, 10U, 5U);

        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT(yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_kind(), (u64)YEW_CTX_KIND_DOC);
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
    YEW_ASSERT_EQ_U64(ed.mouse.held, 0U);
    /* Opening a menu on a pane does not FOCUS it: the rows do that, and
     * only the ones that need to. */
    YEW_ASSERT(ed.focus == before);
    yew_mouse_cancel(&ed);
    yew_ctx_close();
    yew_tty_mouse_motion(false);
    yew_ed_free(&ed);
}

/* Rows the table does not list are ignored, not guessed at. */
void test_mouse_unlisted_regions_are_inert(void)
{
    Ed ed;
    Pane *before;
    u64 dispatches;

    ms_fixture(&ed);
    before = ed.focus;
    dispatches = ed.dispatch_count;
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_NONE, ed.pane_root->rect, 0);
    {
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, 10U, 5U);
        Key up = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE, 10U, 5U);
        Key w = ms_wheel((u8)YEW_MB_WHEEL_DOWN, 10U, 5U, 0U);

        yew_mouse_event(&ed, &press);
        yew_mouse_event(&ed, &up);
        yew_mouse_event(&ed, &w);
    }
    YEW_ASSERT(ed.focus == before);
    YEW_ASSERT_EQ_U64(ed.dispatch_count, dispatches);
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* §3: the wheel                                                    */
/* ---------------------------------------------------------------- */

/*
 * DoD 5.  Two panes exist to be compared, so the wheel scrolls the one
 * under the POINTER — and leaves both the focus and the cursor exactly
 * where they were.  A scroll must never change document state.
 */
void test_mouse_wheel_scrolls_the_pane_under_the_pointer(void)
{
    Ed ed;
    Pane *right;
    Pane *left;
    i32 right_id;
    ByteOff cursor_before;
    LineNo left_top_before;

    ms_fixture(&ed);
    ms_fill_lines(&ed, 80U);
    right = yew_pane_split(&ed, ed.pane_root, YEW_SPLIT_V);
    YEW_ASSERT_NOT_NULL(right);
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 23U});
    left = ed.pane_root->a == right ? ed.pane_root->b : ed.pane_root->a;
    ed.focus = left;
    ed.win = left->win;
    YEW_ASSERT_NOT_NULL(right->win);

    yew_pane_tables_reset(&ed);
    (void)yew_pane_table_add_leaf(&ed, left);
    right_id = yew_pane_table_add_leaf(&ed, right);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, right->rect, right_id);

    cursor_before = yew_ed_cursor(&ed)->pos;
    left_top_before = left->win->vp.top;
    {
        Key w = ms_wheel((u8)YEW_MB_WHEEL_DOWN,
                         (u16)(right->rect.x + 2U),
                         (u16)(right->rect.y + 2U), 0U);

        yew_mouse_event(&ed, &w);
    }
    /* The UNFOCUSED pane moved, by exactly one notch. */
    YEW_ASSERT_EQ_U64(right->win->vp.top.v, (u64)YEW_WHEEL_ROWS);
    /* And nothing else did. */
    YEW_ASSERT(ed.focus == left);
    YEW_ASSERT_EQ_U64(left->win->vp.top.v, left_top_before.v);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, cursor_before.v);
    yew_ed_free(&ed);
}

/*
 * A notch is exactly YEW_WHEEL_ROWS rows, forever: no acceleration, no
 * momentum, no fractional accumulation.  Deterministic (invariant 5)
 * and therefore assertable.
 */
void test_mouse_wheel_has_no_acceleration(void)
{
    Ed ed;
    i32 leaf;
    u32 i;

    ms_fixture(&ed);
    ms_fill_lines(&ed, 200U);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);
    for (i = 1U; i <= 10U; i++) {
        Key w = ms_wheel((u8)YEW_MB_WHEEL_DOWN, 10U, 5U, 0U);

        yew_mouse_event(&ed, &w);
        YEW_ASSERT_EQ_U64(ed.win->vp.top.v, (u64)(i * YEW_WHEEL_ROWS));
    }
    /* Symmetric on the way back, and it stops at the top rather than
     * running negative. */
    for (i = 0U; i < 20U; i++) {
        Key w = ms_wheel((u8)YEW_MB_WHEEL_UP, 10U, 5U, 0U);

        yew_mouse_event(&ed, &w);
    }
    YEW_ASSERT_EQ_U64(ed.win->vp.top.v, 0U);
    yew_ed_free(&ed);
}

/*
 * Ctrl+wheel is the terminal emulator's font-size gesture.  Stealing it
 * would fight the host application over a key the user does not think
 * of as ours, so it is deliberately unbound.
 */
void test_mouse_ctrl_wheel_is_unbound(void)
{
    Ed ed;
    i32 leaf;

    ms_fixture(&ed);
    ms_fill_lines(&ed, 60U);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);
    {
        Key w = ms_wheel((u8)YEW_MB_WHEEL_DOWN, 10U, 5U,
                         (u16)YEW_MOD_CTRL);

        yew_mouse_event(&ed, &w);
    }
    YEW_ASSERT_EQ_U64(ed.win->vp.top.v, 0U);
    yew_ed_free(&ed);
}

void test_mouse_shift_wheel_scrolls_sideways_and_not_when_wrapped(void)
{
    Ed ed;
    i32 leaf;

    ms_fixture(&ed);
    {
        EditCtx ec = yew_ed_edit_ctx(&ed);
        char wide[400];

        (void)memset(wide, 'x', sizeof(wide) - 1U);
        wide[sizeof(wide) - 1U] = '\0';
        yew_edit_insert(&ec, yew_ed_cursor(&ed)->pos, (const u8 *)wide,
                        strlen(wide));
        yew_ed_finish_edit(&ed, &ec);
    }
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);

    ed.win->vp.wrap = false;
    {
        Key w = ms_wheel((u8)YEW_MB_WHEEL_DOWN, 10U, 5U,
                         (u16)YEW_MOD_SHIFT);

        yew_mouse_event(&ed, &w);
        YEW_ASSERT_EQ_U64(ed.win->vp.left.v, (u64)YEW_WHEEL_COLS);
        yew_mouse_event(&ed, &w);
        YEW_ASSERT_EQ_U64(ed.win->vp.left.v, (u64)(2U * YEW_WHEEL_COLS));
    }
    {
        Key w = ms_wheel((u8)YEW_MB_WHEEL_UP, 10U, 5U,
                         (u16)YEW_MOD_SHIFT);

        yew_mouse_event(&ed, &w);
        YEW_ASSERT_EQ_U64(ed.win->vp.left.v, (u64)YEW_WHEEL_COLS);
        yew_mouse_event(&ed, &w);
        YEW_ASSERT_EQ_U64(ed.win->vp.left.v, 0U);
        /* It stops at column 0 rather than running negative. */
        yew_mouse_event(&ed, &w);
        YEW_ASSERT_EQ_U64(ed.win->vp.left.v, 0U);
    }

    /*
     * Wrapped, there is nowhere to go — and the no-op is SILENT,
     * because a wheel is not a command that can fail and a message per
     * notch would be noise.
     */
    ed.win->vp.wrap = true;
    ed.win->vp.left = (CCol){0U};
    yew_msg_clear(&ed);
    {
        Key w = ms_wheel((u8)YEW_MB_WHEEL_DOWN, 10U, 5U,
                         (u16)YEW_MOD_SHIFT);

        yew_mouse_event(&ed, &w);
    }
    YEW_ASSERT_EQ_U64(ed.win->vp.left.v, 0U);
    YEW_ASSERT(!ed.msg.active);
    yew_ed_free(&ed);
}

void test_mouse_wheel_over_the_strip_scrolls_the_strip(void)
{
    Ed ed;

    int i;

    ms_fixture(&ed);
    for (i = 0; i < 8; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-mouse-w%d.txt", i);
        YEW_ASSERT(yew_tab_open(&ed, path) >= 0);
    }
    ed.tabs.scroll = 4;
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){0U, 0U, 10U, 1U}, 0);
    {
        Key w = ms_wheel((u8)YEW_MB_WHEEL_UP, 2U, 0U, 0U);

        yew_mouse_event(&ed, &w);
    }
    /* One ENTRY, not YEW_WHEEL_ROWS of them: the strip's unit is a tab,
     * and three tabs per notch would overshoot on every bar that fits. */
    YEW_ASSERT_EQ_I64(ed.tabs.scroll, 3);
    {
        Key w = ms_wheel((u8)YEW_MB_WHEEL_DOWN, 2U, 0U, 0U);

        yew_mouse_event(&ed, &w);
    }
    YEW_ASSERT_EQ_I64(ed.tabs.scroll, 4);
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* The captured-identity law (DoD 4)                                */
/* ---------------------------------------------------------------- */

/*
 * THE law, made executable.
 *
 * The strip is rebuilt between the press and the release so that the
 * cells the pointer is over now name a DIFFERENT tab.  The click must
 * still land on the tab that was under the pointer when the button went
 * down — that is the only reading of the gesture the user could have
 * meant, and re-resolving at release is how every naive implementation
 * gets it wrong.
 */
void test_mouse_press_captures_its_target_across_a_strip_change(void)
{
    Ed ed;
    u32 target_id;
    int i;

    ms_fixture(&ed);
    for (i = 0; i < 4; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-mouse-%d.txt", i);
        YEW_ASSERT(yew_tab_open(&ed, path) >= 0);
    }
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 5U);
    target_id = yew_tab_at(&ed, 3)->tab_id;
    yew_tab_switch(&ed, 0);

    /* Frame A: tab 3 occupies cells 20..30 on row 0. */
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){20U, 0U, 10U, 1U}, 3);
    {
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, 22U, 0U);

        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT_EQ_U64(ed.mouse.drag_tab_id, target_id);

    /* Frame B: the strip scrolled; those same cells are now tab 1. */
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){20U, 0U, 10U, 1U}, 1);
    {
        Key up = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE, 22U, 0U);

        yew_mouse_event(&ed, &up);
    }
    /* The tab the user AIMED at, resolved by id. */
    YEW_ASSERT_EQ_I64(ed.tabs.active, yew_tab_index_of_id(&ed, target_id));
    yew_ed_free(&ed);
}

/*
 * And the same law against the harder case: the captured tab is CLOSED
 * mid-gesture, so its id resolves to nothing.  The release must do
 * nothing at all rather than fall back to an index — the index would
 * name whichever tab slid into the vacated slot.
 */
void test_mouse_press_on_a_tab_closed_mid_gesture_is_inert(void)
{
    Ed ed;
    u32 target_id;
    int active_before;
    int i;

    ms_fixture(&ed);
    for (i = 0; i < 4; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-mouse-c%d.txt", i);
        YEW_ASSERT(yew_tab_open(&ed, path) >= 0);
    }
    target_id = yew_tab_at(&ed, 3)->tab_id;
    yew_tab_switch(&ed, 0);
    active_before = ed.tabs.active;

    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){20U, 0U, 10U, 1U}, 3);
    {
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, 22U, 0U);

        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT(yew_tab_close(&ed, yew_tab_index_of_id(&ed, target_id)));
    yew_tab_switch(&ed, active_before);
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, target_id), -1);
    {
        Key up = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE, 22U, 0U);

        yew_mouse_event(&ed, &up);
    }
    YEW_ASSERT_EQ_I64(ed.tabs.active, active_before);
    yew_ed_free(&ed);
}

/*
 * A group entry's press captures the GID, and the release enters that
 * group even though the region table has since renamed those cells.
 */
void test_mouse_press_on_a_group_entry_captures_the_gid(void)
{
    Ed ed;
    u32 g;
    int i;

    ms_fixture(&ed);
    for (i = 0; i < 3; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-mouse-g%d.txt", i);
        YEW_ASSERT(yew_tab_open(&ed, path) >= 0);
    }
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 2);
    yew_group_add_member(&ed, g, 3);
    yew_tab_switch(&ed, 0);

    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){0U, 0U, 8U, 1U}, -(i32)g);
    {
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, 2U, 0U);

        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT_EQ_U64(ed.mouse.drag_gid, g);
    YEW_ASSERT_EQ_U64(ed.mouse.drag_tab_id, 0U);
    /* Nothing has happened yet: a press on a tab entry arms, it does
     * not switch.  Switching on press and then dragging would leave a
     * tab activated that the user only meant to move. */
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), 0U);

    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){0U, 0U, 8U, 1U}, 1);
    {
        Key up = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE, 2U, 0U);

        yew_mouse_event(&ed, &up);
    }
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), g);
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* §9: the deferrals, proved inert                                  */
/* ---------------------------------------------------------------- */

/*
 * Dragging a tab into a PANE to open it there is post-1.0.  A release
 * over a pane must therefore CANCEL — not open the file there, and not
 * silently reorder the strip on the way, which is the failure the
 * "nothing moves until the drop" law was written against.
 *
 * Named as a test rather than as a comment so the day somebody wires it
 * up, they have to delete an assertion and think about it.
 */
void test_mouse_tab_dropped_on_a_pane_cancels(void)
{
    Ed ed;
    u32 held;
    int at_press;
    i32 leaf;
    int i;

    ms_fixture(&ed);
    for (i = 0; i < 4; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-mouse-d%d.txt", i);
        YEW_ASSERT(yew_tab_open(&ed, path) >= 0);
    }
    yew_tab_switch(&ed, 0);
    yew_ed_layout(&ed);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    yew_region_frame_begin();
    yew_tab_strip_draw(&ed, ed.tab_strip_rect);
    yew_region_add(YEW_REGION_PANE, ed.pane_root->rect, leaf);

    held = yew_tab_at(&ed, 0)->tab_id;
    at_press = 0;
    {
        u16 x;
        Key press;
        Key motion;
        Key up;

        for (x = 0U; x < 80U; x++) {
            if (yew_strip_slot_at(x, 0U) == 0)
                break;
        }
        YEW_ASSERT(x < 80U);
        press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, x, 0U);
        motion = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_REPEAT, 40U,
                       (u16)(ed.pane_root->rect.y + 5U));
        up = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE, 40U,
                   (u16)(ed.pane_root->rect.y + 5U));
        yew_mouse_event(&ed, &press);
        yew_mouse_event(&ed, &motion);
        yew_mouse_event(&ed, &up);
    }
    /* Not moved, not opened anywhere, and the gesture is over. */
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, held), at_press);
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
    yew_ed_free(&ed);
}

/*
 * Sprint 57.13 §1 rewrote the Sprint 4 fact this test used to pin.
 * Motion with NO button held (SGR base 35, mode 1003) IS decoded now —
 * a mouse REPEAT carrying YEW_MB_NONE — because an open context menu
 * needs it to highlight the row under the pointer.  The router drops it
 * whenever no menu is open: a pointer merely crossing the screen must
 * still produce no cursor motion, no gesture, and no render.  (The
 * menu-open hover half of the contract is pinned beside the widget.)
 */
void test_mouse_hover_without_a_button_is_not_an_event(void)
{
    In in;
    TtyCaps caps;
    Key key;
    Ed ed;
    ByteOff cursor_before;
    i32 leaf;

    (void)memset(&caps, 0, sizeof(caps));
    yew_input_init(&in, &caps);
    /* cb 35 == motion with no button held, which is what a terminal
     * sends under 1003 while the pointer is simply moving. */
    yew_input_feed(&in, (const u8 *)"\x1b[<35;10;5M", 11U);
    YEW_ASSERT(yew_input_next(&in, 0, &key));
    YEW_ASSERT_EQ_U64((u64)key.kind, (u64)YEW_EV_MOUSE);
    YEW_ASSERT_EQ_U64(key.button, (u64)YEW_MB_NONE);
    YEW_ASSERT_EQ_U64(key.ev, (u64)YEW_KEY_REPEAT);
    YEW_ASSERT_EQ_U64(key.col, 9U);
    YEW_ASSERT_EQ_U64(key.row, 4U);
    YEW_ASSERT_EQ_U64(key.mods, 0U);
    /* And a held-button motion still decodes as before, so the shape
     * above is a deliberate distinction rather than a broken feed. */
    yew_input_feed(&in, (const u8 *)"\x1b[<32;10;5M", 11U);
    YEW_ASSERT(yew_input_next(&in, 0, &key));
    YEW_ASSERT_EQ_U64((u64)key.kind, (u64)YEW_EV_MOUSE);
    YEW_ASSERT_EQ_U64(key.button, (u64)YEW_MB_LEFT);
    YEW_ASSERT_EQ_U64(key.ev, (u64)YEW_KEY_REPEAT);
    yew_input_free(&in);

    /* The router, with no menu open, drops the no-button motion on the
     * floor: no phase change, no cursor change, no damage marked. */
    ms_fixture(&ed);
    ms_fill_lines(&ed, 20U);
    yew_ed_layout(&ed);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, ed.pane_root->rect, leaf);
    ed.full_damage = false;
    ed.layout_dirty = false;
    cursor_before = yew_ed_cursor(&ed)->pos;
    YEW_ASSERT(!yew_ctx_active());
    {
        Key hover = ms_ev((u8)YEW_MB_NONE, (u8)YEW_KEY_REPEAT,
                          (u16)(ed.pane_root->rect.x + 5U),
                          (u16)(ed.pane_root->rect.y + 5U));

        yew_mouse_event(&ed, &hover);
        hover.mods = YEW_MOD_CTRL;
        yew_mouse_event(&ed, &hover);
    }
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, cursor_before.v);
    YEW_ASSERT(!ed.full_damage);
    YEW_ASSERT(!ed.layout_dirty);
    YEW_ASSERT(!yew_ctx_active());
    yew_ed_free(&ed);
}

/*
 * DoD 2, made executable rather than left as a grep somebody ran once.
 *
 * Every mouse event enters through yew_mouse_event.  The decoder
 * produces them, the router consumes them, and the event loop's single
 * `case YEW_EV_MOUSE:` hands one to the other — nothing else in the
 * program may turn a pointer event into an action, because a second
 * route is how a click comes to mean two different things depending on
 * which handler saw it first.
 */
void test_mouse_the_router_is_the_only_dispatch_site(void)
{
    static const char *const allowed[] = {
        "src/term/input.c", /* produces them */
        "src/ui/mouse.c"    /* consumes them */
    };
    static const char *const dirs[] = {
        "src", "src/edit", "src/ui", "src/term", "src/text", "src/search",
        "src/ws", "src/util", "src/unicode"
    };
    u32 handoffs = 0U;
    size_t d;

    for (d = 0U; d < YEW_ARRAY_LEN(dirs); d++) {
        DIR *dir = opendir(dirs[d]);
        struct dirent *e;

        if (dir == NULL)
            continue;
        while ((e = readdir(dir)) != NULL) {
            /* Wide enough for the longest dir plus the longest name;
             * the sanitizer lane's -Wformat-truncation cannot prove it
             * from the loop bounds, so the buffer says it instead. */
            char path[512];
            FILE *f;
            char line[512];
            size_t i;
            bool skip = false;
            size_t n = strlen(e->d_name);

            if (n < 3U || strcmp(e->d_name + n - 2U, ".c") != 0)
                continue;
            (void)snprintf(path, sizeof(path), "%s/%s", dirs[d],
                           e->d_name);
            for (i = 0U; i < YEW_ARRAY_LEN(allowed); i++) {
                if (strcmp(path, allowed[i]) == 0)
                    skip = true;
            }
            if (skip)
                continue;
            f = fopen(path, "r");
            if (f == NULL)
                continue;
            while (fgets(line, sizeof(line), f) != NULL) {
                if (strstr(line, "YEW_EV_MOUSE") == NULL)
                    continue;
                /*
                 * The ONE legal mention outside those two files: the
                 * loop's hand-off.  Anything else is a second route.
                 */
                YEW_ASSERT_EQ_STR(path, "src/edit/loop.c");
                YEW_ASSERT_NOT_NULL(strstr(line, "case YEW_EV_MOUSE:"));
                handoffs++;
            }
            (void)fclose(f);
        }
        (void)closedir(dir);
    }
    /* Exactly one, and the walk actually found it — a check that
     * scanned nothing would pass silently. */
    YEW_ASSERT_EQ_U64(handoffs, 1U);
}

/* ---------------------------------------------------------------- */
/* §2: the overlay rows                                             */
/* ---------------------------------------------------------------- */

static const PickItem *ms_pick_items(void *ctx, u32 *n)
{
    static const PickItem items[3] = {
        {"alpha", NULL, 10, 0U},
        {"beta", NULL, 20, 0U},
        {"gamma", NULL, 30, 0U}
    };

    (void)ctx;
    *n = 3U;
    return items;
}

static i32 ms_pick_accepted;

static bool ms_pick_accept(Ed *ed, void *ctx, i32 payload, u8 how)
{
    (void)ed;
    (void)ctx;
    (void)how;
    ms_pick_accepted = payload;
    return true;
}

/*
 * YEW_REGION_PICK_ROW: a press SELECTS and a release ACCEPTS, but only
 * when the release lands on the same row.  A press that slid onto a
 * neighbour before coming up was a mis-aim, and opening the neighbour
 * is the worst available reading of it.
 */
void test_mouse_pick_row_selects_then_accepts_the_same_row(void)
{
    Ed ed;
    PickerSpec spec;

    ms_fixture(&ed);
    (void)memset(&spec, 0, sizeof(spec));
    spec.title = "Pick";
    spec.items = ms_pick_items;
    spec.accept = ms_pick_accept;
    ms_pick_accepted = -1;
    yew_picker_open(&ed, &spec);
    YEW_ASSERT(yew_picker_active(&ed));

    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PICK_ROW, (Rect){10U, 5U, 40U, 1U}, 10);
    yew_region_add(YEW_REGION_PICK_ROW, (Rect){10U, 6U, 40U, 1U}, 20);
    {
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, 12U, 6U);
        Key elsewhere = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE,
                              12U, 5U);

        yew_mouse_event(&ed, &press);
        /* Selected by PAYLOAD, so a re-rank between press and release
         * cannot slide it onto a neighbour. */
        YEW_ASSERT_EQ_I64(yew_picker_selected(&ed), 20);
        /* Released on a DIFFERENT row: nothing is accepted. */
        yew_mouse_event(&ed, &elsewhere);
        YEW_ASSERT_EQ_I64(ms_pick_accepted, -1);
        YEW_ASSERT(yew_picker_active(&ed));
    }
    {
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, 12U, 6U);
        Key up = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE, 30U, 6U);

        yew_mouse_event(&ed, &press);
        yew_mouse_event(&ed, &up);
    }
    YEW_ASSERT_EQ_I64(ms_pick_accepted, 20);
    YEW_ASSERT(!yew_picker_active(&ed));
    yew_picker_close(&ed, false);
    yew_ed_free(&ed);
}

/* And the wheel over the list moves by YEW_WHEEL_ROWS, without
 * accepting anything. */
void test_mouse_wheel_over_a_pick_row_scrolls_the_list(void)
{
    Ed ed;
    PickerSpec spec;

    ms_fixture(&ed);
    (void)memset(&spec, 0, sizeof(spec));
    spec.title = "Pick";
    spec.items = ms_pick_items;
    spec.accept = ms_pick_accept;
    ms_pick_accepted = -1;
    yew_picker_open(&ed, &spec);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PICK_ROW, (Rect){10U, 5U, 40U, 1U}, 10);
    {
        Key w = ms_wheel((u8)YEW_MB_WHEEL_DOWN, 12U, 5U, 0U);

        YEW_ASSERT_EQ_I64(yew_picker_selected(&ed), 10);
        yew_mouse_event(&ed, &w);
        /* Three rows down from row 0 clamps at the last row of three. */
        YEW_ASSERT_EQ_I64(yew_picker_selected(&ed), 30);
        YEW_ASSERT_EQ_I64(ms_pick_accepted, -1);
    }
    yew_picker_close(&ed, false);
    yew_ed_free(&ed);
}

/*
 * YEW_REGION_GP_ROW / _GP_NAME: the group picker's rows.  The dialog is
 * modal, so the press reaches it and nothing under it — its own BLOCK
 * is what guarantees the second half, and this row proves the first.
 */
void test_mouse_gp_rows_reach_the_group_picker(void)
{
    Ed ed;

    ms_fixture(&ed);
    YEW_ASSERT(yew_gp_show(&ed, "/tmp"));
    YEW_ASSERT(yew_gp_active());
    yew_region_frame_begin();
    yew_gp_draw(&ed);
    /* The dialog registered its own rows; find one and click it. */
    {
        u16 y;
        bool found = false;

        for (y = 0U; y < 24U && !found; y++) {
            Region hit = yew_region_hit(20U, y);

            if (hit.kind != YEW_REGION_GP_ROW)
                continue;
            found = true;
            {
                Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS,
                                  20U, y);
                Key w = ms_wheel((u8)YEW_MB_WHEEL_DOWN, 20U, y, 0U);

                yew_mouse_event(&ed, &press);
                YEW_ASSERT(yew_gp_active());
                /* The wheel is claimed too, and does not close it. */
                yew_mouse_event(&ed, &w);
                YEW_ASSERT(yew_gp_active());
            }
        }
        YEW_ASSERT(found);
    }
    yew_gp_close(&ed);
    yew_ed_free(&ed);
}

/*
 * YEW_REGION_CTX_ROW: a press HIGHLIGHTS and a release INVOKES — and,
 * as with the picker, only when the release is on the row the press
 * captured.
 */
void test_mouse_ctx_row_highlights_then_invokes(void)
{
    Ed ed;
    u32 target;
    int i;

    ms_fixture(&ed);
    for (i = 0; i < 3; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-mouse-x%d.txt", i);
        YEW_ASSERT(yew_tab_open(&ed, path) >= 0);
    }
    yew_tab_switch(&ed, 1);
    target = yew_tab_at(&ed, 1)->tab_id;
    yew_ed_layout(&ed);
    YEW_ASSERT(yew_mouse_open_tab_menu(&ed, target, 0U, 2U));
    yew_region_frame_begin();
    yew_mouse_menu_draw(&ed);
    {
        Rect box = yew_ctx_box();
        /* Rows start one cell inside the frame: row i is at
         * box.y + 1 + i, and the border is nobody's row. */
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS,
                          (u16)(box.x + 1U), (u16)(box.y + 4U));
        Key wrong = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE,
                          (u16)(box.x + 1U), (u16)(box.y + 1U));

        yew_mouse_event(&ed, &press);
        /* Row 3 is `Copy Path`; the press highlighted it. */
        YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 3);
        YEW_ASSERT(yew_ctx_active());
        /* Released on row 0 instead: nothing is invoked, and the menu
         * stays up rather than acting on the row the pointer drifted
         * onto. */
        yew_mouse_event(&ed, &wrong);
        YEW_ASSERT(yew_ctx_active());
    }
    {
        Rect box = yew_ctx_box();
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS,
                          (u16)(box.x + 1U), (u16)(box.y + 4U));
        Key up = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE,
                       (u16)(box.x + 2U), (u16)(box.y + 4U));

        yew_mouse_event(&ed, &press);
        yew_mouse_event(&ed, &up);
    }
    YEW_ASSERT(!yew_ctx_active());
    {
        RegVal *v = yew_reg_get(&ed.regs, (u8)'+');

        YEW_ASSERT_NOT_NULL(v);
        YEW_ASSERT(v->bytes.len != 0U);
    }
    yew_ctx_close();
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* Sprint 57.13 §3: context resolution, opening, hover, dispatch     */
/* ---------------------------------------------------------------- */

/*
 * ONE PURE FUNCTION, asked directly.
 *
 * Every surface's menu comes from this answer, so testing it here is
 * cheaper and far more complete than opening fifteen menus and reading
 * their rows: a kind that resolved wrong would otherwise only show up
 * as "right-click over the drawer gives me the tab menu", months later
 * and from a user.
 */
void test_mouse_context_at_names_every_region_kind(void)
{
    Ed ed;
    i32 leaf;
    u32 tab_id;
    u32 g;

    ms_fixture(&ed);
    YEW_ASSERT(yew_tab_open(&ed, "/tmp/yew-mouse-ctx0.txt") >= 0);
    YEW_ASSERT(yew_tab_open(&ed, "/tmp/yew-mouse-ctx1.txt") >= 0);
    tab_id = yew_tab_at(&ed, 1)->tab_id;
    g = yew_group_create(&ed, "/src", NULL);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);

    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, (Rect){0U, 2U, 40U, 10U}, leaf);
    yew_region_add(YEW_REGION_PANE_BORDER, (Rect){40U, 2U, 1U, 10U}, 0);
    yew_region_add(YEW_REGION_TAB, (Rect){0U, 0U, 8U, 1U}, 1);
    yew_region_add(YEW_REGION_TAB, (Rect){8U, 0U, 8U, 1U}, -(i32)g);
    yew_region_add(YEW_REGION_TAB_SCROLL, (Rect){16U, 0U, 2U, 1U}, 1);
    yew_region_add(YEW_REGION_TAB_NEW, (Rect){18U, 0U, 3U, 1U}, 0);
    yew_region_add(YEW_REGION_FUSS_ROW, (Rect){41U, 2U, 20U, 1U}, 7);
    yew_region_add(YEW_REGION_PICK_ROW, (Rect){41U, 3U, 20U, 1U}, 42);
    yew_region_add(YEW_REGION_COMPL_ROW, (Rect){41U, 4U, 20U, 1U}, 5);
    yew_region_add(YEW_REGION_GP_ROW, (Rect){41U, 5U, 20U, 1U}, 2);
    yew_region_add(YEW_REGION_GP_NAME, (Rect){41U, 6U, 20U, 1U}, 0);
    yew_region_add(YEW_REGION_BLOCK, (Rect){41U, 7U, 20U, 1U}, 0);

    {
        CtxContext c = yew_mouse_context_at(&ed, 5U, 5U);

        YEW_ASSERT_EQ_U64((u64)c.kind, (u64)YEW_CTX_KIND_DOC);
        /* The LEAF INDEX, so a row can focus the pane that was pointed
         * at rather than the one that happens to be focused. */
        YEW_ASSERT_EQ_U64(c.id, (u64)leaf);
    }
    YEW_ASSERT_EQ_U64((u64)yew_mouse_context_at(&ed, 40U, 5U).kind,
                      (u64)YEW_CTX_KIND_BORDER);
    {
        CtxContext c = yew_mouse_context_at(&ed, 2U, 0U);

        /* IDENTITY, not the payload index: the strip renumbers. */
        YEW_ASSERT_EQ_U64((u64)c.kind, (u64)YEW_CTX_KIND_TAB);
        YEW_ASSERT_EQ_U64(c.id, tab_id);
    }
    {
        CtxContext c = yew_mouse_context_at(&ed, 10U, 0U);

        YEW_ASSERT_EQ_U64((u64)c.kind, (u64)YEW_CTX_KIND_GROUP);
        YEW_ASSERT_EQ_U64(c.id, g);
    }
    YEW_ASSERT_EQ_U64((u64)yew_mouse_context_at(&ed, 17U, 0U).kind,
                      (u64)YEW_CTX_KIND_STRIP);
    YEW_ASSERT_EQ_U64((u64)yew_mouse_context_at(&ed, 19U, 0U).kind,
                      (u64)YEW_CTX_KIND_STRIP);
    /* The tree cannot say what path 7 is — F mode is not up — and an
     * UNKNOWN row is the drawer, not a guess at file or directory. */
    YEW_ASSERT_EQ_U64((u64)yew_mouse_context_at(&ed, 45U, 2U).kind,
                      (u64)YEW_CTX_KIND_FUSS_BLANK);
    {
        CtxContext c = yew_mouse_context_at(&ed, 45U, 3U);

        YEW_ASSERT_EQ_U64((u64)c.kind, (u64)YEW_CTX_KIND_PICK_ROW);
        YEW_ASSERT_EQ_I64(c.payload, 42);
    }
    YEW_ASSERT_EQ_U64((u64)yew_mouse_context_at(&ed, 45U, 4U).kind,
                      (u64)YEW_CTX_KIND_COMPL_ROW);
    YEW_ASSERT_EQ_U64((u64)yew_mouse_context_at(&ed, 45U, 5U).kind,
                      (u64)YEW_CTX_KIND_GP_ROW);
    YEW_ASSERT_EQ_U64((u64)yew_mouse_context_at(&ed, 45U, 6U).kind,
                      (u64)YEW_CTX_KIND_GP);
    /* A BLOCK with no modal open belongs to nobody, and the editor
     * menu is the honest answer rather than nothing at all. */
    YEW_ASSERT_EQ_U64((u64)yew_mouse_context_at(&ed, 45U, 7U).kind,
                      (u64)YEW_CTX_KIND_EDITOR);
    /* A BLOCK the PANEL owns is the panel's, and only inside its
     * rectangle — it is the one overlay that can be open beside
     * another. */
    ed.win->panel.open = true;
    ed.win->panel.rect = (Rect){41U, 7U, 20U, 1U};
    YEW_ASSERT_EQ_U64((u64)yew_mouse_context_at(&ed, 45U, 7U).kind,
                      (u64)YEW_CTX_KIND_PANEL);
    ed.win->panel.open = false;
    yew_ed_free(&ed);
}

/*
 * A cell no region claimed falls through to GEOMETRY, in the order the
 * sprint fixes.  This is the half that makes "right-click on the bare
 * backdrop opens something" true — and the half that would silently
 * regress to EDITOR everywhere if the rectangles were read in the
 * wrong order.
 */
void test_mouse_context_at_falls_through_to_geometry(void)
{
    Ed ed;

    ms_fixture(&ed);
    yew_region_frame_begin();
    /* One region, far away, so the table is not empty (an empty table
     * is its own diagnosed bug) and claims none of the cells below. */
    yew_region_add(YEW_REGION_BLOCK, (Rect){79U, 23U, 1U, 1U}, 0);

    YEW_ASSERT(ed.footer_rect.h != 0U);
    YEW_ASSERT_EQ_U64((u64)yew_mouse_context_at(&ed, ed.footer_rect.x,
                                                ed.footer_rect.y).kind,
                      (u64)YEW_CTX_KIND_FOOTER);
    YEW_ASSERT(ed.tab_strip_rect.h != 0U);
    YEW_ASSERT_EQ_U64(
        (u64)yew_mouse_context_at(&ed, ed.tab_strip_rect.x,
                                  ed.tab_strip_rect.y).kind,
        (u64)YEW_CTX_KIND_STRIP);
    /* Everything else is the editor — never NONE, which is reserved
     * for the open menu's own cells. */
    YEW_ASSERT_EQ_U64((u64)yew_mouse_context_at(&ed, 40U, 10U).kind,
                      (u64)YEW_CTX_KIND_EDITOR);
    /*
     * THE TRAP, pinned: the drawer's rectangle is derived from the
     * terminal width alone and is NON-empty with F mode down, so the
     * fall-through has to ask whether the drawer is actually up.  An
     * ungated read would hand the left quarter of every screen to a
     * drawer menu.  (Under the shim the rectangle is empty and this
     * cell resolves the same way for the other reason.)
     */
    YEW_ASSERT(!yew_fuss_active(&ed));
    YEW_ASSERT_EQ_U64((u64)yew_mouse_context_at(&ed, 1U, 10U).kind,
                      (u64)YEW_CTX_KIND_EDITOR);
    yew_ed_free(&ed);
}

/*
 * CTRL+LEFT OPENS, AND NEVER ARMS.
 *
 * The second half is the one that matters: a ctrl+click that reached
 * the phase machine would arm a drag and, on the first motion report,
 * enter H mode and start a selection BEHIND the menu it just opened.
 * The user would close the menu and find the buffer highlighted.
 */
void test_mouse_ctrl_left_opens_a_menu_and_never_arms(void)
{
    Ed ed;
    i32 leaf;
    ByteOff before;

    ms_fixture(&ed);
    ms_fill_lines(&ed, 20U);
    yew_ed_layout(&ed);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);
    before = yew_ed_cursor(&ed)->pos;
    {
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS,
                          (u16)(ed.pane_root->rect.x + 4U),
                          (u16)(ed.pane_root->rect.y + 3U));
        Key motion = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_REPEAT,
                           (u16)(ed.pane_root->rect.x + 9U),
                           (u16)(ed.pane_root->rect.y + 5U));

        press.mods = (u16)YEW_MOD_CTRL;
        motion.mods = (u16)YEW_MOD_CTRL;
        yew_mouse_event(&ed, &press);
        YEW_ASSERT(yew_ctx_active());
        YEW_ASSERT_EQ_U64(yew_ctx_kind(), (u64)YEW_CTX_KIND_DOC);
        YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
        YEW_ASSERT_EQ_U64(ed.mouse.held, 0U);
        /* A held-button motion after it finds nothing armed, so no
         * selection begins and the mode is untouched. */
        yew_mouse_event(&ed, &motion);
        YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
        YEW_ASSERT_EQ_U64((u64)ed.mode, (u64)YEW_MODE_L);
        YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, before.v);
    }
    yew_ctx_close();
    yew_tty_mouse_motion(false);
    yew_ed_free(&ed);
}

/*
 * A right press ON the open menu DISMISSES it and stops there.
 *
 * Anywhere else it closes and REOPENS for what is really under the
 * pointer — which is what every menu everywhere does, and what makes
 * "I right-clicked the wrong thing" cost one gesture instead of two.
 */
void test_mouse_right_press_on_an_open_menu_only_closes_it(void)
{
    Ed ed;
    i32 leaf;
    Rect box;

    ms_fixture(&ed);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, ed.pane_root->rect, leaf);
    yew_region_add(YEW_REGION_TAB, (Rect){0U, 0U, 8U, 1U}, 0);
    {
        Key press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS, 10U, 5U);

        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT(yew_ctx_active());
    box = yew_ctx_box();
    yew_mouse_menu_draw(&ed);
    {
        /* Inside the box, on a row: dismissed, nothing invoked. */
        Key press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS,
                          (u16)(box.x + 1U), (u16)(box.y + 1U));

        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT(!yew_ctx_active());
    YEW_ASSERT(!yew_tty_mouse_motion_active());

    /* And a right press somewhere else closes AND reopens, for the
     * thing that is actually there. */
    {
        Key press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS, 10U, 5U);

        yew_mouse_event(&ed, &press);
        YEW_ASSERT(yew_ctx_active());
        press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS, 2U, 0U);
        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT(yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_kind(), (u64)YEW_CTX_KIND_TAB);
    yew_ctx_close();
    yew_tty_mouse_motion(false);
    yew_ed_free(&ed);
}

/*
 * A WHEEL DISMISSES THE MENU AND IS THEN ROUTED NORMALLY.
 *
 * Scrolling the thing under a pop-up while the pop-up stays put is how
 * a menu comes to name a row that has moved: the capture-at-open law
 * says the target is fixed, and the only honest answer to "the view
 * moved" is that the menu is gone.
 */
void test_mouse_wheel_closes_the_menu_then_scrolls(void)
{
    Ed ed;
    i32 leaf;
    LineNo top_before;

    ms_fixture(&ed);
    ms_fill_lines(&ed, 200U);
    yew_ed_layout(&ed);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);
    {
        Key press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS,
                          (u16)(ed.pane_root->rect.x + 2U),
                          (u16)(ed.pane_root->rect.y + 2U));

        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT(yew_ctx_active());
    YEW_ASSERT(yew_tty_mouse_motion_active());
    top_before = yew_win_view_top(ed.win);
    {
        Key wheel = ms_wheel((u8)YEW_MB_WHEEL_UP,
                             (u16)(ed.pane_root->rect.x + 2U),
                             (u16)(ed.pane_root->rect.y + 6U), 0U);

        yew_mouse_event(&ed, &wheel);
    }
    YEW_ASSERT(!yew_ctx_active());
    YEW_ASSERT(!yew_tty_mouse_motion_active());
    /* Closed AND scrolled: the wheel is not swallowed by the dismissal,
     * because a notch that did nothing would read as a dropped event. */
    YEW_ASSERT(top_before.v >= YEW_WHEEL_ROWS);
    YEW_ASSERT_EQ_U64(yew_win_view_top(ed.win).v,
                      top_before.v - YEW_WHEEL_ROWS);
    yew_ed_free(&ed);
}

/*
 * HOVER REPAINTS ON A ROW CHANGE AND NEVER OTHERWISE.
 *
 * A terminal emits motion reports as fast as it can write them —
 * hundreds between two frames — and a router that marked damage per
 * report would turn a pointer crossing a menu into a slideshow on
 * exactly the machines that emit the most.  `overlay_dirty` is the
 * observable: set once per row the highlight actually moves to.
 */
void test_mouse_hover_repaints_only_when_the_row_changes(void)
{
    Ed ed;
    Rect box;
    u32 marks = 0U;
    u16 x;
    int i;

    ms_fixture(&ed);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){0U, 0U, 8U, 1U}, 0);
    YEW_ASSERT(yew_tab_open(&ed, "/tmp/yew-mouse-hover.txt") >= 0);
    {
        Key press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS, 2U, 0U);

        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT(yew_ctx_active());
    yew_mouse_menu_draw(&ed);
    box = yew_ctx_box();
    ed.full_damage = false;
    ed.overlay_dirty = false;
    /* Row 0 already has the highlight at open, so sweeping it marks
     * nothing at all. */
    for (i = 0; i < 20; i++) {
        Key hover = ms_ev((u8)YEW_MB_NONE, (u8)YEW_KEY_REPEAT,
                          (u16)(box.x + 1U + (u16)(i % 3)),
                          (u16)(box.y + 1U));

        yew_mouse_event(&ed, &hover);
        if (ed.overlay_dirty) {
            marks++;
            ed.overlay_dirty = false;
        }
    }
    YEW_ASSERT_EQ_U64(marks, 0U);
    YEW_ASSERT(!ed.full_damage);
    /* Moving to row 1 marks exactly once, however many reports the
     * terminal sends while the pointer sits there. */
    x = (u16)(box.x + 1U);
    for (i = 0; i < 20; i++) {
        Key hover = ms_ev((u8)YEW_MB_NONE, (u8)YEW_KEY_REPEAT, x,
                          (u16)(box.y + 2U));

        yew_mouse_event(&ed, &hover);
        if (ed.overlay_dirty) {
            marks++;
            ed.overlay_dirty = false;
        }
    }
    YEW_ASSERT_EQ_U64(marks, 1U);
    YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 1);
    /* And it is a MENU repaint, not a pane one. */
    YEW_ASSERT(!ed.full_damage);
    yew_ctx_close();
    yew_tty_mouse_motion(false);
    yew_ed_free(&ed);
}

/*
 * MODE 1003 MIRRORS THE MENU, on every path either can take.
 *
 * Invariant 6 in miniature: a terminal left streaming motion reports at
 * an editor with nothing to do with them is a terminal yew did not
 * restore.  The fuzz harness asserts the same thing after every event;
 * this names the individual paths so a regression says which one broke.
 */
void test_mouse_motion_tracking_mirrors_the_open_menu(void)
{
    Ed ed;
    i32 leaf;
    CmdCtx cx = {0};

    ms_fixture(&ed);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, ed.pane_root->rect, leaf);
    /*
     * From a known-disarmed start.  Sibling tests open menus through
     * yew_mouse_open_*_menu and dismiss them with yew_ctx_close(),
     * which is the WIDGET's half of a close and deliberately knows
     * nothing about the terminal; the global claim — 1003 is never
     * armed with no menu open — is the fuzz harness's, asserted after
     * every event of every session.  What this test owns is the
     * individual paths.
     */
    yew_tty_mouse_motion(false);
    YEW_ASSERT(!yew_tty_mouse_motion_active());

    /* Opened by pointer, closed by Esc. */
    {
        Key press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS, 10U, 5U);
        Key esc;

        yew_mouse_event(&ed, &press);
        YEW_ASSERT(yew_tty_mouse_motion_active());
        (void)memset(&esc, 0, sizeof(esc));
        esc.kind = (u16)YEW_EV_KEY;
        esc.code = YEW_KEY_ESCAPE;
        YEW_ASSERT(yew_mouse_menu_key(&ed, &esc));
    }
    YEW_ASSERT(!yew_ctx_active());
    YEW_ASSERT(!yew_tty_mouse_motion_active());

    /* Closed by a left press outside the box. */
    {
        Key press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS, 10U, 5U);

        yew_mouse_event(&ed, &press);
        YEW_ASSERT(yew_tty_mouse_motion_active());
    }
    yew_region_frame_begin();
    yew_mouse_menu_draw(&ed);
    {
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, 79U, 23U);

        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT(!yew_ctx_active());
    YEW_ASSERT(!yew_tty_mouse_motion_active());

    /* Closed by `ed.mouse.disable`, which must not be able to leave it
     * armed behind a router that no longer receives events. */
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, ed.pane_root->rect, leaf);
    {
        Key press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS, 10U, 5U);

        yew_mouse_event(&ed, &press);
        YEW_ASSERT(yew_tty_mouse_motion_active());
    }
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_U64((u64)yew_mouse_cmd_disable(&cx), (u64)YEW_CMD_OK);
    YEW_ASSERT(!yew_ctx_active());
    YEW_ASSERT(!yew_tty_mouse_motion_active());

    /* A keyboard menu is still reachable with the mouse disabled, but it
     * must remain keyboard-only.  Arming 1003 here and restoring 1002 on
     * close would undo YEW_MOUSE=0 at the terminal boundary. */
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, ed.pane_root->rect, leaf);
    cx.iarg = 0;
    YEW_ASSERT_EQ_U64((u64)yew_ui_cmd_context_menu(&cx),
                      (u64)YEW_CMD_OK);
    YEW_ASSERT(yew_ctx_active());
    YEW_ASSERT(!yew_tty_mouse_motion_active());
    {
        Key esc;

        (void)memset(&esc, 0, sizeof(esc));
        esc.kind = (u16)YEW_EV_KEY;
        esc.code = YEW_KEY_ESCAPE;
        YEW_ASSERT(yew_mouse_menu_key(&ed, &esc));
    }
    YEW_ASSERT(!yew_ctx_active());
    YEW_ASSERT(!yew_tty_mouse_motion_active());
    yew_mouse_set_enabled(true);
    yew_ed_free(&ed);
}

/*
 * THE ACTION TABLE APPLIES ITS TARGET before the command runs.
 *
 * Sprint 27 spent a switch per menu kind on this; the table is what
 * keeps a new kind from growing a second one somewhere else.  What is
 * pinned here is that each CtxTarget actually does its half — switch,
 * enter, focus-and-place — because a target that silently did nothing
 * would leave every row acting on whatever was already active.
 */
void test_mouse_menu_targets_switch_enter_and_focus(void)
{
    Ed ed;
    u32 target;
    u32 g;
    int i;

    ms_fixture(&ed);
    for (i = 0; i < 3; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-mouse-t%d.txt", i);
        YEW_ASSERT(yew_tab_open(&ed, path) >= 0);
    }
    target = yew_tab_at(&ed, 3)->tab_id;
    yew_tab_switch(&ed, 0);

    /* CTX_TGT_TAB: the captured tab becomes active first, so the row
     * can be the ordinary registry command. */
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){20U, 0U, 10U, 1U}, 3);
    {
        Key press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS, 22U, 0U);
        Key enter;

        yew_mouse_event(&ed, &press);
        YEW_ASSERT(yew_ctx_active());
        (void)memset(&enter, 0, sizeof(enter));
        enter.kind = (u16)YEW_EV_KEY;
        enter.code = YEW_KEY_ENTER;
        /* Row 0 is Close Tab. */
        YEW_ASSERT(yew_mouse_menu_key(&ed, &enter));
    }
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, target), -1);
    YEW_ASSERT(!yew_tty_mouse_motion_active());

    /* CTX_TGT_GROUP: the captured group is entered first. */
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 1);
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), 0U);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){0U, 0U, 8U, 1U}, -(i32)g);
    {
        Key press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS, 2U, 0U);
        Key enter;

        yew_mouse_event(&ed, &press);
        YEW_ASSERT_EQ_U64(yew_ctx_kind(), (u64)YEW_CTX_KIND_GROUP);
        (void)memset(&enter, 0, sizeof(enter));
        enter.kind = (u16)YEW_EV_KEY;
        enter.code = YEW_KEY_ENTER;
        /* Row 0 is Edit Group..., which opens the group picker. */
        YEW_ASSERT(yew_mouse_menu_key(&ed, &enter));
    }
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), g);
    if (yew_gp_active())
        yew_gp_close(&ed);
    yew_ed_free(&ed);
}

/*
 * CTX_TGT_LEAF: the rows that consume a SELECTION do not move the
 * caret.
 *
 * This is the seventh target Deliverable 4 had to add, and it exists
 * because of exactly this sequence: select some text, right-click it,
 * choose `Copy`.  With CTX_TGT_PANE the caret would be placed at the
 * clicked cell first — which sets anchor = pos — and `Copy` would yank
 * an empty span.  The row set would look perfect and the feature would
 * be dead.
 */
void test_mouse_menu_leaf_target_keeps_the_selection(void)
{
    Ed ed;
    i32 leaf;
    Cursor *c;
    const RegVal *reg;
    u32 row;
    u32 rows;
    bool found = false;

    ms_fixture(&ed);
    ms_fill_lines(&ed, 8U);
    yew_ed_layout(&ed);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);
    /* Highlight, because that is what makes a selection real: the
     * `Copy` row runs `ed.clip.copy`, which refuses outside H. */
    YEW_ASSERT_EQ_U64(yew_mode_enter_highlight(&ed, YEW_MODE_I, false),
                      YEW_CMD_OK);
    c = yew_ed_cursor(&ed);
    c->anchor = (ByteOff){0U};
    c->pos = (ByteOff){4U};
    {
        /* A right press well away from the selection: the row must act
         * on what is SELECTED, not on what is under the pointer. */
        Key press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS,
                          (u16)(ed.pane_root->rect.x + 8U),
                          (u16)(ed.pane_root->rect.y + 5U));

        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT(yew_ctx_active());
    rows = yew_ctx_rows();
    for (row = 0U; row < rows; row++)
        if (strcmp(yew_ctx_row_label(row), "Copy") == 0) {
            YEW_ASSERT(yew_ctx_row_enabled(row));
            yew_ctx_hover((i32)row);
            found = true;
            break;
        }
    YEW_ASSERT(found);
    {
        Key enter;

        (void)memset(&enter, 0, sizeof(enter));
        enter.kind = (u16)YEW_EV_KEY;
        enter.code = YEW_KEY_ENTER;
        YEW_ASSERT(yew_mouse_menu_key(&ed, &enter));
    }
    YEW_ASSERT(!yew_region_frozen());
    /* The caret is where the selection left it, NOT on the clicked
     * cell, and the SYSTEM register holds the four bytes that were
     * selected — `Copy` is `ed.clip.copy`, so `+` is the register the
     * row is about. */
    c = yew_ed_cursor(&ed);
    YEW_ASSERT_EQ_U64(c->anchor.v, 0U);
    reg = yew_reg_get(&ed.regs, (u8)'+');
    YEW_ASSERT_NOT_NULL(reg);
    YEW_ASSERT_EQ_U64(reg->bytes.len, 4U);
    YEW_ASSERT_EQ_MEM(reg->bytes.data, "line", 4U);
    yew_ctx_close();
    yew_tty_mouse_motion(false);
    yew_ed_free(&ed);
}

/*
 * CTX_TGT_PANE, through the table, with the region table FROZEN.
 *
 * The freeze is what makes the capture law enforceable, and it is also
 * the trap: yew_pane_click — the obvious way to focus a leaf and place
 * a cursor — re-resolves both from the region table and would abort.
 * The leaf index and the clicked cell are captured at open time for
 * exactly this reason, and this is the test that would catch it.
 */
void test_mouse_menu_pane_target_places_the_cursor(void)
{
    Ed ed;
    i32 leaf;
    u16 cell_x;
    u16 cell_y;
    ByteOff before;
    ByteOff want;

    ms_fixture(&ed);
    ms_fill_lines(&ed, 40U);
    yew_ed_layout(&ed);
    yew_pane_tables_reset(&ed);
    leaf = yew_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);
    cell_x = (u16)(ed.pane_root->rect.x + 6U);
    cell_y = (u16)(ed.pane_root->rect.y + 4U);
    before = yew_ed_cursor(&ed)->pos;
    /* Where the click cell IS, resolved the ordinary way so the test
     * does not re-derive the layout arithmetic it is checking. */
    yew_win_click_to_cursor(ed.win, cell_x, cell_y);
    want = yew_ed_cursor(&ed)->pos;
    YEW_ASSERT(want.v != before.v);
    yew_ed_cursor(&ed)->pos = before;
    yew_ed_cursor(&ed)->anchor = before;

    {
        Key press = ms_ev((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS, cell_x,
                          cell_y);

        yew_mouse_event(&ed, &press);
    }
    YEW_ASSERT(yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_kind(), (u64)YEW_CTX_KIND_DOC);
    {
        Key enter;
        u32 row;
        u32 rows = yew_ctx_rows();
        bool found = false;

        /*
         * The palette row, by NAME.  It is PANE targeted — focus the
         * leaf that was pointed at, caret on the cell that was clicked,
         * THEN the command — while the rows above it are LEAF targeted
         * precisely so they do not move the caret (57.13 §4), which is
         * the distinction this test exists to hold.
         */
        for (row = 0U; row < rows; row++)
            if (strcmp(yew_ctx_row_label(row), "Command Palette...") == 0) {
                yew_ctx_hover((i32)row);
                found = true;
                break;
            }
        YEW_ASSERT(found);
        (void)memset(&enter, 0, sizeof(enter));
        enter.kind = (u16)YEW_EV_KEY;
        enter.code = YEW_KEY_ENTER;
        YEW_ASSERT(yew_mouse_menu_key(&ed, &enter));
    }
    /* The freeze is balanced on the way out, whatever the row did. */
    YEW_ASSERT(!yew_region_frozen());
    YEW_ASSERT(!yew_ctx_active());
    YEW_ASSERT(!yew_tty_mouse_motion_active());
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, want.v);
    /* The palette is up, which is the command half having run. */
    YEW_ASSERT(yew_picker_active(&ed));
    yew_picker_close(&ed, false);
    yew_ed_free(&ed);
}

/*
 * CTX_TGT_PATH: the captured path travels as `cx.sarg`.
 *
 * A path-addressed row is the only kind that can act on a file the
 * editor has not opened, and the path is COPIED at open time because
 * the tree that owns the original is rebuilt by every status result.
 */
void test_mouse_menu_path_target_carries_the_captured_path(void)
{
    Ed ed;
    CtxContext c;
    u32 path_id;

    ms_fixture(&ed);
    path_id = yew_intern(&ed.interner, "src/ui/mouse.c",
                         sizeof("src/ui/mouse.c") - 1U);
    YEW_ASSERT(path_id != 0U);
    (void)memset(&c, 0, sizeof(c));
    c.kind = YEW_CTX_KIND_FUSS_FILE;
    c.id = path_id;
    c.payload = (i32)path_id;
    yew_ctx_build(&ed, &c);
    YEW_ASSERT(yew_ctx_show(2U, 2U,
                            (Rect){0U, 0U, ed.grid.cols,
                                   (u16)(ed.grid.rows - 1U)}));
    /* Captured, not aliased: the interner's copy could be freed with
     * the tree that put it there. */
    YEW_ASSERT_NOT_NULL(yew_ctx_target_path());
    YEW_ASSERT_EQ_STR(yew_ctx_target_path(), "src/ui/mouse.c");
    YEW_ASSERT_EQ_U64(yew_ctx_target_id(), path_id);
    /* Row 0 is `Open`, the one path-addressed row Deliverable 3
     * carries; Deliverable 4 fills in the rest of §4's FUSS list. */
    YEW_ASSERT(yew_ctx_row_enabled(0U));
    yew_ctx_close();
    yew_tty_mouse_motion(false);
    yew_ed_free(&ed);
}
