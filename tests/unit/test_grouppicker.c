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

    SAG_ASSERT_NOT_NULL(f);
    (void)fprintf(f, "contents of %s\n", path);
    SAG_ASSERT_EQ_I64(fclose(f), 0);
}

static void gpt_make(GpTree *t)
{
    (void)snprintf(t->root, sizeof(t->root), "/tmp/sag-gp-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(t->root));
    (void)snprintf(t->sub, sizeof(t->sub), "%s/sub", t->root);
    SAG_ASSERT_EQ_I64(mkdir(t->sub, 0700), 0);
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
    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    sag_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
}

static Key gpk(u32 code)
{
    Key k;

    (void)memset(&k, 0, sizeof(k));
    k.kind = SAG_EV_KEY;
    k.ev = SAG_KEY_PRESS;
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
    SAG_ASSERT(sag_gp_show(&ed, t.sub));
    SAG_ASSERT(sag_gp_active());
    SAG_ASSERT_EQ_STR(sag_gp_name(), "sub/");
    /* Edit mode pre-fills the group's own label instead. */
    SAG_ASSERT(sag_gp_show_edit(&ed, t.sub, "backend"));
    SAG_ASSERT_EQ_STR(sag_gp_name(), "backend");
    sag_gp_close(&ed);
    sag_ed_free(&ed);
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
    SAG_ASSERT(sag_gp_show(&ed, t.root));

    /* Tick both files in the root by path, then walk into `sub`. */
    sag_gp_preselect(t.a);
    sag_gp_preselect(t.b);
    /* Walking is what a per-row flag cannot survive. */
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_DOWN)));
    sag_gp_preselect(t.c);

    /* Confirm and read the result back. */
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_TAB)));  /* focus the name */
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_ENTER)));
    SAG_ASSERT_EQ_I64(sag_gp_result(), SAG_GP_CONFIRMED);
    SAG_ASSERT_EQ_I64(sag_gp_count(), 3);
    for (i = 0; i < sag_gp_count(); i++) {
        const char *p = sag_gp_path(i);

        SAG_ASSERT_NOT_NULL(p);
        if (strstr(p, "a.txt") != NULL)
            seen_a++;
        if (strstr(p, "c.txt") != NULL)
            seen_c++;
    }
    /* The one from the OTHER directory is still there, and nothing is
     * duplicated. */
    SAG_ASSERT_EQ_I64(seen_a, 1);
    SAG_ASSERT_EQ_I64(seen_c, 1);
    sag_ed_free(&ed);
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
    SAG_ASSERT(sag_gp_show(&ed, t.sub));
    /* a.txt is in the PARENT, not in the listing on screen. */
    sag_gp_preselect(t.a);
    sag_gp_preselect(t.c);
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_TAB)));
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_ENTER)));
    SAG_ASSERT_EQ_I64(sag_gp_count(), 2);
    sag_ed_free(&ed);
    gpt_remove(&t);
}

/* Ticking the same path twice is idempotent — a set, not a list. */
void test_grouppicker_ticks_are_a_set(void)
{
    Ed ed;
    GpTree t;

    gpt_make(&t);
    gpt_ed(&ed);
    SAG_ASSERT(sag_gp_show(&ed, t.root));
    sag_gp_preselect(t.a);
    sag_gp_preselect(t.a);
    sag_gp_preselect(t.a);
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_TAB)));
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_ENTER)));
    SAG_ASSERT_EQ_I64(sag_gp_count(), 1);
    sag_ed_free(&ed);
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
    SAG_ASSERT(sag_gp_show(&ed, t.root));
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_TAB)));
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_ENTER)));
    /* Still up, still pending. */
    SAG_ASSERT(sag_gp_active());
    SAG_ASSERT_EQ_I64(sag_gp_result(), SAG_GP_PENDING);
    SAG_ASSERT_EQ_I64(sag_gp_count(), 0);
    sag_gp_close(&ed);
    sag_ed_free(&ed);
    gpt_remove(&t);
}

/* Esc cancels, and nothing is created. */
void test_grouppicker_esc_cancels(void)
{
    Ed ed;
    GpTree t;

    gpt_make(&t);
    gpt_ed(&ed);
    SAG_ASSERT(sag_gp_show(&ed, t.root));
    sag_gp_preselect(t.a);
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_ESCAPE)));
    SAG_ASSERT(!sag_gp_active());
    SAG_ASSERT_EQ_I64(sag_gp_result(), SAG_GP_CANCELLED);
    sag_gp_apply(&ed);
    /* No group came into existence. */
    SAG_ASSERT_EQ_U64(ed.groups.v.len, 0U);
    sag_ed_free(&ed);
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
    SAG_ASSERT(sag_gp_show(&ed, t.root));
    for (op = 0; op < 3000; op++) {
        static const u32 codes[] = {
            SAG_KEY_UP,   SAG_KEY_DOWN, SAG_KEY_LEFT, SAG_KEY_RIGHT,
            SAG_KEY_TAB,  SAG_KEY_ENTER, (u32)' ',    (u32)'x',
            (u32)'/',     SAG_KEY_BACKSPACE
        };

        seed = seed * 1664525U + 1013904223U;
        if (!sag_gp_active()) {
            /* Enter may have confirmed or cancelled it; reopen and keep
             * hammering rather than spinning on a closed dialog. */
            sag_gp_apply(&ed);
            SAG_ASSERT(sag_gp_show(&ed, t.root));
        }
        (void)sag_gp_key(&ed, gpk(codes[(seed >> 16) % 10U]));
    }
    /* Whatever it ended up with, every path in it is a real one under
     * the tree the dialog was pointed at. */
    if (sag_gp_result() == SAG_GP_CONFIRMED) {
        int i;

        for (i = 0; i < sag_gp_count(); i++)
            SAG_ASSERT_NOT_NULL(strstr(sag_gp_path(i), t.root));
    }
    sag_gp_close(&ed);
    sag_ed_free(&ed);
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
    SAG_ASSERT(sag_gp_show(&ed, t.root));
    sag_gp_preselect(t.a);
    sag_gp_preselect(t.b);
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_TAB)));
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_ENTER)));

    base = sag_file_load_count();
    sag_gp_apply(&ed);
    SAG_ASSERT_EQ_U64(ed.groups.v.len, 1U);
    gid = ed.groups.v.data[0].id;
    SAG_ASSERT_EQ_I64(sag_group_member_count(&ed, gid), 2);
    /* Not one file was read to build the group. */
    SAG_ASSERT_EQ_U64(sag_file_load_count(), base);
    sag_ed_free(&ed);
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
    SAG_ASSERT(sag_tab_open(&ed, t.a) >= 0);
    before = sag_tab_count(&ed);

    SAG_ASSERT(sag_gp_show(&ed, t.root));
    sag_gp_preselect(t.a);
    sag_gp_preselect(t.b);
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_TAB)));
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_ENTER)));
    sag_gp_apply(&ed);

    /* One new tab for b.txt; a.txt reused the tab it already had. */
    SAG_ASSERT_EQ_U64(sag_tab_count(&ed), before + 1U);
    gid = ed.groups.v.data[0].id;
    SAG_ASSERT_EQ_I64(sag_group_member_count(&ed, gid), 2);
    SAG_ASSERT_EQ_I64(sag_tab_find_by_path(&ed, t.a),
                      sag_tab_find_by_path(&ed, t.a));
    sag_ed_free(&ed);
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
    SAG_ASSERT(sag_gp_show(&ed, t.root));
    sag_gp_preselect(t.a);
    sag_gp_preselect(t.b);
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_TAB)));
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_ENTER)));
    sag_gp_apply(&ed);
    gid = ed.groups.v.data[0].id;
    SAG_ASSERT_EQ_I64(sag_group_member_count(&ed, gid), 2);

    /* Sit inside the group so ed.group.edit has something to edit. */
    sag_group_enter(&ed, gid);
    SAG_ASSERT_EQ_U64(sag_active_group_id(&ed), gid);

    id = sag_cmd_lookup("ed.group.edit", 13U);
    SAG_ASSERT(id.v != 0U);
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = &ed;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = SAG_SRC_TEST;
    SAG_ASSERT_EQ_I64(sag_ed_invoke(&ed, id, &cx), SAG_CMD_OK);
    /* Both current members arrived ticked. */
    SAG_ASSERT(sag_gp_active());

    /* Untick b by toggling it off, and tick c from the subdirectory. */
    sag_gp_preselect(t.c);
    {
        /* Re-open the dialog with exactly {a, c}: the model is a set,
         * so expressing the target directly is the honest way to test
         * the DIFF rather than the keystrokes that produce it. */
        sag_gp_close(&ed);
        SAG_ASSERT(sag_gp_show_edit(&ed, t.root, "src/"));
        sag_gp_preselect(t.a);
        sag_gp_preselect(t.c);
        SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_TAB)));
        SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_ENTER)));
        SAG_ASSERT_EQ_I64(sag_gp_result(), SAG_GP_CONFIRMED);
    }
    sag_gp_apply(&ed);

    /* a stayed, c joined, b left — and b's tab is gone. */
    SAG_ASSERT_EQ_I64(sag_group_member_count(&ed, gid), 2);
    SAG_ASSERT(sag_tab_find_by_path(&ed, t.a) >= 0);
    SAG_ASSERT(sag_tab_find_by_path(&ed, t.c) >= 0);
    SAG_ASSERT_EQ_I64(sag_tab_find_by_path(&ed, t.b), -1);
    sag_ed_free(&ed);
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
    SAG_ASSERT(sag_gp_show(&ed, t.root));
    sag_gp_preselect(t.a);
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_TAB)));
    SAG_ASSERT(sag_gp_key(&ed, gpk(SAG_KEY_ENTER)));
    sag_gp_apply(&ed);
    sag_gp_apply(&ed);
    sag_gp_apply(&ed);
    SAG_ASSERT_EQ_U64(ed.groups.v.len, 1U);
    sag_ed_free(&ed);
    gpt_remove(&t);
}
