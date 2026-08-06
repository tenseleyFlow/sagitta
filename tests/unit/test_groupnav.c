/*
 * Sprint 24 §5/§6: the two-row tab bar and the continuous line.
 *
 * The fixture everything below shares is the one the contract names:
 * [t1, G{a,b,c}, t2] — one ungrouped tab, a three-member group, another
 * ungrouped tab.  Six rights from t1 must visit t1,a,b,c,t2,t1; six
 * lefts must visit exactly that sequence reversed.
 *
 * Reversibility is the property worth a gate of its own, because every
 * plausible-but-wrong walk still MOVES.  A walk that resumes at the
 * last-active member mid-stride, or that treats a group as one stop
 * rather than three, looks fine until you try to retrace your steps and
 * find files you never passed.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "ui/groupnav.h"
#include "ui/groups.h"
#include "ui/strip.h"
#include "ui/tabs.h"

/*
 * Builds [t1, G{a,b,c}, t2] and returns the group id.
 *
 * Tab 0 is the scratch document the editor always has, so the array is
 * [scratch, t1, a, b, c, t2] and the row-1 list is
 * [scratch, t1, G, t2] — four entries.
 */
static u32 nav_fixture(Ed *ed)
{
    u32 g;
    int i;

    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    sag_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
    for (i = 0; i < 5; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/sag-nav-%d.txt", i);
        SAG_ASSERT(sag_tab_open(ed, path) >= 0);
    }
    g = sag_group_create(ed, "/src", NULL);
    sag_group_add_member(ed, g, 2);
    sag_group_add_member(ed, g, 3);
    sag_group_add_member(ed, g, 4);
    sag_tab_switch(ed, 1);
    return g;
}

/* ---------------------------------------------------------------- */
/* Row 1: the entry list                                            */
/* ---------------------------------------------------------------- */

void test_groupnav_row1_shows_a_group_once_at_its_first_member(void)
{
    Ed ed;
    StripEntry entries[16];
    u32 g;
    int n;

    g = nav_fixture(&ed);
    n = sag_tab_row1_entries(&ed, entries, 16);
    /* scratch, t1, G, t2 — the three members collapse into one entry. */
    SAG_ASSERT_EQ_I64(n, 4);
    SAG_ASSERT_EQ_I64(entries[0].payload, 0);
    SAG_ASSERT_EQ_I64(entries[1].payload, 1);
    /* Placed where the FIRST member sits, and carrying -gid. */
    SAG_ASSERT_EQ_I64(entries[2].payload, -(i32)g);
    /* The tab after the group keeps its own index, not a renumbered
     * one — row-1 position and tab index are different things. */
    SAG_ASSERT_EQ_I64(entries[3].payload, 5);
    /* The label is bracketed and carries the LIVE count. */
    SAG_ASSERT_NOT_NULL(strstr(entries[2].label, "src/ (3)"));
    SAG_ASSERT_EQ_I64(entries[2].label[0], '[');
    sag_ed_free(&ed);
}

void test_groupnav_row1_active_tracks_the_group_not_the_member(void)
{
    Ed ed;
    StripEntry entries[16];
    int n;

    (void)nav_fixture(&ed);
    n = sag_tab_row1_entries(&ed, entries, 16);
    /* On t1, the active entry is t1 itself. */
    SAG_ASSERT_EQ_I64(sag_tab_row1_active(&ed, entries, n), 1);
    /* Inside the group, every member reports the GROUP's entry — the
     * members are not on row 1 to be pointed at. */
    sag_tab_switch(&ed, 3);
    SAG_ASSERT_EQ_I64(sag_tab_row1_active(&ed, entries, n), 2);
    sag_tab_switch(&ed, 4);
    SAG_ASSERT_EQ_I64(sag_tab_row1_active(&ed, entries, n), 2);
    sag_ed_free(&ed);
}

/* The bar is two rows inside a group and one outside — row 2 needs the
 * group's own entry above it or it reads as a different widget. */
void test_groupnav_strip_reserves_two_rows_inside_a_group(void)
{
    Ed ed;

    (void)nav_fixture(&ed);
    SAG_ASSERT_EQ_U64(sag_tab_strip_rows(&ed), 1U);
    sag_tab_switch(&ed, 3);
    SAG_ASSERT_EQ_U64(sag_tab_strip_rows(&ed), 2U);
    sag_tab_switch(&ed, 1);
    SAG_ASSERT_EQ_U64(sag_tab_strip_rows(&ed), 1U);
    sag_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* The continuous line                                              */
/* ---------------------------------------------------------------- */

/*
 * DoD 5.  Six rights from t1 visit t1,a,b,c,t2,scratch,t1 — every open
 * file, in array order, wrapping at the end.
 */
void test_groupnav_walk_visits_every_file_in_order(void)
{
    Ed ed;
    static const int want[] = {2, 3, 4, 5, 0, 1};
    int i;

    (void)nav_fixture(&ed);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 1);
    for (i = 0; i < 6; i++) {
        sag_file_step(&ed, 1);
        SAG_ASSERT_EQ_I64(ed.tabs.active, want[i]);
    }
    sag_ed_free(&ed);
}

/*
 * DoD 5, the real assertion: N rights then N lefts returns to the start
 * having visited the same sequence reversed.
 *
 * This is what enter_group_at_edge exists for.  A walk that resumed at
 * last_active_member on the way back would re-enter the group at
 * whichever member it happened to leave from, skipping the ones between
 * — and would still end up somewhere plausible.
 */
void test_groupnav_walk_is_reversible(void)
{
    Ed ed;
    int forward[6];
    int back[6];
    int i;

    (void)nav_fixture(&ed);
    for (i = 0; i < 6; i++) {
        sag_file_step(&ed, 1);
        forward[i] = ed.tabs.active;
    }
    /* Back to where we started after a full lap. */
    SAG_ASSERT_EQ_I64(ed.tabs.active, 1);
    for (i = 0; i < 6; i++) {
        sag_file_step(&ed, -1);
        back[i] = ed.tabs.active;
    }
    SAG_ASSERT_EQ_I64(ed.tabs.active, 1);
    /* back[i] retraces forward in reverse: the step that took us TO
     * forward[k] must be undone by the step that takes us back to
     * forward[k-1]. */
    for (i = 0; i < 5; i++)
        SAG_ASSERT_EQ_I64(back[i], forward[4 - i]);
    sag_ed_free(&ed);
}

/*
 * DoD 6, half one: arriving from the right lands on the LAST member.
 * The mid-walk path must not consult last_active_member — asserted by
 * counting, since a walk that consults it still works, it just skips.
 */
void test_groupnav_enter_at_edge_depends_on_direction(void)
{
    Ed ed;
    u64 reads;

    (void)nav_fixture(&ed);
    /* Walking LEFT from t2 (index 5) enters the group at its last
     * member (index 4). */
    sag_tab_switch(&ed, 5);
    reads = sag_group_resume_reads();
    sag_file_step(&ed, -1);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 4);
    /* Not one resume was consulted on the way in. */
    SAG_ASSERT_EQ_U64(sag_group_resume_reads(), reads);

    /* Walking RIGHT from t1 enters at the first member. */
    sag_tab_switch(&ed, 1);
    reads = sag_group_resume_reads();
    sag_file_step(&ed, 1);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 2);
    SAG_ASSERT_EQ_U64(sag_group_resume_reads(), reads);
    sag_ed_free(&ed);
}

/* DoD 6, half two: the EXPLICIT enter resumes where the user left. */
void test_groupnav_explicit_enter_resumes_the_last_member(void)
{
    Ed ed;
    u32 g;

    g = nav_fixture(&ed);
    /* Sit on the middle member, then leave — which records the spot. */
    sag_tab_switch(&ed, 3);
    SAG_ASSERT(sag_group_leave(&ed));
    SAG_ASSERT_EQ_U64(sag_active_group_id(&ed), 0U);

    sag_group_enter(&ed, g);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 3);
    sag_ed_free(&ed);
}

/* A dangling resume path falls back to the lowest ordinal rather than
 * refusing to enter — the stored path is explicitly not authoritative. */
void test_groupnav_enter_falls_back_when_the_path_dangles(void)
{
    Ed ed;
    u32 g;

    g = nav_fixture(&ed);
    sag_tab_switch(&ed, 3);
    SAG_ASSERT(sag_group_leave(&ed));
    /* Close the member the group is pointing at.  The group survives —
     * it still has two members — but its resume path now names a tab
     * that is gone. */
    SAG_ASSERT(sag_tab_close(&ed, 3));
    SAG_ASSERT_EQ_I64(sag_group_member_count(&ed, g), 2);

    sag_tab_switch(&ed, 0);
    sag_group_enter(&ed, g);
    /* The first member, not a refusal and not a stale index. */
    SAG_ASSERT_EQ_U64(sag_active_group_id(&ed), g);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, ed.tabs.active)->group_ordinal, 1U);
    sag_ed_free(&ed);
}

/* Leaving from a MIDDLE member lands on the tab after the group, not on
 * the member's neighbour. */
void test_groupnav_leave_from_the_middle_lands_after_the_group(void)
{
    Ed ed;

    (void)nav_fixture(&ed);
    sag_tab_switch(&ed, 3);
    SAG_ASSERT(sag_group_leave(&ed));
    SAG_ASSERT_EQ_I64(ed.tabs.active, 5);
    SAG_ASSERT_EQ_U64(sag_active_group_id(&ed), 0U);
    sag_ed_free(&ed);
}

/* Every tab in one group: leave has nowhere to go and says so rather
 * than moving somewhere arbitrary. */
void test_groupnav_leave_refuses_when_everything_is_grouped(void)
{
    Ed ed;
    u32 g;
    u32 i;

    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    SAG_ASSERT(sag_tab_open(&ed, "/tmp/sag-nav-all.txt") >= 0);
    g = sag_group_create(&ed, "/src", NULL);
    for (i = 0U; i < sag_tab_count(&ed); i++)
        sag_group_add_member(&ed, g, (int)i);
    sag_tab_switch(&ed, 0);
    SAG_ASSERT(!sag_group_leave(&ed));
    /* Still inside, still on the same tab. */
    SAG_ASSERT_EQ_U64(sag_active_group_id(&ed), g);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 0);
    sag_ed_free(&ed);
}

/*
 * A group with one member is still a group: stepping through it is one
 * stop, and stepping again falls through to the entry beside it.
 */
void test_groupnav_walk_steps_through_a_single_member_group(void)
{
    Ed ed;
    u32 g;

    (void)nav_fixture(&ed);
    /* Shrink the group to one member. */
    g = sag_active_group_id(&ed);
    (void)g;
    sag_group_remove_member(&ed, 3);
    sag_group_remove_member(&ed, 4);
    sag_tab_switch(&ed, 1);
    sag_file_step(&ed, 1);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 2); /* into the group */
    sag_file_step(&ed, 1);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 3); /* straight out the far side */
    sag_ed_free(&ed);
}

/* Every switch passes through hydrate, including the ones the walk
 * makes — otherwise stepping into a deferred member shows nothing. */
void test_groupnav_walking_into_a_member_hydrates_it(void)
{
    Ed ed;

    (void)nav_fixture(&ed);
    SAG_ASSERT(!sag_tab_is_resident(&ed, 2));
    sag_file_step(&ed, 1);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 2);
    SAG_ASSERT(sag_tab_is_resident(&ed, 2));
    sag_ed_free(&ed);
}
