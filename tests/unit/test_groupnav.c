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

    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    yew_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
    for (i = 0; i < 5; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-nav-%d.txt", i);
        YEW_ASSERT(yew_tab_open(ed, path) >= 0);
    }
    g = yew_group_create(ed, "/src", NULL);
    yew_group_add_member(ed, g, 2);
    yew_group_add_member(ed, g, 3);
    yew_group_add_member(ed, g, 4);
    yew_tab_switch(ed, 1);
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
    n = yew_tab_row1_entries(&ed, entries, 16);
    /* scratch, t1, G, t2 — the three members collapse into one entry. */
    YEW_ASSERT_EQ_I64(n, 4);
    YEW_ASSERT_EQ_I64(entries[0].payload, 0);
    YEW_ASSERT_EQ_I64(entries[1].payload, 1);
    /* Placed where the FIRST member sits, and carrying -gid. */
    YEW_ASSERT_EQ_I64(entries[2].payload, -(i32)g);
    /* The tab after the group keeps its own index, not a renumbered
     * one — row-1 position and tab index are different things. */
    YEW_ASSERT_EQ_I64(entries[3].payload, 5);
    /* The modern padded label carries the LIVE count. */
    YEW_ASSERT_NOT_NULL(strstr(entries[2].label, "src/ (3)"));
    YEW_ASSERT_EQ_I64(entries[2].label[0], ' ');
    YEW_ASSERT_EQ_I64(entries[2].label[strlen(entries[2].label) - 1U],
                      ' ');
    yew_ed_free(&ed);
}

void test_groupnav_row1_active_tracks_the_group_not_the_member(void)
{
    Ed ed;
    StripEntry entries[16];
    int n;

    (void)nav_fixture(&ed);
    n = yew_tab_row1_entries(&ed, entries, 16);
    /* On t1, the active entry is t1 itself. */
    YEW_ASSERT_EQ_I64(yew_tab_row1_active(&ed, entries, n), 1);
    /* Inside the group, every member reports the GROUP's entry — the
     * members are not on row 1 to be pointed at. */
    yew_tab_switch(&ed, 3);
    YEW_ASSERT_EQ_I64(yew_tab_row1_active(&ed, entries, n), 2);
    yew_tab_switch(&ed, 4);
    YEW_ASSERT_EQ_I64(yew_tab_row1_active(&ed, entries, n), 2);
    yew_ed_free(&ed);
}

/* The bar is two rows inside a group and one outside — row 2 needs the
 * group's own entry above it or it reads as a different widget. */
void test_groupnav_strip_reserves_two_rows_inside_a_group(void)
{
    Ed ed;

    (void)nav_fixture(&ed);
    YEW_ASSERT_EQ_U64(yew_tab_strip_rows(&ed), 1U);
    yew_tab_switch(&ed, 3);
    YEW_ASSERT_EQ_U64(yew_tab_strip_rows(&ed), 2U);
    yew_tab_switch(&ed, 1);
    YEW_ASSERT_EQ_U64(yew_tab_strip_rows(&ed), 1U);
    yew_ed_free(&ed);
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
    YEW_ASSERT_EQ_I64(ed.tabs.active, 1);
    for (i = 0; i < 6; i++) {
        yew_file_step(&ed, 1);
        YEW_ASSERT_EQ_I64(ed.tabs.active, want[i]);
    }
    yew_ed_free(&ed);
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
        yew_file_step(&ed, 1);
        forward[i] = ed.tabs.active;
    }
    /* Back to where we started after a full lap. */
    YEW_ASSERT_EQ_I64(ed.tabs.active, 1);
    for (i = 0; i < 6; i++) {
        yew_file_step(&ed, -1);
        back[i] = ed.tabs.active;
    }
    YEW_ASSERT_EQ_I64(ed.tabs.active, 1);
    /* back[i] retraces forward in reverse: the step that took us TO
     * forward[k] must be undone by the step that takes us back to
     * forward[k-1]. */
    for (i = 0; i < 5; i++)
        YEW_ASSERT_EQ_I64(back[i], forward[4 - i]);
    yew_ed_free(&ed);
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
    yew_tab_switch(&ed, 5);
    reads = yew_group_resume_reads();
    yew_file_step(&ed, -1);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 4);
    /* Not one resume was consulted on the way in. */
    YEW_ASSERT_EQ_U64(yew_group_resume_reads(), reads);

    /* Walking RIGHT from t1 enters at the first member. */
    yew_tab_switch(&ed, 1);
    reads = yew_group_resume_reads();
    yew_file_step(&ed, 1);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    YEW_ASSERT_EQ_U64(yew_group_resume_reads(), reads);
    yew_ed_free(&ed);
}

/* DoD 6, half two: the EXPLICIT enter resumes where the user left. */
void test_groupnav_explicit_enter_resumes_the_last_member(void)
{
    Ed ed;
    u32 g;

    g = nav_fixture(&ed);
    /* Sit on the middle member, then leave — which records the spot. */
    yew_tab_switch(&ed, 3);
    YEW_ASSERT(yew_group_leave(&ed));
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), 0U);

    yew_group_enter(&ed, g);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 3);
    yew_ed_free(&ed);
}

/* A dangling resume path falls back to the lowest ordinal rather than
 * refusing to enter — the stored path is explicitly not authoritative. */
void test_groupnav_enter_falls_back_when_the_path_dangles(void)
{
    Ed ed;
    u32 g;

    g = nav_fixture(&ed);
    yew_tab_switch(&ed, 3);
    YEW_ASSERT(yew_group_leave(&ed));
    /* Close the member the group is pointing at.  The group survives —
     * it still has two members — but its resume path now names a tab
     * that is gone. */
    YEW_ASSERT(yew_tab_close(&ed, 3));
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, g), 2);

    yew_tab_switch(&ed, 0);
    yew_group_enter(&ed, g);
    /* The first member, not a refusal and not a stale index. */
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), g);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, ed.tabs.active)->group_ordinal, 1U);
    yew_ed_free(&ed);
}

/* Leaving from a MIDDLE member lands on the tab after the group, not on
 * the member's neighbour. */
void test_groupnav_leave_from_the_middle_lands_after_the_group(void)
{
    Ed ed;

    (void)nav_fixture(&ed);
    yew_tab_switch(&ed, 3);
    YEW_ASSERT(yew_group_leave(&ed));
    YEW_ASSERT_EQ_I64(ed.tabs.active, 5);
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), 0U);
    yew_ed_free(&ed);
}

/* Every tab in one group: leave has nowhere to go and says so rather
 * than moving somewhere arbitrary. */
void test_groupnav_leave_refuses_when_everything_is_grouped(void)
{
    Ed ed;
    u32 g;
    u32 i;

    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    yew_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    YEW_ASSERT(yew_tab_open(&ed, "/tmp/yew-nav-all.txt") >= 0);
    g = yew_group_create(&ed, "/src", NULL);
    for (i = 0U; i < yew_tab_count(&ed); i++)
        yew_group_add_member(&ed, g, (int)i);
    yew_tab_switch(&ed, 0);
    YEW_ASSERT(!yew_group_leave(&ed));
    /* Still inside, still on the same tab. */
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), g);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 0);
    yew_ed_free(&ed);
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
    g = yew_active_group_id(&ed);
    (void)g;
    yew_group_remove_member(&ed, 3);
    yew_group_remove_member(&ed, 4);
    yew_tab_switch(&ed, 1);
    yew_file_step(&ed, 1);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2); /* into the group */
    yew_file_step(&ed, 1);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 3); /* straight out the far side */
    yew_ed_free(&ed);
}

/* Every switch passes through hydrate, including the ones the walk
 * makes — otherwise stepping into a deferred member shows nothing. */
void test_groupnav_walking_into_a_member_hydrates_it(void)
{
    Ed ed;

    (void)nav_fixture(&ed);
    YEW_ASSERT(!yew_tab_is_resident(&ed, 2));
    yew_file_step(&ed, 1);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    YEW_ASSERT(yew_tab_is_resident(&ed, 2));
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* Sprint 24 §7: the 500 ms digit-extension window                  */
/* ---------------------------------------------------------------- */

/*
 * The window's whole design is "jump immediately, then arm", so what
 * these tests check is that the FIRST jump already happened before any
 * second digit could arrive — and that a digit which cannot extend it
 * is swallowed rather than typed into the document.
 */

static Key nav_digit(char c, u16 mods)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.kind = YEW_EV_KEY;
    k.ev = YEW_KEY_PRESS;
    k.code = (u32)c;
    k.mods = mods;
    k.ntext = 1U;
    k.text[0] = (u8)c;
    return k;
}

static void nav_goto(Ed *ed, i64 n)
{
    CmdId id = yew_cmd_lookup("ed.tab.goto", 11U);
    CmdCtx cx;

    YEW_ASSERT(id.v != 0U);
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.iarg = n;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_ed_invoke(ed, id, &cx), YEW_CMD_OK);
}

/* Opens enough tabs that two-digit jumps have somewhere to land. */
static void nav_many_tabs(Ed *ed, int n)
{
    int i;

    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    yew_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
    for (i = 1; i < n; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-jmp-%d.txt", i);
        YEW_ASSERT(yew_tab_open(ed, path) >= 0);
    }
}

/*
 * DoD 7: `alt+1` then `5` reaches tab 15 — and the jump to tab 1
 * happened in the same frame, with no wait.
 */
void test_groupnav_digit_jump_extends_to_two_digits(void)
{
    Ed ed;

    nav_many_tabs(&ed, 20);
    ed.now_ms = 1000;
    nav_goto(&ed, 1);
    /* Already there.  Nothing waited half a second to find out whether
     * a second digit was coming. */
    YEW_ASSERT_EQ_I64(ed.tabs.active, 0);
    YEW_ASSERT(yew_tab_jump_armed());

    ed.now_ms = 1100;
    YEW_ASSERT(yew_tab_jump_key(&ed, nav_digit('5', 0U)));
    YEW_ASSERT_EQ_I64(ed.tabs.active, 14); /* tab 15, 0-based */
    /* Re-armed, so a third digit works. */
    YEW_ASSERT(yew_tab_jump_armed());
    yew_ed_free(&ed);
}

/* The modifier may still be held: `alt+1` `alt+5` is the natural way to
 * type the chord, and rejecting it sent the 5 to the main dispatch as
 * its own jump — tab 1 then tab 5, never tab 15. */
void test_groupnav_digit_jump_accepts_held_modifiers(void)
{
    Ed ed;

    nav_many_tabs(&ed, 20);
    ed.now_ms = 1000;
    nav_goto(&ed, 1);
    ed.now_ms = 1100;
    YEW_ASSERT(yew_tab_jump_key(&ed, nav_digit('5', YEW_MOD_ALT)));
    YEW_ASSERT_EQ_I64(ed.tabs.active, 14);

    nav_goto(&ed, 1);
    ed.now_ms = 1200;
    YEW_ASSERT(yew_tab_jump_key(&ed, nav_digit('5', YEW_MOD_CTRL)));
    YEW_ASSERT_EQ_I64(ed.tabs.active, 14);
    yew_ed_free(&ed);
}

/* An expired deadline clears the state BEFORE the key dispatches, so a
 * digit typed much later is its own keystroke and not a continuation. */
void test_groupnav_digit_jump_expires_on_the_clock(void)
{
    Ed ed;

    nav_many_tabs(&ed, 20);
    ed.now_ms = 1000;
    nav_goto(&ed, 1);
    YEW_ASSERT(yew_tab_jump_armed());

    /* One millisecond past the window. */
    ed.now_ms = 1000 + YEW_JUMP_WINDOW_MS;
    YEW_ASSERT(!yew_tab_jump_key(&ed, nav_digit('5', 0U)));
    YEW_ASSERT(!yew_tab_jump_armed());
    /* Not consumed, and the tab did not move. */
    YEW_ASSERT_EQ_I64(ed.tabs.active, 0);
    yew_ed_free(&ed);
}

/* A non-digit clears the window first, then dispatches normally. */
void test_groupnav_digit_jump_releases_a_non_digit(void)
{
    Ed ed;
    Key k;

    nav_many_tabs(&ed, 20);
    ed.now_ms = 1000;
    nav_goto(&ed, 1);
    k = nav_digit('j', 0U);
    YEW_ASSERT(!yew_tab_jump_key(&ed, k));
    YEW_ASSERT(!yew_tab_jump_armed());
    yew_ed_free(&ed);
}

/*
 * Out of range: the window clears, the FIRST jump stands, and the digit
 * is swallowed — it was part of a chord, and a surprise edit while
 * navigating is worse than a dropped key.
 */
void test_groupnav_digit_jump_swallows_an_out_of_range_digit(void)
{
    Ed ed;

    nav_many_tabs(&ed, 12);
    ed.now_ms = 1000;
    nav_goto(&ed, 1);
    ed.now_ms = 1100;
    /* Tab 19 does not exist. */
    YEW_ASSERT(yew_tab_jump_key(&ed, nav_digit('9', 0U)));
    /* CONSUMED — so nothing typed a 9 into the document. */
    YEW_ASSERT(!yew_tab_jump_armed());
    YEW_ASSERT_EQ_I64(ed.tabs.active, 0); /* the first jump stands */
    yew_ed_free(&ed);
}

/*
 * Landing inside a group changes what the next digit means: it picks
 * that group's Nth member by ordinal, which is what row 2 is showing
 * while the user counts.
 */
void test_groupnav_digit_jump_picks_a_group_member(void)
{
    Ed ed;
    u32 g;

    g = nav_fixture(&ed);
    (void)g;
    ed.now_ms = 1000;
    /* Tab 3 (1-based) is the group's first member. */
    nav_goto(&ed, 3);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    YEW_ASSERT(yew_tab_jump_armed());

    ed.now_ms = 1100;
    /* `2` means the SECOND member, not tab 32. */
    YEW_ASSERT(yew_tab_jump_key(&ed, nav_digit('2', 0U)));
    YEW_ASSERT_EQ_I64(ed.tabs.active, 3);
    yew_ed_free(&ed);
}

/* Out of range within a group leaves the first jump standing too. */
void test_groupnav_digit_jump_group_member_out_of_range(void)
{
    Ed ed;

    (void)nav_fixture(&ed);
    ed.now_ms = 1000;
    nav_goto(&ed, 3);
    ed.now_ms = 1100;
    /* The group has three members; there is no ninth. */
    YEW_ASSERT(yew_tab_jump_key(&ed, nav_digit('9', 0U)));
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    YEW_ASSERT(!yew_tab_jump_armed());
    yew_ed_free(&ed);
}

/* The deadline is a timer entry, so the hint clears on an idle editor
 * rather than sitting there promising something until a key arrives. */
void test_groupnav_digit_jump_deadline_is_a_timer(void)
{
    Ed ed;

    nav_many_tabs(&ed, 12);
    ed.now_ms = 1000;
    nav_goto(&ed, 1);
    YEW_ASSERT(yew_tab_jump_armed());
    /* No keys at all — just the clock reaching the deadline. */
    yew_timers_fire(&ed.timers, &ed, 1000 + YEW_JUMP_WINDOW_MS);
    YEW_ASSERT(!yew_tab_jump_armed());
    yew_ed_free(&ed);
}
