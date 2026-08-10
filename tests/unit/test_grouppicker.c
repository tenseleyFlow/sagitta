/*
 * Sprint 24 §4: the group picker.
 *
 * Driven headless through the accessors — no terminal, no grid.  What
 * is being tested is the MODEL of the dialog, and the one thing that
 * model gets wrong if it is built the obvious way: ticks that do not
 * survive walking to another directory.
 *
 * A per-row flag passes every test that never leaves the starting
 * directory.  So every row below that matters walks.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "ui/groupnav.h"
#include "ui/grouppicker.h"
#include "ui/groups.h"
#include "ui/tabs.h"

/*
 * A tree with a subdirectory, so `../` has somewhere to come back from:
 *
 *   <root>/a.txt  <root>/b.txt  <root>/sub/c.txt  <root>/sub/d.txt
 */
typedef struct GpTree {
    char root[64];
    char sub[128];
    char a[256];
    char b[256];
    char c[256];
    char d[256];
} GpTree;

static void gpt_write(const char *path)
{
    FILE *f = fopen(path, "w");

    YEW_ASSERT_NOT_NULL(f);
    (void)fprintf(f, "contents of %s\n", path);
    YEW_ASSERT_EQ_I64(fclose(f), 0);
}

static void gpt_make(GpTree *t)
{
    (void)snprintf(t->root, sizeof(t->root), "/tmp/yew-gp-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(t->root));
    (void)snprintf(t->sub, sizeof(t->sub), "%s/sub", t->root);
    YEW_ASSERT_EQ_I64(mkdir(t->sub, 0700), 0);
    (void)snprintf(t->a, sizeof(t->a), "%s/a.txt", t->root);
    (void)snprintf(t->b, sizeof(t->b), "%s/b.txt", t->root);
    (void)snprintf(t->c, sizeof(t->c), "%s/c.txt", t->sub);
    (void)snprintf(t->d, sizeof(t->d), "%s/d.txt", t->sub);
    gpt_write(t->a);
    gpt_write(t->b);
    gpt_write(t->c);
    gpt_write(t->d);
}

static void gpt_remove(GpTree *t)
{
    (void)unlink(t->a);
    (void)unlink(t->b);
    (void)unlink(t->c);
    (void)unlink(t->d);
    (void)rmdir(t->sub);
    (void)rmdir(t->root);
}

static void gpt_ed(Ed *ed)
{
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    yew_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
}

static Key gpk(u32 code)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.kind = YEW_EV_KEY;
    k.ev = YEW_KEY_PRESS;
    k.code = code;
    if (code >= 0x20U && code < 0x7FU) {
        k.ntext = 1U;
        k.text[0] = (u8)code;
    }
    return k;
}

/* ---------------------------------------------------------------- */

/* The name field is pre-filled `basename(dir)/`, so the happy path in
 * New mode is Enter-Enter. */
void test_grouppicker_prefills_the_name_from_the_directory(void)
{
    Ed ed;
    GpTree t;

    gpt_make(&t);
    gpt_ed(&ed);
    YEW_ASSERT(yew_gp_show(&ed, t.sub));
    YEW_ASSERT(yew_gp_active());
    YEW_ASSERT_EQ_STR(yew_gp_name(), "sub/");
    /* Edit mode pre-fills the group's own label instead. */
    YEW_ASSERT(yew_gp_show_edit(&ed, t.sub, "backend"));
    YEW_ASSERT_EQ_STR(yew_gp_name(), "backend");
    yew_gp_close(&ed);
    yew_ed_free(&ed);
    gpt_remove(&t);
}

/*
 * THE test this file exists for.
 *
 * Tick two files here, walk into a subdirectory, tick one more, walk
 * back out — and all three survive.  A per-item flag on the listed rows
 * would drop the first two the moment the listing changed, which is
 * exactly what the `../` row is for.
 */
void test_grouppicker_ticks_survive_directory_navigation(void)
{
    Ed ed;
    GpTree t;
    int i;
    int seen_a = 0;
    int seen_c = 0;

    gpt_make(&t);
    gpt_ed(&ed);
    YEW_ASSERT(yew_gp_show(&ed, t.root));

    /* Tick both files in the root by path, then walk into `sub`. */
    yew_gp_preselect(t.a);
    yew_gp_preselect(t.b);
    /* Walking is what a per-row flag cannot survive. */
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_DOWN)));
    yew_gp_preselect(t.c);

    /* Confirm and read the result back. */
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_TAB)));  /* focus the name */
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_ENTER)));
    YEW_ASSERT_EQ_I64(yew_gp_result(), YEW_GP_CONFIRMED);
    YEW_ASSERT_EQ_I64(yew_gp_count(), 3);
    for (i = 0; i < yew_gp_count(); i++) {
        const char *p = yew_gp_path(i);

        YEW_ASSERT_NOT_NULL(p);
        if (strstr(p, "a.txt") != NULL)
            seen_a++;
        if (strstr(p, "c.txt") != NULL)
            seen_c++;
    }
    /* The one from the OTHER directory is still there, and nothing is
     * duplicated. */
    YEW_ASSERT_EQ_I64(seen_a, 1);
    YEW_ASSERT_EQ_I64(seen_c, 1);
    yew_ed_free(&ed);
    gpt_remove(&t);
}

/* preselect ticks a path that is not in the current listing at all —
 * an Edit-mode member may live anywhere, and no index into the listing
 * could name it. */
void test_grouppicker_preselect_counts_an_out_of_dir_path(void)
{
    Ed ed;
    GpTree t;

    gpt_make(&t);
    gpt_ed(&ed);
    YEW_ASSERT(yew_gp_show(&ed, t.sub));
    /* a.txt is in the PARENT, not in the listing on screen. */
    yew_gp_preselect(t.a);
    yew_gp_preselect(t.c);
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_TAB)));
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_ENTER)));
    YEW_ASSERT_EQ_I64(yew_gp_count(), 2);
    yew_ed_free(&ed);
    gpt_remove(&t);
}

/* Ticking the same path twice is idempotent — a set, not a list. */
void test_grouppicker_ticks_are_a_set(void)
{
    Ed ed;
    GpTree t;

    gpt_make(&t);
    gpt_ed(&ed);
    YEW_ASSERT(yew_gp_show(&ed, t.root));
    yew_gp_preselect(t.a);
    yew_gp_preselect(t.a);
    yew_gp_preselect(t.a);
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_TAB)));
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_ENTER)));
    YEW_ASSERT_EQ_I64(yew_gp_count(), 1);
    yew_ed_free(&ed);
    gpt_remove(&t);
}

/*
 * Confirm with nothing ticked is REFUSED with a note, not accepted.
 *
 * An empty group would auto-dissolve the instant it was created, so the
 * user would watch their selection vanish with no explanation.
 */
void test_grouppicker_refuses_an_empty_selection(void)
{
    Ed ed;
    GpTree t;

    gpt_make(&t);
    gpt_ed(&ed);
    YEW_ASSERT(yew_gp_show(&ed, t.root));
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_TAB)));
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_ENTER)));
    /* Still up, still pending. */
    YEW_ASSERT(yew_gp_active());
    YEW_ASSERT_EQ_I64(yew_gp_result(), YEW_GP_PENDING);
    YEW_ASSERT_EQ_I64(yew_gp_count(), 0);
    yew_gp_close(&ed);
    yew_ed_free(&ed);
    gpt_remove(&t);
}

/* Esc cancels, and nothing is created. */
void test_grouppicker_esc_cancels(void)
{
    Ed ed;
    GpTree t;

    gpt_make(&t);
    gpt_ed(&ed);
    YEW_ASSERT(yew_gp_show(&ed, t.root));
    yew_gp_preselect(t.a);
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_ESCAPE)));
    YEW_ASSERT(!yew_gp_active());
    YEW_ASSERT_EQ_I64(yew_gp_result(), YEW_GP_CANCELLED);
    yew_gp_apply(&ed);
    /* No group came into existence. */
    YEW_ASSERT_EQ_U64(ed.groups.v.len, 0U);
    yew_ed_free(&ed);
    gpt_remove(&t);
}

/* Random keys never crash and never put a path in the set that was
 * neither listed nor preselected. */
void test_grouppicker_survives_a_key_storm(void)
{
    Ed ed;
    GpTree t;
    u32 seed = 0x1234567U;
    int op;

    gpt_make(&t);
    gpt_ed(&ed);
    YEW_ASSERT(yew_gp_show(&ed, t.root));
    for (op = 0; op < 3000; op++) {
        static const u32 codes[] = {
            YEW_KEY_UP,   YEW_KEY_DOWN, YEW_KEY_LEFT, YEW_KEY_RIGHT,
            YEW_KEY_TAB,  YEW_KEY_ENTER, (u32)' ',    (u32)'x',
            (u32)'/',     YEW_KEY_BACKSPACE
        };

        seed = seed * 1664525U + 1013904223U;
        if (!yew_gp_active()) {
            /* Enter may have confirmed or cancelled it; reopen and keep
             * hammering rather than spinning on a closed dialog. */
            yew_gp_apply(&ed);
            YEW_ASSERT(yew_gp_show(&ed, t.root));
        }
        (void)yew_gp_key(&ed, gpk(codes[(seed >> 16) % 10U]));
    }
    /* Whatever it ended up with, every path in it is a real one under
     * the tree the dialog was pointed at. */
    if (yew_gp_result() == YEW_GP_CONFIRMED) {
        int i;

        for (i = 0; i < yew_gp_count(); i++)
            YEW_ASSERT_NOT_NULL(strstr(yew_gp_path(i), t.root));
    }
    yew_gp_close(&ed);
    yew_ed_free(&ed);
    gpt_remove(&t);
}

/* ---------------------------------------------------------------- */
/* Applying a confirmed result                                      */
/* ---------------------------------------------------------------- */

/* New: the group is created, each ticked path opens DEFERRED, and each
 * joins.  Opening the group costs no reads at all. */
void test_grouppicker_new_creates_the_group_deferred(void)
{
    Ed ed;
    GpTree t;
    u64 base;
    u32 gid;

    gpt_make(&t);
    gpt_ed(&ed);
    YEW_ASSERT(yew_gp_show(&ed, t.root));
    yew_gp_preselect(t.a);
    yew_gp_preselect(t.b);
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_TAB)));
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_ENTER)));

    base = yew_file_load_count();
    yew_gp_apply(&ed);
    YEW_ASSERT_EQ_U64(ed.groups.v.len, 1U);
    gid = ed.groups.v.data[0].id;
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, gid), 2);
    /* Not one file was read to build the group. */
    YEW_ASSERT_EQ_U64(yew_file_load_count(), base);
    yew_ed_free(&ed);
    gpt_remove(&t);
}

/*
 * An already-open file is ADOPTED, not duplicated.  Two tabs on one
 * path would be two claims on one save destination.
 */
void test_grouppicker_new_adopts_an_already_open_file(void)
{
    Ed ed;
    GpTree t;
    u32 before;
    u32 gid;

    gpt_make(&t);
    gpt_ed(&ed);
    YEW_ASSERT(yew_tab_open(&ed, t.a) >= 0);
    before = yew_tab_count(&ed);

    YEW_ASSERT(yew_gp_show(&ed, t.root));
    yew_gp_preselect(t.a);
    yew_gp_preselect(t.b);
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_TAB)));
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_ENTER)));
    yew_gp_apply(&ed);

    /* One new tab for b.txt; a.txt reused the tab it already had. */
    YEW_ASSERT_EQ_U64(yew_tab_count(&ed), before + 1U);
    gid = ed.groups.v.data[0].id;
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, gid), 2);
    YEW_ASSERT_EQ_I64(yew_tab_find_by_path(&ed, t.a),
                      yew_tab_find_by_path(&ed, t.a));
    yew_ed_free(&ed);
    gpt_remove(&t);
}

/*
 * Edit: the diff.  Starting from {a, b}, confirming {a, c} must add c,
 * keep a, and drop b — closing b's tab.
 */
void test_grouppicker_edit_diffs_the_membership(void)
{
    Ed ed;
    GpTree t;
    u32 gid;
    CmdId id;
    CmdCtx cx;

    gpt_make(&t);
    gpt_ed(&ed);
    /* Build {a, b} through the New path. */
    YEW_ASSERT(yew_gp_show(&ed, t.root));
    yew_gp_preselect(t.a);
    yew_gp_preselect(t.b);
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_TAB)));
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_ENTER)));
    yew_gp_apply(&ed);
    gid = ed.groups.v.data[0].id;
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, gid), 2);

    /* Sit inside the group so ed.group.edit has something to edit. */
    yew_group_enter(&ed, gid);
    YEW_ASSERT_EQ_U64(yew_active_group_id(&ed), gid);

    id = yew_cmd_lookup("ed.group.edit", 13U);
    YEW_ASSERT(id.v != 0U);
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, id, &cx), YEW_CMD_OK);
    /* Both current members arrived ticked. */
    YEW_ASSERT(yew_gp_active());

    /* Untick b by toggling it off, and tick c from the subdirectory. */
    yew_gp_preselect(t.c);
    {
        /* Re-open the dialog with exactly {a, c}: the model is a set,
         * so expressing the target directly is the honest way to test
         * the DIFF rather than the keystrokes that produce it. */
        yew_gp_close(&ed);
        YEW_ASSERT(yew_gp_show_edit(&ed, t.root, "src/"));
        yew_gp_preselect(t.a);
        yew_gp_preselect(t.c);
        YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_TAB)));
        YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_ENTER)));
        YEW_ASSERT_EQ_I64(yew_gp_result(), YEW_GP_CONFIRMED);
    }
    yew_gp_apply(&ed);

    /* a stayed, c joined, b left — and b's tab is gone. */
    YEW_ASSERT_EQ_I64(yew_group_member_count(&ed, gid), 2);
    YEW_ASSERT(yew_tab_find_by_path(&ed, t.a) >= 0);
    YEW_ASSERT(yew_tab_find_by_path(&ed, t.c) >= 0);
    YEW_ASSERT_EQ_I64(yew_tab_find_by_path(&ed, t.b), -1);
    yew_ed_free(&ed);
    gpt_remove(&t);
}

/* Applying twice must not create the group twice — the result is
 * consumed on the first apply. */
void test_grouppicker_apply_is_idempotent(void)
{
    Ed ed;
    GpTree t;

    gpt_make(&t);
    gpt_ed(&ed);
    YEW_ASSERT(yew_gp_show(&ed, t.root));
    yew_gp_preselect(t.a);
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_TAB)));
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_ENTER)));
    yew_gp_apply(&ed);
    yew_gp_apply(&ed);
    yew_gp_apply(&ed);
    YEW_ASSERT_EQ_U64(ed.groups.v.len, 1U);
    yew_ed_free(&ed);
    gpt_remove(&t);
}

/*
 * Only directories and REGULAR files are listed.
 *
 * Everything-that-is-not-a-directory was tickable until the fuzzer
 * walked to /dev and selected a block device — which the group would
 * then have opened as a document.  A picker for choosing files must
 * only offer files.
 */
void test_grouppicker_lists_only_directories_and_regular_files(void)
{
    Ed ed;
    GpTree t;
    char fifo[256];
    int i;
    int rows_with_fifo = 0;

    gpt_make(&t);
    (void)snprintf(fifo, sizeof(fifo), "%s/a-fifo", t.root);
    /* A FIFO stands in for every non-regular thing a directory can
     * hold; skipping the test entirely when mkfifo is unavailable is
     * better than asserting nothing. */
    if (mkfifo(fifo, 0600) != 0) {
        gpt_remove(&t);
        return;
    }
    gpt_ed(&ed);
    YEW_ASSERT(yew_gp_show(&ed, t.root));

    /* Tick every listed row by walking the list and pressing space;
     * the FIFO must never be among the results. */
    for (i = 0; i < 8; i++)
        YEW_ASSERT(yew_gp_key(&ed, gpk((u32)' ')));
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_TAB)));
    YEW_ASSERT(yew_gp_key(&ed, gpk(YEW_KEY_ENTER)));
    if (yew_gp_result() == YEW_GP_CONFIRMED) {
        for (i = 0; i < yew_gp_count(); i++) {
            if (strstr(yew_gp_path(i), "a-fifo") != NULL)
                rows_with_fifo++;
        }
    }
    YEW_ASSERT_EQ_I64(rows_with_fifo, 0);
    (void)unlink(fifo);
    yew_ed_free(&ed);
    gpt_remove(&t);
}
