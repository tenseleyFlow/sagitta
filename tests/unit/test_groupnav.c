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
#include "ui/region.h"
#include "ui/tabs.h"
#include "term/grid.h"

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

static CmdStatus nav_invoke(Ed *ed, const char *name)
{
    CmdId id = yew_cmd_lookup(name, strlen(name));
    CmdCtx cx;

    YEW_ASSERT(id.v != 0U);
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    return yew_ed_invoke(ed, id, &cx);
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

void test_groupnav_close_closes_every_clean_member_by_id(void)
{
    Ed ed;
    u32 gid;
    u32 doomed[3];

    gid = nav_fixture(&ed);
    for (int i = 0; i < 3; i++)
        doomed[i] = yew_tab_at(&ed, i + 2)->tab_id;
    yew_tab_switch(&ed, 3);
    YEW_ASSERT_EQ_I64(nav_invoke(&ed, "ed.group.close"), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 3U);
    YEW_ASSERT_EQ_I64(yew_group_find(&ed, gid), -1);
    for (int i = 0; i < 3; i++)
        YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, doomed[i]), -1);
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), 0U);
    yew_ed_free(&ed);
}

void test_groupnav_close_refuses_dirty_group_without_partial_close(void)
{
    Ed ed;
    EditCtx ec;
    u32 gid;
    u32 members[3];

    gid = nav_fixture(&ed);
    for (int i = 0; i < 3; i++)
        members[i] = yew_tab_at(&ed, i + 2)->tab_id;
    yew_tab_switch(&ed, 3);
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U));
    yew_ed_finish_edit(&ed, &ec);
    YEW_ASSERT(yew_tab_modified(&ed, 3));
    YEW_ASSERT_EQ_I64(nav_invoke(&ed, "ed.group.close"),
                      YEW_CMD_ERR_STATE);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 6U);
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, gid), 3);
    for (int i = 0; i < 3; i++)
        YEW_ASSERT(yew_tab_index_of_id(&ed, members[i]) >= 0);
    YEW_ASSERT(!ed.tab_prompt.active);
    yew_ed_free(&ed);
}

void test_groupnav_close_ungrouped_delegates_to_tab_close(void)
{
    Ed ed;
    u32 gid;
    u32 tab_id;

    gid = nav_fixture(&ed);
    tab_id = yew_tab_at(&ed, 1)->tab_id;
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), 0U);
    YEW_ASSERT_EQ_I64(nav_invoke(&ed, "ed.group.close"), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, tab_id), -1);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 5U);
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, gid), 3);
    yew_ed_free(&ed);
}

void test_groupnav_close_ungrouped_retains_dirty_tab_prompt(void)
{
    Ed ed;
    EditCtx ec;
    u32 tab_id;

    (void)nav_fixture(&ed);
    tab_id = yew_tab_at(&ed, 1)->tab_id;
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U));
    yew_ed_finish_edit(&ed, &ec);
    YEW_ASSERT_EQ_I64(nav_invoke(&ed, "ed.group.close"), YEW_CMD_OK);
    YEW_ASSERT(ed.tab_prompt.active);
    YEW_ASSERT_EQ_U64(ed.tab_prompt.tab_id, tab_id);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 6U);
    YEW_ASSERT(yew_tab_prompt_key(&ed, 0x1BU));
    YEW_ASSERT(yew_tab_index_of_id(&ed, tab_id) >= 0);
    yew_ed_free(&ed);
}

void test_groupnav_close_refuses_when_group_contains_every_tab(void)
{
    Ed ed;
    u32 gid;

    gid = nav_fixture(&ed);
    yew_group_add_member(&ed, gid, 0);
    yew_group_add_member(&ed, gid, 1);
    yew_group_add_member(&ed, gid, 5);
    yew_tab_switch(&ed, 0);
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, gid), 6);
    YEW_ASSERT_EQ_I64(nav_invoke(&ed, "ed.group.close"),
                      YEW_CMD_ERR_STATE);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 6U);
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, gid), 6);
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
 * Sprint 57.10 rewrites the landing rule.  `alt+3` from row 1 ENTERS
 * the group (its row-1 entry is number 3), and the window it arms is a
 * row-1 window.  A further ALT digit is not a continuation of that —
 * the bar is now showing members, and alt counts the row you are on —
 * so the window clears and the key dispatches as its own jump, which
 * picks the member.
 */
void test_groupnav_digit_jump_picks_a_group_member(void)
{
    Ed ed;
    u32 g;

    g = nav_fixture(&ed);
    (void)g;
    ed.now_ms = 1000;
    /* Row-1 entry 3 is the group; entering resumes at the first member
     * because nothing has been noted yet. */
    nav_goto(&ed, 3);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    YEW_ASSERT(yew_tab_jump_armed());

    ed.now_ms = 1100;
    /* NOT consumed: the window was a row-1 window and alt now means
     * members.  It cleared, and the key goes on to the keymap. */
    YEW_ASSERT(!yew_tab_jump_key(&ed, nav_digit('2', YEW_MOD_ALT)));
    YEW_ASSERT(!yew_tab_jump_armed());
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    /* ...where `alt+2` is ed.tab.goto 2, and inside the group that is
     * the SECOND member. */
    nav_goto(&ed, 2);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 3);
    yew_ed_free(&ed);
}

/* Out of range within a group leaves the first jump standing too. */
void test_groupnav_digit_jump_group_member_out_of_range(void)
{
    Ed ed;

    (void)nav_fixture(&ed);
    yew_tab_switch(&ed, 2);
    ed.now_ms = 1000;
    /* Inside the group: member 1. */
    nav_goto(&ed, 1);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    ed.now_ms = 1100;
    /* The group has three members; there is no nineteenth. */
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

/* ---------------------------------------------------------------- */
/* Sprint 57.10: positional numbering and row-aware jumps           */
/* ---------------------------------------------------------------- */

/*
 * The fixture's row 1 is [scratch, t1, G, t2].  Every entry carries its
 * POSITION, group included: with the flat index the bar would read
 * 1, 2, src/, 6 and `alt+4` would land on a tab labelled 6.
 */
void test_groupnav_row1_numbers_entries_by_position(void)
{
    Ed ed;
    StripEntry entries[16];
    int n;

    (void)nav_fixture(&ed);
    n = yew_tab_row1_entries(&ed, entries, (int)YEW_ARRAY_LEN(entries));
    YEW_ASSERT_EQ_I64(n, 4);
    YEW_ASSERT_EQ_STR(entries[0].label, " 1 untitled ");
    YEW_ASSERT_EQ_STR(entries[1].label, " 2 yew-nav-0.txt ");
    YEW_ASSERT_EQ_STR(entries[2].label, " 3 src/ (3) ");
    YEW_ASSERT_EQ_STR(entries[3].label, " 4 yew-nav-4.txt ");
    yew_ed_free(&ed);
}

/* The text of grid row `y`, single-byte cells only, right-trimmed. */
static void nav_row_text(const Ed *ed, u16 y, char *out, size_t cap)
{
    size_t n = 0U;
    u16 x;

    for (x = 0U; x < ed->grid.cols && n + 1U < cap; x++) {
        const Cell *c = &ed->grid.back[(size_t)y * ed->grid.cols + x];

        out[n++] = (char)(c->utf8[0] >= 32U && c->utf8[0] < 127U
                              ? c->utf8[0] : ' ');
    }
    while (n > 0U && out[n - 1U] == ' ')
        n--;
    out[n] = '\0';
}

/* Row 2 numbers the members 1..n by ordinal — the number `alt+N`
 * inside the group addresses. */
void test_groupnav_row2_numbers_members_by_ordinal(void)
{
    Ed ed;
    char row[128];
    u32 g;

    g = nav_fixture(&ed);
    YEW_ASSERT(yew_grid_init(&ed.grid, &ed.interner, 4U, 80U));
    ed.grid_ready = true;
    yew_tab_switch(&ed, 3);
    yew_region_frame_begin();
    yew_tab_member_strip_draw(&ed, (Rect){0U, 0U, 80U, 1U}, g);
    nav_row_text(&ed, 0U, row, sizeof(row));
    YEW_ASSERT_EQ_STR(row,
                      " 1 yew-nav-1.txt  2 yew-nav-2.txt  3 yew-nav-3.txt");
    yew_ed_free(&ed);
}

static CmdStatus nav_goto_status(Ed *ed, const char *name, i64 n)
{
    CmdId id = yew_cmd_lookup(name, strlen(name));
    CmdCtx cx;

    YEW_ASSERT(id.v != 0U);
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.iarg = n;
    cx.source = YEW_SRC_TEST;
    return yew_ed_invoke(ed, id, &cx);
}

/* Inside a group `alt+N` counts row 2: member N, nothing else. */
void test_groupnav_goto_inside_a_group_picks_the_member(void)
{
    Ed ed;

    (void)nav_fixture(&ed);
    yew_tab_switch(&ed, 2);
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto", 2), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 3);
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto", 3), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 4);
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto", 1), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    /* Member 4 does not exist; `4` is NOT row-1 entry 4 (t2) from in
     * here, and the jump refuses rather than leaving the group. */
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto", 4),
                      YEW_CMD_ERR_ARG);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    /* 0 is the tenth key; there is no tenth member either. */
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto", 0),
                      YEW_CMD_ERR_ARG);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    yew_ed_free(&ed);
}

/*
 * Outside a group `alt+N` counts row 1, and a group entry is ENTERED —
 * the same resume-at-last-active route `t <down>` takes, so number,
 * chord and click cannot land on three different members.
 */
void test_groupnav_goto_outside_a_group_counts_row1(void)
{
    Ed ed;

    (void)nav_fixture(&ed);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 1);
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto", 4), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 5); /* t2: entry 4, index 5 */
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto", 5),
                      YEW_CMD_ERR_ARG);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 5);
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto", 3), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2); /* entered at the first member */
    YEW_ASSERT_EQ_I64(yew_active_group_id(&ed) != 0U, 1);
    yew_ed_free(&ed);
}

/*
 * `ctrl+N` is row 1 from ANYWHERE.  Leaving a group by number notes the
 * position, so coming back by number resumes on the member you left.
 */
void test_groupnav_goto_bar_counts_row1_from_inside_a_group(void)
{
    Ed ed;

    (void)nav_fixture(&ed);
    yew_tab_switch(&ed, 3); /* member b */
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto_bar", 1),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 0); /* scratch, not member 1 */
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto_bar", 3),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 3); /* back on b, not a */
    /* The group's own entry from inside it: stays put. */
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto_bar", 3),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 3);
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto_bar", 4),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 5);
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto_bar", 5),
                      YEW_CMD_ERR_ARG);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 5);
    yew_ed_free(&ed);
}

/* Leaving by `alt+N` from inside is impossible (alt counts members),
 * but leaving by `ctrl+N` and returning by `alt+N` resumes too. */
void test_groupnav_goto_resumes_the_member_left_by_number(void)
{
    Ed ed;

    (void)nav_fixture(&ed);
    yew_tab_switch(&ed, 4); /* member c */
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto_bar", 2),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 1);
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto", 3), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 4);
    yew_ed_free(&ed);
}

/* `0` is the tenth key on the digit row for the bar command too. */
void test_groupnav_goto_bar_maps_zero_to_ten(void)
{
    Ed ed;

    nav_many_tabs(&ed, 12);
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto_bar", 0),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 9);
    yew_ed_free(&ed);
}

/* Twelve members, so a two-digit member number has somewhere to land. */
static u32 nav_big_group(Ed *ed)
{
    u32 g;
    int i;

    nav_many_tabs(ed, 16);
    g = yew_group_create(ed, "/big", NULL);
    for (i = 1; i <= 12; i++)
        yew_group_add_member(ed, g, i);
    yew_tab_switch(ed, 1);
    return g;
}

/* Inside a group the window extends the MEMBER number: `alt+1` `2` is
 * member 12, and the hint says so. */
void test_groupnav_digit_jump_extends_member_numbers(void)
{
    Ed ed;

    (void)nav_big_group(&ed);
    ed.now_ms = 1000;
    nav_goto(&ed, 1);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 1);
    YEW_ASSERT(yew_tab_jump_armed());
    YEW_ASSERT_NOT_NULL(strstr((const char *)ed.msg.text, "member 1"));
    ed.now_ms = 1100;
    YEW_ASSERT(yew_tab_jump_key(&ed, nav_digit('2', YEW_MOD_ALT)));
    YEW_ASSERT_EQ_I64(ed.tabs.active, 12); /* member 12 */
    YEW_ASSERT(yew_tab_jump_armed());
    /* Member 120 does not exist: swallowed, the jump stands. */
    ed.now_ms = 1200;
    YEW_ASSERT(yew_tab_jump_key(&ed, nav_digit('0', 0U)));
    YEW_ASSERT_EQ_I64(ed.tabs.active, 12);
    YEW_ASSERT(!yew_tab_jump_armed());
    yew_ed_free(&ed);
}

/* A row-1 window extends row-1 numbers even when the jump entered a
 * group: `ctrl+1` `2` is entry 12, wherever entry 1 led. */
void test_groupnav_digit_jump_extends_row1_numbers_across_a_group(void)
{
    Ed ed;
    u32 g;
    int i;

    nav_many_tabs(&ed, 16);
    g = yew_group_create(&ed, "/big", NULL);
    for (i = 1; i <= 3; i++)
        yew_group_add_member(&ed, g, i);
    yew_tab_switch(&ed, 0);
    /* Row 1: [scratch, G, t4, t5, ..., t15] — fourteen entries. */
    ed.now_ms = 1000;
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto_bar", 2),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 1); /* entered G */
    ed.now_ms = 1100;
    YEW_ASSERT(yew_tab_jump_key(&ed, nav_digit('1', YEW_MOD_CTRL)));
    /* Entry 21 does not exist; consumed, the first jump stands. */
    YEW_ASSERT_EQ_I64(ed.tabs.active, 1);
    YEW_ASSERT(!yew_tab_jump_armed());

    ed.now_ms = 2000;
    YEW_ASSERT_EQ_I64(nav_goto_status(&ed, "ed.tab.goto_bar", 1),
                      YEW_CMD_OK);
    ed.now_ms = 2100;
    YEW_ASSERT(yew_tab_jump_key(&ed, nav_digit('2', 0U)));
    /* Entry 12 is t13: index 13 (scratch, three members, t4..). */
    YEW_ASSERT_EQ_I64(ed.tabs.active, 13);
    yew_ed_free(&ed);
}

/*
 * Modifier discipline.  A held `ctrl` continues only a row-1 window; a
 * held `alt` continues only a window of the kind alt would open now.
 * Anything else clears the window and lets the key be its own jump.
 */
void test_groupnav_digit_jump_modifier_discipline(void)
{
    Ed ed;
    u32 g;

    (void)nav_big_group(&ed);
    ed.now_ms = 1000;
    /* Member window; a ctrl digit is a row-1 jump, not member 14. */
    nav_goto(&ed, 1);
    ed.now_ms = 1100;
    YEW_ASSERT(!yew_tab_jump_key(&ed, nav_digit('4', YEW_MOD_CTRL)));
    YEW_ASSERT(!yew_tab_jump_armed());
    YEW_ASSERT_EQ_I64(ed.tabs.active, 1);

    yew_ed_free(&ed);

    /* Row-1 window opened from outside; still outside, so alt continues
     * it: `alt+1` `alt+2` is entry 12.  Row 1 here is
     * [scratch, G{t1,t2}, t3, ..., t19], so entry k >= 3 is index k. */
    nav_many_tabs(&ed, 20);
    g = yew_group_create(&ed, "/two", NULL);
    yew_group_add_member(&ed, g, 1);
    yew_group_add_member(&ed, g, 2);
    yew_tab_switch(&ed, 0);
    ed.now_ms = 2000;
    nav_goto(&ed, 1);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 0);
    ed.now_ms = 2100;
    YEW_ASSERT(yew_tab_jump_key(&ed, nav_digit('2', YEW_MOD_ALT)));
    YEW_ASSERT_EQ_I64(ed.tabs.active, 12);
    YEW_ASSERT(yew_tab_jump_armed());
    yew_ed_free(&ed);
}
