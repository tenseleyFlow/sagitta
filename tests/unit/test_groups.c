/*
 * Sprint 24 §1/§2: the group model.
 *
 * The model's entire design is "compute membership, never store it", so
 * these tests refuse to take the model's word for anything.  The storm
 * at the bottom runs 5k random ops against an INDEPENDENT oracle that
 * keeps the member lists the real model deliberately does not, and
 * compares after every single op.
 *
 * The two named regressions above it — set_ordinal moving right, and
 * reorder_block past a run of ungrouped tabs — are the off-by-one and
 * the interleave that the sprint contract calls out by name.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text/file.h"

#include "edit/ed.h"
#include "ui/groups.h"
#include "ui/tabs.h"

static void gp_fixture(Ed *ed)
{
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    yew_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
}

/* Opens `n` extra tabs so the array has 1 + n entries. */
static void gp_open_many(Ed *ed, u32 n)
{
    u32 i;

    for (i = 0U; i < n; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/yew-grp-%u.txt",
                       (unsigned)i);
        YEW_ASSERT(yew_tab_open(ed, path) >= 0);
    }
}

/* ---------------------------------------------------------------- */
/* Creation and labelling                                           */
/* ---------------------------------------------------------------- */

void test_groups_create_labels_from_the_directory_basename(void)
{
    Ed ed;
    u32 g;
    char label[64];

    gp_fixture(&ed);
    /* An empty name means "call it after the directory". */
    g = yew_group_create(&ed, "/home/u/proj/src", "");
    YEW_ASSERT(g != 0U);
    yew_group_label(&ed, g, label, sizeof(label));
    /* Zero members so far, and the count is live even now. */
    YEW_ASSERT_EQ_STR(label, "src/ (0)");

    /* A trailing slash on the origin must not produce an empty name. */
    g = yew_group_create(&ed, "/home/u/proj/tests/", NULL);
    yew_group_label(&ed, g, label, sizeof(label));
    YEW_ASSERT_EQ_STR(label, "tests/ (0)");

    /* The root has no basename to take. */
    g = yew_group_create(&ed, "/", NULL);
    yew_group_label(&ed, g, label, sizeof(label));
    YEW_ASSERT_EQ_STR(label, "/ (0)");

    /* An explicit name wins over the directory. */
    g = yew_group_create(&ed, "/home/u/proj/src", "backend");
    yew_group_label(&ed, g, label, sizeof(label));
    YEW_ASSERT_EQ_STR(label, "backend (0)");
    yew_ed_free(&ed);
}

/* Ids are monotonic and a dissolved one never resolves again — the same
 * discipline tab_id has, for the same reason. */
void test_groups_ids_are_monotonic_and_never_reused(void)
{
    Ed ed;
    u32 a;
    u32 b;
    u32 c;

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    a = yew_group_create(&ed, "/a", NULL);
    b = yew_group_create(&ed, "/b", NULL);
    YEW_ASSERT(b > a);
    yew_group_dissolve(&ed, a);
    YEW_ASSERT_EQ_I64(yew_group_find(&ed, a), -1);
    c = yew_group_create(&ed, "/c", NULL);
    YEW_ASSERT(c > b);
    YEW_ASSERT_EQ_I64(yew_group_find(&ed, a), -1);
    yew_ed_free(&ed);
}

void test_groups_label_count_is_computed_not_stored(void)
{
    Ed ed;
    u32 g;
    char label[64];

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 1);
    yew_group_add_member(&ed, g, 2);
    yew_group_label(&ed, g, label, sizeof(label));
    YEW_ASSERT_EQ_STR(label, "src/ (2)");

    /* Closing a member changes the label with no one telling it to. */
    YEW_ASSERT(yew_tab_close(&ed, 1));
    yew_group_label(&ed, g, label, sizeof(label));
    YEW_ASSERT_EQ_STR(label, "src/ (1)");
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* Membership                                                       */
/* ---------------------------------------------------------------- */

void test_groups_membership_lives_on_the_tab(void)
{
    Ed ed;
    u32 g;
    int members[8];
    int n;

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 1);
    yew_group_add_member(&ed, g, 3);
    /* Ordinals are 1-based and assigned in join order. */
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 1)->group_id, g);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 1)->group_ordinal, 1U);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 3)->group_ordinal, 2U);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 0)->group_id, 0U);

    n = yew_group_members(&ed, g, members, 8);
    YEW_ASSERT_EQ_I64(n, 2);
    YEW_ASSERT_EQ_I64(members[0], 1);
    YEW_ASSERT_EQ_I64(members[1], 3);
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, g), 2);
    yew_ed_free(&ed);
}

/* A tab belongs to exactly one group: joining a second LEAVES the
 * first, and the first's ordinals compact behind it. */
void test_groups_a_tab_joins_only_one_group(void)
{
    Ed ed;
    u32 a;
    u32 b;

    gp_fixture(&ed);
    gp_open_many(&ed, 4U);
    a = yew_group_create(&ed, "/a", NULL);
    b = yew_group_create(&ed, "/b", NULL);
    yew_group_add_member(&ed, a, 1);
    yew_group_add_member(&ed, a, 2);
    yew_group_add_member(&ed, a, 3);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 3)->group_ordinal, 3U);

    /* Tab 2 (ordinal 2) defects to b. */
    yew_group_add_member(&ed, b, 2);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 2)->group_id, b);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 2)->group_ordinal, 1U);
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, a), 2);
    /* a's ordinals closed the hole rather than leaving a gap at 2. */
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 1)->group_ordinal, 1U);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 3)->group_ordinal, 2U);
    yew_ed_free(&ed);
}

/* Re-adding a member is a no-op, not a renumber to the end. */
void test_groups_readding_a_member_keeps_its_ordinal(void)
{
    Ed ed;
    u32 g;

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 1);
    yew_group_add_member(&ed, g, 2);
    yew_group_add_member(&ed, g, 1);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 1)->group_ordinal, 1U);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 2)->group_ordinal, 2U);
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, g), 2);
    yew_ed_free(&ed);
}

void test_groups_removing_the_last_member_auto_dissolves(void)
{
    Ed ed;
    u32 g;

    gp_fixture(&ed);
    gp_open_many(&ed, 2U);
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 1);
    YEW_ASSERT(yew_group_find(&ed, g) >= 0);
    yew_group_remove_member(&ed, 1);
    /* Gone, because an empty group is a row-1 entry that resolves to
     * nothing and a walk step into a hole. */
    YEW_ASSERT_EQ_I64(yew_group_find(&ed, g), -1);
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), 0U);
    yew_ed_free(&ed);
}

/* Closing the last member goes through the same door — tab close calls
 * remove_member BEFORE the array compaction. */
void test_groups_closing_the_last_member_auto_dissolves(void)
{
    Ed ed;
    u32 g;

    gp_fixture(&ed);
    gp_open_many(&ed, 2U);
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 2);
    YEW_ASSERT(yew_tab_close(&ed, 2));
    YEW_ASSERT_EQ_I64(yew_group_find(&ed, g), -1);
    yew_ed_free(&ed);
}

/*
 * Closing a member compacts the TABS array under the group.  The
 * survivors' ordinals must describe the survivors, not the array they
 * used to sit in.
 */
void test_groups_close_compacts_ordinals_not_indices(void)
{
    Ed ed;
    u32 g;
    int members[8];
    int n;

    gp_fixture(&ed);
    gp_open_many(&ed, 4U);
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 1);
    yew_group_add_member(&ed, g, 2);
    yew_group_add_member(&ed, g, 3);
    /* Close the MIDDLE member: ordinal 2 vacates, ordinal 3 becomes 2,
     * and every index above 2 shifts down by one. */
    YEW_ASSERT(yew_tab_close(&ed, 2));
    n = yew_group_members(&ed, g, members, 8);
    YEW_ASSERT_EQ_I64(n, 2);
    YEW_ASSERT_EQ_I64(members[0], 1);
    YEW_ASSERT_EQ_I64(members[1], 2); /* was index 3 */
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 1)->group_ordinal, 1U);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 2)->group_ordinal, 2U);
    yew_ed_free(&ed);
}

void test_groups_dissolve_ungroups_stragglers(void)
{
    Ed ed;
    u32 g;
    u32 i;

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 1);
    yew_group_add_member(&ed, g, 2);
    yew_group_dissolve(&ed, g);
    /*
     * Not one tab may be left naming the dead id: an orphan answers
     * "grouped" to every question and resolves to no group, so row 1
     * skips it as a member and row 2 never lists it — the file becomes
     * unreachable by any navigation.
     */
    for (i = 0U; i < yew_tab_count(&ed); i++) {
        YEW_ASSERT_EQ_U64(yew_tab_at(&ed, (int)i)->group_id, 0U);
        YEW_ASSERT_EQ_U64(yew_tab_at(&ed, (int)i)->group_ordinal, 0U);
    }
    yew_ed_free(&ed);
}

void test_groups_prune_empty_removes_only_empty_groups(void)
{
    Ed ed;
    u32 a;
    u32 b;

    gp_fixture(&ed);
    gp_open_many(&ed, 2U);
    a = yew_group_create(&ed, "/a", NULL);
    b = yew_group_create(&ed, "/b", NULL);
    yew_group_add_member(&ed, b, 1);
    yew_group_prune_empty(&ed);
    YEW_ASSERT_EQ_I64(yew_group_find(&ed, a), -1);
    YEW_ASSERT(yew_group_find(&ed, b) >= 0);
    yew_ed_free(&ed);
}

void test_groups_active_group_id_is_derived(void)
{
    Ed ed;
    u32 g;

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 2);
    yew_tab_switch(&ed, 0);
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), 0U);
    yew_tab_switch(&ed, 2);
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), g);
    /* Assigning the active index RAW — which several call sites do —
     * still gives the right answer, because nothing cached it. */
    ed.tabs.active = 0;
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), 0U);
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* set_ordinal: the rightward off-by-one                            */
/* ---------------------------------------------------------------- */

/*
 * The contract names this case: move right by one in a 3-member group.
 *
 * The bug it guards is "skip the moved member, insert at pos" WITHOUT
 * removing it first — the member's own vacated slot is still counted,
 * so every rightward move lands one short and the call looks like a
 * no-op.
 */
void test_groups_set_ordinal_moves_right_by_exactly_one(void)
{
    Ed ed;
    u32 g;
    int members[8];

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 1); /* ordinal 1 */
    yew_group_add_member(&ed, g, 2); /* ordinal 2 */
    yew_group_add_member(&ed, g, 3); /* ordinal 3 */

    /* Member at ordinal 1 moves to position 2: [1,2,3] -> [2,1,3]. */
    yew_group_set_ordinal(&ed, 1, 2);
    YEW_ASSERT_EQ_I64(yew_group_members(&ed, g, members, 8), 3);
    YEW_ASSERT_EQ_I64(members[0], 2);
    YEW_ASSERT_EQ_I64(members[1], 1);
    YEW_ASSERT_EQ_I64(members[2], 3);
    /* Renumbered 1..n with no holes and no duplicates. */
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 2)->group_ordinal, 1U);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 1)->group_ordinal, 2U);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 3)->group_ordinal, 3U);
    yew_ed_free(&ed);
}

void test_groups_set_ordinal_moves_left_and_clamps(void)
{
    Ed ed;
    u32 g;
    int members[8];

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 1);
    yew_group_add_member(&ed, g, 2);
    yew_group_add_member(&ed, g, 3);

    /* Last to first. */
    yew_group_set_ordinal(&ed, 3, 1);
    YEW_ASSERT_EQ_I64(yew_group_members(&ed, g, members, 8), 3);
    YEW_ASSERT_EQ_I64(members[0], 3);
    YEW_ASSERT_EQ_I64(members[1], 1);
    YEW_ASSERT_EQ_I64(members[2], 2);

    /* Out of range clamps rather than corrupting the run. */
    yew_group_set_ordinal(&ed, 3, 99);
    YEW_ASSERT_EQ_I64(yew_group_members(&ed, g, members, 8), 3);
    YEW_ASSERT_EQ_I64(members[2], 3);
    yew_group_set_ordinal(&ed, 3, -4);
    YEW_ASSERT_EQ_I64(yew_group_members(&ed, g, members, 8), 3);
    YEW_ASSERT_EQ_I64(members[0], 3);
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* reorder_block: the interleave regression                         */
/* ---------------------------------------------------------------- */

/*
 * The contract names this one too: a 2-member group moved past 3
 * ungrouped tabs.
 *
 * Moving members one at a time drags already-placed ones back down and
 * leaves the group INTERLEAVED with the tabs it passed — the group
 * renders as one row-1 entry, so the interleave shows up as ungrouped
 * tabs mysteriously changing places.
 */
void test_groups_reorder_block_stays_contiguous_past_other_tabs(void)
{
    Ed ed;
    u32 g;
    u32 ids[5];
    u32 i;

    gp_fixture(&ed);
    gp_open_many(&ed, 4U); /* 5 tabs: 0..4 */
    for (i = 0U; i < 5U; i++)
        ids[i] = yew_tab_at(&ed, (int)i)->tab_id;
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 0);
    yew_group_add_member(&ed, g, 1);

    /* Move the block to the end: the 3 ungrouped tabs slide left and
     * keep their relative order; the members stay adjacent. */
    yew_group_reorder_block(&ed, g, 3);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 0)->tab_id, ids[2]);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 1)->tab_id, ids[3]);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 2)->tab_id, ids[4]);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 3)->tab_id, ids[0]);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 4)->tab_id, ids[1]);
    /* Contiguous, and in ordinal order. */
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 3)->group_ordinal, 1U);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 4)->group_ordinal, 2U);
    yew_ed_free(&ed);
}

void test_groups_reorder_block_keeps_the_active_tab(void)
{
    Ed ed;
    u32 g;
    u32 active_id;

    gp_fixture(&ed);
    gp_open_many(&ed, 4U);
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 0);
    yew_group_add_member(&ed, g, 1);
    yew_tab_switch(&ed, 4);
    active_id = yew_tab_at(&ed, 4)->tab_id;

    yew_group_reorder_block(&ed, g, 3);
    /* Active follows the TAB, not the number. */
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, ed.tabs.active)->tab_id, active_id);
    yew_ed_free(&ed);
}

void test_groups_reorder_block_clamps_the_destination(void)
{
    Ed ed;
    u32 g;
    u32 first;

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 2);
    yew_group_add_member(&ed, g, 3);
    first = yew_tab_at(&ed, 2)->tab_id;

    /* Past the end lands the block at the end, not outside the array. */
    yew_group_reorder_block(&ed, g, 999);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 4U);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 2)->tab_id, first);
    yew_group_reorder_block(&ed, g, -5);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 0)->tab_id, first);
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* The membership storm                                             */
/* ---------------------------------------------------------------- */

/*
 * An INDEPENDENT oracle: it keeps, per group, the very member list the
 * real model refuses to keep.  If the model's computed answer and the
 * oracle's stored one ever disagree, one of them is wrong — and the
 * oracle is the naive one, so it is the model that has to explain.
 *
 * The oracle tracks tabs by tab_ID, because the array compacts under
 * both of them and an index-keyed oracle would drift the same way the
 * model is being tested for.
 */
enum {
    ORACLE_MAX_TABS = 64,
    ORACLE_MAX_GROUPS = 16,
    STORM_OPS = 5000
};

typedef struct Oracle {
    u32 gid[ORACLE_MAX_GROUPS];
    u32 members[ORACLE_MAX_GROUPS][ORACLE_MAX_TABS]; /* tab_ids, ordered */
    int n_members[ORACLE_MAX_GROUPS];
    int n_groups;
} Oracle;

static int oracle_find(const Oracle *o, u32 gid)
{
    int i;

    for (i = 0; i < o->n_groups; i++) {
        if (o->gid[i] == gid)
            return i;
    }
    return -1;
}

static void oracle_drop_group(Oracle *o, int at)
{
    (void)memmove(&o->gid[at], &o->gid[at + 1],
                  (size_t)(o->n_groups - at - 1) * sizeof(o->gid[0]));
    (void)memmove(&o->members[at], &o->members[at + 1],
                  (size_t)(o->n_groups - at - 1) * sizeof(o->members[0]));
    (void)memmove(&o->n_members[at], &o->n_members[at + 1],
                  (size_t)(o->n_groups - at - 1) * sizeof(o->n_members[0]));
    o->n_groups--;
}

/* Removes a tab_id from whichever group holds it, dissolving an emptied
 * group exactly as the model does. */
static void oracle_remove(Oracle *o, u32 tid)
{
    int i;
    int j;

    for (i = 0; i < o->n_groups; i++) {
        for (j = 0; j < o->n_members[i]; j++) {
            if (o->members[i][j] != tid)
                continue;
            (void)memmove(&o->members[i][j], &o->members[i][j + 1],
                          (size_t)(o->n_members[i] - j - 1) *
                              sizeof(o->members[i][0]));
            o->n_members[i]--;
            if (o->n_members[i] == 0)
                oracle_drop_group(o, i);
            return;
        }
    }
}

/* Deterministic PRNG: the storm must replay byte-identically. */
static u32 storm_rand(u32 *state)
{
    *state = *state * 1664525U + 1013904223U;
    return (*state >> 16) & 0x7FFFU;
}

static void storm_check(Ed *ed, const Oracle *o)
{
    int i;
    int j;

    /* Every group the model has, the oracle has — and vice versa. */
    YEW_ASSERT_EQ_I64((int)ed->groups.v.len, o->n_groups);
    for (i = 0; i < o->n_groups; i++) {
        int members[ORACLE_MAX_TABS];
        int n;

        YEW_ASSERT(yew_group_find(ed, o->gid[i]) >= 0);
        n = yew_group_members(ed, o->gid[i], members, ORACLE_MAX_TABS);
        YEW_ASSERT_EQ_I64(n, o->n_members[i]);
        YEW_ASSERT_EQ_I64(yew_group_member_count(ed, o->gid[i]),
                          o->n_members[i]);
        /* DoD 10: a live group always has members. */
        YEW_ASSERT(n > 0);
        for (j = 0; j < n; j++) {
            const Tab *t = yew_tab_at(ed, members[j]);

            /* Same members, in the same order. */
            YEW_ASSERT_EQ_U64(t->tab_id, o->members[i][j]);
            /* Ordinals are exactly 1..n, in order, with no holes. */
            YEW_ASSERT_EQ_U64(t->group_ordinal, (u32)(j + 1));
            YEW_ASSERT_EQ_U64(t->group_id, o->gid[i]);
        }
    }
    /* No tab points at a group that does not exist. */
    for (i = 0; i < (int)yew_tab_count(ed); i++) {
        const Tab *t = yew_tab_at(ed, i);

        if (t->group_id == 0U) {
            YEW_ASSERT_EQ_U64(t->group_ordinal, 0U);
            continue;
        }
        YEW_ASSERT(yew_group_find(ed, t->group_id) >= 0);
        YEW_ASSERT(oracle_find(o, t->group_id) >= 0);
    }
}

void test_groups_membership_storm_matches_a_naive_oracle(void)
{
    Ed ed;
    Oracle o;
    u32 seed = 0x5A61771AU; /* pinned: the storm replays identically */
    int op;

    gp_fixture(&ed);
    gp_open_many(&ed, 7U);
    (void)memset(&o, 0, sizeof(o));

    for (op = 0; op < STORM_OPS; op++) {
        u32 r = storm_rand(&seed);
        int ntabs = (int)yew_tab_count(&ed);
        int idx = ntabs > 0 ? (int)(storm_rand(&seed) % (u32)ntabs) : 0;

        switch (r % 6U) {
        case 0: /* create a group */
            if (o.n_groups < ORACLE_MAX_GROUPS) {
                u32 g = yew_group_create(&ed, "/src", NULL);

                /* An empty group exists only until the next prune or
                 * the first member leaves; the oracle records it once
                 * it has a member, so create alone changes nothing it
                 * tracks.  Add the member immediately to keep the two
                 * models describing the same world. */
                yew_group_add_member(&ed, g, idx);
                oracle_remove(&o, yew_tab_at(&ed, idx)->tab_id);
                o.gid[o.n_groups] = g;
                o.members[o.n_groups][0] = yew_tab_at(&ed, idx)->tab_id;
                o.n_members[o.n_groups] = 1;
                o.n_groups++;
            }
            break;
        case 1: /* join an existing group */
            if (o.n_groups > 0 && ntabs > 0) {
                int gi = (int)(storm_rand(&seed) % (u32)o.n_groups);
                u32 gid = o.gid[gi];
                u32 tid = yew_tab_at(&ed, idx)->tab_id;

                if (yew_tab_at(&ed, idx)->group_id != gid) {
                    yew_group_add_member(&ed, gid, idx);
                    oracle_remove(&o, tid);
                    /* oracle_remove may have dissolved a group and
                     * shifted the table, so re-find the target. */
                    gi = oracle_find(&o, gid);
                    if (gi >= 0) {
                        o.members[gi][o.n_members[gi]] = tid;
                        o.n_members[gi]++;
                    }
                }
            }
            break;
        case 2: /* leave a group */
            if (ntabs > 0) {
                u32 tid = yew_tab_at(&ed, idx)->tab_id;

                yew_group_remove_member(&ed, idx);
                oracle_remove(&o, tid);
            }
            break;
        case 3: /* close a tab */
            if (ntabs > 1) {
                u32 tid = yew_tab_at(&ed, idx)->tab_id;

                YEW_ASSERT(yew_tab_close(&ed, idx));
                oracle_remove(&o, tid);
            }
            break;
        case 4: /* open a tab */
            if (ntabs < ORACLE_MAX_TABS - 1) {
                char path[64];

                (void)snprintf(path, sizeof(path), "/tmp/yew-storm-%d.txt",
                               op);
                YEW_ASSERT(yew_tab_open(&ed, path) >= 0);
            }
            break;
        default: /* reorder a member within its group */
            if (ntabs > 0 && yew_tab_at(&ed, idx)->group_id != 0U) {
                u32 gid = yew_tab_at(&ed, idx)->group_id;
                int gi = oracle_find(&o, gid);
                int n = gi >= 0 ? o.n_members[gi] : 0;

                if (n > 0) {
                    int pos = (int)(storm_rand(&seed) % (u32)n) + 1;
                    u32 tid = yew_tab_at(&ed, idx)->tab_id;
                    int at = 0;
                    int k;

                    yew_group_set_ordinal(&ed, idx, pos);
                    /* Mirror it: remove, then insert at pos-1. */
                    for (k = 0; k < n; k++) {
                        if (o.members[gi][k] == tid)
                            at = k;
                    }
                    (void)memmove(&o.members[gi][at], &o.members[gi][at + 1],
                                  (size_t)(n - at - 1) *
                                      sizeof(o.members[gi][0]));
                    for (k = n - 1; k > pos - 1; k--)
                        o.members[gi][k] = o.members[gi][k - 1];
                    o.members[gi][pos - 1] = tid;
                }
            }
            break;
        }
        /* Every op, not every hundred: a divergence names the op that
         * caused it rather than the batch it hid in. */
        storm_check(&ed, &o);
    }
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* Sprint 24 §3: lazy hydration                                     */
/* ---------------------------------------------------------------- */

/*
 * Every row below is about the SAME hazard, from a different angle:
 * something makes a never-read tab look loaded, so the real file is
 * never read and whatever is in the fabricated buffer is what a save
 * writes over it.  Residency is asked of the allocation precisely so
 * that "looks loaded" and "is loaded" cannot come apart.
 */

typedef struct GpFiles {
    char dir[64];
    char paths[40][128];
    int n;
} GpFiles;

static void gp_files_make(GpFiles *f, int n)
{
    int i;

    (void)snprintf(f->dir, sizeof(f->dir), "/tmp/yew-grp-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->dir));
    f->n = n;
    for (i = 0; i < n; i++) {
        FILE *fp;
        /* Built in a local first: source and destination are members
         * of the same struct, which snprintf is entitled to assume do
         * not overlap. */
        char path[128];

        (void)snprintf(path, sizeof(path), "%s/f%02d.txt", f->dir, i);
        (void)snprintf(f->paths[i], sizeof(f->paths[i]), "%s", path);
        fp = fopen(f->paths[i], "w");
        YEW_ASSERT_NOT_NULL(fp);
        (void)fprintf(fp, "file %d line one\nline two\n", i);
        YEW_ASSERT_EQ_I64(fclose(fp), 0);
    }
}

static void gp_files_remove(GpFiles *f)
{
    int i;

    for (i = 0; i < f->n; i++)
        (void)unlink(f->paths[i]);
    YEW_ASSERT_EQ_I64(rmdir(f->dir), 0);
}

/*
 * DoD 4, the headline claim: opening a 40-file group performs exactly
 * ONE file read, and switching to member 2 performs the second.
 *
 * Counted rather than asserted structurally because "deferred" is only
 * worth anything if it actually avoids the syscall.
 */
void test_groups_opening_a_forty_file_group_reads_one_file(void)
{
    Ed ed;
    GpFiles f;
    u32 g;
    u64 base;
    int i;

    gp_files_make(&f, 40);
    gp_fixture(&ed);
    g = yew_group_create(&ed, f.dir, NULL);

    base = yew_file_load_count();
    for (i = 0; i < 40; i++) {
        int idx = yew_tab_open(&ed, f.paths[i]);

        YEW_ASSERT(idx >= 0);
        yew_group_add_member(&ed, g, idx);
    }
    /* Forty tabs, and not one of them has been read. */
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, g), 40);
    YEW_ASSERT_EQ_U64(yew_file_load_count(), base);
    for (i = 0; i < 40; i++)
        YEW_ASSERT(!yew_tab_is_resident(&ed, yew_tab_count(&ed) - 40U + (u32)i));

    /* Viewing the first member costs exactly one read. */
    yew_tab_switch(&ed, 1);
    YEW_ASSERT_EQ_U64(yew_file_load_count(), base + 1U);
    YEW_ASSERT(yew_tab_is_resident(&ed, 1));

    /* The second member costs the second. */
    yew_tab_switch(&ed, 2);
    YEW_ASSERT_EQ_U64(yew_file_load_count(), base + 2U);

    /* Going back reads nothing: a resident tab returns immediately. */
    yew_tab_switch(&ed, 1);
    YEW_ASSERT_EQ_U64(yew_file_load_count(), base + 2U);

    yew_ed_free(&ed);
    gp_files_remove(&f);
}

void test_groups_defer_and_hydrate_round_trip(void)
{
    Ed ed;
    GpFiles f;
    u64 before;
    int idx;

    gp_files_make(&f, 2);
    gp_fixture(&ed);
    idx = yew_tab_open(&ed, f.paths[0]);
    YEW_ASSERT(idx >= 0);
    YEW_ASSERT(!yew_tab_is_resident(&ed, idx));

    yew_tab_switch(&ed, idx);
    YEW_ASSERT(yew_tab_is_resident(&ed, idx));

    /* Deferring the ACTIVE tab is refused: the window would be left
     * pointing at no text with a cursor in it. */
    yew_tab_defer(&ed, idx);
    YEW_ASSERT(yew_tab_is_resident(&ed, idx));

    yew_tab_switch(&ed, 0);
    yew_tab_defer(&ed, idx);
    YEW_ASSERT(!yew_tab_is_resident(&ed, idx));
    /* A tab read from nowhere cannot be modified. */
    YEW_ASSERT(!yew_tab_modified(&ed, idx));

    /* And it rereads on the way back. */
    before = yew_file_load_count();
    yew_tab_switch(&ed, idx);
    YEW_ASSERT_EQ_U64(yew_file_load_count(), before + 1U);
    YEW_ASSERT(yew_tab_is_resident(&ed, idx));

    yew_ed_free(&ed);
    gp_files_remove(&f);
}

/*
 * Save refuses a non-resident tab with a DISTINCT status, not an I/O
 * error: an I/O error sends the user to check permissions on a file
 * that is perfectly fine, and writing the empty buffer destroys it.
 */
void test_groups_save_refuses_a_non_resident_tab(void)
{
    Ed ed;
    GpFiles f;
    FILE *fp;
    char first[64];
    int idx;

    gp_files_make(&f, 1);
    gp_fixture(&ed);
    idx = yew_tab_open(&ed, f.paths[0]);
    YEW_ASSERT(idx >= 0);
    YEW_ASSERT(!yew_tab_is_resident(&ed, idx));

    /* Point the editor at the non-resident tab's window WITHOUT going
     * through the switch that would hydrate it — the shape of every
     * historical bug here is a path that skipped hydrate. */
    ed.win = yew_tab_at(&ed, idx)->focus->win;
    YEW_ASSERT_EQ_I64(yew_ed_file_save(&ed, false), YEW_CMD_ERR_STATE);

    /* The file on disk is untouched — not truncated to nothing. */
    fp = fopen(f.paths[0], "r");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_NOT_NULL(fgets(first, sizeof(first), fp));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_EQ_STR(first, "file 0 line one\n");

    yew_ed_free(&ed);
    gp_files_remove(&f);
}

/* Two tabs on one path share ONE buffer, so hydrating through either
 * makes both resident — and there is only ever one claim on the save
 * destination. */
void test_groups_one_buffer_per_path(void)
{
    Ed ed;
    GpFiles f;
    int a;
    int b;

    gp_files_make(&f, 1);
    gp_fixture(&ed);
    a = yew_tab_open(&ed, f.paths[0]);
    b = yew_tab_open(&ed, f.paths[0]);
    /* The second open switches to the first tab rather than duplicating
     * it (Sprint 23), so both names resolve to the same tab. */
    YEW_ASSERT_EQ_I64(a, b);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 2U);
    yew_ed_free(&ed);
    gp_files_remove(&f);
}
