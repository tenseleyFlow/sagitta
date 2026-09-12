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

/*
 * The NAME above carries a number, so the number is pinned here: a
 * retune that leaves the name behind fails to compile rather than
 * quietly lying about what it proves.
 */
_Static_assert(YEW_DRAG_DWELL_MS == 500,
               "the dwell boundary row is named for 500 ms");
_Static_assert(YEW_DRAG_FLASH_MS == 125,
               "the cue is a derived quarter of the dwell");

void test_drag_dwell_opens_a_group_at_500ms_and_not_at_499(void)
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

    /* 499 ms: still counting.  A drag that merely PASSES over a group
     * must not make its members flash open. */
    yew_mouse_tick(&f.ed, f.ed.now_ms + YEW_DRAG_DWELL_MS - 1);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), 0U);

    /* 500 ms: open. */
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

/* The row-1 entry index the strip is drawing as active. */
static int dg_active_entry(DragFixture *f)
{
    StripEntry entries[YEW_TAB_MAX];
    int n = yew_tab_row1_entries(&f->ed, entries,
                                 (int)YEW_ARRAY_LEN(entries));

    return yew_tab_row1_active(&f->ed, entries, n);
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
        /* The fourth boundary.  At 500 ms it IS the dwell, so the cue
         * is already done; the row stays because the quarter clamp is
         * what keeps it dark for any dwell that does not divide by
         * four (250 ms rolled into a fifth, lit quarter at 248). */
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
    /* One tick per millisecond across the whole 500 ms dwell: three
     * quarter boundaries at 125/250/375, and not one repaint anywhere
     * else. */
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
/* The preview belongs to the pointer                                */
/* ---------------------------------------------------------------- */

/*
 * DOGFOOD BUG: the dwell's preview stuck to the first group it opened.
 *
 * `preview_gid` was assigned in exactly one place — yew_mouse_tick's
 * open — and nothing ever put it back to 0 short of the gesture ending.
 * So the pointer could walk off a group, across the whole strip, and
 * row 2 kept showing the group it had left; and because
 * yew_mouse_dwell_flash and the open both stand down while
 * `preview_gid == dwell_gid`, coming BACK to that group announced
 * nothing and re-opened nothing.  Row 2 is a picture of where the
 * pointer is, so leaving the group has to take it away.
 */
void test_drag_dwell_preview_retracts_when_the_pointer_leaves(void)
{
    DragFixture f;
    u32 g;
    int gslot;
    int plain;

    dg_fixture(&f, 5U);
    g = dg_make_group(&f, 4, 5);
    yew_tab_switch(&f.ed, 0);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    gslot = dg_slot_of_payload(-(i32)g);
    plain = dg_slot_of_payload(2);
    YEW_ASSERT(gslot >= 0);
    YEW_ASSERT(plain >= 0);

    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, gslot), 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    yew_mouse_tick(&f.ed, f.ed.now_ms + YEW_DRAG_DWELL_MS);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), g);
    dg_paint(&f);
    YEW_ASSERT_EQ_U64(yew_tab_strip_rows(&f.ed), 2U);

    /* Off the group, onto an ordinary row-1 slot: row 2 goes with it. */
    f.ed.layout_dirty = false;
    f.ed.full_damage = false;
    {
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, plain), 0U);

        yew_mouse_event(&f.ed, &motion);
    }
    YEW_ASSERT_EQ_U64(f.ed.mouse.dwell_gid, 0U);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), 0U);
    /* A strip-row count change is the layout's, not a repaint's. */
    YEW_ASSERT(f.ed.layout_dirty);
    YEW_ASSERT(f.ed.full_damage);
    dg_paint(&f);
    YEW_ASSERT_EQ_U64(yew_tab_strip_rows(&f.ed), 1U);
    yew_ed_free(&f.ed);
}

/*
 * And coming back announces itself again.
 *
 * This is the half of the bug the user actually described: with the
 * preview stuck, a second hover over the SAME group was the one case
 * where `preview_gid == dwell_gid`, so the cue never lit and the strip
 * never re-opened — it was already open, showing a group the pointer
 * had left minutes ago.
 */
void test_drag_dwell_reopens_a_group_the_pointer_returns_to(void)
{
    DragFixture f;
    u32 g;
    int gslot;
    int plain;
    i64 t;

    dg_fixture(&f, 5U);
    g = dg_make_group(&f, 4, 5);
    yew_tab_switch(&f.ed, 0);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    gslot = dg_slot_of_payload(-(i32)g);
    plain = dg_slot_of_payload(2);
    YEW_ASSERT(gslot >= 0);
    YEW_ASSERT(plain >= 0);

    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, gslot), 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    t = f.ed.now_ms;
    yew_mouse_tick(&f.ed, t + YEW_DRAG_DWELL_MS);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), g);
    dg_paint(&f);
    {
        Key away = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, plain), 0U);

        f.ed.now_ms = t + YEW_DRAG_DWELL_MS;
        yew_mouse_event(&f.ed, &away);
    }
    dg_paint(&f);
    /* Back onto the same group: the cue runs from the top and the open
     * happens again on its own clock. */
    {
        Key back = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, gslot), 0U);

        f.ed.now_ms = t + 2 * YEW_DRAG_DWELL_MS;
        yew_mouse_event(&f.ed, &back);
    }
    YEW_ASSERT_EQ_U64(f.ed.mouse.dwell_gid, g);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), 0U);
    YEW_ASSERT_EQ_U64(yew_mouse_dwell_flash(&f.ed), g);
    YEW_ASSERT_EQ_I64(yew_mouse_deadline(&f.ed, f.ed.now_ms),
                      YEW_DRAG_FLASH_MS);
    yew_mouse_tick(&f.ed, f.ed.now_ms + YEW_DRAG_DWELL_MS);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), g);
    yew_ed_free(&f.ed);
}

/*
 * Row 2 is the preview's OWN surface, so the pointer arriving on it
 * must not close the thing it was sent to use.  This is the clause that
 * keeps the join gesture — dwell on row 1, drop on row 2 — alive.
 */
void test_drag_dwell_preview_survives_the_pointer_on_row_2(void)
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
    yew_mouse_tick(&f.ed, f.ed.now_ms + YEW_DRAG_DWELL_MS);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), g);
    dg_paint(&f);
    {
        Key down = dg_ev((u8)YEW_KEY_REPEAT, 2U, 1U);

        yew_mouse_event(&f.ed, &down);
    }
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), g);
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* The neighbours get out of the way                                 */
/* ---------------------------------------------------------------- */

/*
 * The reported workspace: `1 untitled`, group `ch4/`, group `ch5/`, and
 * the tab being dragged — or the same strip with four plain tabs, which
 * is the same geometry with nothing to dwell on.
 */
static void dg_grouped_fixture(DragFixture *f, bool grouped)
{
    if (!grouped) {
        dg_fixture(f, 3U);
        return;
    }
    dg_fixture(f, 5U);
    (void)dg_make_group(f, 1, 2);
    (void)dg_make_group(f, 3, 4);
    yew_tab_switch(&f->ed, 0);
    yew_ed_layout(&f->ed);
}

/* The tab-array index of the entry that gets dragged in that fixture. */
static int dg_held_index(bool grouped)
{
    return grouped ? 5 : 3;
}

/* Stable identity for a row-1 entry: a group's id, or a tab's id lifted
 * clear of it.  Tab INDICES move when a drop commits, so a payload
 * cannot be compared across one. */
static u32 dg_entry_key(Ed *ed, i32 payload)
{
    if (payload < 0)
        return (u32)(-payload);
    return 0x10000U + yew_tab_at(ed, (int)payload)->tab_id;
}

/* Row 1's entries, left to right, as they are DRAWN.  The held entry is
 * a gap and registers nothing, so a drag's answer omits it. */
static int dg_row1_keys(DragFixture *f, u32 *out, int cap)
{
    int n = 0;
    u16 x;

    for (x = 0U; x < f->ed.grid.cols; x++) {
        Region hit = yew_region_hit(x, 0U);
        u32 key;

        if (hit.kind != (u16)YEW_REGION_TAB)
            continue;
        key = dg_entry_key(&f->ed, hit.payload);
        if (n > 0 && out[n - 1] == key)
            continue;
        YEW_ASSERT(n < cap);
        out[n++] = key;
    }
    return n;
}

/* The grip the press established: how far behind the pointer the float
 * — and so the carried entry — is drawn. */
static u16 dg_grab_dx(DragFixture *f)
{
    i32 payload = 0;
    u16 fx = 0U;
    u16 fy = 0U;
    u16 grab = 0U;

    YEW_ASSERT(yew_mouse_drag_float(&f->ed, &payload, &fx, &fy, &grab));
    return grab;
}

/* The leftmost cell the entry with this payload is drawn at, or -1. */
static int dg_entry_x(i32 payload)
{
    u16 x;

    for (x = 0U; x < 80U; x++) {
        Region hit = yew_region_hit(x, 0U);

        if (hit.kind == (u16)YEW_REGION_TAB && hit.payload == payload)
            return (int)x;
    }
    return -1;
}

/* The half-open cell range row-1 slot `slot` currently occupies. */
static void dg_slot_span(int slot, u16 *c0, u16 *c1)
{
    u16 x;
    bool seen = false;

    *c0 = 0U;
    *c1 = 0U;
    for (x = 0U; x < 80U; x++) {
        if (yew_strip_slot_at(x, 0U) != slot)
            continue;
        if (!seen) {
            *c0 = x;
            seen = true;
        }
        *c1 = (u16)(x + 1U);
    }
    YEW_ASSERT(seen);
}

/*
 * DOGFOOD BUG: "non active tabs aren't dynamic during the drag".
 *
 * The float is drawn at `press_x − press_rgn.rect.x` behind the
 * pointer, so the entry the user is carrying covers whatever is under
 * THOSE cells — but the target was resolved from the bare pointer cell.
 * Grab a wide tab near its right edge and the carried entry sits
 * squarely on top of its neighbour while the strip insists nothing has
 * happened; the neighbour only moves once the POINTER has crossed, a
 * whole tab-width later, and then everything jumps at once.
 *
 * The strip is the user's exact one: `untitled`, two groups, and the
 * tab being dragged left across them.
 */
void test_drag_neighbours_slide_out_of_the_carried_tabs_way(void)
{
    DragFixture f;
    u32 ga;
    u32 gb;
    u32 held;
    u16 c0;
    u16 c1;
    u16 press_x;
    u16 drop_x = 0U;
    int xa0;
    int xb0;

    dg_fixture(&f, 5U);
    ga = dg_make_group(&f, 1, 2);
    gb = dg_make_group(&f, 3, 4);
    yew_tab_switch(&f.ed, 0);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    /* untitled, group a, group b, the tab being dragged. */
    YEW_ASSERT_EQ_I64(yew_strip_slot_count(), 4);
    held = yew_tab_at(&f.ed, 5)->tab_id;
    xa0 = dg_entry_x(-(i32)ga);
    xb0 = dg_entry_x(-(i32)gb);
    YEW_ASSERT(xa0 > 0 && xb0 > xa0);

    /* Grabbed near its RIGHT edge, which is what makes the float and the
     * pointer disagree. */
    dg_slot_span(3, &c0, &c1);
    YEW_ASSERT_EQ_I64((int)(c1 - c0), 18);
    press_x = (u16)(c1 - 2U);
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, press_x, 0U);

        yew_mouse_event(&f.ed, &press);
    }
    /*
     * Two cells left.  THE POINTER IS STILL INSIDE THE TAB'S OWN SLOT —
     * and the entry it is carrying is already sitting over the second
     * group past that group's midpoint, because the float trails the
     * pointer by the grip the press took.  Before the fix this moved
     * nothing at all.
     */
    YEW_ASSERT_EQ_I64(yew_strip_slot_at((u16)(press_x - 14U), 0U), 3);
    {
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, (u16)(press_x - 14U), 0U);

        yew_mouse_event(&f.ed, &motion);
    }
    dg_paint(&f);
    YEW_ASSERT(f.ed.mouse.drag_to_valid);
    YEW_ASSERT_EQ_I64(f.ed.mouse.drag_to_slot, 2);
    /* The second group slid RIGHT to make room; the first has not
     * moved, because the carried entry has not reached it. */
    YEW_ASSERT(dg_entry_x(-(i32)gb) > xb0);
    YEW_ASSERT_EQ_I64(dg_entry_x(-(i32)ga), xa0);

    /* Keep sliding left until the carried entry's own left edge is on
     * the first group: both groups now sit to the right of where they
     * started, in their original order. */
    {
        u16 a0;
        u16 a1;
        Key motion;

        dg_slot_span(1, &a0, &a1);
        drop_x = (u16)(a0 + dg_grab_dx(&f));
        motion = dg_ev((u8)YEW_KEY_REPEAT, drop_x, 0U);
        yew_mouse_event(&f.ed, &motion);
        dg_paint(&f);
        YEW_ASSERT_EQ_I64(f.ed.mouse.drag_to_slot, 1);
        YEW_ASSERT(dg_entry_x(-(i32)ga) > xa0);
        YEW_ASSERT(dg_entry_x(-(i32)gb) > dg_entry_x(-(i32)ga));
    }
    /* And releasing there lands exactly the strip that was drawn. */
    {
        Key up = dg_ev((u8)YEW_KEY_RELEASE, drop_x, 0U);

        yew_mouse_event(&f.ed, &up);
    }
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&f.ed, held), 1);
    dg_paint(&f);
    {
        u32 keys[8];
        int n = dg_row1_keys(&f, keys, (int)YEW_ARRAY_LEN(keys));

        YEW_ASSERT_EQ_I64(n, 4);
        YEW_ASSERT_EQ_U64(keys[1], 0x10000U + held);
        YEW_ASSERT_EQ_U64(keys[2], ga);
        YEW_ASSERT_EQ_U64(keys[3], gb);
    }
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* A group entry's hover band                                        */
/* ---------------------------------------------------------------- */

/*
 * DOGFOOD BUG: a group entry moved out from under the pointer that was
 * trying to REST on it.
 *
 * "Tab groups should have some space for allowing hover over without
 * shifting, in addition to the trigger points to shift in place
 * left/right that a standard tab has.  Currently the tab group wants to
 * move left/right of where I am hovering because it is assuming I want
 * to shift left right, not hover."
 *
 * The half-width rule and the dwell want opposite things from the same
 * cells: the dwell asks the user to hold still over a group for half a
 * second, and the swap slides that group away the moment the carried
 * entry covers half of it.  A group therefore gets a CENTRAL DEAD BAND
 * — its middle half — where nothing shifts, and keeps the ordinary
 * rule outside it.  Plain tabs are untouched, which the row below
 * pins.
 */
static void dg_carry_to(DragFixture *f, u16 col)
{
    Key motion = dg_ev((u8)YEW_KEY_REPEAT, col, 0U);

    yew_mouse_event(&f->ed, &motion);
    dg_paint(f);
}

/* The same, onto row 2. */
static void dg_carry_to_row2(DragFixture *f, u16 col)
{
    Key motion = dg_ev((u8)YEW_KEY_REPEAT, col, 1U);

    yew_mouse_event(&f->ed, &motion);
    dg_paint(f);
}

/*
 * The pointer column that puts the carried entry's crossing edge
 * exactly on `want`.  Pressed at slot 0's left edge, so the grip is 0
 * and the carried entry's leading edge IS the pointer; the trailing
 * edge trails it by the carried entry's width.
 */
static u16 dg_col_for_trail(DragFixture *f, i32 want)
{
    u16 c0 = 0U;
    u16 c1 = 0U;

    (void)f;
    dg_slot_span(0, &c0, &c1);
    YEW_ASSERT(want - (i32)(c1 - c0) >= 0);
    return (u16)(want - (i32)(c1 - c0));
}

void test_drag_a_group_entry_holds_still_inside_its_hover_band(void)
{
    DragFixture f;
    u32 g;
    u16 c0 = 0U;
    u16 c1 = 0U;
    u16 a0 = 0U;
    u16 a1 = 0U;
    i32 w;

    dg_fixture(&f, 5U);
    g = dg_make_group(&f, 1, 2);
    yew_tab_switch(&f.ed, 0);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    /* untitled, the group, and the three loose tabs after it. */
    YEW_ASSERT_EQ_I64(dg_slot_of_payload(-(i32)g), 1);
    dg_slot_span(0, &c0, &c1);
    dg_slot_span(1, &a0, &a1);
    w = (i32)a1 - (i32)a0;
    /* A band needs room to exist; below four cells there is none and the
     * half-width rule stands. */
    YEW_ASSERT(w >= 8);

    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, c0, 0U);

        yew_mouse_event(&f.ed, &press);
    }
    /*
     * PAST THE GROUP'S MIDPOINT and inside its middle half.  The old
     * rule swapped here, which is the report: the group slid left out
     * from under a pointer that had come to rest on it.
     */
    dg_carry_to(&f, dg_col_for_trail(&f, (i32)a0 + w / 2 + 1));
    YEW_ASSERT(f.ed.mouse.drag_to_valid);
    YEW_ASSERT_EQ_I64(f.ed.mouse.drag_to_slot, 0);

    /* The far edge of the band: the outer quarter is an ordinary swap
     * trigger, so pushing past it still moves the group. */
    dg_carry_to(&f, dg_col_for_trail(&f, (i32)a1 - w / 4 + 1));
    YEW_ASSERT_EQ_I64(f.ed.mouse.drag_to_slot, 1);
    yew_ed_free(&f.ed);
}

/*
 * THE BAND AND THE DWELL ARE THE SAME GESTURE.
 *
 * The band exists so a carried tab can COME TO REST on a group; the
 * dwell is what resting on a group is for.  If the dwell kept reading
 * the reorder target it would read the carried tab's own slot while the
 * band held the group still, and the one place a tab can join a group
 * would be the one place the pointer is not allowed to linger.
 *
 * So the dwell reads what the carried entry OVERLAPS MOST, and the
 * reorder target stays what it was.  Both halves are asserted here,
 * because either alone is the bug.
 */
void test_drag_resting_in_a_group_band_still_opens_it(void)
{
    DragFixture f;
    u32 g;
    u16 c0 = 0U;
    u16 c1 = 0U;
    u16 a0 = 0U;
    u16 a1 = 0U;
    i32 w;

    dg_fixture(&f, 5U);
    g = dg_make_group(&f, 1, 2);
    yew_tab_switch(&f.ed, 0);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    YEW_ASSERT_EQ_I64(dg_slot_of_payload(-(i32)g), 1);
    dg_slot_span(0, &c0, &c1);
    dg_slot_span(1, &a0, &a1);
    w = (i32)a1 - (i32)a0;
    YEW_ASSERT(w >= 8);

    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, c0, 0U);

        yew_mouse_event(&f.ed, &press);
    }
    /* The far edge of the band: as far onto the group as the pointer can
     * go without the strip moving, which is where a user who means to
     * rest on it ends up. */
    dg_carry_to(&f, dg_col_for_trail(&f, (i32)a1 - w / 4));
    YEW_ASSERT_EQ_I64(f.ed.mouse.drag_to_slot, 0); /* nothing shifted */
    YEW_ASSERT_EQ_U64(f.ed.mouse.dwell_gid, g);    /* and it is counting */

    yew_mouse_tick(&f.ed, f.ed.now_ms + YEW_DRAG_DWELL_MS);
    YEW_ASSERT_EQ_U64(yew_mouse_preview_group(&f.ed), g);
    yew_ed_free(&f.ed);
}

/*
 * The same band, approached from the RIGHT.  The crossing edge is the
 * carried entry's leading one, and the band is the same middle half of
 * the group it is moving over.
 */
void test_drag_a_group_entry_holds_still_coming_from_the_right(void)
{
    DragFixture f;
    u32 g;
    u16 c0 = 0U;
    u16 c1 = 0U;
    u16 a0 = 0U;
    u16 a1 = 0U;
    i32 w;

    dg_fixture(&f, 5U);
    g = dg_make_group(&f, 2, 3);
    yew_tab_switch(&f.ed, 0);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    YEW_ASSERT_EQ_I64(dg_slot_of_payload(-(i32)g), 2);
    dg_slot_span(3, &c0, &c1);
    dg_slot_span(2, &a0, &a1);
    w = (i32)a1 - (i32)a0;
    YEW_ASSERT(w >= 8);

    {
        /* Pressed at its LEFT edge, so the grip is 0 and the pointer is
         * the leading edge. */
        Key press = dg_ev((u8)YEW_KEY_PRESS, c0, 0U);

        yew_mouse_event(&f.ed, &press);
    }
    /* Inside the middle half, coming left: nothing shifts. */
    dg_carry_to(&f, (u16)((i32)a0 + w / 2 - 1));
    YEW_ASSERT(f.ed.mouse.drag_to_valid);
    YEW_ASSERT_EQ_I64(f.ed.mouse.drag_to_slot, 3);
    /* Past the band's left edge, into the outer quarter: it swaps. */
    dg_carry_to(&f, (u16)((i32)a0 + w / 4 - 1));
    YEW_ASSERT_EQ_I64(f.ed.mouse.drag_to_slot, 2);
    yew_ed_free(&f.ed);
}

/*
 * THE CONTRAST, and the half of the deliverable that is a promise not
 * to change anything else: a PLAIN neighbour still swaps at half its
 * width, one cell past the midpoint, with no band anywhere.
 */
void test_drag_a_plain_neighbour_still_swaps_at_half_its_width(void)
{
    DragFixture f;
    u16 c0 = 0U;
    u16 c1 = 0U;
    u16 a0 = 0U;
    u16 a1 = 0U;
    i32 w;

    dg_fixture(&f, 5U);
    yew_tab_switch(&f.ed, 0);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    dg_slot_span(0, &c0, &c1);
    dg_slot_span(1, &a0, &a1);
    w = (i32)a1 - (i32)a0;
    YEW_ASSERT(w >= 8);

    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, c0, 0U);

        yew_mouse_event(&f.ed, &press);
    }
    /* One cell short of the midpoint: still where it was. */
    dg_carry_to(&f, dg_col_for_trail(&f, (i32)a0 + w / 2));
    YEW_ASSERT(f.ed.mouse.drag_to_valid);
    YEW_ASSERT_EQ_I64(f.ed.mouse.drag_to_slot, 0);
    /* One cell past it: swapped — no band, exactly as before. */
    dg_carry_to(&f, dg_col_for_trail(&f, (i32)a0 + w / 2 + 1));
    YEW_ASSERT_EQ_I64(f.ed.mouse.drag_to_slot, 1);
    yew_ed_free(&f.ed);
}

/* ---------------------------------------------------------------- */
/* Row 2 previews the drop too                                       */
/* ---------------------------------------------------------------- */

/*
 * DOGFOOD BUG: "when hovering drag in a tab group, the row 2 tabs don't
 * shift like we just spent time getting the row 1 tabs shifting
 * left/right in response to the dragged tab."
 *
 * Row 2 already ACCEPTED a drop — `drop_target_row2` computed a group
 * and an ordinal — while showing nothing at all about where the tab
 * would land.  The picture and the outcome disagreed, which this
 * codebase treats as a bug in the picture.
 */

/* The leftmost cell row 2 draws the entry with this payload at, or -1. */
static int dg_row2_entry_x(i32 payload)
{
    u16 x;

    for (x = 0U; x < 80U; x++) {
        Region hit = yew_region_hit(x, 1U);

        if (hit.kind == (u16)YEW_REGION_TAB && hit.payload == payload)
            return (int)x;
    }
    return -1;
}

/*
 * THE GAP, read off the picture: the row-2 position whose cells carry
 * no region.  The held entry keeps its cells and loses its ink and its
 * region (57.14 §2), so the one slot the registry does not answer for
 * is where the drop is aimed.
 */
static int dg_row2_gap_slot(void)
{
    int i;

    for (i = 0; i < yew_strip_member_slot_count(); i++) {
        u16 a0 = 0U;
        u16 a1 = 0U;

        if (!yew_strip_member_slot_cells(i, &a0, &a1))
            continue;
        if (yew_region_hit(a0, 1U).kind == (u16)YEW_REGION_NONE)
            return i;
    }
    return -1;
}

/* Scratch, three loose tabs, a two-member group, one more loose tab —
 * and the ACTIVE tab inside the group, so row 2 is pinned. */
static u32 dg_row2_fixture(DragFixture *f)
{
    u32 g;

    dg_fixture(f, 5U);
    g = dg_make_group(f, 3, 4);
    yew_tab_switch(&f->ed, 3);
    yew_ed_layout(&f->ed);
    dg_paint(f);
    YEW_ASSERT_EQ_I64(f->ed.tab_strip_rect.h, 2);
    YEW_ASSERT_EQ_I64(yew_strip_member_slot_count(), 2);
    return g;
}

void test_drag_row2_opens_a_gap_where_the_member_will_land(void)
{
    DragFixture f;
    u16 a0 = 0U;
    u16 a1 = 0U;
    int at_rest_3;
    int at_rest_4;

    (void)dg_row2_fixture(&f);
    at_rest_3 = dg_row2_entry_x(3);
    at_rest_4 = dg_row2_entry_x(4);
    YEW_ASSERT(at_rest_3 >= 0 && at_rest_4 > at_rest_3);
    YEW_ASSERT(yew_strip_member_slot_cells(0, &a0, &a1));

    {
        /* A loose row-1 tab, grabbed at its left edge. */
        u16 c0 = 0U;
        u16 c1 = 0U;
        Key press;

        dg_slot_span(0, &c0, &c1);
        press = dg_ev((u8)YEW_KEY_PRESS, c0, 0U);
        yew_mouse_event(&f.ed, &press);
    }
    /* Onto row 2, aimed at the FIRST member: both members slide right
     * to open a space in front of them. */
    dg_carry_to_row2(&f, a0);
    YEW_ASSERT_EQ_I64(dg_row2_gap_slot(), 0);
    YEW_ASSERT(dg_row2_entry_x(3) > at_rest_3);
    YEW_ASSERT(dg_row2_entry_x(4) > at_rest_4);

    /* Aimed past them all: they close back up and the gap is last. */
    dg_carry_to_row2(&f, (u16)(yew_strip_member_tail_x() + 1U));
    YEW_ASSERT_EQ_I64(dg_row2_gap_slot(), 2);
    YEW_ASSERT_EQ_I64(dg_row2_entry_x(3), at_rest_3);
    YEW_ASSERT_EQ_I64(dg_row2_entry_x(4), at_rest_4);
    yew_ed_free(&f.ed);
}

/*
 * WHICH ROW OWNS THE PREVIEW.  Row 1 while the pointer is on row 1, row
 * 2 while it is on row 2, and the handover in both directions is the
 * whole of this row: a tab carried back up to row 1 must stop previewing
 * a member position, or row 2 keeps a space open for a drop that is no
 * longer aimed at it.
 */
void test_drag_moving_between_rows_hands_the_preview_over(void)
{
    DragFixture f;
    u16 a0 = 0U;
    u16 a1 = 0U;
    int at_rest_3;

    (void)dg_row2_fixture(&f);
    at_rest_3 = dg_row2_entry_x(3);
    YEW_ASSERT(yew_strip_member_slot_cells(0, &a0, &a1));
    {
        u16 c0 = 0U;
        u16 c1 = 0U;
        Key press;

        dg_slot_span(0, &c0, &c1);
        press = dg_ev((u8)YEW_KEY_PRESS, c0, 0U);
        yew_mouse_event(&f.ed, &press);
    }
    /* Row 1 owns it: row 2 is untouched, and there is no gap on it. */
    dg_carry_to(&f, dg_slot_x(&f, 2));
    YEW_ASSERT_EQ_I64(dg_row2_gap_slot(), -1);
    YEW_ASSERT_EQ_I64(dg_row2_entry_x(3), at_rest_3);

    /* Row 2 takes it. */
    dg_carry_to_row2(&f, a0);
    YEW_ASSERT_EQ_I64(dg_row2_gap_slot(), 0);
    YEW_ASSERT(dg_row2_entry_x(3) > at_rest_3);

    /* And gives it back on the way up. */
    dg_carry_to(&f, dg_slot_x(&f, 2));
    YEW_ASSERT_EQ_I64(dg_row2_gap_slot(), -1);
    YEW_ASSERT_EQ_I64(dg_row2_entry_x(3), at_rest_3);
    yew_ed_free(&f.ed);
}

/*
 * ROW 2'S HALF OF THE AGREEMENT, swept cell by cell.
 *
 * Each column on its own fixture, so the release is against exactly the
 * frame that was drawn: the gap the user is looking at and the ordinal
 * `drop_target_row2` commits are the same number, everywhere on the row.
 */
void test_drag_row2_previewed_gap_is_the_ordinal_the_drop_commits(void)
{
    u16 x;
    u16 tail_x;
    u16 press_x;
    u32 seen = 0U;

    {
        DragFixture probe;
        u16 c0 = 0U;
        u16 c1 = 0U;

        (void)dg_row2_fixture(&probe);
        dg_slot_span(0, &c0, &c1);
        press_x = c0;
        tail_x = (u16)(yew_strip_member_tail_x() + 3U);
        yew_ed_free(&probe.ed);
    }
    for (x = 0U; x < tail_x; x++) {
        DragFixture f;
        u32 g;
        u32 held;
        int gap;

        g = dg_row2_fixture(&f);
        held = yew_tab_at(&f.ed, 0)->tab_id;
        {
            Key press = dg_ev((u8)YEW_KEY_PRESS, press_x, 0U);

            yew_mouse_event(&f.ed, &press);
        }
        dg_carry_to_row2(&f, x);
        gap = dg_row2_gap_slot();
        YEW_ASSERT(gap >= 0);
        seen = (u32)(seen | (1U << (unsigned)gap));
        {
            Key up = dg_ev((u8)YEW_KEY_RELEASE, x, 1U);

            yew_mouse_event(&f.ed, &up);
        }
        {
            int idx = yew_tab_index_of_id(&f.ed, held);

            YEW_ASSERT(idx >= 0);
            YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, idx)->group_id, g);
            /* THE AGREEMENT: the gap the frame showed, 1-based. */
            YEW_ASSERT_EQ_U64(yew_tab_at(&f.ed, idx)->group_ordinal,
                              (u32)gap + 1U);
        }
        yew_ed_free(&f.ed);
    }
    /* Every one of the three positions was actually reached, or the
     * sweep proved agreement about a single answer. */
    YEW_ASSERT_EQ_U64(seen, 0x7U);
}

/*
 * THE PRE-DRAG SLOT TABLE'S WHOLE JOB: the strip the user is looking at
 * and the list the release commits are the same answer.
 *
 * Swept cell by cell across the whole strip, each pointer position
 * driven on its own fixture so the release is against exactly the frame
 * that was drawn.  A targeting rule that reads the carried entry's
 * cells rather than the pointer's has to keep this, or the gap becomes
 * a lie.
 */
static void dg_sweep_preview_matches_drop(bool grouped)
{
    u16 x;
    u16 tail_x;
    u16 press_x;
    int prev_to = -1;

    {
        DragFixture probe;
        u16 c0;
        u16 c1;

        dg_grouped_fixture(&probe, grouped);
        dg_paint(&probe);
        YEW_ASSERT_EQ_I64(yew_strip_slot_count(), 4);
        dg_slot_span(3, &c0, &c1);
        press_x = (u16)(c1 - 2U);
        tail_x = yew_strip_tail_x();
        yew_ed_free(&probe.ed);
    }
    /* Right to left, which is the direction the report was about. */
    for (x = tail_x; x-- > 0U;) {
        DragFixture f;
        u32 during[8];
        u32 after[8];
        u32 held_key;
        int n_during;
        int n_after;
        int to;
        int i;
        int j;

        /* The press cell itself: the pointer never left it, so this is
         * a click and there is no drag to agree with. */
        if (x == press_x)
            continue;
        dg_grouped_fixture(&f, grouped);
        dg_paint(&f);
        held_key = 0x10000U + yew_tab_at(&f.ed, dg_held_index(grouped))->tab_id;
        {
            Key press = dg_ev((u8)YEW_KEY_PRESS, press_x, 0U);
            Key motion = dg_ev((u8)YEW_KEY_REPEAT, x, 0U);

            yew_mouse_event(&f.ed, &press);
            yew_mouse_event(&f.ed, &motion);
        }
        dg_paint(&f);
        YEW_ASSERT(f.ed.mouse.drag_to_valid);
        YEW_ASSERT(!f.ed.mouse.drag_to_tail);
        to = f.ed.mouse.drag_to_slot;
        /*
         * MONOTONE.  Carrying the entry further left can only move its
         * landing place left; a target that jumped back would be the
         * strip fighting the pointer.
         */
        if (prev_to >= 0)
            YEW_ASSERT(to <= prev_to);
        prev_to = to;
        n_during = dg_row1_keys(&f, during, (int)YEW_ARRAY_LEN(during));
        {
            Key up = dg_ev((u8)YEW_KEY_RELEASE, x, 0U);

            yew_mouse_event(&f.ed, &up);
        }
        dg_paint(&f);
        n_after = dg_row1_keys(&f, after, (int)YEW_ARRAY_LEN(after));
        /* The gap was one entry wide, and it was the held one. */
        YEW_ASSERT_EQ_I64(n_after, n_during + 1);
        YEW_ASSERT_EQ_U64(after[to], held_key);
        /* Everything else kept the order the drag had drawn. */
        for (i = 0, j = 0; i < n_after; i++) {
            if (i == to)
                continue;
            YEW_ASSERT_EQ_U64(after[i], during[j]);
            j++;
        }
        yew_ed_free(&f.ed);
    }
    /* The sweep reached the far left: the entry can be carried to the
     * head of the strip, which is where the report ended. */
    YEW_ASSERT_EQ_I64(prev_to, 0);
}

void test_drag_every_previewed_gap_is_where_the_drop_lands(void)
{
    dg_sweep_preview_matches_drop(false);
}

/*
 * THE REPORTED STRIP, swept: `1 untitled`, two directory groups, and the
 * tab being carried left across both of them.  Every pointer position
 * has an insertion point, the groups only ever slide right and keep
 * their order, and the release lands exactly the row that was drawn.
 */
void test_drag_the_dogfood_strip_reorders_at_every_step(void)
{
    dg_sweep_preview_matches_drop(true);
}

/*
 * THE CUE IS VISIBLE ON THE ACTIVE ENTRY TOO.
 *
 * §3 toggles YEW_ATTR_REVERSE rather than substituting a style exactly
 * so that a group which is already the active row-1 entry — drawn
 * reversed — still announces the dwell, by losing the reverse for a
 * quarter instead of gaining it.  A cue that painted "active" over the
 * active entry would announce nothing, which is the one way this could
 * be running and still be invisible.
 */
void test_drag_dwell_flash_is_visible_on_the_active_group_entry(void)
{
    DragFixture f;
    u32 g;
    int gslot;
    i64 t0;
    bool base;
    bool lit;
    bool dark;

    dg_fixture(&f, 5U);
    g = dg_make_group(&f, 4, 5);
    /* The ACTIVE tab is one of the group's members, so the group's own
     * row-1 entry is the active entry. */
    yew_tab_switch(&f.ed, 4);
    yew_ed_layout(&f.ed);
    dg_paint(&f);
    gslot = dg_slot_of_payload(-(i32)g);
    YEW_ASSERT(gslot >= 0);
    /* The premise: this entry is the row-1 ACTIVE one. */
    YEW_ASSERT_EQ_I64(dg_active_entry(&f), gslot);
    base = dg_entry_reversed(&f, g);

    /* An UNGROUPED tab carried onto it: a member of the group would
     * have nothing to join and would never dwell. */
    {
        Key press = dg_ev((u8)YEW_KEY_PRESS, dg_slot_x(&f, 0), 0U);
        Key motion = dg_ev((u8)YEW_KEY_REPEAT, dg_slot_x(&f, gslot), 0U);

        yew_mouse_event(&f.ed, &press);
        yew_mouse_event(&f.ed, &motion);
    }
    YEW_ASSERT_EQ_U64(f.ed.mouse.dwell_gid, g);
    t0 = f.ed.now_ms;
    f.ed.now_ms = t0;
    YEW_ASSERT_EQ_U64(yew_mouse_dwell_flash(&f.ed), g);
    dg_paint(&f);
    lit = dg_entry_reversed(&f, g);
    f.ed.now_ms = t0 + YEW_DRAG_FLASH_MS;
    YEW_ASSERT_EQ_U64(yew_mouse_dwell_flash(&f.ed), 0U);
    dg_paint(&f);
    dark = dg_entry_reversed(&f, g);
    /* The two quarters do not paint the same cells — which is the whole
     * claim the word "flash" makes — and the dark quarter is the entry's
     * ordinary active look, so the cue neither cancels out nor leaves a
     * stuck highlight behind. */
    YEW_ASSERT_EQ_U64(lit, !base);
    YEW_ASSERT_EQ_U64(dark, base);
    yew_ed_free(&f.ed);
}

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
