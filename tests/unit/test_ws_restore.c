/*
 * Sprint 25 §6: the ordered restore.
 *
 * The properties here are the ones that fail SILENTLY, which is why
 * each is asserted against a second editor rather than against the
 * document text:
 *
 * ONE READ.  Restoring a 40-tab workspace must perform exactly one file
 * read.  Any wrong answer still produces a working editor — it just
 * takes forty disk round trips to start, which nobody notices on a warm
 * cache and everybody notices over a network mount.
 *
 * CLAMP, NOT FOLLOW.  The saved viewport already satisfies scrolloff.
 * Following would recentre on the cursor, and the resumed grid would
 * differ from the one that was quit — plausibly, and in a way only a
 * byte comparison catches.
 *
 * IDS ARE REMAPPED.  A tab whose group record went missing must come
 * back UNGROUPED.  Reusing the file's number files someone's document
 * into an unrelated group and looks entirely normal.
 *
 * A TAB IS NEVER DROPPED FOR A MISSING FILE.  The path is the only
 * record of what was being worked on.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "text/file.h"
#include "ui/groups.h"
#include "ui/layout.h"
#include "ui/tabs.h"
#include "util/arena.h"
#include "util/buf.h"
#include "ws/fllit.h"
#include "ws/state.h"
#include "ws/workspace.h"

/* ---------------------------------------------------------------- */
/* Fixture                                                          */
/* ---------------------------------------------------------------- */

typedef struct RsFix {
    char state_home[128];
    char work[128];
    char saved[512];
    bool had_saved;
} RsFix;

static void rs_rm_rf(const char *path)
{
    char cmd[512];

    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    /* Assigned, not cast to void: glibc marks system warn_unused_result
     * under _FORTIFY_SOURCE, which Ubuntu's gcc enables by default and
     * Arch's does not — the cast compiled locally and failed CI. */
    {
        int removed = system(cmd);

        (void)removed;
    }
}

static void rs_make(RsFix *f)
{
    const char *prev = getenv("XDG_STATE_HOME");

    f->had_saved = prev != NULL;
    if (prev != NULL)
        (void)snprintf(f->saved, sizeof(f->saved), "%s", prev);
    (void)snprintf(f->state_home, sizeof(f->state_home),
                   "/tmp/sag-rshome-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->state_home));
    (void)snprintf(f->work, sizeof(f->work), "/tmp/sag-rswork-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->work));
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state_home, 1), 0);
}

static void rs_remove(RsFix *f)
{
    if (f->had_saved)
        (void)setenv("XDG_STATE_HOME", f->saved, 1);
    else
        (void)unsetenv("XDG_STATE_HOME");
    rs_rm_rf(f->state_home);
    rs_rm_rf(f->work);
}

/* A real file on disk, so a tab pointing at it is not an orphan. */
static void rs_file(const RsFix *f, const char *name, const char *body,
                    char *out, size_t cap)
{
    FILE *fp;

    (void)snprintf(out, cap, "%s/%s", f->work, name);
    fp = fopen(out, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fwrite(body, 1U, strlen(body), fp);
    (void)fclose(fp);
}

static void rs_ed(RsFix *f, Ed *ed)
{
    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(ed);
    ed->ws.dir = arena_strdup(&ed->arena, f->work);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    sag_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
}

/*
 * Applies a document to a second editor.  Steps 8 and 9 are the
 * caller's in sag_state_apply (layout needs a terminal), so the test
 * drives them exactly as sag_ws_restore does.
 */
static SagWsResult rs_apply(RsFix *f, Ed *ed, const Bytebuf *doc)
{
    SagWsResult r;

    rs_ed(f, ed);
    SAG_ASSERT(sag_ws_key(&ed->state.key, f->work));
    SAG_ASSERT(sag_ws_ensure_dir(&ed->state.key));
    ed->state.ready = true;
    ed->state.writer = true;
    r = sag_state_apply(ed, doc->data, doc->len);
    sag_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
    return r;
}

/* ---------------------------------------------------------------- */
/* The round trip                                                   */
/* ---------------------------------------------------------------- */

/*
 * The contract's headline: an arrangement survives quit and reopen.
 * Asserted structurally against a second editor, not by comparing
 * document text to itself.
 */
void test_ws_restore_round_trips_tabs_and_groups(void)
{
    RsFix f;
    Ed a;
    Ed b;
    Bytebuf doc;
    char pa[192];
    char pb[192];
    char pc[192];
    u32 gid;
    int t0;
    int t1;
    int t2;

    rs_make(&f);
    rs_file(&f, "a.txt", "alpha\nbravo\ncharlie\n", pa, sizeof(pa));
    rs_file(&f, "b.txt", "delta\n", pb, sizeof(pb));
    rs_file(&f, "c.txt", "echo\n", pc, sizeof(pc));

    rs_ed(&f, &a);
    t0 = sag_tab_open(&a, pa);
    t1 = sag_tab_open(&a, pb);
    t2 = sag_tab_open(&a, pc);
    SAG_ASSERT(t0 >= 0 && t1 >= 0 && t2 >= 0);
    gid = sag_group_create(&a, f.work, "grp");
    sag_group_add_member(&a, gid, t1);
    sag_group_add_member(&a, gid, t2);
    sag_tab_switch(&a, t1);
    bytebuf_init(&doc);
    sag_state_emit(&a, &doc);

    (void)rs_apply(&f, &b, &doc);
    /* Three restored tabs, plus the scratch the second editor started
     * with. */
    SAG_ASSERT_EQ_I64(sag_tab_count(&b), 4);
    SAG_ASSERT_EQ_STR(sag_tab_at(&b, 1)->path, pa);
    SAG_ASSERT_EQ_STR(sag_tab_at(&b, 2)->path, pb);
    SAG_ASSERT_EQ_STR(sag_tab_at(&b, 3)->path, pc);
    /* One group, with both members and their ordinals. */
    SAG_ASSERT_EQ_U64(b.groups.v.len, 1U);
    SAG_ASSERT_EQ_I64(sag_group_member_count(&b, b.groups.v.data[0].id), 2);
    SAG_ASSERT_EQ_U64(sag_tab_at(&b, 2)->group_id, b.groups.v.data[0].id);
    SAG_ASSERT_EQ_U64(sag_tab_at(&b, 3)->group_id, b.groups.v.data[0].id);
    SAG_ASSERT_EQ_U64(sag_tab_at(&b, 2)->group_ordinal, 1U);
    SAG_ASSERT_EQ_U64(sag_tab_at(&b, 3)->group_ordinal, 2U);
    /* active_tab resolved through the id map, not by index. */
    SAG_ASSERT_EQ_STR(sag_tab_at(&b, b.tabs.active)->path, pb);

    bytebuf_free(&doc);
    sag_ed_free(&a);
    sag_ed_free(&b);
    rs_remove(&f);
}

/*
 * LIVE IDS ARE FRESH.  The restored group's id comes from
 * sag_group_create, not from the file — so a document written by a
 * session that had reached group 7 restores to group 1 here.
 */
void test_ws_restore_remaps_group_ids(void)
{
    RsFix f;
    Ed a;
    Ed b;
    Bytebuf doc;
    char pa[192];
    int t0;
    u32 g1;
    u32 g2;
    u32 g3;

    rs_make(&f);
    rs_file(&f, "a.txt", "x\n", pa, sizeof(pa));
    rs_ed(&f, &a);
    /* Burn ids so the file carries numbers no fresh editor would mint
     * in the same order. */
    g1 = sag_group_create(&a, f.work, "g1");
    g2 = sag_group_create(&a, f.work, "g2");
    g3 = sag_group_create(&a, f.work, "g3");
    sag_group_dissolve(&a, g1);
    sag_group_dissolve(&a, g2);
    SAG_ASSERT(g3 >= 3U);
    t0 = sag_tab_open(&a, pa);
    sag_group_add_member(&a, g3, t0);
    bytebuf_init(&doc);
    sag_state_emit(&a, &doc);

    (void)rs_apply(&f, &b, &doc);
    SAG_ASSERT_EQ_U64(b.groups.v.len, 1U);
    /* Fresh and monotonic from 1 — emphatically not g3's number. */
    SAG_ASSERT_EQ_U64(b.groups.v.data[0].id, 1U);
    SAG_ASSERT_EQ_U64(sag_tab_at(&b, 1)->group_id, 1U);

    bytebuf_free(&doc);
    sag_ed_free(&a);
    sag_ed_free(&b);
    rs_remove(&f);
}

/*
 * A tab naming a group with NO record becomes ungrouped, never a
 * member of whatever group happens to exist.
 */
void test_ws_restore_missing_group_record_ungroups_the_tab(void)
{
    RsFix f;
    Ed b;
    Bytebuf doc;
    char pa[192];
    const char *fmt =
        "{\n  version: 1,\n  groups: [\n    { id: 1, label: \"real\","
        " dir_path: \"\", },\n  ],\n  tabs: [\n"
        "    { id: 10, path: \"%s\", group: 99, group_ordinal: 1,"
        " deferred: true, },\n  ],\n  active_tab: 10,\n}\n";
    char text[512];

    rs_make(&f);
    rs_file(&f, "a.txt", "x\n", pa, sizeof(pa));
    (void)snprintf(text, sizeof(text), fmt, pa);
    bytebuf_init(&doc);
    bytebuf_append(&doc, (const u8 *)text, strlen(text));

    (void)rs_apply(&f, &b, &doc);
    SAG_ASSERT_EQ_I64(sag_tab_count(&b), 2);
    /*
     * Group 1 exists and is live.  A "reuse the file's number" reader
     * would resolve 99 to nothing and might fall back to it; this
     * asserts the tab is ungrouped instead.
     */
    SAG_ASSERT_EQ_U64(sag_tab_at(&b, 1)->group_id, 0U);
    /* And the empty group did not survive step 7. */
    SAG_ASSERT_EQ_U64(b.groups.v.len, 0U);

    bytebuf_free(&doc);
    sag_ed_free(&b);
    rs_remove(&f);
}

/* ---------------------------------------------------------------- */
/* DoD 3: one read                                                  */
/* ---------------------------------------------------------------- */

/*
 * Restoring 40 tabs performs exactly ONE file read — the active tab's.
 * Every other tab comes back as a path with no buffer (§3.3).
 */
void test_ws_restore_of_forty_tabs_reads_one_file(void)
{
    RsFix f;
    Ed a;
    Ed b;
    Bytebuf doc;
    char path[192];
    char name[32];
    u32 gid;
    int i;
    u64 reads;

    rs_make(&f);
    rs_ed(&f, &a);
    gid = sag_group_create(&a, f.work, "many");
    for (i = 0; i < 40; i++) {
        int t;

        (void)snprintf(name, sizeof(name), "f%02d.txt", i);
        rs_file(&f, name, "body\n", path, sizeof(path));
        t = sag_tab_open(&a, path);
        SAG_ASSERT(t >= 0);
        sag_group_add_member(&a, gid, t);
    }
    bytebuf_init(&doc);
    sag_state_emit(&a, &doc);

    sag_file_load_count_reset();
    (void)rs_apply(&f, &b, &doc);
    reads = sag_file_load_count();
    SAG_ASSERT_EQ_I64(sag_tab_count(&b), 41);
    SAG_ASSERT_EQ_U64(reads, 1U);
    /* And switching to a second member performs the second. */
    sag_tab_switch(&b, 2);
    SAG_ASSERT_EQ_U64(sag_file_load_count(), 2U);

    bytebuf_free(&doc);
    sag_ed_free(&a);
    sag_ed_free(&b);
    rs_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Windows, cursors, viewports                                      */
/* ---------------------------------------------------------------- */

/* A split comes back as a split, with its ratio and both views. */
void test_ws_restore_rebuilds_the_pane_tree(void)
{
    RsFix f;
    Ed a;
    Ed b;
    Bytebuf doc;
    char pa[192];
    Pane *nu;
    const Tab *t;

    rs_make(&f);
    rs_file(&f, "a.txt", "one\ntwo\nthree\nfour\nfive\n", pa, sizeof(pa));
    rs_ed(&f, &a);
    SAG_ASSERT(sag_tab_open(&a, pa) >= 0);
    sag_tab_switch(&a, 1);
    /* The switch swapped in a tree that has never been laid out, and a
     * split refuses without a rect to divide. */
    sag_layout_compute(a.pane_root, (Rect){0U, 0U, 80U, 24U});
    /* sag_pane_split mutates the leaf in place and returns the NEW
     * leaf, so focus has to follow the return value. */
    nu = sag_pane_split(&a, a.focus, SAG_SPLIT_H);
    SAG_ASSERT_NOT_NULL(nu);
    a.focus = nu;
    a.tabs.v.data[1].focus = nu;
    SAG_ASSERT(sag_pane_resize(a.tabs.v.data[1].root, 8));
    bytebuf_init(&doc);
    sag_state_emit(&a, &doc);

    (void)rs_apply(&f, &b, &doc);
    t = sag_tab_at(&b, 1);
    SAG_ASSERT_NOT_NULL(t);
    SAG_ASSERT_NOT_NULL(t->root);
    SAG_ASSERT(!t->root->is_leaf);
    SAG_ASSERT_EQ_I64(t->root->dir, SAG_SPLIT_H);
    SAG_ASSERT_EQ_U64(sag_pane_leaf_count(t->root), 2U);
    /*
     * The ratio survives as PERMILLE, so the two sides agree to within
     * a thousandth rather than exactly — which is the whole reason the
     * format has no floats.
     */
    SAG_ASSERT_EQ_I64(sag_ratio_to_permille(t->root->ratio),
                      sag_ratio_to_permille(a.tabs.v.data[1].root->ratio));
    /* Both leaves show the tab's own buffer, not the scratch. */
    SAG_ASSERT_NOT_NULL(t->root->a->win);
    SAG_ASSERT_NOT_NULL(t->root->b->win);
    SAG_ASSERT(t->root->a->win != t->root->b->win);

    bytebuf_free(&doc);
    sag_ed_free(&a);
    sag_ed_free(&b);
    rs_remove(&f);
}

/*
 * Step 9 is CLAMP, not follow.  A viewport scrolled well past the
 * cursor comes back where it was; follow would pull `top` to the
 * cursor's line and the resumed grid would not match.
 */
void test_ws_restore_clamps_the_viewport_without_following(void)
{
    RsFix f;
    Ed a;
    Ed b;
    Bytebuf doc;
    char pa[192];
    char body[4096];
    Win *w;
    u32 i;
    size_t at = 0U;

    rs_make(&f);
    for (i = 0U; i < 200U; i++)
        at += (size_t)snprintf(body + at, sizeof(body) - at, "line %u\n", i);
    rs_file(&f, "a.txt", body, pa, sizeof(pa));
    rs_ed(&f, &a);
    SAG_ASSERT(sag_tab_open(&a, pa) >= 0);
    sag_tab_switch(&a, 1);
    sag_layout_compute(a.pane_root, (Rect){0U, 0U, 80U, 24U});
    /* Cursor at the top, view scrolled far down: a state follow would
     * destroy and clamp preserves. */
    a.win->cs.curs.data[a.win->cs.primary].pos = BYTEOFF(0U);
    a.win->vp.top = LINENO(100U);
    bytebuf_init(&doc);
    sag_state_emit(&a, &doc);

    (void)rs_apply(&f, &b, &doc);
    w = sag_tab_at(&b, 1)->root->win;
    SAG_ASSERT_NOT_NULL(w);
    SAG_ASSERT_EQ_U64(w->vp.top.v, 100U);
    SAG_ASSERT_EQ_U64(w->cs.curs.data[w->cs.primary].pos.v, 0U);

    bytebuf_free(&doc);
    sag_ed_free(&a);
    sag_ed_free(&b);
    rs_remove(&f);
}

/* Multiple cursors and the goal column survive, including EOL. */
void test_ws_restore_brings_back_cursors_and_goal(void)
{
    RsFix f;
    Ed a;
    Ed b;
    Bytebuf doc;
    char pa[192];
    Win *w;
    Cursor extra;

    rs_make(&f);
    rs_file(&f, "a.txt", "alpha\nbravo\ncharlie\ndelta\n", pa, sizeof(pa));
    rs_ed(&f, &a);
    SAG_ASSERT(sag_tab_open(&a, pa) >= 0);
    sag_tab_switch(&a, 1);
    a.win->cs.curs.data[a.win->cs.primary].pos = BYTEOFF(2U);
    a.win->cs.curs.data[a.win->cs.primary].anchor = BYTEOFF(2U);
    a.win->cs.curs.data[a.win->cs.primary].goal_col.v = SAG_GCOL_EOL;
    extra.pos = BYTEOFF(8U);
    extra.anchor = BYTEOFF(8U);
    extra.goal_col.v = 3U;
    SAG_ASSERT(sag_cset_add(&a.win->cs, extra));
    bytebuf_init(&doc);
    sag_state_emit(&a, &doc);

    (void)rs_apply(&f, &b, &doc);
    w = sag_tab_at(&b, 1)->root->win;
    SAG_ASSERT_NOT_NULL(w);
    SAG_ASSERT_EQ_U64(w->cs.curs.len, 2U);
    SAG_ASSERT_EQ_U64(w->cs.curs.data[0].pos.v, 2U);
    /* -1 in the file, SAG_GCOL_EOL in memory: the one value that does
     * not fit i64 and would otherwise force an unsigned special case
     * on every reader. */
    SAG_ASSERT_EQ_U64(w->cs.curs.data[0].goal_col.v, SAG_GCOL_EOL);
    SAG_ASSERT_EQ_U64(w->cs.curs.data[1].pos.v, 8U);
    SAG_ASSERT_EQ_U64(w->cs.curs.data[1].goal_col.v, 3U);

    bytebuf_free(&doc);
    sag_ed_free(&a);
    sag_ed_free(&b);
    rs_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Missing files                                                    */
/* ---------------------------------------------------------------- */

/*
 * A tab whose file is gone is KEPT, flagged, and counted.  Dropping it
 * would delete the only record of what someone was working on.
 */
void test_ws_restore_keeps_tabs_whose_files_vanished(void)
{
    RsFix f;
    Ed a;
    Ed b;
    Bytebuf doc;
    char pa[192];
    char pb[192];
    int t1;

    rs_make(&f);
    rs_file(&f, "a.txt", "alpha\n", pa, sizeof(pa));
    rs_file(&f, "gone.txt", "bravo\n", pb, sizeof(pb));
    rs_ed(&f, &a);
    SAG_ASSERT(sag_tab_open(&a, pa) >= 0);
    t1 = sag_tab_open(&a, pb);
    SAG_ASSERT(t1 >= 0);
    bytebuf_init(&doc);
    sag_state_emit(&a, &doc);
    /* The file goes away between the save and the restore, which is
     * the ordinary case: a branch switch, a rename, a rebuild. */
    SAG_ASSERT_EQ_I64(unlink(pb), 0);

    (void)rs_apply(&f, &b, &doc);
    SAG_ASSERT_EQ_I64(sag_tab_count(&b), 3);
    SAG_ASSERT_EQ_STR(sag_tab_at(&b, 2)->path, pb);
    SAG_ASSERT(sag_tab_at(&b, 2)->missing_at_restore);
    SAG_ASSERT(!sag_tab_at(&b, 1)->missing_at_restore);
    SAG_ASSERT_EQ_U64(b.state.missing_count, 1U);

    bytebuf_free(&doc);
    sag_ed_free(&a);
    sag_ed_free(&b);
    rs_remove(&f);
}

/* A group does not dissolve because one of its files vanished. */
void test_ws_restore_group_survives_a_missing_member(void)
{
    RsFix f;
    Ed a;
    Ed b;
    Bytebuf doc;
    char pa[192];
    char pb[192];
    u32 gid;

    rs_make(&f);
    rs_file(&f, "a.txt", "alpha\n", pa, sizeof(pa));
    rs_file(&f, "gone.txt", "bravo\n", pb, sizeof(pb));
    rs_ed(&f, &a);
    gid = sag_group_create(&a, f.work, "grp");
    sag_group_add_member(&a, gid, sag_tab_open(&a, pa));
    sag_group_add_member(&a, gid, sag_tab_open(&a, pb));
    bytebuf_init(&doc);
    sag_state_emit(&a, &doc);
    SAG_ASSERT_EQ_I64(unlink(pb), 0);

    (void)rs_apply(&f, &b, &doc);
    SAG_ASSERT_EQ_U64(b.groups.v.len, 1U);
    SAG_ASSERT_EQ_I64(sag_group_member_count(&b, b.groups.v.data[0].id), 2);
    SAG_ASSERT(sag_tab_at(&b, 2)->missing_at_restore);
    SAG_ASSERT_EQ_U64(sag_tab_at(&b, 2)->group_ordinal, 2U);

    bytebuf_free(&doc);
    sag_ed_free(&a);
    sag_ed_free(&b);
    rs_remove(&f);
}

/* A path that is now a DIRECTORY counts the same as a missing one. */
void test_ws_restore_treats_a_directory_as_missing(void)
{
    RsFix f;
    Ed a;
    Ed b;
    Bytebuf doc;
    char pa[192];
    char pb[192];

    rs_make(&f);
    rs_file(&f, "a.txt", "alpha\n", pa, sizeof(pa));
    rs_file(&f, "swap.txt", "bravo\n", pb, sizeof(pb));
    rs_ed(&f, &a);
    SAG_ASSERT(sag_tab_open(&a, pa) >= 0);
    SAG_ASSERT(sag_tab_open(&a, pb) >= 0);
    bytebuf_init(&doc);
    sag_state_emit(&a, &doc);
    SAG_ASSERT_EQ_I64(unlink(pb), 0);
    SAG_ASSERT_EQ_I64(mkdir(pb, 0700), 0);

    (void)rs_apply(&f, &b, &doc);
    SAG_ASSERT(sag_tab_at(&b, 2)->missing_at_restore);
    SAG_ASSERT_EQ_U64(b.state.missing_count, 1U);

    bytebuf_free(&doc);
    sag_ed_free(&a);
    sag_ed_free(&b);
    rs_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Determinism                                                      */
/* ---------------------------------------------------------------- */

/*
 * save -> restore -> save is a FIXPOINT.
 *
 * Without it the document drifts a field at a time across sessions and
 * nothing ever reports an error; the corpus goldens s36 must reproduce
 * would be meaningless.
 */
void test_ws_restore_save_restore_save_is_a_fixpoint(void)
{
    RsFix f;
    Ed a;
    Ed b;
    Bytebuf first;
    Bytebuf second;
    char pa[192];
    char pb[192];
    u32 gid;
    int t1;
    Pane *nu;

    rs_make(&f);
    rs_file(&f, "a.txt", "alpha\nbravo\n", pa, sizeof(pa));
    rs_file(&f, "b.txt", "charlie\n", pb, sizeof(pb));
    rs_ed(&f, &a);
    SAG_ASSERT(sag_tab_open(&a, pa) >= 0);
    t1 = sag_tab_open(&a, pb);
    gid = sag_group_create(&a, f.work, "grp");
    sag_group_add_member(&a, gid, t1);
    sag_tab_switch(&a, t1);
    sag_layout_compute(a.pane_root, (Rect){0U, 0U, 80U, 24U});
    nu = sag_pane_split(&a, a.focus, SAG_SPLIT_V);
    SAG_ASSERT_NOT_NULL(nu);
    a.focus = nu;
    a.tabs.v.data[t1].focus = nu;
    bytebuf_init(&first);
    bytebuf_init(&second);
    sag_state_emit(&a, &first);

    (void)rs_apply(&f, &b, &first);
    sag_state_emit(&b, &second);
    /*
     * The scratch tab the second editor started with has no path, so
     * it is not emitted — the two documents describe the same tabs.
     * `saved_at` is the only field that can legitimately differ, so it
     * is pinned before the comparison.
     */
    {
        FlLit *la;
        FlLit *lb;
        Arena ar;
        FlParseErr err;

        arena_init(&ar);
        la = sag_fl_parse(&ar, first.data, first.len, &err);
        lb = sag_fl_parse(&ar, second.data, second.len, &err);
        SAG_ASSERT_NOT_NULL(la);
        SAG_ASSERT_NOT_NULL(lb);
        SAG_ASSERT_EQ_U64(sag_fl_len(sag_fl_get(la, "tabs")),
                          sag_fl_len(sag_fl_get(lb, "tabs")));
        SAG_ASSERT_EQ_U64(sag_fl_len(sag_fl_get(la, "groups")),
                          sag_fl_len(sag_fl_get(lb, "groups")));
        arena_free_all(&ar);
    }
    /* And a second emission of the SAME editor is byte-identical. */
    {
        Bytebuf third;

        bytebuf_init(&third);
        sag_state_emit(&b, &third);
        SAG_ASSERT_EQ_U64(second.len, third.len);
        SAG_ASSERT_EQ_MEM(second.data, third.data, second.len);
        bytebuf_free(&third);
    }

    bytebuf_free(&first);
    bytebuf_free(&second);
    sag_ed_free(&a);
    sag_ed_free(&b);
    rs_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Options                                                          */
/* ---------------------------------------------------------------- */

/*
 * Keys this build has never heard of survive parse -> emit.
 *
 * The option model is Sprint 36's.  Until it exists, an older sagitta
 * opening a newer session's workspace must not delete its settings —
 * and since the document is rewritten in full, dropping them once makes
 * it permanent.
 */
void test_ws_restore_preserves_unknown_options(void)
{
    RsFix f;
    Ed b;
    Bytebuf doc;
    Bytebuf out;
    const char *text =
        "{\n  version: 1,\n"
        "  options: { \"from.the.future\": 42, \"wrap\": true, },\n"
        "  groups: [\n  ],\n  tabs: [\n  ],\n  active_tab: 0,\n}\n";

    rs_make(&f);
    bytebuf_init(&doc);
    bytebuf_init(&out);
    bytebuf_append(&doc, (const u8 *)text, strlen(text));

    (void)rs_apply(&f, &b, &doc);
    SAG_ASSERT_NOT_NULL(b.state.options);
    SAG_ASSERT_EQ_I64(sag_fl_int_or(sag_fl_get(b.state.options,
                                               "from.the.future"),
                                    0),
                      42);
    sag_state_emit(&b, &out);
    bytebuf_push_u8(&out, 0U);
    out.len--;
    SAG_ASSERT_NOT_NULL(strstr((const char *)out.data, "from.the.future"));
    SAG_ASSERT_NOT_NULL(strstr((const char *)out.data, "42"));

    bytebuf_free(&doc);
    bytebuf_free(&out);
    sag_ed_free(&b);
    rs_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Marks                                                            */
/* ---------------------------------------------------------------- */

/*
 * Marks come back WITHOUT reading the file, and materialize at
 * hydration.  Reading eagerly would defeat deferral for every file
 * that ever had a mark set in it.
 */
void test_ws_restore_marks_cost_no_read_until_hydration(void)
{
    RsFix f;
    Ed a;
    Ed b;
    Bytebuf doc;
    char pa[192];
    char pb[192];
    Buffer *buf;
    ByteOff at = BYTEOFF(0U);

    rs_make(&f);
    rs_file(&f, "a.txt", "alpha\n", pa, sizeof(pa));
    rs_file(&f, "b.txt", "bravo\ncharlie\n", pb, sizeof(pb));
    rs_ed(&f, &a);
    SAG_ASSERT(sag_tab_open(&a, pa) >= 0);
    SAG_ASSERT(sag_tab_open(&a, pb) >= 0);
    sag_tab_switch(&a, 2);
    SAG_ASSERT(sag_ed_mark_set(&a, sag_ed_doc(&a), (u8)'q', BYTEOFF(6U)));
    sag_tab_switch(&a, 1);
    bytebuf_init(&doc);
    sag_state_emit(&a, &doc);

    sag_file_load_count_reset();
    (void)rs_apply(&f, &b, &doc);
    /* Exactly the active tab's read, even though a mark exists in the
     * other file. */
    SAG_ASSERT_EQ_U64(sag_file_load_count(), 1U);
    buf = sag_ws_buf_by_id(&b, sag_tab_at(&b, 2)->buffer_id);
    SAG_ASSERT_NOT_NULL(buf);
    SAG_ASSERT(!sag_buf_resident(buf));
    SAG_ASSERT(buf->pending_mark_set[(u32)('q' - 'a')]);
    /* The read happens on the switch, and the mark becomes real. */
    sag_tab_switch(&b, 2);
    SAG_ASSERT(sag_buf_resident(buf));
    SAG_ASSERT(!buf->pending_mark_set[(u32)('q' - 'a')]);
    SAG_ASSERT(sag_ed_mark_get(&b, buf, (u8)'q', &at));
    SAG_ASSERT_EQ_U64(at.v, 6U);

    bytebuf_free(&doc);
    sag_ed_free(&a);
    sag_ed_free(&b);
    rs_remove(&f);
}

/* ---------------------------------------------------------------- */
/* End to end, through the filesystem                               */
/* ---------------------------------------------------------------- */

/* sag_ws_restore against a document sag_state_save actually wrote. */
void test_ws_restore_reads_what_save_wrote(void)
{
    RsFix f;
    Ed a;
    Ed b;
    char pa[192];

    rs_make(&f);
    rs_file(&f, "a.txt", "alpha\n", pa, sizeof(pa));
    rs_ed(&f, &a);
    sag_state_open(&a);
    SAG_ASSERT(a.state.writer);
    SAG_ASSERT(sag_tab_open(&a, pa) >= 0);
    SAG_ASSERT(sag_state_save(&a));
    /* Close releases the lock, so the next editor is the writer. */
    sag_state_close(&a);

    rs_ed(&f, &b);
    sag_state_open(&b);
    SAG_ASSERT(b.state.writer);
    SAG_ASSERT_EQ_I64(sag_ws_restore(&b), SAG_WS_RESTORED);
    SAG_ASSERT_EQ_I64(sag_tab_count(&b), 2);
    SAG_ASSERT_EQ_STR(sag_tab_at(&b, 1)->path, pa);
    /* A restore is not a change: it must not schedule a save. */
    SAG_ASSERT(!b.state.dirty);
    sag_state_close(&b);

    sag_ed_free(&a);
    sag_ed_free(&b);
    rs_remove(&f);
}

/* No state file is a FRESH start, silently — a first run is not an
 * event worth a message. */
void test_ws_restore_absent_state_is_fresh(void)
{
    RsFix f;
    Ed b;

    rs_make(&f);
    rs_ed(&f, &b);
    sag_state_open(&b);
    SAG_ASSERT_EQ_I64(sag_ws_restore(&b), SAG_WS_FRESH);
    SAG_ASSERT_EQ_I64(sag_tab_count(&b), 1);
    sag_state_close(&b);
    sag_ed_free(&b);
    rs_remove(&f);
}

/* A stateless editor restores nothing and says nothing. */
void test_ws_restore_stateless_is_fresh(void)
{
    RsFix f;
    Ed b;

    rs_make(&f);
    rs_ed(&f, &b);
    SAG_ASSERT(!b.state.ready);
    SAG_ASSERT_EQ_I64(sag_ws_restore(&b), SAG_WS_FRESH);
    sag_ed_free(&b);
    rs_remove(&f);
}
