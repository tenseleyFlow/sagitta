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

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/pane_cmds.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/mouse.h"
#include "ui/region.h"
#include "ui/tabs.h"
#include "ui/viewport.h"

/* ---------------------------------------------------------------- */
/* Fixture                                                          */
/* ---------------------------------------------------------------- */

static void ms_fixture(Ed *ed)
{
    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    SAG_ASSERT(sag_grid_init(&ed->grid, &ed->interner, 24U, 80U));
    ed->grid_ready = true;
    /* The real layout, not a hand-placed rect: the viewport's row and
     * column counts come from it, and a zero-row viewport makes
     * sag_vp_clamp a no-op — which would let a wheel test pass while
     * proving nothing. */
    sag_ed_layout(ed);
    ed->now_ms = 1000;
}

static Key ms_ev(u8 button, u8 ev, u16 x, u16 y)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.kind = (u16)SAG_EV_MOUSE;
    k.button = button;
    k.ev = ev;
    k.col = x;
    k.row = y;
    return k;
}

static Key ms_wheel(u8 button, u16 x, u16 y, u16 mods)
{
    Key k = ms_ev(button, (u8)SAG_KEY_PRESS, x, y);

    k.mods = mods;
    return k;
}

/* A frame whose only region is the pane, registered with the rect the
 * layout actually gave it. */
static void ms_frame_pane(const Pane *leaf, i32 leaf_payload)
{
    sag_region_frame_begin();
    sag_region_add(SAG_REGION_PANE, leaf->rect, leaf_payload);
}

static void ms_fill_lines(Ed *ed, u32 n)
{
    u32 i;

    for (i = 0U; i < n; i++) {
        EditCtx ec = sag_ed_edit_ctx(ed);
        char line[64];

        (void)snprintf(line, sizeof(line), "line %u padding padding\n",
                       (unsigned)i);
        sag_edit_insert(&ec, sag_ed_cursor(ed)->pos, (const u8 *)line,
                        strlen(line));
        sag_ed_finish_edit(ed, &ec);
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
    sag_pane_tables_reset(&ed);
    leaf = sag_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);

    SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_IDLE);
    {
        Key press = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_PRESS, 10U, 5U);

        sag_mouse_event(&ed, &press);
    }
    SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_ARMED);
    SAG_ASSERT(ed.mouse.held != 0U);
    SAG_ASSERT_EQ_U64(ed.mouse.press_x, 10U);
    SAG_ASSERT_EQ_U64(ed.mouse.press_y, 5U);
    /* The region was CAPTURED, not merely hit-tested and thrown away. */
    SAG_ASSERT_EQ_U64((u64)ed.mouse.press_rgn.kind,
                      (u64)SAG_REGION_PANE);

    /* Motion within the pressed cell is still a click, not a drag. */
    {
        Key motion = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_REPEAT, 10U, 5U);

        sag_mouse_event(&ed, &motion);
    }
    SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_ARMED);

    /* One cell away, and it becomes a drag. */
    {
        Key motion = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_REPEAT, 11U, 5U);

        sag_mouse_event(&ed, &motion);
    }
    SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_DRAG_SEL);

    {
        Key up = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_RELEASE, 11U, 5U);

        sag_mouse_event(&ed, &up);
    }
    SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_IDLE);
    SAG_ASSERT_EQ_U64(ed.mouse.held, 0U);
    sag_ed_free(&ed);
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
    sag_pane_tables_reset(&ed);
    leaf = sag_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);

    for (i = 0U; i < 8U; i++) {
        Key w = ms_wheel(i % 2U == 0U ? (u8)SAG_MB_WHEEL_DOWN
                                      : (u8)SAG_MB_WHEEL_UP,
                         10U, 5U, 0U);

        sag_mouse_event(&ed, &w);
        SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_IDLE);
        SAG_ASSERT_EQ_U64(ed.mouse.held, 0U);
    }

    /* And mid-drag: the drag survives, the wheel changes no phase. */
    {
        Key press = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_PRESS, 10U, 5U);
        Key motion = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_REPEAT, 14U, 5U);
        Key w = ms_wheel((u8)SAG_MB_WHEEL_DOWN, 10U, 5U, 0U);

        sag_mouse_event(&ed, &press);
        sag_mouse_event(&ed, &motion);
        SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_DRAG_SEL);
        sag_mouse_event(&ed, &w);
        SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_DRAG_SEL);
        SAG_ASSERT(ed.mouse.held != 0U);
    }
    sag_ed_free(&ed);
}

void test_mouse_escape_and_focus_out_cancel_a_drag(void)
{
    Ed ed;
    i32 leaf;

    ms_fixture(&ed);
    sag_pane_tables_reset(&ed);
    leaf = sag_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);
    {
        Key press = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_PRESS, 10U, 5U);
        Key motion = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_REPEAT, 14U, 5U);

        sag_mouse_event(&ed, &press);
        sag_mouse_event(&ed, &motion);
    }
    SAG_ASSERT(sag_mouse_gesture_active(&ed));
    sag_mouse_cancel(&ed);
    SAG_ASSERT(!sag_mouse_gesture_active(&ed));
    SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_IDLE);

    /*
     * Cancelling an idle router is a no-op rather than a crash: a
     * FOCUS_OUT arrives whenever the user alt-tabs, drag or no drag.
     */
    sag_mouse_cancel(&ed);
    SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_IDLE);
    sag_ed_free(&ed);
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
    sag_pane_tables_reset(&ed);
    leaf = sag_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);
    {
        Key up = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_RELEASE, 10U, 5U);

        sag_mouse_event(&ed, &up);
    }
    SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_IDLE);
    SAG_ASSERT_EQ_U64(ed.mouse.held, 0U);
    sag_ed_free(&ed);
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
    right = sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_V);
    SAG_ASSERT_NOT_NULL(right);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 23U});
    sag_pane_tables_reset(&ed);
    left_id = sag_pane_table_add_leaf(&ed, ed.focus == right
                                               ? ed.pane_root->a
                                               : ed.focus);
    right_id = sag_pane_table_add_leaf(&ed, right);
    SAG_ASSERT(left_id >= 0 && right_id >= 0);

    ed.focus = sag_pane_leaf_by_index(&ed, left_id);
    ed.win = ed.focus->win;
    sag_region_frame_begin();
    sag_region_add(SAG_REGION_PANE, right->rect, right_id);
    {
        Key press = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_PRESS,
                          (u16)(right->rect.x + 2U),
                          (u16)(right->rect.y + 1U));
        Key up = press;

        up.ev = (u8)SAG_KEY_RELEASE;
        sag_mouse_event(&ed, &press);
        sag_mouse_event(&ed, &up);
    }
    SAG_ASSERT(ed.focus == right);
    sag_ed_free(&ed);
}

/*
 * SAG_REGION_BLOCK is what makes a dialog modal to the mouse without
 * any dialog knowing the router exists.  A press on it changes nothing
 * and, crucially, does not reach whatever is underneath.
 */
void test_mouse_block_region_swallows_everything(void)
{
    Ed ed;
    i32 leaf;
    Pane *before;

    ms_fixture(&ed);
    sag_pane_tables_reset(&ed);
    leaf = sag_pane_table_add_leaf(&ed, ed.pane_root);
    before = ed.focus;
    sag_region_frame_begin();
    /* The pane first, the block on top: last-added wins (s22). */
    sag_region_add(SAG_REGION_PANE, ed.pane_root->rect, leaf);
    sag_region_add(SAG_REGION_BLOCK, ed.pane_root->rect, 0);
    {
        Key press = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_PRESS, 10U, 5U);
        Key motion = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_REPEAT, 20U, 9U);
        Key up = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_RELEASE, 20U, 9U);

        sag_mouse_event(&ed, &press);
        SAG_ASSERT_EQ_U64((u64)ed.mouse.press_rgn.kind,
                          (u64)SAG_REGION_BLOCK);
        sag_mouse_event(&ed, &motion);
        /* No drag begins on a block: there is nothing there to drag. */
        SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_ARMED);
        sag_mouse_event(&ed, &up);
    }
    SAG_ASSERT(ed.focus == before);
    SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_IDLE);
    sag_ed_free(&ed);
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
    sag_pane_tables_reset(&ed);
    leaf = sag_pane_table_add_leaf(&ed, ed.pane_root);
    before = ed.focus;
    ms_frame_pane(ed.pane_root, leaf);
    {
        Key press = ms_ev((u8)SAG_MB_RIGHT, (u8)SAG_KEY_PRESS, 10U, 5U);

        sag_mouse_event(&ed, &press);
    }
    SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_IDLE);
    SAG_ASSERT_EQ_U64(ed.mouse.held, 0U);
    SAG_ASSERT(ed.focus == before);
    sag_ed_free(&ed);
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
    sag_region_frame_begin();
    sag_region_add(SAG_REGION_NONE, ed.pane_root->rect, 0);
    {
        Key press = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_PRESS, 10U, 5U);
        Key up = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_RELEASE, 10U, 5U);
        Key w = ms_wheel((u8)SAG_MB_WHEEL_DOWN, 10U, 5U, 0U);

        sag_mouse_event(&ed, &press);
        sag_mouse_event(&ed, &up);
        sag_mouse_event(&ed, &w);
    }
    SAG_ASSERT(ed.focus == before);
    SAG_ASSERT_EQ_U64(ed.dispatch_count, dispatches);
    SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_IDLE);
    sag_ed_free(&ed);
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
    right = sag_pane_split(&ed, ed.pane_root, SAG_SPLIT_V);
    SAG_ASSERT_NOT_NULL(right);
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 23U});
    left = ed.pane_root->a == right ? ed.pane_root->b : ed.pane_root->a;
    ed.focus = left;
    ed.win = left->win;
    SAG_ASSERT_NOT_NULL(right->win);

    sag_pane_tables_reset(&ed);
    (void)sag_pane_table_add_leaf(&ed, left);
    right_id = sag_pane_table_add_leaf(&ed, right);
    sag_region_frame_begin();
    sag_region_add(SAG_REGION_PANE, right->rect, right_id);

    cursor_before = sag_ed_cursor(&ed)->pos;
    left_top_before = left->win->vp.top;
    {
        Key w = ms_wheel((u8)SAG_MB_WHEEL_DOWN,
                         (u16)(right->rect.x + 2U),
                         (u16)(right->rect.y + 2U), 0U);

        sag_mouse_event(&ed, &w);
    }
    /* The UNFOCUSED pane moved, by exactly one notch. */
    SAG_ASSERT_EQ_U64(right->win->vp.top.v, (u64)SAG_WHEEL_ROWS);
    /* And nothing else did. */
    SAG_ASSERT(ed.focus == left);
    SAG_ASSERT_EQ_U64(left->win->vp.top.v, left_top_before.v);
    SAG_ASSERT_EQ_U64(sag_ed_cursor(&ed)->pos.v, cursor_before.v);
    sag_ed_free(&ed);
}

/*
 * A notch is exactly SAG_WHEEL_ROWS rows, forever: no acceleration, no
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
    sag_pane_tables_reset(&ed);
    leaf = sag_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);
    for (i = 1U; i <= 10U; i++) {
        Key w = ms_wheel((u8)SAG_MB_WHEEL_DOWN, 10U, 5U, 0U);

        sag_mouse_event(&ed, &w);
        SAG_ASSERT_EQ_U64(ed.win->vp.top.v, (u64)(i * SAG_WHEEL_ROWS));
    }
    /* Symmetric on the way back, and it stops at the top rather than
     * running negative. */
    for (i = 0U; i < 20U; i++) {
        Key w = ms_wheel((u8)SAG_MB_WHEEL_UP, 10U, 5U, 0U);

        sag_mouse_event(&ed, &w);
    }
    SAG_ASSERT_EQ_U64(ed.win->vp.top.v, 0U);
    sag_ed_free(&ed);
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
    sag_pane_tables_reset(&ed);
    leaf = sag_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);
    {
        Key w = ms_wheel((u8)SAG_MB_WHEEL_DOWN, 10U, 5U,
                         (u16)SAG_MOD_CTRL);

        sag_mouse_event(&ed, &w);
    }
    SAG_ASSERT_EQ_U64(ed.win->vp.top.v, 0U);
    sag_ed_free(&ed);
}

void test_mouse_shift_wheel_scrolls_sideways_and_not_when_wrapped(void)
{
    Ed ed;
    i32 leaf;

    ms_fixture(&ed);
    {
        EditCtx ec = sag_ed_edit_ctx(&ed);
        char wide[400];

        (void)memset(wide, 'x', sizeof(wide) - 1U);
        wide[sizeof(wide) - 1U] = '\0';
        sag_edit_insert(&ec, sag_ed_cursor(&ed)->pos, (const u8 *)wide,
                        strlen(wide));
        sag_ed_finish_edit(&ed, &ec);
    }
    sag_pane_tables_reset(&ed);
    leaf = sag_pane_table_add_leaf(&ed, ed.pane_root);
    ms_frame_pane(ed.pane_root, leaf);

    ed.win->vp.wrap = false;
    {
        Key w = ms_wheel((u8)SAG_MB_WHEEL_DOWN, 10U, 5U,
                         (u16)SAG_MOD_SHIFT);

        sag_mouse_event(&ed, &w);
        SAG_ASSERT_EQ_U64(ed.win->vp.left.v, (u64)SAG_WHEEL_COLS);
        sag_mouse_event(&ed, &w);
        SAG_ASSERT_EQ_U64(ed.win->vp.left.v, (u64)(2U * SAG_WHEEL_COLS));
    }
    {
        Key w = ms_wheel((u8)SAG_MB_WHEEL_UP, 10U, 5U,
                         (u16)SAG_MOD_SHIFT);

        sag_mouse_event(&ed, &w);
        SAG_ASSERT_EQ_U64(ed.win->vp.left.v, (u64)SAG_WHEEL_COLS);
        sag_mouse_event(&ed, &w);
        SAG_ASSERT_EQ_U64(ed.win->vp.left.v, 0U);
        /* It stops at column 0 rather than running negative. */
        sag_mouse_event(&ed, &w);
        SAG_ASSERT_EQ_U64(ed.win->vp.left.v, 0U);
    }

    /*
     * Wrapped, there is nowhere to go — and the no-op is SILENT,
     * because a wheel is not a command that can fail and a message per
     * notch would be noise.
     */
    ed.win->vp.wrap = true;
    ed.win->vp.left = (CCol){0U};
    sag_msg_clear(&ed);
    {
        Key w = ms_wheel((u8)SAG_MB_WHEEL_DOWN, 10U, 5U,
                         (u16)SAG_MOD_SHIFT);

        sag_mouse_event(&ed, &w);
    }
    SAG_ASSERT_EQ_U64(ed.win->vp.left.v, 0U);
    SAG_ASSERT(!ed.msg.active);
    sag_ed_free(&ed);
}

void test_mouse_wheel_over_the_strip_scrolls_the_strip(void)
{
    Ed ed;

    int i;

    ms_fixture(&ed);
    for (i = 0; i < 8; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/sag-mouse-w%d.txt", i);
        SAG_ASSERT(sag_tab_open(&ed, path) >= 0);
    }
    ed.tabs.scroll = 4;
    sag_region_frame_begin();
    sag_region_add(SAG_REGION_TAB, (Rect){0U, 0U, 10U, 1U}, 0);
    {
        Key w = ms_wheel((u8)SAG_MB_WHEEL_UP, 2U, 0U, 0U);

        sag_mouse_event(&ed, &w);
    }
    /* One ENTRY, not SAG_WHEEL_ROWS of them: the strip's unit is a tab,
     * and three tabs per notch would overshoot on every bar that fits. */
    SAG_ASSERT_EQ_I64(ed.tabs.scroll, 3);
    {
        Key w = ms_wheel((u8)SAG_MB_WHEEL_DOWN, 2U, 0U, 0U);

        sag_mouse_event(&ed, &w);
    }
    SAG_ASSERT_EQ_I64(ed.tabs.scroll, 4);
    sag_ed_free(&ed);
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

        (void)snprintf(path, sizeof(path), "/tmp/sag-mouse-%d.txt", i);
        SAG_ASSERT(sag_tab_open(&ed, path) >= 0);
    }
    SAG_ASSERT_EQ_U64(sag_tab_count(&ed), 5U);
    target_id = sag_tab_at(&ed, 3)->tab_id;
    sag_tab_switch(&ed, 0);

    /* Frame A: tab 3 occupies cells 20..30 on row 0. */
    sag_region_frame_begin();
    sag_region_add(SAG_REGION_TAB, (Rect){20U, 0U, 10U, 1U}, 3);
    {
        Key press = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_PRESS, 22U, 0U);

        sag_mouse_event(&ed, &press);
    }
    SAG_ASSERT_EQ_U64(ed.mouse.drag_tab_id, target_id);

    /* Frame B: the strip scrolled; those same cells are now tab 1. */
    sag_region_frame_begin();
    sag_region_add(SAG_REGION_TAB, (Rect){20U, 0U, 10U, 1U}, 1);
    {
        Key up = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_RELEASE, 22U, 0U);

        sag_mouse_event(&ed, &up);
    }
    /* The tab the user AIMED at, resolved by id. */
    SAG_ASSERT_EQ_I64(ed.tabs.active, sag_tab_index_of_id(&ed, target_id));
    sag_ed_free(&ed);
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

        (void)snprintf(path, sizeof(path), "/tmp/sag-mouse-c%d.txt", i);
        SAG_ASSERT(sag_tab_open(&ed, path) >= 0);
    }
    target_id = sag_tab_at(&ed, 3)->tab_id;
    sag_tab_switch(&ed, 0);
    active_before = ed.tabs.active;

    sag_region_frame_begin();
    sag_region_add(SAG_REGION_TAB, (Rect){20U, 0U, 10U, 1U}, 3);
    {
        Key press = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_PRESS, 22U, 0U);

        sag_mouse_event(&ed, &press);
    }
    SAG_ASSERT(sag_tab_close(&ed, sag_tab_index_of_id(&ed, target_id)));
    sag_tab_switch(&ed, active_before);
    SAG_ASSERT_EQ_I64(sag_tab_index_of_id(&ed, target_id), -1);
    {
        Key up = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_RELEASE, 22U, 0U);

        sag_mouse_event(&ed, &up);
    }
    SAG_ASSERT_EQ_I64(ed.tabs.active, active_before);
    sag_ed_free(&ed);
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

        (void)snprintf(path, sizeof(path), "/tmp/sag-mouse-g%d.txt", i);
        SAG_ASSERT(sag_tab_open(&ed, path) >= 0);
    }
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 2);
    sag_group_add_member(&ed, g, 3);
    sag_tab_switch(&ed, 0);

    sag_region_frame_begin();
    sag_region_add(SAG_REGION_TAB, (Rect){0U, 0U, 8U, 1U}, -(i32)g);
    {
        Key press = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_PRESS, 2U, 0U);

        sag_mouse_event(&ed, &press);
    }
    SAG_ASSERT_EQ_U64(ed.mouse.drag_gid, g);
    SAG_ASSERT_EQ_U64(ed.mouse.drag_tab_id, 0U);
    /* Nothing has happened yet: a press on a tab entry arms, it does
     * not switch.  Switching on press and then dragging would leave a
     * tab activated that the user only meant to move. */
    SAG_ASSERT_EQ_U64(sag_active_group_id(&ed), 0U);

    sag_region_frame_begin();
    sag_region_add(SAG_REGION_TAB, (Rect){0U, 0U, 8U, 1U}, 1);
    {
        Key up = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_RELEASE, 2U, 0U);

        sag_mouse_event(&ed, &up);
    }
    SAG_ASSERT_EQ_U64(sag_active_group_id(&ed), g);
    sag_ed_free(&ed);
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

        (void)snprintf(path, sizeof(path), "/tmp/sag-mouse-d%d.txt", i);
        SAG_ASSERT(sag_tab_open(&ed, path) >= 0);
    }
    sag_tab_switch(&ed, 0);
    sag_ed_layout(&ed);
    sag_pane_tables_reset(&ed);
    leaf = sag_pane_table_add_leaf(&ed, ed.pane_root);
    sag_region_frame_begin();
    sag_tab_strip_draw(&ed, ed.tab_strip_rect);
    sag_region_add(SAG_REGION_PANE, ed.pane_root->rect, leaf);

    held = sag_tab_at(&ed, 0)->tab_id;
    at_press = 0;
    {
        u16 x;
        Key press;
        Key motion;
        Key up;

        for (x = 0U; x < 80U; x++) {
            if (sag_strip_slot_at(x, 0U) == 0)
                break;
        }
        SAG_ASSERT(x < 80U);
        press = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_PRESS, x, 0U);
        motion = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_REPEAT, 40U,
                       (u16)(ed.pane_root->rect.y + 5U));
        up = ms_ev((u8)SAG_MB_LEFT, (u8)SAG_KEY_RELEASE, 40U,
                   (u16)(ed.pane_root->rect.y + 5U));
        sag_mouse_event(&ed, &press);
        sag_mouse_event(&ed, &motion);
        sag_mouse_event(&ed, &up);
    }
    /* Not moved, not opened anywhere, and the gesture is over. */
    SAG_ASSERT_EQ_I64(sag_tab_index_of_id(&ed, held), at_press);
    SAG_ASSERT_EQ_U64((u64)ed.mouse.phase, (u64)SAG_MP_IDLE);
    sag_ed_free(&ed);
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
    sag_input_init(&in, &caps);
    /* cb 35 == motion with no button held, which is what a terminal
     * sends while the pointer is simply moving. */
    sag_input_feed(&in, (const u8 *)"\x1b[<35;10;5M", 11U);
    got = sag_input_next(&in, 0, &key);
    SAG_ASSERT(!got || key.kind != (u16)SAG_EV_MOUSE);
    /* And a held-button motion IS decoded, so the test above is about
     * the no-button case rather than about a broken feed. */
    sag_input_feed(&in, (const u8 *)"\x1b[<32;10;5M", 11U);
    SAG_ASSERT(sag_input_next(&in, 0, &key));
    SAG_ASSERT_EQ_U64((u64)key.kind, (u64)SAG_EV_MOUSE);
    SAG_ASSERT_EQ_U64(key.ev, (u64)SAG_KEY_REPEAT);
    sag_input_free(&in);
}
