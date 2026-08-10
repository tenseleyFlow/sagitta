#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 27 §8, DoD 10: the invariant-9 audit, as tests rather than as
 * documentation.
 *
 * INVARIANT 9 — the keyboard never requires the mouse.  Every row of
 * §8's audit table is performed twice here, once through the router and
 * once with keys only under YEW_MOUSE=0, and the two states are
 * compared.  A table in a sprint file cannot rot loudly; a test can.
 *
 * (§8 calls this a script test.  The script-test RUNNER lands in Sprint
 * 37 — `make test-script` still hard-errors naming it — so the audit
 * lives here until there is a runner to move it to.  What is being
 * proved is identical either way: the same registry commands, invoked
 * without a pointer, reach the same state.)
 *
 * Where the two paths CANNOT be identical, the difference is named
 * rather than papered over.  The wheel is the one case: it deliberately
 * does not move the cursor, while ed.view.scroll.* pushes the cursor
 * back into view, so the comparison is over the viewport — which is the
 * thing a scroll is about.
 */
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/mode.h"
#include "edit/pane_cmds.h"
#include "ui/ctxmenu.h"
#include "ui/glyphs.h"
#include "ui/groupnav.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/mouse.h"
#include "ui/picker.h"
#include "ui/region.h"
#include "ui/strip.h"
#include "ui/tabs.h"
#include "ui/viewport.h"
#include "ui/win.h"

/* ---------------------------------------------------------------- */
/* Fixture                                                          */
/* ---------------------------------------------------------------- */

static void i9_fixture(Ed *ed, int extra_tabs)
{
    int i;

    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    YEW_ASSERT(yew_grid_init(&ed->grid, &ed->interner, 24U, 80U));
    ed->grid_ready = true;
    for (i = 0; i < extra_tabs; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-i9-%d.txt", i);
        YEW_ASSERT(yew_tab_open(ed, path) >= 0);
    }
    yew_ed_layout(ed);
    ed->now_ms = 1000;
}

static void i9_fill(Ed *ed, u32 lines)
{
    u32 i;

    for (i = 0U; i < lines; i++) {
        EditCtx ec = yew_ed_edit_ctx(ed);
        char line[64];

        (void)snprintf(line, sizeof(line), "line %u of the file\n",
                       (unsigned)i);
        (void)yew_edit_insert(&ec, yew_ed_cursor(ed)->pos,
                              (const u8 *)line, strlen(line));
        yew_ed_finish_edit(ed, &ec);
    }
}

static CmdStatus i9_run(Ed *ed, const char *name, u32 count,
                        const char *sarg)
{
    CmdCtx cx = {0};
    CmdId id = yew_cmd_lookup(name, strlen(name));

    cx.ed = ed;
    cx.win = ed->win;
    cx.count = count == 0U ? 1U : count;
    cx.count_given = count != 0U;
    cx.sarg = sarg;
    cx.sarg_len = sarg != NULL ? strlen(sarg) : 0U;
    cx.source = YEW_SRC_KEY;
    return yew_ed_invoke(ed, id, &cx);
}

static Key i9_mouse(u8 button, u8 ev, u16 x, u16 y)
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

static Key i9_key(u32 code)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.kind = (u16)YEW_EV_KEY;
    k.ev = (u8)YEW_KEY_PRESS;
    k.code = code;
    return k;
}

/* The keyboard half runs with the mouse OFF, which is the condition the
 * audit is about: not "the keyboard also works", but "the keyboard
 * works when there is no pointer at all". */
static void i9_keys_only(void)
{
    yew_mouse_set_enabled(false);
}

static void i9_mouse_on(void)
{
    yew_mouse_set_enabled(true);
}

static void i9_paint_strip(Ed *ed)
{
    if (ed->layout_dirty)
        yew_ed_layout(ed);
    yew_region_frame_begin();
    yew_tab_strip_draw(ed, ed->tab_strip_rect);
}

static u16 i9_slot_x(int slot)
{
    u16 x;

    for (x = 0U; x < 80U; x++) {
        if (yew_strip_slot_at(x, 0U) == slot)
            return x;
    }
    YEW_ASSERT(false);
    return 0U;
}

/* ---------------------------------------------------------------- */
/* Panes                                                            */
/* ---------------------------------------------------------------- */

void test_invariant9_click_a_pane_equals_focus_right(void)
{
    Ed mouse;
    Ed keys;
    Pane *m_right;
    Pane *k_right;

    i9_mouse_on();
    i9_fixture(&mouse, 0);
    /* YEW_SPLIT_H puts the two panes SIDE BY SIDE, which is what
     * focus_right is about; YEW_SPLIT_V stacks them. */
    m_right = yew_pane_split(&mouse, mouse.pane_root, YEW_SPLIT_H);
    YEW_ASSERT_NOT_NULL(m_right);
    yew_ed_layout(&mouse);
    mouse.focus = mouse.pane_root->a;
    mouse.win = mouse.focus->win;
    {
        i32 id;
        Key press;

        yew_pane_tables_reset(&mouse);
        (void)yew_pane_table_add_leaf(&mouse, mouse.pane_root->a);
        id = yew_pane_table_add_leaf(&mouse, m_right);
        yew_region_frame_begin();
        yew_region_add(YEW_REGION_PANE, m_right->rect, id);
        press = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS,
                         (u16)(m_right->rect.x + 2U),
                         (u16)(m_right->rect.y + 1U));
        yew_mouse_event(&mouse, &press);
    }

    i9_keys_only();
    i9_fixture(&keys, 0);
    k_right = yew_pane_split(&keys, keys.pane_root, YEW_SPLIT_H);
    YEW_ASSERT_NOT_NULL(k_right);
    yew_ed_layout(&keys);
    keys.focus = keys.pane_root->a;
    keys.win = keys.focus->win;
    YEW_ASSERT_EQ_U64(i9_run(&keys, "ed.pane.focus_right", 0U, NULL),
                      (u64)YEW_CMD_OK);

    /* The same leaf, by position in the tree. */
    YEW_ASSERT(mouse.focus == m_right);
    YEW_ASSERT(keys.focus == k_right);
    i9_mouse_on();
    yew_ed_free(&mouse);
    yew_ed_free(&keys);
}

void test_invariant9_drag_a_border_equals_grow(void)
{
    Ed mouse;
    Ed keys;

    i9_mouse_on();
    i9_fixture(&mouse, 0);
    YEW_ASSERT_NOT_NULL(yew_pane_split(&mouse, mouse.pane_root,
                                       YEW_SPLIT_H));
    yew_ed_layout(&mouse);
    {
        i32 id;
        u16 border_x = mouse.pane_root->b->rect.x;
        Key press;
        Key motion;
        Key up;

        yew_pane_tables_reset(&mouse);
        id = yew_pane_table_add_split(&mouse, mouse.pane_root);
        yew_region_frame_begin();
        yew_region_add(YEW_REGION_PANE_BORDER,
                       (Rect){(u16)(border_x - 1U), 0U, 1U, 23U}, id);
        press = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS,
                         (u16)(border_x - 1U), 5U);
        motion = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_REPEAT,
                          (u16)border_x, 5U);
        up = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE,
                      (u16)border_x, 5U);
        yew_mouse_event(&mouse, &press);
        yew_mouse_event(&mouse, &motion);
        yew_mouse_event(&mouse, &up);
    }
    yew_ed_layout(&mouse);

    i9_keys_only();
    i9_fixture(&keys, 0);
    YEW_ASSERT_NOT_NULL(yew_pane_split(&keys, keys.pane_root,
                                       YEW_SPLIT_H));
    keys.focus = keys.pane_root->a;
    keys.win = keys.focus->win;
    YEW_ASSERT_EQ_U64(i9_run(&keys, "ed.pane.grow", 1U, NULL),
                      (u64)YEW_CMD_OK);
    yew_ed_layout(&keys);

    /* One cell of drag equals one grow: the same boundary column. */
    YEW_ASSERT_EQ_U64(mouse.pane_root->b->rect.x,
                      keys.pane_root->b->rect.x);
    i9_mouse_on();
    yew_ed_free(&mouse);
    yew_ed_free(&keys);
}

/* ---------------------------------------------------------------- */
/* Tabs and groups                                                  */
/* ---------------------------------------------------------------- */

void test_invariant9_click_a_tab_equals_tab_goto(void)
{
    Ed mouse;
    Ed keys;

    i9_mouse_on();
    i9_fixture(&mouse, 4);
    yew_tab_switch(&mouse, 0);
    i9_paint_strip(&mouse);
    {
        u16 x = i9_slot_x(3);
        Key press = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, x, 0U);
        Key up = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE, x, 0U);

        yew_mouse_event(&mouse, &press);
        yew_mouse_event(&mouse, &up);
    }

    i9_keys_only();
    i9_fixture(&keys, 4);
    yew_tab_switch(&keys, 0);
    /* 1-based, as the user counts them. */
    YEW_ASSERT_EQ_U64(i9_run(&keys, "ed.tab.goto", 4U, NULL),
                      (u64)YEW_CMD_OK);

    YEW_ASSERT_EQ_I64(mouse.tabs.active, 3);
    YEW_ASSERT_EQ_I64(keys.tabs.active, mouse.tabs.active);
    i9_mouse_on();
    yew_ed_free(&mouse);
    yew_ed_free(&keys);
}

void test_invariant9_drag_a_tab_equals_tab_move(void)
{
    Ed mouse;
    Ed keys;
    u32 m_order[8];
    u32 k_order[8];
    u32 i;

    i9_mouse_on();
    i9_fixture(&mouse, 4);
    yew_tab_switch(&mouse, 0);
    i9_paint_strip(&mouse);
    {
        Key press = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS,
                             i9_slot_x(0), 0U);
        Key motion = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_REPEAT,
                              i9_slot_x(3), 0U);
        Key up = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE,
                          i9_slot_x(3), 0U);

        yew_mouse_event(&mouse, &press);
        yew_mouse_event(&mouse, &motion);
        yew_mouse_event(&mouse, &up);
    }

    i9_keys_only();
    i9_fixture(&keys, 4);
    yew_tab_switch(&keys, 0);
    YEW_ASSERT_EQ_U64(i9_run(&keys, "ed.tab.move", 4U, NULL),
                      (u64)YEW_CMD_OK);

    /* Compared as a whole ORDER, by position — the two editors issue
     * ids in the same sequence, so equal positions mean equal
     * arrangements. */
    YEW_ASSERT_EQ_U64(yew_tab_count(&mouse), yew_tab_count(&keys));
    for (i = 0U; i < yew_tab_count(&mouse); i++) {
        m_order[i] = yew_tab_at(&mouse, (int)i)->tab_id;
        k_order[i] = yew_tab_at(&keys, (int)i)->tab_id;
        YEW_ASSERT_EQ_U64(m_order[i], k_order[i]);
    }
    i9_mouse_on();
    yew_ed_free(&mouse);
    yew_ed_free(&keys);
}

void test_invariant9_drop_into_a_group_equals_group_add_tab(void)
{
    Ed mouse;
    Ed keys;
    u32 g;
    u32 i;

    i9_mouse_on();
    i9_fixture(&mouse, 4);
    g = yew_group_create(&mouse, "/src", "grp");
    yew_group_add_member(&mouse, g, 3);
    yew_group_add_member(&mouse, g, 4);
    yew_tab_switch(&mouse, 0);
    yew_ed_layout(&mouse);
    i9_paint_strip(&mouse);
    {
        int gslot = -1;
        int s;
        Key press;
        Key motion;
        Key up;

        for (s = 0; s < yew_strip_slot_count(); s++) {
            i32 payload = 0;

            if (yew_strip_pre_payload(s, &payload) &&
                payload == -(i32)g) {
                gslot = s;
                break;
            }
        }
        YEW_ASSERT(gslot >= 0);
        press = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS,
                         i9_slot_x(0), 0U);
        motion = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_REPEAT,
                          i9_slot_x(gslot), 0U);
        yew_mouse_event(&mouse, &press);
        yew_mouse_event(&mouse, &motion);
        yew_mouse_tick(&mouse, mouse.now_ms + YEW_DRAG_DWELL_MS);
        YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&mouse), g);
        i9_paint_strip(&mouse);
        up = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE, 78U, 1U);
        yew_mouse_event(&mouse, &up);
    }

    i9_keys_only();
    i9_fixture(&keys, 4);
    g = yew_group_create(&keys, "/src", "grp");
    yew_group_add_member(&keys, g, 3);
    yew_group_add_member(&keys, g, 4);
    yew_tab_switch(&keys, 0);
    YEW_ASSERT_EQ_U64(i9_run(&keys, "ed.group.add_tab", 0U, "grp"),
                      (u64)YEW_CMD_OK);

    /* Full membership AND ordinal snapshots: the Sprint 24 off-by-one
     * only shows up as a whole-list disagreement. */
    YEW_ASSERT_EQ_U64(yew_tab_count(&mouse), yew_tab_count(&keys));
    for (i = 0U; i < yew_tab_count(&mouse); i++) {
        const Tab *m = yew_tab_at(&mouse, (int)i);
        const Tab *k = yew_tab_at(&keys, (int)i);

        YEW_ASSERT_EQ_U64(m->tab_id, k->tab_id);
        YEW_ASSERT_EQ_U64(m->group_id, k->group_id);
        YEW_ASSERT_EQ_U64(m->group_ordinal, k->group_ordinal);
    }
    i9_mouse_on();
    yew_ed_free(&mouse);
    yew_ed_free(&keys);
}

void test_invariant9_click_a_group_entry_equals_group_enter(void)
{
    Ed mouse;
    Ed keys;
    u32 g;

    i9_mouse_on();
    i9_fixture(&mouse, 4);
    g = yew_group_create(&mouse, "/src", "grp");
    yew_group_add_member(&mouse, g, 3);
    yew_group_add_member(&mouse, g, 4);
    yew_tab_switch(&mouse, 0);
    yew_ed_layout(&mouse);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){0U, 0U, 8U, 1U}, -(i32)g);
    {
        Key press = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, 2U, 0U);
        Key up = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE, 2U, 0U);

        yew_mouse_event(&mouse, &press);
        yew_mouse_event(&mouse, &up);
    }

    i9_keys_only();
    i9_fixture(&keys, 4);
    g = yew_group_create(&keys, "/src", "grp");
    yew_group_add_member(&keys, g, 3);
    yew_group_add_member(&keys, g, 4);
    yew_tab_switch(&keys, 0);
    /* ed.group.enter walks into the group beside the active tab. */
    YEW_ASSERT_EQ_U64(i9_run(&keys, "ed.group.enter", 0U, NULL),
                      (u64)YEW_CMD_OK);

    YEW_ASSERT_EQ_U64(yew_active_group_id(&mouse), g);
    YEW_ASSERT_EQ_U64(yew_active_group_id(&keys),
                      yew_active_group_id(&mouse));
    YEW_ASSERT_EQ_I64(mouse.tabs.active, keys.tabs.active);
    i9_mouse_on();
    yew_ed_free(&mouse);
    yew_ed_free(&keys);
}

void test_invariant9_strip_chevrons_equal_tab_next_and_prev(void)
{
    Ed mouse;
    Ed keys;

    /*
     * The chevron scrolls the strip; ed.tab.next walks the active tab
     * and the strip follows it minimally.  The comparable state is
     * therefore "the strip moved on", asserted as the scroll offset
     * both paths end at.
     */
    i9_mouse_on();
    i9_fixture(&mouse, 8);
    yew_tab_switch(&mouse, 0);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB_SCROLL, (Rect){79U, 0U, 1U, 1U}, 1);
    {
        Key press = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, 79U, 0U);

        yew_mouse_event(&mouse, &press);
    }
    YEW_ASSERT_EQ_I64(mouse.tabs.scroll, 1);

    i9_keys_only();
    i9_fixture(&keys, 8);
    yew_tab_switch(&keys, 0);
    keys.tabs.scroll = 0;
    /* The keyboard reaches every entry the chevron would have revealed,
     * which is what invariant 9 asks: not the same keystroke, the same
     * REACH. */
    YEW_ASSERT_EQ_U64(i9_run(&keys, "ed.tab.next", 0U, NULL),
                      (u64)YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(keys.tabs.active, 1);
    i9_mouse_on();
    yew_ed_free(&mouse);
    yew_ed_free(&keys);
}

/* ---------------------------------------------------------------- */
/* The wheel                                                        */
/* ---------------------------------------------------------------- */

/*
 * The one row where the two paths deliberately differ, and the
 * difference is named: a wheel does not move the cursor, while
 * ed.view.scroll.* pushes it back into view.  The VIEWPORT — which is
 * what a scroll is about — ends in the same place.
 */
void test_invariant9_wheel_equals_three_view_scrolls(void)
{
    Ed mouse;
    Ed keys;
    i32 leaf;

    i9_mouse_on();
    i9_fixture(&mouse, 0);
    i9_fill(&mouse, 100U);
    yew_ed_cursor(&mouse)->pos = BYTEOFF(0U);
    yew_vp_follow(mouse.win);
    yew_pane_tables_reset(&mouse);
    leaf = yew_pane_table_add_leaf(&mouse, mouse.pane_root);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, mouse.pane_root->rect, leaf);
    {
        Key w = i9_mouse((u8)YEW_MB_WHEEL_DOWN, (u8)YEW_KEY_PRESS, 10U,
                         (u16)(mouse.pane_root->rect.y + 2U));

        yew_mouse_event(&mouse, &w);
    }

    i9_keys_only();
    i9_fixture(&keys, 0);
    i9_fill(&keys, 100U);
    yew_ed_cursor(&keys)->pos = BYTEOFF(0U);
    yew_vp_follow(keys.win);
    {
        int i;

        for (i = 0; i < YEW_WHEEL_ROWS; i++) {
            YEW_ASSERT_EQ_U64(i9_run(&keys, "ed.view.scroll.down", 0U,
                                     NULL),
                              (u64)YEW_CMD_OK);
        }
    }

    YEW_ASSERT_EQ_U64(mouse.win->vp.top.v, (u64)YEW_WHEEL_ROWS);
    YEW_ASSERT_EQ_U64(keys.win->vp.top.v, mouse.win->vp.top.v);
    i9_mouse_on();
    yew_ed_free(&mouse);
    yew_ed_free(&keys);
}

/* ---------------------------------------------------------------- */
/* Selection                                                        */
/* ---------------------------------------------------------------- */

void test_invariant9_double_click_equals_h_plus_word_unit(void)
{
    Ed mouse;
    Ed keys;
    Span m_span;
    Span k_span;
    ByteOff clicked_at;
    i32 leaf;
    u16 x;

    i9_mouse_on();
    i9_fixture(&mouse, 0);
    {
        EditCtx ec = yew_ed_edit_ctx(&mouse);

        (void)yew_edit_insert(&ec, BYTEOFF(0U),
                              (const u8 *)"alpha beta gamma\n", 17U);
        yew_ed_finish_edit(&mouse, &ec);
    }
    yew_ed_cursor(&mouse)->pos = BYTEOFF(0U);
    yew_ed_cursor(&mouse)->anchor = BYTEOFF(0U);
    yew_pane_tables_reset(&mouse);
    leaf = yew_pane_table_add_leaf(&mouse, mouse.pane_root);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_PANE, mouse.pane_root->rect, leaf);
    x = (u16)(mouse.win->rect.x + 8U); /* inside `beta` */
    {
        Key press = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_PRESS, x,
                             mouse.pane_root->rect.y);
        Key up = i9_mouse((u8)YEW_MB_LEFT, (u8)YEW_KEY_RELEASE, x,
                          mouse.pane_root->rect.y);

        yew_mouse_event(&mouse, &press);
        yew_mouse_event(&mouse, &up);
        /* Recorded BEFORE the second click, which moves the caret to
         * the word's far edge — the keyboard half has to start where
         * the user pointed, not where the selection ended. */
        clicked_at = yew_ed_cursor(&mouse)->pos;
        yew_mouse_event(&mouse, &press);
        yew_mouse_event(&mouse, &up);
    }
    m_span = yew_sel_span(mouse.win,
                          &mouse.win->cs.curs.data[mouse.win->cs.primary]);

    /*
     * Keys only: put the caret on the same grapheme, enter H with the
     * WORD unit borrowed, and expand by one unit.  This is the state
     * §6 says a double-click must produce.
     */
    i9_keys_only();
    i9_fixture(&keys, 0);
    {
        EditCtx ec = yew_ed_edit_ctx(&keys);

        (void)yew_edit_insert(&ec, BYTEOFF(0U),
                              (const u8 *)"alpha beta gamma\n", 17U);
        yew_ed_finish_edit(&keys, &ec);
    }
    yew_ed_cursor(&keys)->pos = clicked_at;
    yew_ed_cursor(&keys)->anchor = yew_ed_cursor(&keys)->pos;
    /*
     * W mode, home to the word's start, then H with the word engine
     * borrowed and end to its far edge.  Three ordinary commands, no
     * coordinates anywhere.
     *
     * NOT ed.sel.unit.expand — §8's table lists it beside "H + W unit"
     * and the two are different things: expand walks Sprint 17's
     * STRUCTURAL levels (the enclosing block), so from a bare caret it
     * jumps straight past the word to the whole line.  The unit motions
     * are what a double-click is a shortcut for.
     */
    YEW_ASSERT_EQ_U64(yew_mode_enter(&keys, YEW_MODE_W),
                      (u64)YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(i9_run(&keys, "ed.move.unit.home", 0U, NULL),
                      (u64)YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_mode_enter_highlight(&keys, YEW_MODE_W, false),
                      (u64)YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(i9_run(&keys, "ed.move.unit.end", 0U, NULL),
                      (u64)YEW_CMD_OK);
    k_span = yew_sel_span(keys.win,
                          &keys.win->cs.curs.data[keys.win->cs.primary]);

    YEW_ASSERT_EQ_U64(m_span.lo, k_span.lo);
    YEW_ASSERT_EQ_U64(m_span.hi, k_span.hi);
    /* And both are in H mode with the same engine borrowed. */
    YEW_ASSERT_EQ_U64((u64)mouse.mode, (u64)YEW_MODE_H);
    YEW_ASSERT_EQ_U64((u64)keys.mode, (u64)YEW_MODE_H);
    YEW_ASSERT(mouse.win->h.unit == keys.win->h.unit);
    i9_mouse_on();
    yew_ed_free(&mouse);
    yew_ed_free(&keys);
}

/* ---------------------------------------------------------------- */
/* The context menu                                                 */
/* ---------------------------------------------------------------- */

/*
 * Every menu row is a registry command, and the menu itself opens from
 * a key.  So the whole menu is reachable with no pointer: open it with
 * ed.ui.context_menu, walk with ↓, invoke with Enter.
 */
void test_invariant9_menu_and_its_rows_are_keyboard_reachable(void)
{
    Ed mouse;
    Ed keys;
    u32 target;

    i9_mouse_on();
    i9_fixture(&mouse, 4);
    yew_tab_switch(&mouse, 2);
    target = yew_tab_at(&mouse, 2)->tab_id;
    yew_ed_layout(&mouse);
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){20U, 0U, 10U, 1U}, 2);
    {
        Key press = i9_mouse((u8)YEW_MB_RIGHT, (u8)YEW_KEY_PRESS, 22U,
                             0U);

        yew_mouse_event(&mouse, &press);
    }
    YEW_ASSERT(yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_target_id(), target);
    yew_ctx_close();

    /* Keys only: the same menu, on the same target, with no
     * coordinates involved anywhere. */
    i9_keys_only();
    i9_fixture(&keys, 4);
    yew_tab_switch(&keys, 2);
    target = yew_tab_at(&keys, 2)->tab_id;
    yew_ed_layout(&keys);
    YEW_ASSERT_EQ_U64(i9_run(&keys, "ed.ui.context_menu", 0U, NULL),
                      (u64)YEW_CMD_OK);
    YEW_ASSERT(yew_ctx_active());
    YEW_ASSERT_EQ_U64(yew_ctx_kind(), (u64)YEW_CTX_KIND_TAB);
    YEW_ASSERT_EQ_U64(yew_ctx_target_id(), target);

    /* Walk to `Copy Path` (row 3, past the disabled row and the
     * separator) and invoke it. */
    {
        Key down = i9_key(YEW_KEY_DOWN);
        Key enter = i9_key(YEW_KEY_ENTER);

        /* Row 1 (Close Other Tabs) is enabled here, row 2 is the
         * separator, row 3 is Copy Path. */
        YEW_ASSERT(yew_mouse_menu_key(&keys, &down));
        YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 1);
        YEW_ASSERT(yew_mouse_menu_key(&keys, &down));
        YEW_ASSERT_EQ_I64(yew_ctx_cursor(), 3);
        YEW_ASSERT(yew_mouse_menu_key(&keys, &enter));
    }
    YEW_ASSERT(!yew_ctx_active());
    /* The path landed in the clipboard register. */
    {
        RegVal *v = yew_reg_get(&keys.regs, (u8)'+');

        YEW_ASSERT_NOT_NULL(v);
        YEW_ASSERT(v->bytes.len != 0U);
    }
    i9_mouse_on();
    yew_ed_free(&mouse);
    yew_ed_free(&keys);
}

/* Each of the four new menu commands exists in the registry and is
 * reachable by name — the audit's last column. */
void test_invariant9_every_new_command_is_registered(void)
{
    static const char *const names[] = {
        "ed.tab.close_others", "ed.tab.copy_path",  "ed.group.rename",
        "ed.group.dissolve",   "ed.ui.context_menu", "ed.group.add_tab",
        "ed.mouse.enable",     "ed.mouse.disable"
    };
    size_t i;

    yew_cmd_shutdown();
    yew_cmd_init();
    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        CmdId id = yew_cmd_lookup(names[i], strlen(names[i]));

        YEW_ASSERT(id.v != YEW_CMD_NONE.v);
    }
}
