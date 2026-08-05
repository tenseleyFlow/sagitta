/*
 * Sprint 23 §1/§2: the tab model.
 *
 * Nearly every row here is about ONE hazard: closing a tab compacts the
 * array, so an index saved a moment ago now names a different tab.  In
 * facsimile that let a closed pane's text be written over its
 * sibling's file — the index was still in range, it had just come to
 * mean something else.
 *
 * So the tests hold ids across mutations and assert what the ids point
 * at afterwards, rather than asserting positions.
 */
#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"
#include "ui/region.h"
#include "ui/tabs.h"

static void tb_fixture(Ed *ed)
{
    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    /* The editor always has exactly one tab. */
    SAG_ASSERT_EQ_U64(sag_tab_count(ed), 1U);
    SAG_ASSERT_EQ_I64(ed->tabs.active, 0);
    sag_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
}

/* Opens `n` extra tabs, returning their ids. */
static void tb_open_many(Ed *ed, u32 n, u32 *ids)
{
    u32 i;

    for (i = 0U; i < n; i++) {
        char path[64];
        int idx;

        (void)snprintf(path, sizeof(path), "/tmp/sag-tab-%u.txt",
                       (unsigned)i);
        idx = sag_tab_open(ed, path);
        SAG_ASSERT(idx >= 0);
        ids[i] = sag_tab_at(ed, idx)->tab_id;
    }
}

void test_tabs_ids_are_monotonic_and_never_reused(void)
{
    Ed ed;
    u32 ids[4];
    u32 first;

    tb_fixture(&ed);
    tb_open_many(&ed, 4U, ids);
    SAG_ASSERT_EQ_U64(sag_tab_count(&ed), 5U);
    /* Strictly increasing, and none is the invalid 0. */
    SAG_ASSERT(ids[0] != 0U);
    SAG_ASSERT(ids[1] > ids[0]);
    SAG_ASSERT(ids[2] > ids[1]);
    SAG_ASSERT(ids[3] > ids[2]);

    first = ids[0];
    SAG_ASSERT(sag_tab_close(&ed, sag_tab_index_of_id(&ed, first)));
    /* A stale id resolves to NOTHING, not to whatever took the slot. */
    SAG_ASSERT_EQ_I64(sag_tab_index_of_id(&ed, first), -1);
    /* And the next id issued is not the freed one. */
    {
        int idx = sag_tab_open(&ed, "/tmp/sag-tab-new.txt");

        SAG_ASSERT(idx >= 0);
        SAG_ASSERT(sag_tab_at(&ed, idx)->tab_id > ids[3]);
    }
    sag_ed_free(&ed);
}

/*
 * The storm: close three of seven and confirm every survivor's id still
 * names the same tab, at its new index.
 */
void test_tabs_ids_survive_a_close_storm(void)
{
    Ed ed;
    u32 ids[6];
    u32 keep[4];
    u32 i;

    tb_fixture(&ed);
    tb_open_many(&ed, 6U, ids);
    SAG_ASSERT_EQ_U64(sag_tab_count(&ed), 7U);

    /* Survivors: the original tab plus ids 0, 3, 5. */
    keep[0] = sag_tab_at(&ed, 0)->tab_id;
    keep[1] = ids[0];
    keep[2] = ids[3];
    keep[3] = ids[5];

    /* Close 1, 2 and 4 — by id, resolved fresh each time, because the
     * indices move underneath. */
    SAG_ASSERT(sag_tab_close(&ed, sag_tab_index_of_id(&ed, ids[1])));
    SAG_ASSERT(sag_tab_close(&ed, sag_tab_index_of_id(&ed, ids[2])));
    SAG_ASSERT(sag_tab_close(&ed, sag_tab_index_of_id(&ed, ids[4])));
    SAG_ASSERT_EQ_U64(sag_tab_count(&ed), 4U);

    for (i = 0U; i < 4U; i++) {
        int at = sag_tab_index_of_id(&ed, keep[i]);

        SAG_ASSERT(at >= 0);
        /* The id names the same tab it always did, wherever it now
         * sits. */
        SAG_ASSERT_EQ_U64(sag_tab_at(&ed, at)->tab_id, keep[i]);
    }
    /* The closed ids are gone rather than aliased onto survivors. */
    SAG_ASSERT_EQ_I64(sag_tab_index_of_id(&ed, ids[1]), -1);
    SAG_ASSERT_EQ_I64(sag_tab_index_of_id(&ed, ids[2]), -1);
    SAG_ASSERT_EQ_I64(sag_tab_index_of_id(&ed, ids[4]), -1);
    sag_ed_free(&ed);
}

/*
 * The regression fixture the sprint names: closing the ACTIVE tab while
 * a lower-indexed tab exists must activate the neighbour, not whatever
 * the old index now points at.
 */
void test_tabs_survivor_is_chosen_by_id_before_compaction(void)
{
    Ed ed;
    u32 ids[3];
    u32 right_id;

    tb_fixture(&ed);
    tb_open_many(&ed, 3U, ids);
    /* Tabs: [orig, A, B, C].  Make B active and close it. */
    sag_tab_switch(&ed, 2);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 2);
    right_id = ids[2]; /* C, to B's right */

    SAG_ASSERT(sag_tab_close(&ed, 2));
    /* C becomes active — by id.  A naive implementation keeps index 2,
     * which after compaction is a different tab entirely (or past the
     * end). */
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, ed.tabs.active)->tab_id, right_id);

    /* Closing the last tab falls back to the LEFT neighbour. */
    {
        int last = (int)sag_tab_count(&ed) - 1;
        u32 left_id = sag_tab_at(&ed, last - 1)->tab_id;

        sag_tab_switch(&ed, last);
        SAG_ASSERT(sag_tab_close(&ed, last));
        SAG_ASSERT_EQ_U64(sag_tab_at(&ed, ed.tabs.active)->tab_id,
                          left_id);
    }
    sag_ed_free(&ed);
}

/* Closing a tab that is NOT active leaves the active tab active — the
 * same tab, whatever its index became. */
void test_tabs_closing_another_tab_keeps_the_active_one(void)
{
    Ed ed;
    u32 ids[3];
    u32 active_id;

    tb_fixture(&ed);
    tb_open_many(&ed, 3U, ids);
    sag_tab_switch(&ed, 3);
    active_id = sag_tab_at(&ed, 3)->tab_id;

    /* Close a tab to its LEFT, which shifts it down one index. */
    SAG_ASSERT(sag_tab_close(&ed, 1));
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, ed.tabs.active)->tab_id, active_id);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 2);
    sag_ed_free(&ed);
}

/*
 * shifted_index, exhaustively over n = 6, against a naive simulation
 * that actually moves elements.  The formula is small and wrong in
 * exactly one direction if you get an inequality backwards.
 */
void test_tabs_shifted_index_matches_a_naive_simulation(void)
{
    enum { N = 6 };
    int from;
    int to;
    int i;

    for (from = 0; from < N; from++) {
        for (to = 0; to < N; to++) {
            int model[N];
            int moved;
            int j;

            for (i = 0; i < N; i++)
                model[i] = i;
            /* Naive: remove then insert, which is what "insertion, not
             * swap" means. */
            moved = model[from];
            if (to > from) {
                for (j = from; j < to; j++)
                    model[j] = model[j + 1];
            } else {
                for (j = from; j > to; j--)
                    model[j] = model[j - 1];
            }
            model[to] = moved;

            for (i = 0; i < N; i++) {
                int want = -1;
                int k;

                for (k = 0; k < N; k++) {
                    if (model[k] == i) {
                        want = k;
                        break;
                    }
                }
                if (sag_tab_shifted_index(i, from, to) != want)
                    (void)fprintf(stderr,
                                  "shifted_index(%d, %d, %d) = %d, "
                                  "simulation says %d\n", i, from, to,
                                  sag_tab_shifted_index(i, from, to),
                                  want);
                SAG_ASSERT_EQ_I64(sag_tab_shifted_index(i, from, to),
                                  want);
            }
        }
    }
}

/* Reorder is an insertion: the tabs passed keep their relative order. */
void test_tabs_reorder_is_insertion_not_swap(void)
{
    Ed ed;
    u32 ids[4];
    u32 order[5];
    u32 i;

    tb_fixture(&ed);
    tb_open_many(&ed, 4U, ids);
    for (i = 0U; i < 5U; i++)
        order[i] = sag_tab_at(&ed, (int)i)->tab_id;

    /* Move the first tab three places right. */
    sag_tab_reorder(&ed, 0, 3);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 0)->tab_id, order[1]);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 1)->tab_id, order[2]);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 2)->tab_id, order[3]);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 3)->tab_id, order[0]);
    /* A swap would have put order[3] at 0; the three it passed kept
     * their relative order instead. */
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 4)->tab_id, order[4]);
    sag_ed_free(&ed);
}

/* Active is a position, so it rides the shift. */
void test_tabs_reorder_remaps_the_active_position(void)
{
    Ed ed;
    u32 ids[4];
    u32 active_id;

    tb_fixture(&ed);
    tb_open_many(&ed, 4U, ids);
    sag_tab_switch(&ed, 2);
    active_id = sag_tab_at(&ed, 2)->tab_id;

    sag_tab_reorder(&ed, 0, 4);
    /* Everything above 0 shifted down by one, including the active. */
    SAG_ASSERT_EQ_I64(ed.tabs.active, 1);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, ed.tabs.active)->tab_id, active_id);

    /* Moving the active tab itself takes the active index with it. */
    sag_tab_reorder(&ed, 1, 3);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 3);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 3)->tab_id, active_id);
    sag_ed_free(&ed);
}

/* One tab per file: opening a path already open switches to it. */
void test_tabs_open_switches_when_the_path_is_already_open(void)
{
    Ed ed;
    int first;
    int again;

    tb_fixture(&ed);
    first = sag_tab_open(&ed, "/tmp/sag-tab-dup.txt");
    SAG_ASSERT(first >= 0);
    SAG_ASSERT_EQ_U64(sag_tab_count(&ed), 2U);

    again = sag_tab_open(&ed, "/tmp/sag-tab-dup.txt");
    SAG_ASSERT_EQ_I64(again, first);
    /* No second tab, and it switched rather than opening. */
    SAG_ASSERT_EQ_U64(sag_tab_count(&ed), 2U);
    SAG_ASSERT_EQ_I64(ed.tabs.active, first);
    sag_ed_free(&ed);
}

/*
 * find_by_path canonicalizes at the single site where a name is
 * established, so a relative path finds the tab opened by an absolute
 * one.
 */
void test_tabs_find_by_path_canonicalizes(void)
{
    Ed ed;
    int idx;

    tb_fixture(&ed);
    /* A path that exists, so realpath resolves it. */
    idx = sag_tab_open(&ed, "/tmp/.");
    SAG_ASSERT(idx >= 0);
    /* "/tmp/." and "/tmp" canonicalize to the same thing. */
    SAG_ASSERT_EQ_I64(sag_tab_find_by_path(&ed, "/tmp"), idx);
    SAG_ASSERT_EQ_I64(sag_tab_find_by_path(&ed, "/tmp/./."), idx);
    SAG_ASSERT_EQ_I64(sag_tab_find_by_path(&ed, "/nonexistent-xyz"), -1);
    sag_ed_free(&ed);
}

/*
 * DoD 6.  The cap fails LOUDLY and changes nothing: a silent failure
 * made facsimile's callers load the new file into the still-active tab,
 * and the next save wrote it over the old file's path.
 */
void test_tabs_cap_refuses_and_mutates_nothing(void)
{
    Ed ed;
    u32 before_count;
    int before_active;
    u32 before_next_id;

    tb_fixture(&ed);
    /* The view cap bites before the tab cap on this build, and either
     * way the contract is the same: -1, and nothing moved. */
    while (sag_tab_count(&ed) < (u32)SAG_TAB_MAX) {
        char path[64];
        int idx;

        (void)snprintf(path, sizeof(path), "/tmp/sag-cap-%u.txt",
                       (unsigned)sag_tab_count(&ed));
        idx = sag_tab_open(&ed, path);
        if (idx < 0)
            break;
    }
    before_count = sag_tab_count(&ed);
    before_active = ed.tabs.active;
    before_next_id = ed.tabs.next_tab_id;

    SAG_ASSERT_EQ_I64(sag_tab_open(&ed, "/tmp/sag-cap-over.txt"), -1);
    /* Nothing moved: no tab, no active change, and no id burned. */
    SAG_ASSERT_EQ_U64(sag_tab_count(&ed), before_count);
    SAG_ASSERT_EQ_I64(ed.tabs.active, before_active);
    SAG_ASSERT_EQ_U64(ed.tabs.next_tab_id, before_next_id);
    sag_ed_free(&ed);
}

/*
 * DoD 4: the modified marker is DERIVED.  Edit makes it true, undo back
 * to the clean state makes it false again — which a cached flag gets
 * wrong exactly once and then lies forever.
 */
void test_tabs_modified_derives_from_undo_state(void)
{
    Ed ed;
    EditCtx ec;

    tb_fixture(&ed);
    SAG_ASSERT(!sag_tab_modified(&ed, 0));

    ec = sag_ed_edit_ctx(&ed);
    SAG_ASSERT(sag_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"hello", 5U));
    sag_ed_finish_edit(&ed, &ec);
    SAG_ASSERT(sag_tab_modified(&ed, 0));

    /* Undo back to clean clears it. */
    ec = sag_ed_edit_ctx(&ed);
    SAG_ASSERT(sag_undo(&ec));
    sag_ed_finish_edit(&ed, &ec);
    SAG_ASSERT(!sag_tab_modified(&ed, 0));

    /* And redo sets it again. */
    ec = sag_ed_edit_ctx(&ed);
    SAG_ASSERT(sag_redo(&ec));
    sag_ed_finish_edit(&ed, &ec);
    SAG_ASSERT(sag_tab_modified(&ed, 0));
    sag_ed_free(&ed);
}

/* Switching swaps the visible tree; each Win keeps its own cursor and
 * viewport with no save/restore choreography. */
void test_tabs_switch_swaps_the_pane_tree(void)
{
    Ed ed;
    u32 ids[2];
    Pane *root0;
    Pane *root1;

    tb_fixture(&ed);
    root0 = ed.pane_root;
    tb_open_many(&ed, 2U, ids);
    sag_tab_switch(&ed, 1);
    root1 = ed.pane_root;
    SAG_ASSERT(root1 != root0);
    SAG_ASSERT(ed.focus == sag_tab_at(&ed, 1)->focus);
    SAG_ASSERT(ed.win == ed.focus->win);

    sag_tab_switch(&ed, 0);
    SAG_ASSERT(ed.pane_root == root0);
    SAG_ASSERT(ed.focus == sag_tab_at(&ed, 0)->focus);
    sag_ed_free(&ed);
}

/* Sprint 24 owns groups; until then the fields are inert and the
 * deferred placeholder is asserted false. */
/* A freshly opened tab is ungrouped.  Sprint 24 made the group fields
 * live, so what is asserted here is the DEFAULT, not inertness. */
void test_tabs_open_leaves_a_tab_ungrouped(void)
{
    Ed ed;
    u32 ids[2];
    u32 i;

    tb_fixture(&ed);
    tb_open_many(&ed, 2U, ids);
    for (i = 0U; i < sag_tab_count(&ed); i++) {
        const Tab *t = sag_tab_at(&ed, (int)i);

        SAG_ASSERT_EQ_U64(t->group_id, 0U);
        SAG_ASSERT_EQ_U64(t->group_ordinal, 0U);
    }
    sag_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* Commands and the dirty-close prompt (§5/§6)                      */
/* ---------------------------------------------------------------- */

static CmdStatus tb_invoke(Ed *ed, const char *name, i64 iarg,
                           const char *sarg)
{
    CmdId id = sag_cmd_lookup(name, (u32)strlen(name));
    CmdCtx cx;

    SAG_ASSERT(id.v != 0U);
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.iarg = iarg;
    cx.sarg = sarg;
    cx.sarg_len = sarg == NULL ? 0U : (u32)strlen(sarg);
    cx.source = SAG_SRC_TEST;
    return sag_ed_invoke(ed, id, &cx);
}

/* DoD 8: 0 reaches tab 10, and out of range messages and stays put. */
void test_tabs_goto_maps_zero_to_ten_and_refuses_out_of_range(void)
{
    Ed ed;
    u32 ids[9];

    tb_fixture(&ed);
    tb_open_many(&ed, 9U, ids); /* 10 tabs total */
    SAG_ASSERT_EQ_U64(sag_tab_count(&ed), 10U);

    SAG_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.goto", 1, NULL), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 0);
    SAG_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.goto", 3, NULL), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 2);
    /* 0 is the tenth key on the digit row, so it means tab 10. */
    SAG_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.goto", 0, NULL), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 9);

    /* Out of range: refused, and the active tab does not move. */
    SAG_ASSERT(tb_invoke(&ed, "ed.tab.goto", 99, NULL) != SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 9);
    sag_ed_free(&ed);
}

void test_tabs_next_and_prev_are_cyclic(void)
{
    Ed ed;
    u32 ids[2];

    tb_fixture(&ed);
    tb_open_many(&ed, 2U, ids);
    sag_tab_switch(&ed, 0);

    SAG_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.next", 0, NULL), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 1);
    SAG_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.next", 0, NULL), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 2);
    /* Round the ring rather than stopping at the end. */
    SAG_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.next", 0, NULL), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 0);
    SAG_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.prev", 0, NULL), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 2);
    sag_ed_free(&ed);
}

void test_tabs_move_reorders_the_active_tab(void)
{
    Ed ed;
    u32 ids[3];
    u32 active_id;

    tb_fixture(&ed);
    tb_open_many(&ed, 3U, ids);
    sag_tab_switch(&ed, 0);
    active_id = sag_tab_at(&ed, 0)->tab_id;

    SAG_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.move", 3, NULL), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(ed.tabs.active, 2);
    SAG_ASSERT_EQ_U64(sag_tab_at(&ed, 2)->tab_id, active_id);
    SAG_ASSERT(tb_invoke(&ed, "ed.tab.move", 99, NULL) != SAG_CMD_OK);
    sag_ed_free(&ed);
}

/* A clean tab closes without a question. */
void test_tabs_close_command_is_silent_when_clean(void)
{
    Ed ed;
    u32 ids[1];

    tb_fixture(&ed);
    tb_open_many(&ed, 1U, ids);
    sag_tab_switch(&ed, 1);
    SAG_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.close", 0, NULL),
                      SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_tab_count(&ed), 1U);
    SAG_ASSERT(!ed.tab_prompt.active);
    /* The last tab refuses to close. */
    SAG_ASSERT(tb_invoke(&ed, "ed.tab.close", 0, NULL) != SAG_CMD_OK);
    sag_ed_free(&ed);
}

/*
 * The prompt holds the tab_ID.  If the tab it asked about disappears
 * while the question is up, the question dissolves rather than
 * answering for whichever tab inherited the index.
 */
void test_tabs_dirty_prompt_holds_the_id_not_the_index(void)
{
    Ed ed;
    u32 ids[2];
    EditCtx ec;
    u32 asked_id;
    u32 other_id;

    tb_fixture(&ed);
    tb_open_many(&ed, 2U, ids);
    sag_tab_switch(&ed, 2);
    /* Dirty the active tab so closing it asks. */
    ec = sag_ed_edit_ctx(&ed);
    SAG_ASSERT(sag_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U));
    sag_ed_finish_edit(&ed, &ec);
    SAG_ASSERT(sag_tab_modified(&ed, 2));

    SAG_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.close", 0, NULL),
                      SAG_CMD_OK);
    SAG_ASSERT(ed.tab_prompt.active);
    asked_id = ed.tab_prompt.tab_id;
    SAG_ASSERT_EQ_U64(asked_id, sag_tab_at(&ed, 2)->tab_id);

    /* Something else closes a LOWER tab while the question is up, so
     * index 2 now names a different tab entirely. */
    other_id = sag_tab_at(&ed, 1)->tab_id;
    SAG_ASSERT(sag_tab_close(&ed, 1));
    SAG_ASSERT_EQ_I64(sag_tab_index_of_id(&ed, other_id), -1);
    /* The question still refers to the tab it asked about, now at 1. */
    SAG_ASSERT_EQ_I64(sag_tab_index_of_id(&ed, asked_id), 1);

    /* Discard answers for THAT tab, not for whatever index 2 became. */
    SAG_ASSERT(sag_tab_prompt_key(&ed, (u8)'d'));
    SAG_ASSERT_EQ_I64(sag_tab_index_of_id(&ed, asked_id), -1);
    SAG_ASSERT(!ed.tab_prompt.active);
    sag_ed_free(&ed);
}

void test_tabs_dirty_prompt_esc_cancels_the_close(void)
{
    Ed ed;
    u32 ids[1];
    EditCtx ec;
    u32 id;

    tb_fixture(&ed);
    tb_open_many(&ed, 1U, ids);
    sag_tab_switch(&ed, 1);
    ec = sag_ed_edit_ctx(&ed);
    SAG_ASSERT(sag_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U));
    sag_ed_finish_edit(&ed, &ec);
    id = sag_tab_at(&ed, 1)->tab_id;

    SAG_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.close", 0, NULL),
                      SAG_CMD_OK);
    SAG_ASSERT(ed.tab_prompt.active);
    SAG_ASSERT(sag_tab_prompt_key(&ed, 0x1BU));
    /* Cancelled: the tab is still there, with its text. */
    SAG_ASSERT(!ed.tab_prompt.active);
    SAG_ASSERT_EQ_I64(sag_tab_index_of_id(&ed, id), 1);
    SAG_ASSERT(sag_tab_modified(&ed, 1));
    sag_ed_free(&ed);
}

/* Any other key is swallowed with the question restated, rather than
 * falling through to whatever it is normally bound to. */
void test_tabs_dirty_prompt_swallows_other_keys(void)
{
    Ed ed;
    u32 ids[1];
    EditCtx ec;

    tb_fixture(&ed);
    tb_open_many(&ed, 1U, ids);
    sag_tab_switch(&ed, 1);
    ec = sag_ed_edit_ctx(&ed);
    SAG_ASSERT(sag_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U));
    sag_ed_finish_edit(&ed, &ec);

    SAG_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.close", 0, NULL),
                      SAG_CMD_OK);
    /* `j` would normally move the cursor; here it is consumed. */
    SAG_ASSERT(sag_tab_prompt_key(&ed, (u8)'j'));
    SAG_ASSERT(ed.tab_prompt.active);
    SAG_ASSERT_EQ_U64(sag_tab_count(&ed), 2U);
    sag_ed_free(&ed);
}

/*
 * DoD 9: a negative SAG_REGION_TAB payload means the renderer and the
 * router disagree about the sign convention Sprint 24 will use, which
 * is exactly what writing it down early was meant to prevent.
 */
void test_tabs_negative_region_payload_is_a_bug(void)
{
    int fds[2];
    pid_t child;
    pid_t waited;
    int status;
    char output[1024];
    ssize_t got;

    SAG_ASSERT_EQ_I64(pipe(fds), 0);
    SAG_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    SAG_ASSERT(child >= 0);
    if (child == 0) {
        Ed ed;

        (void)close(fds[0]);
        (void)dup2(fds[1], STDERR_FILENO);
        (void)close(fds[1]);
        tb_fixture(&ed);
        sag_region_frame_begin();
        /* A hand-built region carrying the convention Sprint 24 will
         * introduce.  Reaching the router today means the renderer and
         * the router disagree, which is what writing the sign rule down
         * early was meant to prevent. */
        sag_region_add(SAG_REGION_TAB, (Rect){0U, 0U, 8U, 1U}, -3);
        (void)sag_tab_strip_click(&ed, 1U, 0U);
        _exit(0);
    }
    SAG_ASSERT_EQ_I64(close(fds[1]), 0);
    got = read(fds[0], output, sizeof(output) - 1U);
    SAG_ASSERT(got >= 0);
    output[got < 0 ? 0U : (size_t)got] = '\0';
    SAG_ASSERT_EQ_I64(close(fds[0]), 0);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    SAG_ASSERT_EQ_I64(waited, child);
    SAG_ASSERT(WIFEXITED(status));
    SAG_ASSERT_EQ_I64(WEXITSTATUS(status), SAG_EXIT_BUG);
    SAG_ASSERT(strstr(output, "Sprint 24") != NULL);
}
