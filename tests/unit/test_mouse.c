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
#include "edit/pane_cmds.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/ctxmenu.h"
#include "ui/grouppicker.h"
#include "ui/mouse.h"
#include "ui/picker.h"
#include "ui/region.h"
#include "ui/tabs.h"
#include "ui/viewport.h"

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
 * §9: a right-click inside a pane is UNBOUND.  Named as a test rather
 * than as a comment so nobody quietly wires a document context menu to
 * it before Sprint 52 has decided what belongs in one.
 */
void test_mouse_right_click_in_a_pane_does_nothing(void)
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
    YEW_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)YEW_MP_IDLE);
    YEW_ASSERT_EQ_U64(ed.mouse.held, 0U);
    YEW_ASSERT(ed.focus == before);
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
 * Sprint 47's hover has nothing to misroute: motion with NO button held
 * is not decoded at all (Sprint 4 rejects the no-button report), so a
 * pointer merely crossing the screen produces no event and no work.
 */
void test_mouse_hover_without_a_button_is_not_an_event(void)
{
    In in;
    TtyCaps caps;
    Key key;
    bool got;

    (void)memset(&caps, 0, sizeof(caps));
    yew_input_init(&in, &caps);
    /* cb 35 == motion with no button held, which is what a terminal
     * sends while the pointer is simply moving. */
    yew_input_feed(&in, (const u8 *)"\x1b[<35;10;5M", 11U);
    got = yew_input_next(&in, 0, &key);
    YEW_ASSERT(!got || key.kind != (u16)YEW_EV_MOUSE);
    /* And a held-button motion IS decoded, so the test above is about
     * the no-button case rather than about a broken feed. */
    yew_input_feed(&in, (const u8 *)"\x1b[<32;10;5M", 11U);
    YEW_ASSERT(yew_input_next(&in, 0, &key));
    YEW_ASSERT_EQ_U64((u64)key.kind, (u64)YEW_EV_MOUSE);
    YEW_ASSERT_EQ_U64(key.ev, (u64)YEW_KEY_REPEAT);
    yew_input_free(&in);
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
        Key press = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS,
                          (u16)(box.x + 1U), (u16)(box.y + 3U));
        Key wrong = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE,
                          (u16)(box.x + 1U), box.y);

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
                          (u16)(box.x + 1U), (u16)(box.y + 3U));
        Key up = ms_ev((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE,
                       (u16)(box.x + 2U), (u16)(box.y + 3U));

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
