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
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"
#include "ui/groups.h"
#include "ui/region.h"
#include "ui/tabs.h"

static void tb_fixture(Ed *ed)
{
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    /* The editor always has exactly one tab. */
    YEW_ASSERT_EQ_U64(yew_tab_count(ed), 1U);
    YEW_ASSERT_EQ_I64(ed->tabs.active, 0);
    yew_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
}

/* Opens `n` extra tabs, returning their ids. */
static void tb_open_many(Ed *ed, u32 n, u32 *ids)
{
    u32 i;

    for (i = 0U; i < n; i++) {
        char path[64];
        int idx;

        (void)snprintf(path, sizeof(path), "/tmp/yew-tab-%u.txt",
                       (unsigned)i);
        idx = yew_tab_open(ed, path);
        YEW_ASSERT(idx >= 0);
        ids[i] = yew_tab_at(ed, idx)->tab_id;
    }
}

void test_tabs_ids_are_monotonic_and_never_reused(void)
{
    Ed ed;
    u32 ids[4];
    u32 first;

    tb_fixture(&ed);
    tb_open_many(&ed, 4U, ids);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 5U);
    /* Strictly increasing, and none is the invalid 0. */
    YEW_ASSERT(ids[0] != 0U);
    YEW_ASSERT(ids[1] > ids[0]);
    YEW_ASSERT(ids[2] > ids[1]);
    YEW_ASSERT(ids[3] > ids[2]);

    first = ids[0];
    YEW_ASSERT(yew_tab_close(&ed, yew_tab_index_of_id(&ed, first)));
    /* A stale id resolves to NOTHING, not to whatever took the slot. */
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, first), -1);
    /* And the next id issued is not the freed one. */
    {
        int idx = yew_tab_open(&ed, "/tmp/yew-tab-new.txt");

        YEW_ASSERT(idx >= 0);
        YEW_ASSERT(yew_tab_at(&ed, idx)->tab_id > ids[3]);
    }
    yew_ed_free(&ed);
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
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 7U);

    /* Survivors: the original tab plus ids 0, 3, 5. */
    keep[0] = yew_tab_at(&ed, 0)->tab_id;
    keep[1] = ids[0];
    keep[2] = ids[3];
    keep[3] = ids[5];

    /* Close 1, 2 and 4 — by id, resolved fresh each time, because the
     * indices move underneath. */
    YEW_ASSERT(yew_tab_close(&ed, yew_tab_index_of_id(&ed, ids[1])));
    YEW_ASSERT(yew_tab_close(&ed, yew_tab_index_of_id(&ed, ids[2])));
    YEW_ASSERT(yew_tab_close(&ed, yew_tab_index_of_id(&ed, ids[4])));
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 4U);

    for (i = 0U; i < 4U; i++) {
        int at = yew_tab_index_of_id(&ed, keep[i]);

        YEW_ASSERT(at >= 0);
        /* The id names the same tab it always did, wherever it now
         * sits. */
        YEW_ASSERT_EQ_U64(yew_tab_at(&ed, at)->tab_id, keep[i]);
    }
    /* The closed ids are gone rather than aliased onto survivors. */
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, ids[1]), -1);
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, ids[2]), -1);
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, ids[4]), -1);
    yew_ed_free(&ed);
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
    yew_tab_switch(&ed, 2);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    right_id = ids[2]; /* C, to B's right */

    YEW_ASSERT(yew_tab_close(&ed, 2));
    /* C becomes active — by id.  A naive implementation keeps index 2,
     * which after compaction is a different tab entirely (or past the
     * end). */
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, ed.tabs.active)->tab_id, right_id);

    /* Closing the last tab falls back to the LEFT neighbour. */
    {
        int last = (int)yew_tab_count(&ed) - 1;
        u32 left_id = yew_tab_at(&ed, last - 1)->tab_id;

        yew_tab_switch(&ed, last);
        YEW_ASSERT(yew_tab_close(&ed, last));
        YEW_ASSERT_EQ_U64(yew_tab_at(&ed, ed.tabs.active)->tab_id,
                          left_id);
    }
    yew_ed_free(&ed);
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
    yew_tab_switch(&ed, 3);
    active_id = yew_tab_at(&ed, 3)->tab_id;

    /* Close a tab to its LEFT, which shifts it down one index. */
    YEW_ASSERT(yew_tab_close(&ed, 1));
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, ed.tabs.active)->tab_id, active_id);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    yew_ed_free(&ed);
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
                if (yew_tab_shifted_index(i, from, to) != want)
                    (void)fprintf(stderr,
                                  "shifted_index(%d, %d, %d) = %d, "
                                  "simulation says %d\n", i, from, to,
                                  yew_tab_shifted_index(i, from, to),
                                  want);
                YEW_ASSERT_EQ_I64(yew_tab_shifted_index(i, from, to),
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
        order[i] = yew_tab_at(&ed, (int)i)->tab_id;

    /* Move the first tab three places right. */
    yew_tab_reorder(&ed, 0, 3);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 0)->tab_id, order[1]);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 1)->tab_id, order[2]);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 2)->tab_id, order[3]);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 3)->tab_id, order[0]);
    /* A swap would have put order[3] at 0; the three it passed kept
     * their relative order instead. */
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 4)->tab_id, order[4]);
    yew_ed_free(&ed);
}

/* Active is a position, so it rides the shift. */
void test_tabs_reorder_remaps_the_active_position(void)
{
    Ed ed;
    u32 ids[4];
    u32 active_id;

    tb_fixture(&ed);
    tb_open_many(&ed, 4U, ids);
    yew_tab_switch(&ed, 2);
    active_id = yew_tab_at(&ed, 2)->tab_id;

    yew_tab_reorder(&ed, 0, 4);
    /* Everything above 0 shifted down by one, including the active. */
    YEW_ASSERT_EQ_I64(ed.tabs.active, 1);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, ed.tabs.active)->tab_id, active_id);

    /* Moving the active tab itself takes the active index with it. */
    yew_tab_reorder(&ed, 1, 3);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 3);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 3)->tab_id, active_id);
    yew_ed_free(&ed);
}

/* One tab per file: opening a path already open switches to it. */
void test_tabs_open_switches_when_the_path_is_already_open(void)
{
    Ed ed;
    int first;
    int again;

    tb_fixture(&ed);
    first = yew_tab_open(&ed, "/tmp/yew-tab-dup.txt");
    YEW_ASSERT(first >= 0);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 2U);

    again = yew_tab_open(&ed, "/tmp/yew-tab-dup.txt");
    YEW_ASSERT_EQ_I64(again, first);
    /* No second tab, and it switched rather than opening. */
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 2U);
    YEW_ASSERT_EQ_I64(ed.tabs.active, first);
    yew_ed_free(&ed);
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
    idx = yew_tab_open(&ed, "/tmp/.");
    YEW_ASSERT(idx >= 0);
    /* "/tmp/." and "/tmp" canonicalize to the same thing. */
    YEW_ASSERT_EQ_I64(yew_tab_find_by_path(&ed, "/tmp"), idx);
    YEW_ASSERT_EQ_I64(yew_tab_find_by_path(&ed, "/tmp/./."), idx);
    YEW_ASSERT_EQ_I64(yew_tab_find_by_path(&ed, "/nonexistent-xyz"), -1);
    yew_ed_free(&ed);
}

/*
 * A workspace entry may be a symlink whose target lives outside the
 * workspace.  File identity must follow the canonical target, while UI
 * surfaces retain the logical spelling the user opened from the workspace.
 */
void test_tabs_display_path_preserves_workspace_spelling(void)
{
    Ed ed;
    char root[] = "/tmp/yew-tab-root-XXXXXX";
    char target[] = "/tmp/yew-tab-target-XXXXXX";
    char link_path[PATH_MAX];
    char file_path[PATH_MAX];
    char cwd[PATH_MAX];
    FILE *fp;
    int idx;

    YEW_ASSERT_NOT_NULL(mkdtemp(root));
    YEW_ASSERT_NOT_NULL(mkdtemp(target));
    YEW_ASSERT_NOT_NULL(getcwd(cwd, sizeof(cwd)));
    (void)snprintf(link_path, sizeof(link_path), "%s/linked", root);
    (void)snprintf(file_path, sizeof(file_path), "%s/note.txt", target);
    fp = fopen(file_path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    if (fp != NULL)
        YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_EQ_I64(symlink(target, link_path), 0);

    tb_fixture(&ed);
    YEW_ASSERT(yew_ed_set_workspace_root(&ed, root));
    YEW_ASSERT_EQ_I64(chdir(root), 0);
    idx = yew_tab_open(&ed, "./linked/note.txt");
    YEW_ASSERT_EQ_I64(chdir(cwd), 0);

    YEW_ASSERT(idx >= 0);
    if (idx >= 0) {
        const Tab *tab = yew_tab_at(&ed, idx);

        YEW_ASSERT_EQ_STR(tab->path, file_path);
        YEW_ASSERT_EQ_STR(yew_tab_display_path(tab), "linked/note.txt");
    }

    yew_ed_free(&ed);
    (void)unlink(link_path);
    (void)unlink(file_path);
    (void)rmdir(root);
    (void)rmdir(target);
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
    while (yew_tab_count(&ed) < (u32)YEW_TAB_MAX) {
        char path[64];
        int idx;

        (void)snprintf(path, sizeof(path), "/tmp/yew-cap-%u.txt",
                       (unsigned)yew_tab_count(&ed));
        idx = yew_tab_open(&ed, path);
        if (idx < 0)
            break;
    }
    before_count = yew_tab_count(&ed);
    before_active = ed.tabs.active;
    before_next_id = ed.tabs.next_tab_id;

    YEW_ASSERT_EQ_I64(yew_tab_open(&ed, "/tmp/yew-cap-over.txt"), -1);
    /* Nothing moved: no tab, no active change, and no id burned. */
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), before_count);
    YEW_ASSERT_EQ_I64(ed.tabs.active, before_active);
    YEW_ASSERT_EQ_U64(ed.tabs.next_tab_id, before_next_id);
    yew_ed_free(&ed);
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
    YEW_ASSERT(!yew_tab_modified(&ed, 0));

    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"hello", 5U));
    yew_ed_finish_edit(&ed, &ec);
    YEW_ASSERT(yew_tab_modified(&ed, 0));

    /* Undo back to clean clears it. */
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_undo(&ec));
    yew_ed_finish_edit(&ed, &ec);
    YEW_ASSERT(!yew_tab_modified(&ed, 0));

    /* And redo sets it again. */
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_redo(&ec));
    yew_ed_finish_edit(&ed, &ec);
    YEW_ASSERT(yew_tab_modified(&ed, 0));
    yew_ed_free(&ed);
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
    yew_tab_switch(&ed, 1);
    root1 = ed.pane_root;
    YEW_ASSERT(root1 != root0);
    YEW_ASSERT(ed.focus == yew_tab_at(&ed, 1)->focus);
    YEW_ASSERT(ed.win == ed.focus->win);

    yew_tab_switch(&ed, 0);
    YEW_ASSERT(ed.pane_root == root0);
    YEW_ASSERT(ed.focus == yew_tab_at(&ed, 0)->focus);
    yew_ed_free(&ed);
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
    for (i = 0U; i < yew_tab_count(&ed); i++) {
        const Tab *t = yew_tab_at(&ed, (int)i);

        YEW_ASSERT_EQ_U64(t->group_id, 0U);
        YEW_ASSERT_EQ_U64(t->group_ordinal, 0U);
    }
    yew_ed_free(&ed);
}

/* ---------------------------------------------------------------- */
/* Commands and the dirty-close prompt (§5/§6)                      */
/* ---------------------------------------------------------------- */

static CmdStatus tb_invoke(Ed *ed, const char *name, i64 iarg,
                           const char *sarg)
{
    CmdId id = yew_cmd_lookup(name, (u32)strlen(name));
    CmdCtx cx;

    YEW_ASSERT(id.v != 0U);
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.iarg = iarg;
    cx.sarg = sarg;
    cx.sarg_len = sarg == NULL ? 0U : (u32)strlen(sarg);
    cx.source = YEW_SRC_TEST;
    return yew_ed_invoke(ed, id, &cx);
}

/* DoD 8: 0 reaches tab 10, and out of range messages and stays put. */
void test_tabs_goto_maps_zero_to_ten_and_refuses_out_of_range(void)
{
    Ed ed;
    u32 ids[9];

    tb_fixture(&ed);
    tb_open_many(&ed, 9U, ids); /* 10 tabs total */
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 10U);

    YEW_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.goto", 1, NULL), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 0);
    YEW_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.goto", 3, NULL), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    /* 0 is the tenth key on the digit row, so it means tab 10. */
    YEW_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.goto", 0, NULL), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 9);

    /* Out of range: refused, and the active tab does not move. */
    YEW_ASSERT(tb_invoke(&ed, "ed.tab.goto", 99, NULL) != YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 9);
    yew_ed_free(&ed);
}

void test_tabs_next_and_prev_are_cyclic(void)
{
    Ed ed;
    u32 ids[2];

    tb_fixture(&ed);
    tb_open_many(&ed, 2U, ids);
    yew_tab_switch(&ed, 0);

    YEW_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.next", 0, NULL), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 1);
    YEW_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.next", 0, NULL), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    /* Round the ring rather than stopping at the end. */
    YEW_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.next", 0, NULL), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 0);
    YEW_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.prev", 0, NULL), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    yew_ed_free(&ed);
}

void test_tabs_move_reorders_the_active_tab(void)
{
    Ed ed;
    u32 ids[3];
    u32 active_id;

    tb_fixture(&ed);
    tb_open_many(&ed, 3U, ids);
    yew_tab_switch(&ed, 0);
    active_id = yew_tab_at(&ed, 0)->tab_id;

    YEW_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.move", 3, NULL), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);
    YEW_ASSERT_EQ_U64(yew_tab_at(&ed, 2)->tab_id, active_id);
    YEW_ASSERT(tb_invoke(&ed, "ed.tab.move", 99, NULL) != YEW_CMD_OK);
    yew_ed_free(&ed);
}

/* A clean tab closes without a question. */
void test_tabs_close_command_is_silent_when_clean(void)
{
    Ed ed;
    u32 ids[1];

    tb_fixture(&ed);
    tb_open_many(&ed, 1U, ids);
    yew_tab_switch(&ed, 1);
    YEW_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.close", 0, NULL),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 1U);
    YEW_ASSERT(!ed.tab_prompt.active);
    /* The last tab refuses to close. */
    YEW_ASSERT(tb_invoke(&ed, "ed.tab.close", 0, NULL) != YEW_CMD_OK);
    yew_ed_free(&ed);
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
    yew_tab_switch(&ed, 2);
    /* Dirty the active tab so closing it asks. */
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U));
    yew_ed_finish_edit(&ed, &ec);
    YEW_ASSERT(yew_tab_modified(&ed, 2));

    YEW_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.close", 0, NULL),
                      YEW_CMD_OK);
    YEW_ASSERT(ed.tab_prompt.active);
    asked_id = ed.tab_prompt.tab_id;
    YEW_ASSERT_EQ_U64(asked_id, yew_tab_at(&ed, 2)->tab_id);

    /* Something else closes a LOWER tab while the question is up, so
     * index 2 now names a different tab entirely. */
    other_id = yew_tab_at(&ed, 1)->tab_id;
    YEW_ASSERT(yew_tab_close(&ed, 1));
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, other_id), -1);
    /* The question still refers to the tab it asked about, now at 1. */
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, asked_id), 1);

    /* Discard answers for THAT tab, not for whatever index 2 became. */
    YEW_ASSERT(yew_tab_prompt_key(&ed, (u8)'d'));
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, asked_id), -1);
    YEW_ASSERT(!ed.tab_prompt.active);
    yew_ed_free(&ed);
}

void test_tabs_dirty_prompt_esc_cancels_the_close(void)
{
    Ed ed;
    u32 ids[1];
    EditCtx ec;
    u32 id;

    tb_fixture(&ed);
    tb_open_many(&ed, 1U, ids);
    yew_tab_switch(&ed, 1);
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U));
    yew_ed_finish_edit(&ed, &ec);
    id = yew_tab_at(&ed, 1)->tab_id;

    YEW_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.close", 0, NULL),
                      YEW_CMD_OK);
    YEW_ASSERT(ed.tab_prompt.active);
    YEW_ASSERT(yew_tab_prompt_key(&ed, 0x1BU));
    /* Cancelled: the tab is still there, with its text. */
    YEW_ASSERT(!ed.tab_prompt.active);
    YEW_ASSERT_EQ_I64(yew_tab_index_of_id(&ed, id), 1);
    YEW_ASSERT(yew_tab_modified(&ed, 1));
    yew_ed_free(&ed);
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
    yew_tab_switch(&ed, 1);
    ec = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U));
    yew_ed_finish_edit(&ed, &ec);

    YEW_ASSERT_EQ_U64(tb_invoke(&ed, "ed.tab.close", 0, NULL),
                      YEW_CMD_OK);
    /* `j` would normally move the cursor; here it is consumed. */
    YEW_ASSERT(yew_tab_prompt_key(&ed, (u8)'j'));
    YEW_ASSERT(ed.tab_prompt.active);
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), 2U);
    yew_ed_free(&ed);
}

/*
 * The sign convention, now live.
 *
 * Sprint 23 asserted that a negative YEW_REGION_TAB payload was a bug,
 * because no renderer could produce one yet.  Sprint 24's row-1
 * renderer produces exactly that for a group, and the router reads the
 * same rule region.h wrote down — so the assertion flips from "this
 * cannot happen" to "this means enter the group".
 */
void test_tabs_negative_region_payload_enters_the_group(void)
{
    Ed ed;
    u32 g;

    tb_fixture(&ed);
    {
        u32 ids[3];

        tb_open_many(&ed, 3U, ids);
    }
    g = yew_group_create(&ed, "/src", NULL);
    yew_group_add_member(&ed, g, 2);
    yew_group_add_member(&ed, g, 3);
    yew_tab_switch(&ed, 0);

    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){0U, 0U, 8U, 1U}, -(i32)g);
    YEW_ASSERT(yew_tab_strip_click(&ed, 1U, 0U));
    /* Entered the group, landing on its first member. */
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), g);
    YEW_ASSERT_EQ_I64(ed.tabs.active, 2);

    /* A positive payload still means a tab index, unchanged. */
    yew_region_frame_begin();
    yew_region_add(YEW_REGION_TAB, (Rect){0U, 0U, 8U, 1U}, 1);
    YEW_ASSERT(yew_tab_strip_click(&ed, 1U, 0U));
    YEW_ASSERT_EQ_I64(ed.tabs.active, 1);
    yew_ed_free(&ed);
}
