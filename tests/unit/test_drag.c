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

void test_drag_dwell_opens_a_group_at_250ms_and_not_at_249(void)
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

    /* 249 ms: still counting.  A drag that merely PASSES over a group
     * must not make its members flash open. */
    yew_mouse_tick(&f.ed, f.ed.now_ms + YEW_DRAG_DWELL_MS - 1);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), 0U);

    /* 250 ms: open. */
    yew_mouse_tick(&f.ed, f.ed.now_ms + YEW_DRAG_DWELL_MS);
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
 * the pointer, so the REGION at those cells is the held entry's — since
 * 57.14 §2 that means nothing at all, because the gap it leaves is drawn
 * and never registered.  The pre-drag list still says a group is there.
 * The dwell must read the pre-drag list, or it would never fire — the
 * pointer is always over the thing it is holding.
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

        /*
         * The two now disagree, which is the whole point of the fixture.
         * Sprint 27 had the region name the held tab here; since 57.14
         * §2 the held entry is a GAP that registers nothing at all — so
         * the registry answers NONE where the pre-drag list still says a
         * group is.  Either way the region table cannot answer the
         * dwell's question, and the pre-drag list is the only thing that
         * can.
         */
        YEW_ASSERT_EQ_U64((u64)hit.kind, (u64)YEW_REGION_NONE);
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
void test_drag_reports_a_deadline_at_every_flash_edge(void)
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
    /*
     * EVERY phase edge, because each one repaints the cue: the three
     * quarter boundaries, then the open.  Sprint 27 waited only for the
     * open, which is why a cue was not expressible then.
     */
    YEW_ASSERT_EQ_I64(yew_mouse_deadline(&f.ed, f.ed.now_ms),
                      YEW_DRAG_FLASH_MS);
    YEW_ASSERT_EQ_I64(yew_mouse_deadline(&f.ed,
                                         f.ed.now_ms + YEW_DRAG_FLASH_MS),
                      YEW_DRAG_FLASH_MS);
    YEW_ASSERT_EQ_I64(
        yew_mouse_deadline(&f.ed, f.ed.now_ms + 2 * YEW_DRAG_FLASH_MS),
        YEW_DRAG_FLASH_MS);
    /* Past the third quarter the next thing to happen is the open, so
     * the settle is one sleep and not two. */
    YEW_ASSERT_EQ_I64(
        yew_mouse_deadline(&f.ed, f.ed.now_ms + 3 * YEW_DRAG_FLASH_MS),
        YEW_DRAG_DWELL_MS - 3 * YEW_DRAG_FLASH_MS);
    YEW_ASSERT_EQ_I64(yew_mouse_deadline(&f.ed,
                                         f.ed.now_ms + YEW_DRAG_DWELL_MS - 1),
                      1);
    YEW_ASSERT_EQ_I64(yew_mouse_deadline(&f.ed,
                                         f.ed.now_ms + YEW_DRAG_DWELL_MS), 0);
    /* Once it has fired there is nothing left to wait for. */
    yew_mouse_tick(&f.ed, f.ed.now_ms + YEW_DRAG_DWELL_MS);
    YEW_ASSERT_EQ_I64(yew_mouse_deadline(&f.ed, f.ed.now_ms), -1);
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

/* ---------------------------------------------------------------- */
/* Sprint 57.14 §1: row 1 is the group exit                          */
/* ---------------------------------------------------------------- */

/* The middle of the row-2 member entry whose payload is `want`. */
static u16 dg_row2_x(DragFixture *f, i32 want)
{
    u16 x;

    (void)f;
    for (x = 0U; x < 80U; x++) {
        Region hit = yew_region_hit(x, 1U);

        if (hit.kind == YEW_REGION_TAB && hit.payload == want)
            return x;
    }
    YEW_ASSERT(false);
    return 0U;
}

/*
 * A member dropped on an ordinary row-1 slot LEAVES its group.
 *
 * Row 1 is the ungrouped bar, so landing on it means "live here"; row 2
 * is the only place a drop JOINS.  Before 57.14 the blank tail was the
 * only exit, and §1 explains why that was a lockout rather than a
 * preference.
 */
void test_drag_row1_slot_extracts_a_member_from_its_group(void)
{
    DragFixture f;
    u32 g;
    u32 held;
    int target;

    dg_fixture(&f, 5U);
    g = dg_make_group(&f, 4, 5);
    yew_tab_switch(&f.ed, 4);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    held = yew_tab_at(&f.ed, 4)->tab_id;
    target = dg_slot_of_payload(1);
    YEW_ASSERT(target >= 0);

    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_row2_x(&f, 4), 1U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, target), 0U);
        Key up = dg_ev((u8)YEW_KEY_RELEASE, dg_slot_x(&f, target), 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
        YEW_ASSERT_EQ_U64((u64)f.ed.mouse.phase, (u64)YEW_MP_DRAG_TAB);
        dg_paint(&f);
        yew_mouse_event(&f.ed, &up);
    }
    {
        int idx = yew_tab_index_of_id(&f.ed, held);

        YEW_ASSERT_EQ_I64(idx, 1);
        YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, idx)->group_id, 0U);
    }
    /* The group survives, with the member that stayed. */
    YEW_ASSERT_EQ_I64(yew_group_member_count(&f.ed, g), 1);
    yew_ed_free(&f.ed);
}

/*
 * THE DISSOLVE.  The extracted tab was its group's LAST member, so
 * yew_group_remove_member deletes the group — and with it a row-1 entry,
 * which renumbers every slot to its right.  The drop was aimed at a slot
 * to the group's right, so a destination re-read after the removal would
 * name the wrong tab.  Resolving to a tab INDEX first is what makes this
 * land where the user pointed.
 */
void test_drag_extracting_a_sole_member_onto_a_slot_to_its_right(void)
{
    DragFixture f;
    u32 g;
    u32 held;
    u32 passed_a;
    u32 passed_b;
    int target;

    dg_fixture(&f, 3U);
    g = yew_group_create(&f.ed, "/src", "grp");
    YEW_ASSERT(g != 0U);
    yew_group_add_member(&f.ed, g, 1);
    yew_tab_switch(&f.ed, 1);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    /* Four row-1 entries: tab 0, the group, tabs 2 and 3. */
    YEW_ASSERT_EQ_I64(yew_strip_slot_count(), 4);
    held = yew_tab_at(&f.ed, 1)->tab_id;
    passed_a = yew_tab_at(&f.ed, 2)->tab_id;
    passed_b = yew_tab_at(&f.ed, 3)->tab_id;
    target = dg_slot_of_payload(3);
    YEW_ASSERT_EQ_I64(target, 3);

    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_row2_x(&f, 1), 1U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, target), 0U);
        Key up = dg_ev((u8)YEW_KEY_RELEASE, dg_slot_x(&f, target), 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
        dg_paint(&f);
        yew_mouse_event(&f.ed, &up);
    }
    /* The group is gone, and the tab landed on the slot it was aimed
     * at — the two it passed keeping their relative order. */
    YEW_ASSERT_EQ_I64(yew_group_member_count(&f.ed, g), 0);
    YEW_ASSERT_EQ_I64(yew_group_find(&f.ed, g), -1);
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&f.ed, passed_a), 1);
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&f.ed, passed_b), 2);
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&f.ed, held), 3);
    YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, 3)->group_id, 0U);
    /* And row 1 is one entry SHORTER than it was: the dissolve deleted
     * the group's entry and renumbered everything to its right. */
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    YEW_ASSERT_EQ_I64(yew_strip_slot_count(), 4);
    yew_ed_free(&f.ed);
}

/*
 * THE LOCKOUT, tested where it bit.  strip_render draws the tail control
 * only in its `draw_new` arm, which an overflowing row 1 never reaches —
 * so with the strip overflowing there was NOTHING to aim at, and a group
 * member could not be dragged out at all.  Every visible slot is an exit
 * now, so the gesture works with no tail on screen.
 */
void test_drag_row1_exit_works_when_the_strip_overflows(void)
{
    DragFixture f;
    u32 g;
    u32 held;
    int target = -1;
    u16 x;
    bool tail = false;
    bool chevron = false;

    dg_fixture(&f, 7U);
    /* The group sits EARLY, with plenty of entries to its right, so the
     * strip overflows to the right and strip_render's `draw_new` arm —
     * the only place the tail is drawn — is never reached. */
    g = dg_make_group(&f, 1, 2);
    yew_tab_switch(&f.ed, 1);
    /* Narrow enough that row 1 overflows.  RESIZE rather than a second
     * init, which would leak the first's buffers. */
    YEW_ASSERT(yew_grid_resize(&f.ed.grid, 24U, 24U));
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    for (x = 0U; x < 24U; x++) {
        Region hit = yew_region_hit(x, 0U);

        if (hit.kind == YEW_REGION_TAB_NEW)
            tail = true;
        if (hit.kind == YEW_REGION_TAB_SCROLL)
            chevron = true;
    }
    /* The premise: overflow, and therefore no tail to aim at. */
    YEW_ASSERT(chevron);
    YEW_ASSERT(!tail);

    held = yew_tab_at(&f.ed, 1)->tab_id;
    for (x = 0U; x < 24U && target < 0; x++) {
        int slot = yew_strip_slot_at(x, 0U);
        i32 pre = 0;

        if (slot >= 0 && yew_strip_pre_payload(slot, &pre) && pre >= 0)
            target = slot;
    }
    YEW_ASSERT(target >= 0);
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_row2_x(&f, 1), 1U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, target), 0U);
        Key up = dg_ev((u8)YEW_KEY_RELEASE, dg_slot_x(&f, target), 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
        dg_paint(&f);
        yew_mouse_event(&f.ed, &up);
    }
    {
        int idx = yew_tab_index_of_id(&f.ed, held);

        YEW_ASSERT(idx >= 0);
        YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, idx)->group_id, 0U);
    }
    YEW_ASSERT_EQ_I64(yew_group_member_count(&f.ed, g), 1);
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* Sprint 57.14 §2: the float and the gap                            */
/* ---------------------------------------------------------------- */

/* The text of grid row `y`, single-byte cells only. */
static void dg_row_text(const Ed *ed, u16 y, char *out, size_t cap)
{
    size_t n = 0U;
    u16 x;

    for (x = 0U; x < ed->grid.cols && n + 1U < cap; x++) {
        const Cell *c = &ed->grid.back[(size_t)y * ed->grid.cols + x];

        out[n++] = (char)(c->utf8[0] >= 32U && c->utf8[0] < 127U
                              ? c->utf8[0] : ' ');
    }
    out[n] = '\0';
}

/*
 * THE FLOAT IS DRAWN AND NEVER REGISTERED.
 *
 * Drawn: its cells carry the held tab's name.  Never registered: the
 * registry answers nothing over any of them.  A region there would make
 * the pointer hover whatever it is holding, wherever it went — the same
 * failure the pre-drag slot table exists to keep out of the dwell.
 */
void test_drag_float_is_drawn_and_registers_no_region(void)
{
    DragFixture f;
    Rect fl;
    char row[128];
    u16 x;

    dg_fixture(&f, 5U);
    dg_paint(&f);
    /* Nothing in flight: no float. */
    YEW_ASSERT_EQ_U64(yew_strip_float_rect().w, 0U);
    {
        /* Grabbed one cell into the entry, and carried to a column far
         * from every entry so the cells under it are unclaimed. */
        Key press = dg_ev((u8)YEW_KEY_PRESS, (u16)(dg_slot_x(&f, 1) + 1U),
                          0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, 60U, 3U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    YEW_ASSERT_EQ_U64((u64)f.ed.mouse.phase, (u64)YEW_MP_DRAG_TAB);
    dg_paint(&f);
    fl = yew_strip_float_rect();
    YEW_ASSERT(fl.w > 0U);
    YEW_ASSERT_EQ_U64(fl.y, 3U);
    /* It follows the pointer's grip, not the pointer's left edge. */
    YEW_ASSERT_EQ_U64(fl.x, 59U);
    dg_row_text(&f.ed, fl.y, row, sizeof(row));
    YEW_ASSERT_NOT_NULL(strstr(row, "yew-drag-0.txt"));
    /* And the registry knows nothing about any of it. */
    for (x = fl.x; x < (u16)(fl.x + fl.w); x++)
        YEW_ASSERT_EQ_U64((u64)yew_region_hit(x, fl.y).kind,
                          (u64)YEW_REGION_NONE);
    /* The float has no number: the numbers address POSITIONS on a row,
     * and a float is between them. */
    YEW_ASSERT(strstr(row + fl.x, " 2 yew-drag-0.txt") == NULL);
    /*
     * Moving the pointer inside ONE cell repaints nothing; crossing into
     * the next cell repaints, because that is where the float now is.
     */
    f.ed.full_damage = false;
    {
        Key same = dg_ev((u8)YEW_KEY_REPEAT, 60U, 3U);
        Key next = dg_ev((u8)YEW_KEY_REPEAT, 61U, 3U);

        yew_mouse_event(&f.ed, &same);
        YEW_ASSERT(!f.ed.full_damage);
        yew_mouse_event(&f.ed, &next);
        YEW_ASSERT(f.ed.full_damage);
    }
    /* And the release takes it off the screen, even though this drag
     * never named a target and changes nothing. */
    f.ed.full_damage = false;
    {
        Key up = dg_ev((u8)YEW_KEY_RELEASE, 61U, 3U);

        yew_mouse_event(&f.ed, &up);
    }
    YEW_ASSERT(f.ed.full_damage);
    dg_paint(&f);
    YEW_ASSERT_EQ_U64(yew_strip_float_rect().w, 0U);
    yew_ed_free(&f.ed);
}

/*
 * The held entry exists EXACTLY ONCE.
 *
 * The insertion shift still permutes the list — that is the whole point
 * of Sprint 27's preview, and the gap is where the drop lands — but the
 * entry itself is no longer drawn there, and no region carries its
 * payload.  The pre-drag slot table still does, because that is what the
 * drop aims with.
 */
void test_drag_held_entry_leaves_a_gap_in_the_strip(void)
{
    DragFixture f;
    char before[128];
    char during[128];
    u16 x;
    bool held_region = false;

    dg_fixture(&f, 5U);
    dg_paint(&f);
    dg_row_text(&f.ed, 0U, before, sizeof(before));
    YEW_ASSERT_NOT_NULL(strstr(before, "yew-drag-0.txt"));
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 1), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, 3), 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    dg_paint(&f);
    for (x = 0U; x < 80U; x++) {
        Region hit = yew_region_hit(x, 0U);

        if (hit.kind == YEW_REGION_TAB && hit.payload == 1)
            held_region = true;
    }
    /* No entry, and no target for it. */
    YEW_ASSERT(!held_region);
    {
        int at = -1;
        int i;

        for (i = 0; i < yew_strip_slot_count(); i++) {
            i32 pre = 0;

            if (yew_strip_pre_payload(i, &pre) && pre == 1)
                at = i;
        }
        /* The pre-drag table still names the held tab at its ORIGINAL
         * slot — that list never moves, which is exactly why the dwell
         * and the drop read it. */
        YEW_ASSERT_EQ_I64(at, 1);
    }
    dg_row_text(&f.ed, 0U, during, sizeof(during));
    /* Drawn once: the strip row no longer carries it at all, only the
     * float does — and the float is on the same row here. */
    {
        Rect fl = yew_strip_float_rect();
        char strip_only[128];
        u16 i;

        YEW_ASSERT_EQ_U64(fl.y, 0U);
        (void)memcpy(strip_only, during, sizeof(strip_only));
        for (i = fl.x; i < (u16)(fl.x + fl.w) && i < 80U; i++)
            strip_only[i] = '.';
        YEW_ASSERT(strstr(strip_only, "yew-drag-0.txt") == NULL);
    }
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* Sprint 57.14 §3: the dwell's two-flash cue                        */
/* ---------------------------------------------------------------- */

/*
 * Does the group's row-1 entry render reversed right now?
 *
 * Found through the REGION the render registered, not through the
 * pre-drag slot table: the preview has permuted the strip, so the cells
 * a slot number names are not where that entry is currently drawn.
 */
static bool dg_entry_reversed(DragFixture *f, u32 gid)
{
    u16 x;

    for (x = 0U; x < f->ed.grid.cols; x++) {
        Region hit = yew_region_hit(x, 0U);

        if (hit.kind == YEW_REGION_TAB && hit.payload == -(i32)gid)
            return (f->ed.grid.back[x].attrs & YEW_ATTR_REVERSE) != 0U;
    }
    YEW_ASSERT(false);
    return false;
}

/*
 * A quarter of the dwell, lit, dark, lit, dark — then the strip opens.
 *
 * THE CUE IS A FUNCTION OF THE CLOCK, not of a paint counter: the test
 * paints the SAME instant twice at two points and gets the same cells,
 * which is invariant 5 applied to something that blinks.  Nothing here
 * sleeps; ed->now_ms is the clock.
 */
void test_drag_dwell_flashes_twice_before_opening(void)
{
    DragFixture f;
    u32 g;
    int gslot;
    i64 t0;
    bool unlit;
    int i;
    static const struct {
        i64 at;
        bool lit;
    } phases[] = {
        {0, true},          /* the cue starts immediately */
        {1, true},
        {YEW_DRAG_FLASH_MS - 1, true},
        {YEW_DRAG_FLASH_MS, false},      /* first gap */
        {2 * YEW_DRAG_FLASH_MS, true},   /* second flash */
        {3 * YEW_DRAG_FLASH_MS, false},  /* settle */
        /* The clamp: 4·FLASH is 248, inside the dwell, and an unclamped
         * quarter would light the cue for the two milliseconds before
         * the strip opens. */
        {4 * YEW_DRAG_FLASH_MS, false},
        {YEW_DRAG_DWELL_MS - 1, false}
    };

    dg_fixture(&f, 5U);
    g = dg_make_group(&f, 4, 5);
    yew_tab_switch(&f.ed, 0);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    gslot = dg_slot_of_payload(-(i32)g);
    YEW_ASSERT(gslot >= 0);
    /* What the entry looks like with no dwell on it, to compare against. */
    unlit = dg_entry_reversed(&f, g);

    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, gslot), 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    YEW_ASSERT_EQ_U64(f.ed.mouse.dwell_gid, g);
    t0 = f.ed.now_ms;
    for (i = 0; i < (int)YEW_ARRAY_LEN(phases); i++) {
        f.ed.now_ms = t0 + phases[i].at;
        YEW_ASSERT_EQ_U64(yew_mouse_dwell_flash(&f.ed),
                          phases[i].lit ? g : 0U);
        dg_paint(&f);
        YEW_ASSERT_EQ_U64(dg_entry_reversed(&f, g),
                          phases[i].lit ? !unlit : unlit);
        /* The same instant, painted again: byte-identical. */
        dg_paint(&f);
        YEW_ASSERT_EQ_U64(dg_entry_reversed(&f, g),
                          phases[i].lit ? !unlit : unlit);
    }
    /* Once the strip is open the cue is done — the members ARE the
     * answer, and a blinking entry above them would still be asking. */
    f.ed.now_ms = t0 + YEW_DRAG_DWELL_MS;
    yew_mouse_tick(&f.ed, f.ed.now_ms);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), g);
    YEW_ASSERT_EQ_U64(yew_mouse_dwell_flash(&f.ed), 0U);
    yew_ed_free(&f.ed);
}

/*
 * The cue marks damage exactly once per edge.
 *
 * The loop paints when something says it must; ticking inside a quarter
 * must say nothing, or a dwell becomes a repaint storm on whatever
 * cadence the loop happens to wake on.
 */
void test_drag_dwell_flash_marks_damage_only_at_an_edge(void)
{
    DragFixture f;
    u32 g;
    int gslot;
    i64 t0;
    int i;
    int edges = 0;

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
    t0 = f.ed.now_ms;
    /* One tick per millisecond across the whole dwell: three quarter
     * boundaries, and not one repaint anywhere else. */
    for (i = 0; i < YEW_DRAG_DWELL_MS; i++) {
        f.ed.full_damage = false;
        yew_mouse_tick(&f.ed, t0 + i);
        if (f.ed.full_damage)
            edges++;
    }
    YEW_ASSERT_EQ_I64(edges, 3);
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* 57.14 × 57.15: the float and the user-owned scroll                */
/* ---------------------------------------------------------------- */

/* The first cell of the `>N` chevron on row 1, or 0xFFFF when none. */
static u16 dg_chevron_x(u16 cols)
{
    u16 x;

    for (x = 0U; x < cols; x++) {
        Region hit = yew_region_hit(x, 0U);

        if (hit.kind == YEW_REGION_TAB_SCROLL && hit.payload > 0)
            return x;
    }
    return 0xFFFFU;
}

/*
 * Sprint 57.14 §2 meets Sprint 57.15 §1.
 *
 * The float is drawn from the PAYLOAD at the pointer, and the gap is a
 * position in the entry list — neither is a function of the offset, so
 * a strip the user scrolled away from the active tab must carry both
 * unchanged.  The interesting half is the other direction: a drag
 * renders row 1 every time the pointer crosses a cell, and each of
 * those renders hands `&ed->tabs.scroll` to the one placement engine,
 * which writes the value back.  If the drag's renders forgot that the
 * offset was the user's, the strip would walk back to the active entry
 * mid-gesture and the drop would land somewhere the user was not
 * looking.
 */
void test_drag_float_survives_a_user_scrolled_strip(void)
{
    DragFixture f;
    u16 chev;
    int scrolled;
    int held_slot = -1;
    int to_slot = -1;
    int i;
    i32 held_payload = 0;
    bool held_region = false;
    Rect fl;
    u16 x;

    dg_fixture(&f, 7U);
    /* Tab 0 stays active and far to the left, so "follow the active
     * entry" and "the offset the user chose" disagree. */
    yew_tab_switch(&f.ed, 0);
    /* Wide enough for two entries beside the chevron — the drag needs
     * two visible slots — and narrow enough to overflow.  RESIZE rather
     * than a second init, which would leak the first's buffers. */
    YEW_ASSERT(yew_grid_resize(&f.ed.grid, 24U, 44U));
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    chev = dg_chevron_x(44U);
    YEW_ASSERT(chev != 0xFFFFU);
    /* Two clicks on the chevron: the offset is now the user's. */
    for (i = 0; i < 2; i++) {
        YEW_ASSERT(yew_tab_strip_click(&f.ed, chev, 0U));
        dg_paint(&f);
    }
    scrolled = f.ed.tabs.scroll;
    YEW_ASSERT(scrolled > 0);
    YEW_ASSERT(yew_tabs_scroll_is_owned(&f.ed.tabs, false));

    /* Two visible slots to drag between, both of them tabs. */
    for (x = 0U; x < 44U; x++) {
        int slot = yew_strip_slot_at(x, 0U);
        i32 pre = 0;

        if (slot < 0 || !yew_strip_pre_payload(slot, &pre) || pre < 0)
            continue;
        if (held_slot < 0)
            held_slot = slot;
        else if (slot != held_slot)
            to_slot = slot;
    }
    YEW_ASSERT(held_slot >= 0);
    YEW_ASSERT(to_slot >= 0);
    YEW_ASSERT(yew_strip_pre_payload(held_slot, &held_payload));
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, held_slot), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, to_slot), 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    YEW_ASSERT_EQ_U64((u64)f.ed.mouse.phase, (u64)YEW_MP_DRAG_TAB);
    dg_paint(&f);
    /* The render did not walk the offset back toward tab 0. */
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, scrolled);
    YEW_ASSERT(yew_tabs_scroll_is_owned(&f.ed.tabs, false));
    /* The float is at the pointer and claims nothing. */
    fl = yew_strip_float_rect();
    YEW_ASSERT(fl.w > 0U);
    for (x = fl.x; x < (u16)(fl.x + fl.w); x++)
        YEW_ASSERT_EQ_U64((u64)yew_region_hit(x, fl.y).kind,
                          (u64)YEW_REGION_NONE);
    /* And the gap: the held entry has no region on the scrolled row. */
    for (x = 0U; x < 44U; x++) {
        Region hit = yew_region_hit(x, 0U);

        if (hit.kind == YEW_REGION_TAB && hit.payload == held_payload)
            held_region = true;
    }
    YEW_ASSERT(!held_region);
    /* A second paint of the same state is the same offset (invariant
     * 5): the walk-back would have shown up here if anywhere. */
    dg_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, scrolled);
    yew_ed_free(&f.ed);
}

/*
 * The drag AUTOSCROLL (Sprint 27) is one of Sprint 57.15's explicit
 * gestures: holding the pointer on the chevron is the user asking for
 * the offset, so `strip_scroll` takes ownership and the strip stays
 * where the drop landed instead of snapping back to an active tab the
 * user dragged away from.  On `tab-drag-feel` alone the very next
 * render walked it back, which is the defect 57.15 §1 exists for.
 *
 * Nothing here has to clear the flag afterwards: the drop reorders the
 * tab and does not change which tab is ACTIVE, and `yew_tab_switch` —
 * the one funnel every change of the active entry comes through — is
 * what resumes the follow.  Asserted at the bottom.
 */
void test_drag_autoscroll_keeps_the_strip_where_the_drop_landed(void)
{
    DragFixture f;
    u16 chev;
    i64 t0;
    int scrolled;

    dg_fixture(&f, 7U);
    yew_tab_switch(&f.ed, 0);
    YEW_ASSERT(yew_grid_resize(&f.ed.grid, 24U, 24U));
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    YEW_ASSERT(!yew_tabs_scroll_is_owned(&f.ed.tabs, false));
    chev = dg_chevron_x(24U);
    YEW_ASSERT(chev != 0xFFFFU);
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, chev, 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    t0 = f.ed.now_ms;
    yew_mouse_tick(&f.ed, t0 + YEW_DRAG_SCROLL_MS);
    yew_mouse_tick(&f.ed, t0 + 2 * YEW_DRAG_SCROLL_MS);
    scrolled = f.ed.tabs.scroll;
    YEW_ASSERT(scrolled >= 2);
    YEW_ASSERT(yew_tabs_scroll_is_owned(&f.ed.tabs, false));
    /* The render that follows the autoscroll does not undo it. */
    dg_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, scrolled);
    {
        Key up = dg_ev((u8)YEW_KEY_RELEASE, chev, 0U);

        yew_mouse_event(&f.ed, &up);
    }
    dg_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, scrolled);
    YEW_ASSERT(yew_tabs_scroll_is_owned(&f.ed.tabs, false));
    /* And the follow resumes the moment the active entry moves. */
    yew_tab_switch(&f.ed, 0);
    YEW_ASSERT(!yew_tabs_scroll_is_owned(&f.ed.tabs, false));
    dg_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, 0);
    yew_ed_free(&f.ed);
}

/*
 * Sprint 57.15 §2's reveal is a NO-BUTTON clock, and a press is the end
 * of no-button.
 *
 * `hover_track` only ever runs on a motion report with no button held,
 * so once a button goes down `hover_x`/`hover_y` freeze at the last
 * cell the pointer visited unheld.  If that cell was a chevron, the
 * reveal keeps firing every 300 ms against a stale position for as long
 * as the button is down — the strip runs away under a drag that is
 * nowhere near it, and it fights the drag's own 120 ms autoscroll for
 * the same offset.  The press ends the hover; the next unheld motion
 * report is what arms it again.
 */
void test_drag_a_press_ends_the_hover_reveal(void)
{
    DragFixture f;
    u16 chev;
    i64 t0;
    int after_hover;

    dg_fixture(&f, 7U);
    yew_tab_switch(&f.ed, 0);
    YEW_ASSERT(yew_grid_resize(&f.ed.grid, 24U, 24U));
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    chev = dg_chevron_x(24U);
    YEW_ASSERT(chev != 0xFFFFU);
    t0 = f.ed.now_ms;
    {
        Key hover = dg_ev((u8)YEW_KEY_REPEAT, chev, 0U);

        hover.button = (u8)YEW_MB_NONE;
        yew_mouse_event(&f.ed, &hover);
    }
    YEW_ASSERT(f.ed.mouse.hover_chevron);
    /* One reveal step, to prove the clock is live before the press. */
    yew_mouse_tick(&f.ed, t0 + YEW_HOVER_SCROLL_MS);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, 1);
    dg_paint(&f);
    after_hover = f.ed.tabs.scroll;
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, after_hover),
                          0U);

        yew_mouse_event(&f.ed, &press);
    }
    YEW_ASSERT(!f.ed.mouse.hover_chevron);
    /*
     * Two whole reveal windows with the button down and the pointer
     * never returning: the strip does not move, and the deadline does
     * not ask the loop to wake for a reveal that is not running.
     */
    YEW_ASSERT_EQ_I64(yew_mouse_deadline(&f.ed, t0 + YEW_HOVER_SCROLL_MS),
                      -1);
    yew_mouse_tick(&f.ed, t0 + 2 * YEW_HOVER_SCROLL_MS);
    yew_mouse_tick(&f.ed, t0 + 3 * YEW_HOVER_SCROLL_MS);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, after_hover);
    yew_ed_free(&f.ed);
}

/*
 * Sprint 57.15 §2's mode-1003 owner reads a product of the render, and
 * Sprint 57.14 added a float to that render which is drawn LAST and
 * registers nothing.  The float must therefore leave the chevron
 * answer exactly as it found it, in flight and after the drop — a
 * float that silenced the strip would strand the hover reveal for the
 * rest of the session.
 */
void test_drag_float_does_not_disturb_the_chevron_answer(void)
{
    DragFixture f;
    u16 chev;

    dg_fixture(&f, 7U);
    /* Tab 0 active, so the strip sits at offset 0 and the only chevron
     * is the `>N` on the right — `yew_strip_layout`'s walk only ever
     * moves the first visible entry FORWARD, so an offset left over
     * from a narrow strip would still draw a `<` on a wide one. */
    yew_tab_switch(&f.ed, 0);
    YEW_ASSERT(yew_grid_resize(&f.ed.grid, 24U, 24U));
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    YEW_ASSERT(yew_mouse_chevron_drawn());
    chev = dg_chevron_x(24U);
    YEW_ASSERT(chev != 0xFFFFU);
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, chev, 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    dg_paint(&f);
    YEW_ASSERT(yew_strip_float_rect().w > 0U);
    YEW_ASSERT(yew_mouse_chevron_drawn());
    {
        Key up = dg_ev((u8)YEW_KEY_RELEASE, chev, 0U);

        yew_mouse_event(&f.ed, &up);
    }
    dg_paint(&f);
    YEW_ASSERT_EQ_U64(yew_strip_float_rect().w, 0U);
    YEW_ASSERT(yew_mouse_chevron_drawn());
    /*
     * And the answer comes back DOWN when the strip stops needing it.
     * The switch first, because `yew_strip_layout`'s walk only moves
     * the first visible entry forward: an offset the drag left behind
     * still draws a `<` on a strip wide enough for everything, and
     * following the active entry is what brings it home.
     */
    yew_tab_switch(&f.ed, 0);
    YEW_ASSERT(yew_grid_resize(&f.ed.grid, 24U, 200U));
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    YEW_ASSERT_EQ_I64(f.ed.tabs.scroll, 0);
    YEW_ASSERT(!yew_mouse_chevron_drawn());
    yew_ed_free(&f.ed);
}
