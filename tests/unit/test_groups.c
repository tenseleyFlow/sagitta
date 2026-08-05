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
#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "ui/groups.h"
#include "ui/tabs.h"

static void gp_fixture(Ed *ed)
{
    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    sag_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
}

/* Opens `n` extra tabs so the array has 1 + n entries. */
static void gp_open_many(Ed *ed, u32 n)
{
    u32 i;

    for (i = 0U; i < n; i++) {
        char path[64];

        (void)snprintf(path, sizeof(path), "/tmp/sag-grp-%u.txt",
                       (unsigned)i);
        SAG_ASSERT(sag_tab_open(ed, path) >= 0);
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
    g = sag_group_create(&ed, "/home/u/proj/src", "");
    SAG_ASSERT(g != 0U);
    sag_group_label(&ed, g, label, sizeof(label));
    /* Zero members so far, and the count is live even now. */
    SAG_ASSERT_EQ_STR(label, "src/ (0)");

    /* A trailing slash on the origin must not produce an empty name. */
    g = sag_group_create(&ed, "/home/u/proj/tests/", NULL);
    sag_group_label(&ed, g, label, sizeof(label));
    SAG_ASSERT_EQ_STR(label, "tests/ (0)");

    /* The root has no basename to take. */
    g = sag_group_create(&ed, "/", NULL);
    sag_group_label(&ed, g, label, sizeof(label));
    SAG_ASSERT_EQ_STR(label, "/ (0)");

    /* An explicit name wins over the directory. */
    g = sag_group_create(&ed, "/home/u/proj/src", "backend");
    sag_group_label(&ed, g, label, sizeof(label));
    SAG_ASSERT_EQ_STR(label, "backend (0)");
    sag_ed_free(&ed);
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
    a = sag_group_create(&ed, "/a", NULL);
    b = sag_group_create(&ed, "/b", NULL);
    SAG_ASSERT(b > a);
    sag_group_dissolve(&ed, a);
    SAG_ASSERT_EQ_I64(sag_group_find(&ed, a), -1);
    c = sag_group_create(&ed, "/c", NULL);
    SAG_ASSERT(c > b);
    SAG_ASSERT_EQ_I64(sag_group_find(&ed, a), -1);
    sag_ed_free(&ed);
}

void test_groups_label_count_is_computed_not_stored(void)
{
    Ed ed;
    u32 g;
    char label[64];

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 1);
    sag_group_add_member(&ed, g, 2);
    sag_group_label(&ed, g, label, sizeof(label));
    SAG_ASSERT_EQ_STR(label, "src/ (2)");

    /* Closing a member changes the label with no one telling it to. */
    SAG_ASSERT(sag_tab_close(&ed, 1));
    sag_group_label(&ed, g, label, sizeof(label));
    SAG_ASSERT_EQ_STR(label, "src/ (1)");
    sag_ed_free(&ed);
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
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 1);
    sag_group_add_member(&ed, g, 3);
    /* Ordinals are 1-based and assigned in join order. */
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 1)->group_id, g);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 1)->group_ordinal, 1U);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 3)->group_ordinal, 2U);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 0)->group_id, 0U);

    n = sag_group_members(&ed, g, members, 8);
    SAG_ASSERT_EQ_I64(n, 2);
    SAG_ASSERT_EQ_I64(members[0], 1);
    SAG_ASSERT_EQ_I64(members[1], 3);
    SAG_ASSERT_EQ_I64(sag_group_member_count(&ed, g), 2);
    sag_ed_free(&ed);
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
    a = sag_group_create(&ed, "/a", NULL);
    b = sag_group_create(&ed, "/b", NULL);
    sag_group_add_member(&ed, a, 1);
    sag_group_add_member(&ed, a, 2);
    sag_group_add_member(&ed, a, 3);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 3)->group_ordinal, 3U);

    /* Tab 2 (ordinal 2) defects to b. */
    sag_group_add_member(&ed, b, 2);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 2)->group_id, b);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 2)->group_ordinal, 1U);
    SAG_ASSERT_EQ_I64(sag_group_member_count(&ed, a), 2);
    /* a's ordinals closed the hole rather than leaving a gap at 2. */
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 1)->group_ordinal, 1U);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 3)->group_ordinal, 2U);
    sag_ed_free(&ed);
}

/* Re-adding a member is a no-op, not a renumber to the end. */
void test_groups_readding_a_member_keeps_its_ordinal(void)
{
    Ed ed;
    u32 g;

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 1);
    sag_group_add_member(&ed, g, 2);
    sag_group_add_member(&ed, g, 1);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 1)->group_ordinal, 1U);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 2)->group_ordinal, 2U);
    SAG_ASSERT_EQ_I64(sag_group_member_count(&ed, g), 2);
    sag_ed_free(&ed);
}

void test_groups_removing_the_last_member_auto_dissolves(void)
{
    Ed ed;
    u32 g;

    gp_fixture(&ed);
    gp_open_many(&ed, 2U);
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 1);
    SAG_ASSERT(sag_group_find(&ed, g) >= 0);
    sag_group_remove_member(&ed, 1);
    /* Gone, because an empty group is a row-1 entry that resolves to
     * nothing and a walk step into a hole. */
    SAG_ASSERT_EQ_I64(sag_group_find(&ed, g), -1);
    SAG_ASSERT_EQ_U64(sag_active_group_id(&ed), 0U);
    sag_ed_free(&ed);
}

/* Closing the last member goes through the same door — tab close calls
 * remove_member BEFORE the array compaction. */
void test_groups_closing_the_last_member_auto_dissolves(void)
{
    Ed ed;
    u32 g;

    gp_fixture(&ed);
    gp_open_many(&ed, 2U);
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 2);
    SAG_ASSERT(sag_tab_close(&ed, 2));
    SAG_ASSERT_EQ_I64(sag_group_find(&ed, g), -1);
    sag_ed_free(&ed);
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
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 1);
    sag_group_add_member(&ed, g, 2);
    sag_group_add_member(&ed, g, 3);
    /* Close the MIDDLE member: ordinal 2 vacates, ordinal 3 becomes 2,
     * and every index above 2 shifts down by one. */
    SAG_ASSERT(sag_tab_close(&ed, 2));
    n = sag_group_members(&ed, g, members, 8);
    SAG_ASSERT_EQ_I64(n, 2);
    SAG_ASSERT_EQ_I64(members[0], 1);
    SAG_ASSERT_EQ_I64(members[1], 2); /* was index 3 */
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 1)->group_ordinal, 1U);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 2)->group_ordinal, 2U);
    sag_ed_free(&ed);
}

void test_groups_dissolve_ungroups_stragglers(void)
{
    Ed ed;
    u32 g;
    u32 i;

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 1);
    sag_group_add_member(&ed, g, 2);
    sag_group_dissolve(&ed, g);
    /*
     * Not one tab may be left naming the dead id: an orphan answers
     * "grouped" to every question and resolves to no group, so row 1
     * skips it as a member and row 2 never lists it — the file becomes
     * unreachable by any navigation.
     */
    for (i = 0U; i < sag_tab_count(&ed); i++) {
        SAG_ASSERT_EQ_U64(sag_tab_at(&ed, (int)i)->group_id, 0U);
        SAG_ASSERT_EQ_U64(sag_tab_at(&ed, (int)i)->group_ordinal, 0U);
    }
    sag_ed_free(&ed);
}

void test_groups_prune_empty_removes_only_empty_groups(void)
{
    Ed ed;
    u32 a;
    u32 b;

    gp_fixture(&ed);
    gp_open_many(&ed, 2U);
    a = sag_group_create(&ed, "/a", NULL);
    b = sag_group_create(&ed, "/b", NULL);
    sag_group_add_member(&ed, b, 1);
    sag_group_prune_empty(&ed);
    SAG_ASSERT_EQ_I64(sag_group_find(&ed, a), -1);
    SAG_ASSERT(sag_group_find(&ed, b) >= 0);
    sag_ed_free(&ed);
}

void test_groups_active_group_id_is_derived(void)
{
    Ed ed;
    u32 g;

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 2);
    sag_tab_switch(&ed, 0);
    SAG_ASSERT_EQ_U64(sag_active_group_id(&ed), 0U);
    sag_tab_switch(&ed, 2);
    SAG_ASSERT_EQ_U64(sag_active_group_id(&ed), g);
    /* Assigning the active index RAW — which several call sites do —
     * still gives the right answer, because nothing cached it. */
    ed.tabs.active = 0;
    SAG_ASSERT_EQ_U64(sag_active_group_id(&ed), 0U);
    sag_ed_free(&ed);
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
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 1); /* ordinal 1 */
    sag_group_add_member(&ed, g, 2); /* ordinal 2 */
    sag_group_add_member(&ed, g, 3); /* ordinal 3 */

    /* Member at ordinal 1 moves to position 2: [1,2,3] -> [2,1,3]. */
    sag_group_set_ordinal(&ed, 1, 2);
    SAG_ASSERT_EQ_I64(sag_group_members(&ed, g, members, 8), 3);
    SAG_ASSERT_EQ_I64(members[0], 2);
    SAG_ASSERT_EQ_I64(members[1], 1);
    SAG_ASSERT_EQ_I64(members[2], 3);
    /* Renumbered 1..n with no holes and no duplicates. */
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 2)->group_ordinal, 1U);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 1)->group_ordinal, 2U);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 3)->group_ordinal, 3U);
    sag_ed_free(&ed);
}

void test_groups_set_ordinal_moves_left_and_clamps(void)
{
    Ed ed;
    u32 g;
    int members[8];

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 1);
    sag_group_add_member(&ed, g, 2);
    sag_group_add_member(&ed, g, 3);

    /* Last to first. */
    sag_group_set_ordinal(&ed, 3, 1);
    SAG_ASSERT_EQ_I64(sag_group_members(&ed, g, members, 8), 3);
    SAG_ASSERT_EQ_I64(members[0], 3);
    SAG_ASSERT_EQ_I64(members[1], 1);
    SAG_ASSERT_EQ_I64(members[2], 2);

    /* Out of range clamps rather than corrupting the run. */
    sag_group_set_ordinal(&ed, 3, 99);
    SAG_ASSERT_EQ_I64(sag_group_members(&ed, g, members, 8), 3);
    SAG_ASSERT_EQ_I64(members[2], 3);
    sag_group_set_ordinal(&ed, 3, -4);
    SAG_ASSERT_EQ_I64(sag_group_members(&ed, g, members, 8), 3);
    SAG_ASSERT_EQ_I64(members[0], 3);
    sag_ed_free(&ed);
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
        ids[i] = sag_tab_at(&ed, (int)i)->tab_id;
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 0);
    sag_group_add_member(&ed, g, 1);

    /* Move the block to the end: the 3 ungrouped tabs slide left and
     * keep their relative order; the members stay adjacent. */
    sag_group_reorder_block(&ed, g, 3);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 0)->tab_id, ids[2]);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 1)->tab_id, ids[3]);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 2)->tab_id, ids[4]);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 3)->tab_id, ids[0]);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 4)->tab_id, ids[1]);
    /* Contiguous, and in ordinal order. */
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 3)->group_ordinal, 1U);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 4)->group_ordinal, 2U);
    sag_ed_free(&ed);
}

void test_groups_reorder_block_keeps_the_active_tab(void)
{
    Ed ed;
    u32 g;
    u32 active_id;

    gp_fixture(&ed);
    gp_open_many(&ed, 4U);
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 0);
    sag_group_add_member(&ed, g, 1);
    sag_tab_switch(&ed, 4);
    active_id = sag_tab_at(&ed, 4)->tab_id;

    sag_group_reorder_block(&ed, g, 3);
    /* Active follows the TAB, not the number. */
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, ed.tabs.active)->tab_id, active_id);
    sag_ed_free(&ed);
}

void test_groups_reorder_block_clamps_the_destination(void)
{
    Ed ed;
    u32 g;
    u32 first;

    gp_fixture(&ed);
    gp_open_many(&ed, 3U);
    g = sag_group_create(&ed, "/src", NULL);
    sag_group_add_member(&ed, g, 2);
    sag_group_add_member(&ed, g, 3);
    first = sag_tab_at(&ed, 2)->tab_id;

    /* Past the end lands the block at the end, not outside the array. */
    sag_group_reorder_block(&ed, g, 999);
    SAG_ASSERT_EQ_U64(sag_tab_count(&ed), 4U);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 2)->tab_id, first);
    sag_group_reorder_block(&ed, g, -5);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 0)->tab_id, first);
    sag_ed_free(&ed);
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
    SAG_ASSERT_EQ_I64((int)ed->groups.v.len, o->n_groups);
    for (i = 0; i < o->n_groups; i++) {
        int members[ORACLE_MAX_TABS];
        int n;

        SAG_ASSERT(sag_group_find(ed, o->gid[i]) >= 0);
        n = sag_group_members(ed, o->gid[i], members, ORACLE_MAX_TABS);
        SAG_ASSERT_EQ_I64(n, o->n_members[i]);
        SAG_ASSERT_EQ_I64(sag_group_member_count(ed, o->gid[i]),
                          o->n_members[i]);
        /* DoD 10: a live group always has members. */
        SAG_ASSERT(n > 0);
        for (j = 0; j < n; j++) {
            const Tab *t = sag_tab_at(ed, members[j]);

            /* Same members, in the same order. */
            SAG_ASSERT_EQ_U64(t->tab_id, o->members[i][j]);
            /* Ordinals are exactly 1..n, in order, with no holes. */
            SAG_ASSERT_EQ_U64(t->group_ordinal, (u32)(j + 1));
            SAG_ASSERT_EQ_U64(t->group_id, o->gid[i]);
        }
    }
    /* No tab points at a group that does not exist. */
    for (i = 0; i < (int)sag_tab_count(ed); i++) {
        const Tab *t = sag_tab_at(ed, i);

        if (t->group_id == 0U) {
            SAG_ASSERT_EQ_U64(t->group_ordinal, 0U);
            continue;
        }
        SAG_ASSERT(sag_group_find(ed, t->group_id) >= 0);
        SAG_ASSERT(oracle_find(o, t->group_id) >= 0);
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
        int ntabs = (int)sag_tab_count(&ed);
        int idx = ntabs > 0 ? (int)(storm_rand(&seed) % (u32)ntabs) : 0;

        switch (r % 6U) {
        case 0: /* create a group */
            if (o.n_groups < ORACLE_MAX_GROUPS) {
                u32 g = sag_group_create(&ed, "/src", NULL);

                /* An empty group exists only until the next prune or
                 * the first member leaves; the oracle records it once
                 * it has a member, so create alone changes nothing it
                 * tracks.  Add the member immediately to keep the two
                 * models describing the same world. */
                sag_group_add_member(&ed, g, idx);
                oracle_remove(&o, sag_tab_at(&ed, idx)->tab_id);
                o.gid[o.n_groups] = g;
                o.members[o.n_groups][0] = sag_tab_at(&ed, idx)->tab_id;
                o.n_members[o.n_groups] = 1;
                o.n_groups++;
            }
            break;
        case 1: /* join an existing group */
            if (o.n_groups > 0 && ntabs > 0) {
                int gi = (int)(storm_rand(&seed) % (u32)o.n_groups);
                u32 gid = o.gid[gi];
                u32 tid = sag_tab_at(&ed, idx)->tab_id;

                if (sag_tab_at(&ed, idx)->group_id != gid) {
                    sag_group_add_member(&ed, gid, idx);
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
                u32 tid = sag_tab_at(&ed, idx)->tab_id;

                sag_group_remove_member(&ed, idx);
                oracle_remove(&o, tid);
            }
            break;
        case 3: /* close a tab */
            if (ntabs > 1) {
                u32 tid = sag_tab_at(&ed, idx)->tab_id;

                SAG_ASSERT(sag_tab_close(&ed, idx));
                oracle_remove(&o, tid);
            }
            break;
        case 4: /* open a tab */
            if (ntabs < ORACLE_MAX_TABS - 1) {
                char path[64];

                (void)snprintf(path, sizeof(path), "/tmp/sag-storm-%d.txt",
                               op);
                SAG_ASSERT(sag_tab_open(&ed, path) >= 0);
            }
            break;
        default: /* reorder a member within its group */
            if (ntabs > 0 && sag_tab_at(&ed, idx)->group_id != 0U) {
                u32 gid = sag_tab_at(&ed, idx)->group_id;
                int gi = oracle_find(&o, gid);
                int n = gi >= 0 ? o.n_members[gi] : 0;

                if (n > 0) {
                    int pos = (int)(storm_rand(&seed) % (u32)n) + 1;
                    u32 tid = sag_tab_at(&ed, idx)->tab_id;
                    int at = 0;
                    int k;

                    sag_group_set_ordinal(&ed, idx, pos);
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
    sag_ed_free(&ed);
}
