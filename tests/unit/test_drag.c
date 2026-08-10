/*
 * Sprint 27 §4: tab and group drag-reorder, dwell, and auto-scroll.
 *
 * THE ONE THING THIS FILE IS ABOUT: nothing in Tabs.v changes until the
 * drop.  The drag carries a target and the renderer draws the strip as
 * if the held entry were already there; release applies it once.
 *
 * Facsimile's reasoning, and each clause is a bug avoided.  Swapping
 * live would look identical and be far worse underneath: snapping back
 * on cancel would mean undoing an arbitrary number of moves, a drag
 * that wandered off the bar would leave the array half-shuffled, and a
 * future drop-into-a-pane must not have quietly reordered the strip on
 * the way.
 *
 * The second thing it is about is the dwell's SOURCE.  The region table
 * describes the previewed strip — the held entry has been moved under
 * the pointer — so hit-testing it always answers "you are hovering the
 * thing you are holding".  test_drag_dwell_reads_the_pre_drag_list
 * builds a fixture where the two disagree and proves which one the
 * dwell uses.
 */
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/mouse.h"
#include "ui/region.h"
#include "ui/strip.h"
#include "ui/tabs.h"

typedef struct DragFixture {
    Ed ed;
    u32 ids[8];
} DragFixture;

static void dg_open(DragFixture *f, u32 n)
{
    u32 i;

    for (i = 0U; i < n; i++) {
        char path[64];
        int idx;

        (void)snprintf(path, sizeof(path), "/tmp/yew-drag-%u.txt",
                       (unsigned)i);
        idx = yew_tab_open(&f->ed, path);
        YEW_ASSERT(idx >= 0);
        f->ids[i] = yew_tab_at(&f->ed, idx)->tab_id;
    }
}

static void dg_fixture(DragFixture *f, u32 extra_tabs)
{
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    YEW_ASSERT(yew_grid_init(&f->ed.grid, &f->ed.interner, 24U, 80U));
    f->ed.grid_ready = true;
    dg_open(f, extra_tabs);
    yew_ed_layout(&f->ed);
    f->ed.now_ms = 1000;
}

/*
 * One frame of the strip.  Regions AND the pre-drag slot table are both
 * products of the render, which is the Sprint 22 law: placement is
 * established once, while drawing, and never re-derived.
 */
static void dg_paint(DragFixture *f)
{
    if (f->ed.layout_dirty)
        yew_ed_layout(&f->ed);
    yew_region_frame_begin();
    yew_tab_strip_draw(&f->ed, f->ed.tab_strip_rect);
}

static Key dg_ev(u8 ev, u16 x, u16 y)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.kind = (u16)YEW_EV_MOUSE;
    k.button = (u8)YEW_MB_LEFT;
    k.ev = ev;
    k.col = x;
    k.row = y;
    return k;
}

/* The middle cell of the row-1 slot `slot` currently occupies. */
static u16 dg_slot_x(DragFixture *f, int slot)
{
    u16 x;

    (void)f;
    for (x = 0U; x < 80U; x++) {
        if (yew_strip_slot_at(x, 0U) == slot)
            return x;
    }
    YEW_ASSERT(false);
    return 0U;
}

/* A snapshot of the order and membership, as ids and ordinals. */
typedef struct TabSnap {
    u32 n;
    u32 id[16];
    u32 gid[16];
    u32 ordinal[16];
} TabSnap;

static TabSnap dg_snap(Ed *ed)
{
    TabSnap s;
    u32 i;

    (void)memset(&s, 0, sizeof(s));
    s.n = yew_tab_count(ed);
    YEW_ASSERT(s.n <= YEW_ARRAY_LEN(s.id));
    for (i = 0U; i < s.n; i++) {
        Tab *t = yew_tab_at(ed, (int)i);

        s.id[i] = t->tab_id;
        s.gid[i] = t->group_id;
        s.ordinal[i] = t->group_ordinal;
    }
    return s;
}

static bool dg_snap_eq(const TabSnap *a, const TabSnap *b)
{
    return a->n == b->n && memcmp(a, b, sizeof(*a)) == 0;
}

/* ---------------------------------------------------------------- */
/* DoD 8: the array is frozen for the drag's lifetime                */
/* ---------------------------------------------------------------- */

void test_drag_never_mutates_the_tab_array_before_release(void)
{
    DragFixture f;
    TabSnap before;
    u16 x;
    int step;

    dg_fixture(&f, 5U);
    dg_paint(&f);
    before = dg_snap(&f.ed);

    x = dg_slot_x(&f, 0);
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, x, 0U);

        yew_mouse_event(&f.ed, &press);
    }
    YEW_ASSERT_EQ_U64((u64)f.ed.mouse.phase, (u64)YEW_MP_ARMED);

    /* Fifty motion events, sweeping the whole bar, each one checked. */
    for (step = 0; step < 50; step++) {
        Key motion = dg_ev((u8)YEW_KEY_REPEAT,
                           (u16)(1U + (u16)(step % 60)), 0U);
        TabSnap now;

        yew_mouse_event(&f.ed, &motion);
        now = dg_snap(&f.ed);
        YEW_ASSERT(dg_snap_eq(&before, &now));
        dg_paint(&f);
    }
    YEW_ASSERT_EQ_U64((u64)f.ed.mouse.phase, (u64)YEW_MP_DRAG_TAB);
    /* And the preview is a PICTURE: it reports a target, while the
     * array still does not know about it. */
    {
        i32 payload = 0;
        int to = -1;
        TabSnap now = dg_snap(&f.ed);

        YEW_ASSERT(yew_mouse_drag_preview(&f.ed, &payload, &to));
        YEW_ASSERT(to >= 0);
        YEW_ASSERT(dg_snap_eq(&before, &now));
    }
    yew_ed_free(&f.ed);
}

/*
 * Cancelling is free precisely because nothing was mutated.  Esc
 * mid-drag has to leave the array byte-identical, not undo a sequence
 * of moves.
 */
void test_drag_cancel_restores_nothing_because_nothing_moved(void)
{
    DragFixture f;
    TabSnap before;
    TabSnap after;

    dg_fixture(&f, 5U);
    dg_paint(&f);
    before = dg_snap(&f.ed);
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, 3), 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    YEW_ASSERT_EQ_U64((u64)f.ed.mouse.phase, (u64)YEW_MP_DRAG_TAB);
    yew_mouse_cancel(&f.ed);
    after = dg_snap(&f.ed);
    YEW_ASSERT(dg_snap_eq(&before, &after));
    YEW_ASSERT(!yew_mouse_gesture_active(&f.ed));
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* The drop                                                          */
/* ---------------------------------------------------------------- */

void test_drag_drop_reorders_by_insertion(void)
{
    DragFixture f;
    u32 moved;

    dg_fixture(&f, 5U);
    dg_paint(&f);
    moved = yew_tab_at(&f.ed, 0)->tab_id;
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);
        Key motion;
        Key up;

        yew_mouse_event(&f.ed, &press);
        motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, 3), 0U);
        yew_mouse_event(&f.ed, &motion);
        up = dg_ev((u8)YEW_KEY_RELEASE, dg_slot_x(&f, 3), 0U);
        yew_mouse_event(&f.ed, &up);
    }
    /* Insertion, not swap: the three it passed keep their order. */
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&f.ed, moved), 3);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, 0)->tab_id, f.ids[0]);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, 1)->tab_id, f.ids[1]);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, 2)->tab_id, f.ids[2]);
    yew_ed_free(&f.ed);
}

/*
 * A drag released where it started is a no-op, not a reorder-by-zero
 * that happens to look like one.  Worth its own row because the naive
 * implementation calls yew_tab_reorder(i, i) and only the state's
 * equality proves that was harmless.
 */
void test_drag_drop_where_it_started_changes_nothing(void)
{
    DragFixture f;
    TabSnap before;
    TabSnap after;
    u16 x;

    dg_fixture(&f, 5U);
    dg_paint(&f);
    before = dg_snap(&f.ed);
    x = dg_slot_x(&f, 2);
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, x, 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, (u16)(x + 1U), 0U);
        Key up = dg_ev((u8)YEW_KEY_RELEASE, x, 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
        dg_paint(&f);
        yew_mouse_event(&f.ed, &up);
    }
    after = dg_snap(&f.ed);
    YEW_ASSERT(dg_snap_eq(&before, &after));
    yew_ed_free(&f.ed);
}

/*
 * A count that moves mid-drag cancels it.  The array was supposed to be
 * frozen, so a change means something ELSE mutated it — an async job
 * closing a file, a script — and the target the user aimed at no longer
 * means what it did.
 */
void test_drag_a_changed_tab_count_cancels(void)
{
    DragFixture f;
    u32 moved;
    int at_press;

    dg_fixture(&f, 5U);
    dg_paint(&f);
    moved = yew_tab_at(&f.ed, 0)->tab_id;
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, 3), 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    at_press = yew_tab_index_of_id(&f.ed, moved);
    /* Something else closes a tab under the drag. */
    YEW_ASSERT(yew_tab_close(&f.ed, 5));
    dg_paint(&f);
    {
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, 2), 0U);

        yew_mouse_event(&f.ed, &motion);
    }
    YEW_ASSERT_EQ_U64((u64)f.ed.mouse.phase, (u64)YEW_MP_IDLE);
    /* And the held tab did not move. */
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&f.ed, moved), at_press);
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* The dwell                                                         */
/* ---------------------------------------------------------------- */

static u32 dg_make_group(DragFixture *f, int a, int b)
{
    u32 g = yew_group_create(&f->ed, "/src", "grp");

    YEW_ASSERT(g != 0U);
    yew_group_add_member(&f->ed, g, a);
    yew_group_add_member(&f->ed, g, b);
    return g;
}

/* Finds the row-1 slot whose PRE-DRAG payload is `want`. */
static int dg_slot_of_payload(i32 want)
{
    int i;

    for (i = 0; i < yew_strip_slot_count(); i++) {
        i32 got = 0;

        if (yew_strip_pre_payload(i, &got) && got == want)
            return i;
    }
    return -1;
}

void test_drag_dwell_opens_a_group_at_400ms_and_not_at_399(void)
{
    DragFixture f;
    u32 g;
    int gslot;

    dg_fixture(&f, 5U);
    g = dg_make_group(&f, 4, 5);
    yew_tab_switch(&f.ed, 0);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    gslot = dg_slot_of_payload(-(i32)g);
    YEW_ASSERT(gslot >= 0);

    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, gslot), 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    YEW_ASSERT_EQ_U64(f.ed.mouse.dwell_gid, g);

    /* 399 ms: still counting.  A drag that merely PASSES over a group
     * must not make its members flash open. */
    yew_mouse_tick(&f.ed, f.ed.now_ms + 399);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), 0U);

    /* 400 ms: open. */
    yew_mouse_tick(&f.ed, f.ed.now_ms + 400);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), g);
    yew_ed_free(&f.ed);
}

void test_drag_passing_over_three_groups_opens_none(void)
{
    DragFixture f;
    u32 g1;
    u32 g2;
    u32 g3;
    i64 t;

    dg_fixture(&f, 7U);
    g1 = dg_make_group(&f, 2, 3);
    g2 = dg_make_group(&f, 4, 5);
    g3 = dg_make_group(&f, 6, 7);
    yew_tab_switch(&f.ed, 0);
    yew_ed_layout(&f.ed);
    dg_paint(&f);

    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);

        yew_mouse_event(&f.ed, &press);
    }
    t = f.ed.now_ms;
    /* Three groups crossed in 300 ms: 100 ms each, none of them long
     * enough, and the clock restarts on every change of target. */
    {
        u32 gids[3];
        int i;

        gids[0] = g1;
        gids[1] = g2;
        gids[2] = g3;
        for (i = 0; i < 3; i++) {
            int slot = dg_slot_of_payload(-(i32)gids[i]);
            Key motion;

            YEW_ASSERT(slot >= 0);
            f.ed.now_ms = t + 100 * i;
            motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, slot), 0U);
            yew_mouse_event(&f.ed, &motion);
            yew_mouse_tick(&f.ed, f.ed.now_ms);
            YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), 0U);
            dg_paint(&f);
        }
    }
    yew_mouse_tick(&f.ed, t + 300);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), 0U);
    yew_ed_free(&f.ed);
}

/*
 * THE fixture where the two answers disagree.
 *
 * A drag is in flight and the preview has moved the held entry under
 * the pointer, so the REGION at those cells names the held tab.  The
 * pre-drag list still says a group is there.  The dwell must read the
 * pre-drag list, or it would never fire at all — the pointer would
 * always be "over the thing it is holding".
 */
void test_drag_dwell_reads_the_pre_drag_list_not_the_region(void)
{
    DragFixture f;
    u32 g;
    int gslot;
    u16 x;

    dg_fixture(&f, 5U);
    g = dg_make_group(&f, 4, 5);
    yew_tab_switch(&f.ed, 0);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    gslot = dg_slot_of_payload(-(i32)g);
    YEW_ASSERT(gslot >= 0);
    x = dg_slot_x(&f, gslot);

    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, x, 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    /* Repaint WITH the preview: the held entry now occupies the slot. */
    dg_paint(&f);
    {
        Region hit = yew_region_hit(dg_slot_x(&f, gslot), 0U);
        i32 pre = 0;

        /* The two now disagree, which is the whole point of the
         * fixture: the region says the held tab, the pre-drag list says
         * the group. */
        YEW_ASSERT_EQ_U64((u64)hit.kind, (u64)YEW_REGION_TAB);
        YEW_ASSERT_EQ_I64(hit.payload, 0);
        YEW_ASSERT(yew_strip_pre_payload(gslot, &pre));
        YEW_ASSERT_EQ_I64(pre, -(i32)g);
    }
    {
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, gslot), 0U);

        yew_mouse_event(&f.ed, &motion);
    }
    /* And the dwell armed on the GROUP. */
    YEW_ASSERT_EQ_U64(f.ed.mouse.dwell_gid, g);
    yew_mouse_tick(&f.ed, f.ed.now_ms + YEW_DRAG_DWELL_MS);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), g);
    yew_ed_free(&f.ed);
}

/* A tab never dwells into the group it already belongs to: there is
 * nothing to join, and opening the strip would offer a meaningless
 * drop. */
void test_drag_never_dwells_into_its_own_group(void)
{
    DragFixture f;
    u32 g;
    int gslot;

    dg_fixture(&f, 5U);
    g = dg_make_group(&f, 4, 5);
    yew_tab_switch(&f.ed, 4);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    gslot = dg_slot_of_payload(-(i32)g);
    YEW_ASSERT(gslot >= 0);

    /* Hold a MEMBER of the group, from row 2, and hover the group's own
     * row-1 entry. */
    {
        Key press;
        Key motion;
        Region row2 = yew_region_hit(1U, 1U);

        YEW_ASSERT_EQ_U64((u64)row2.kind, (u64)YEW_REGION_TAB);
        press = dg_ev((u8)YEW_KEY_PRESS, 1U, 1U);
        yew_mouse_event(&f.ed, &press);
        motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, gslot), 0U);
        yew_mouse_event(&f.ed, &motion);
    }
    YEW_ASSERT_EQ_U64(f.ed.mouse.dwell_gid, 0U);
    yew_mouse_tick(&f.ed, f.ed.now_ms + 1000);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), 0U);
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* Joining a group                                                   */
/* ---------------------------------------------------------------- */

/*
 * DoD: the join reaches the same state the keyboard path does.
 *
 * Asserted by comparing FULL membership and ordinal snapshots, not by
 * spot-checking one tab: the ordinal off-by-one Sprint 24 pinned only
 * shows up as a whole-list disagreement.
 */
void test_drag_join_matches_the_keyboard_sequence(void)
{
    DragFixture mouse;
    DragFixture keys;
    TabSnap via_mouse;
    TabSnap via_keys;
    u32 g;
    int gslot;

    /* The mouse path: drag tab 0 onto the group, dwell, drop on row 2. */
    dg_fixture(&mouse, 5U);
    g = dg_make_group(&mouse, 4, 5);
    yew_tab_switch(&mouse.ed, 0);
    yew_ed_layout(&mouse.ed);
    dg_paint(&mouse);
    gslot = dg_slot_of_payload(-(i32)g);
    YEW_ASSERT(gslot >= 0);
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&mouse, 0), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT,
                           dg_slot_x(&mouse, gslot), 0U);

        yew_mouse_event(&mouse.ed, &press);
        yew_mouse_event(&mouse.ed, &motion);
        yew_mouse_tick(&mouse.ed, mouse.ed.now_ms + YEW_DRAG_DWELL_MS);
        YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&mouse.ed), g);
        dg_paint(&mouse);
        {
            /* Row 2's blank tail: append to the group. */
            Key up = dg_ev((u8)YEW_KEY_RELEASE, 78U, 1U);

            yew_mouse_event(&mouse.ed, &up);
        }
    }
    via_mouse = dg_snap(&mouse.ed);

    /* The keyboard path: Sprint 24's commands, in Sprint 24's order. */
    dg_fixture(&keys, 5U);
    g = dg_make_group(&keys, 4, 5);
    yew_tab_switch(&keys.ed, 0);
    {
        int pos = yew_group_member_count(&keys.ed, g) + 1;
        int members[16];
        int n;
        int lowest = -1;
        int i;

        yew_group_add_member(&keys.ed, g, 0);
        yew_group_set_ordinal(&keys.ed, 0, pos);
        n = yew_group_members(&keys.ed, g, members,
                              (int)YEW_ARRAY_LEN(members));
        for (i = 0; i < n; i++) {
            if (lowest < 0 || members[i] < lowest)
                lowest = members[i];
        }
        yew_group_reorder_block(&keys.ed, g, lowest);
    }
    via_keys = dg_snap(&keys.ed);

    YEW_ASSERT_EQ_U64(via_mouse.n, via_keys.n);
    YEW_ASSERT(dg_snap_eq(&via_mouse, &via_keys));
    yew_ed_free(&mouse.ed);
    yew_ed_free(&keys.ed);
}

/*
 * Row 1's blank tail is the ONLY way to carry a tab out of a group when
 * the group is the sole row-1 entry — there is nothing else to aim at.
 */
void test_drag_drop_on_the_blank_tail_leaves_a_sole_group(void)
{
    DragFixture f;
    u32 g;
    u32 held;

    dg_fixture(&f, 2U);
    /* Every tab in one group, so row 1 has exactly one entry. */
    g = yew_group_create(&f.ed, "/src", "grp");
    yew_group_add_member(&f.ed, g, 0);
    yew_group_add_member(&f.ed, g, 1);
    yew_group_add_member(&f.ed, g, 2);
    yew_tab_switch(&f.ed, 0);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    YEW_ASSERT_EQ_I64(yew_strip_slot_count(), 1);

    held = yew_tab_at(&f.ed, 1)->tab_id;
    {
        /* Press a member on row 2, drag right onto row 1's blank tail. */
        Key press = dg_ev((u8)YEW_KEY_PRESS, 12U, 1U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, 70U, 0U);
        Key up = dg_ev((u8)YEW_KEY_RELEASE, 70U, 0U);
        Region row2 = yew_region_hit(12U, 1U);

        YEW_ASSERT_EQ_U64((u64)row2.kind, (u64)YEW_REGION_TAB);
        YEW_ASSERT_EQ_I64(row2.payload, 1);
        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
        YEW_ASSERT_EQ_U64((u64)f.ed.mouse.phase, (u64)YEW_MP_DRAG_TAB);
        dg_paint(&f);
        yew_mouse_event(&f.ed, &up);
    }
    {
        int idx = yew_tab_index_of_id(&f.ed, held);

        YEW_ASSERT(idx >= 0);
        YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, idx)->group_id, 0U);
    }
    /* The group survives with its other two members. */
    YEW_ASSERT_EQ_I64(yew_group_member_count(&f.ed, g), 2);
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* Auto-scroll                                                       */
/* ---------------------------------------------------------------- */

/*
 * Held over a chevron, the strip scrolls ON A TIMER.
 *
 * A fast pointer emits far more motion reports than a slow one, so a
 * strip that scrolled per report would fly past the target at a speed
 * that depends on how the terminal batches its reports.  A thousand
 * motion events at one instant must move it exactly nothing.
 */
void test_drag_autoscroll_is_throttled_to_one_entry_per_window(void)
{
    DragFixture f;
    int scroll_before;
    int i;
    i64 t0;

    dg_fixture(&f, 7U);
    /* A narrow strip, so the `>N` chevron exists to hold over.  RESIZE
     * rather than a second init, which would leak the first's
     * buffers. */
    YEW_ASSERT(yew_grid_resize(&f.ed.grid, 24U, 24U));
    yew_ed_layout(&f.ed);
    dg_paint(&f);

    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);

        yew_mouse_event(&f.ed, &press);
    }
    /* Find the chevron and park the pointer on it. */
    {
        u16 x;
        u16 chev = 0U;
        bool found = false;

        for (x = 0U; x < 24U; x++) {
            if (yew_region_hit(x, 0U).kind == YEW_REGION_TAB_SCROLL) {
                chev = x;
                found = true;
                break;
            }
        }
        YEW_ASSERT(found);
        scroll_before = f.ed.tabs.scroll;
        t0 = f.ed.now_ms;
        for (i = 0; i < 1000; i++) {
            Key motion = dg_ev((u8)YEW_KEY_REPEAT, chev, 0U);

            yew_mouse_event(&f.ed, &motion);
        }
        /* A thousand reports at one instant: no scroll at all. */
        YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, scroll_before);

        /*
         * The clock is what moves it, one entry per window.  Absolute
         * timestamps, because yew_mouse_tick advances the editor's own
         * clock — reading it back would let the test drift a window per
         * call and prove nothing about the throttle.
         */
        yew_mouse_tick(&f.ed, t0 + YEW_DRAG_SCROLL_MS);
        YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, scroll_before + 1);
        yew_mouse_tick(&f.ed, t0 + YEW_DRAG_SCROLL_MS + 1);
        YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, scroll_before + 1);
        yew_mouse_tick(&f.ed, t0 + 2 * YEW_DRAG_SCROLL_MS);
        YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, scroll_before + 2);
    }
    yew_ed_free(&f.ed);
}

/*
 * The router's deadline is what makes the dwell possible at all: a
 * pointer resting on a group emits no further events, so the loop has
 * to be told to wake up.
 */
void test_drag_reports_a_deadline_while_dwelling(void)
{
    DragFixture f;
    u32 g;
    int gslot;

    dg_fixture(&f, 5U);
    g = dg_make_group(&f, 4, 5);
    yew_tab_switch(&f.ed, 0);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    gslot = dg_slot_of_payload(-(i32)g);
    YEW_ASSERT(gslot >= 0);

    /* Idle: nothing to wake up for. */
    YEW_ASSERT_EQ_I64(yew_mouse_deadline(&f.ed, f.ed.now_ms), -1);
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, gslot), 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    YEW_ASSERT_EQ_I64(yew_mouse_deadline(&f.ed, f.ed.now_ms),
                      YEW_DRAG_DWELL_MS);
    YEW_ASSERT_EQ_I64(yew_mouse_deadline(&f.ed, f.ed.now_ms + 399), 1);
    YEW_ASSERT_EQ_I64(yew_mouse_deadline(&f.ed, f.ed.now_ms + 400), 0);
    /* Once it has fired there is nothing left to wait for. */
    yew_mouse_tick(&f.ed, f.ed.now_ms + 400);
    YEW_ASSERT_EQ_I64(yew_mouse_deadline(&f.ed, f.ed.now_ms + 400), -1);
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* Dragging a group                                                  */
/* ---------------------------------------------------------------- */

void test_drag_a_group_moves_the_whole_block(void)
{
    DragFixture f;
    u32 g;
    int gslot;
    u32 member_a;
    u32 member_b;

    dg_fixture(&f, 5U);
    g = dg_make_group(&f, 0, 1);
    yew_tab_switch(&f.ed, 3);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    member_a = yew_tab_at(&f.ed, 0)->tab_id;
    member_b = yew_tab_at(&f.ed, 1)->tab_id;
    gslot = dg_slot_of_payload(-(i32)g);
    YEW_ASSERT_EQ_I64(gslot, 0);

    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, gslot), 0U);
        Key motion;
        Key up;

        yew_mouse_event(&f.ed, &press);
        YEW_ASSERT_EQ_U64(f.ed.mouse.drag_gid, g);
        motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, 2), 0U);
        yew_mouse_event(&f.ed, &motion);
        YEW_ASSERT_EQ_U64((u64)f.ed.mouse.phase, (u64)YEW_MP_DRAG_GROUP);
        dg_paint(&f);
        up = dg_ev((u8)YEW_KEY_RELEASE, dg_slot_x(&f, 2), 0U);
        yew_mouse_event(&f.ed, &up);
    }
    /* Both members travelled, and they are still contiguous. */
    {
        int a = yew_tab_index_of_id(&f.ed, member_a);
        int b = yew_tab_index_of_id(&f.ed, member_b);

        YEW_ASSERT(a >= 0 && b >= 0);
        YEW_ASSERT_EQ_I64(b, a + 1);
        YEW_ASSERT(a > 0);
    }
    yew_ed_free(&f.ed);
}
