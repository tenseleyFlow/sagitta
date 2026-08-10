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
                   "/tmp/yew-rshome-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->state_home));
    (void)snprintf(f->work, sizeof(f->work), "/tmp/yew-rswork-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->work));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state_home, 1), 0);
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
    YEW_ASSERT_NOT_NULL(fp);
    (void)fwrite(body, 1U, strlen(body), fp);
    (void)fclose(fp);
}

static void rs_ed(RsFix *f, Ed *ed)
{
    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(ed);
    ed->ws.dir = arena_strdup(&ed->arena, f->work);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    yew_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
}

/*
 * Applies a document to a second editor.  Steps 8 and 9 are the
 * caller's in yew_state_apply (layout needs a terminal), so the test
 * drives them exactly as yew_ws_restore does.
 */
static YewWsResult rs_apply(RsFix *f, Ed *ed, const Bytebuf *doc)
{
    YewWsResult r;

    rs_ed(f, ed);
    YEW_ASSERT(yew_ws_key(&ed->state.key, f->work));
    YEW_ASSERT(yew_ws_ensure_dir(&ed->state.key));
    ed->state.ready = true;
    ed->state.writer = true;
    r = yew_state_apply(ed, doc->data, doc->len);
    yew_layout_compute(ed->pane_root, (Rect){0U, 0U, 80U, 24U});
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
    t0 = yew_tab_open(&a, pa);
    t1 = yew_tab_open(&a, pb);
    t2 = yew_tab_open(&a, pc);
    YEW_ASSERT(t0 >= 0 && t1 >= 0 && t2 >= 0);
    gid = yew_group_create(&a, f.work, "grp");
    yew_group_add_member(&a, gid, t1);
    yew_group_add_member(&a, gid, t2);
    yew_tab_switch(&a, t1);
    bytebuf_init(&doc);
    yew_state_emit(&a, &doc);

    (void)rs_apply(&f, &b, &doc);
    /* Three restored tabs, plus the scratch the second editor started
     * with. */
    YEW_ASSERT_EQ_I64(yew_tab_count(&b), 4);
    YEW_ASSERT_EQ_STR(yew_tab_at(&b, 1)->path, pa);
    YEW_ASSERT_EQ_STR(yew_tab_at(&b, 2)->path, pb);
    YEW_ASSERT_EQ_STR(yew_tab_at(&b, 3)->path, pc);
    /* One group, with both members and their ordinals. */
    YEW_ASSERT_EQ_U64(b.groups.v.len, 1U);
    YEW_ASSERT_EQ_I64(yew_group_member_count(&b, b.groups.v.data[0].id), 2);
    YEW_ASSERT_EQ_U64(yew_tab_at(&b, 2)->group_id, b.groups.v.data[0].id);
    YEW_ASSERT_EQ_U64(yew_tab_at(&b, 3)->group_id, b.groups.v.data[0].id);
    YEW_ASSERT_EQ_U64(yew_tab_at(&b, 2)->group_ordinal, 1U);
    YEW_ASSERT_EQ_U64(yew_tab_at(&b, 3)->group_ordinal, 2U);
    /* active_tab resolved through the id map, not by index. */
    YEW_ASSERT_EQ_STR(yew_tab_at(&b, b.tabs.active)->path, pb);

    bytebuf_free(&doc);
    yew_ed_free(&a);
    yew_ed_free(&b);
    rs_remove(&f);
}

/*
 * LIVE IDS ARE FRESH.  The restored group's id comes from
 * yew_group_create, not from the file — so a document written by a
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
    g1 = yew_group_create(&a, f.work, "g1");
    g2 = yew_group_create(&a, f.work, "g2");
    g3 = yew_group_create(&a, f.work, "g3");
    yew_group_dissolve(&a, g1);
    yew_group_dissolve(&a, g2);
    YEW_ASSERT(g3 >= 3U);
    t0 = yew_tab_open(&a, pa);
    yew_group_add_member(&a, g3, t0);
    bytebuf_init(&doc);
    yew_state_emit(&a, &doc);

    (void)rs_apply(&f, &b, &doc);
    YEW_ASSERT_EQ_U64(b.groups.v.len, 1U);
    /* Fresh and monotonic from 1 — emphatically not g3's number. */
    YEW_ASSERT_EQ_U64(b.groups.v.data[0].id, 1U);
    YEW_ASSERT_EQ_U64(yew_tab_at(&b, 1)->group_id, 1U);

    bytebuf_free(&doc);
    yew_ed_free(&a);
    yew_ed_free(&b);
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
    YEW_ASSERT_EQ_I64(yew_tab_count(&b), 2);
    /*
     * Group 1 exists and is live.  A "reuse the file's number" reader
     * would resolve 99 to nothing and might fall back to it; this
     * asserts the tab is ungrouped instead.
     */
    YEW_ASSERT_EQ_U64(yew_tab_at(&b, 1)->group_id, 0U);
    /* And the empty group did not survive step 7. */
    YEW_ASSERT_EQ_U64(b.groups.v.len, 0U);

    bytebuf_free(&doc);
    yew_ed_free(&b);
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
    gid = yew_group_create(&a, f.work, "many");
    for (i = 0; i < 40; i++) {
        int t;

        (void)snprintf(name, sizeof(name), "f%02d.txt", i);
        rs_file(&f, name, "body\n", path, sizeof(path));
        t = yew_tab_open(&a, path);
        YEW_ASSERT(t >= 0);
        yew_group_add_member(&a, gid, t);
    }
    bytebuf_init(&doc);
    yew_state_emit(&a, &doc);

    yew_file_load_count_reset();
    (void)rs_apply(&f, &b, &doc);
    reads = yew_file_load_count();
    YEW_ASSERT_EQ_I64(yew_tab_count(&b), 41);
    YEW_ASSERT_EQ_U64(reads, 1U);
    /* And switching to a second member performs the second. */
    yew_tab_switch(&b, 2);
    YEW_ASSERT_EQ_U64(yew_file_load_count(), 2U);

    bytebuf_free(&doc);
    yew_ed_free(&a);
    yew_ed_free(&b);
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
    YEW_ASSERT(yew_tab_open(&a, pa) >= 0);
    yew_tab_switch(&a, 1);
    /* The switch swapped in a tree that has never been laid out, and a
     * split refuses without a rect to divide. */
    yew_layout_compute(a.pane_root, (Rect){0U, 0U, 80U, 24U});
    /* yew_pane_split mutates the leaf in place and returns the NEW
     * leaf, so focus has to follow the return value. */
    nu = yew_pane_split(&a, a.focus, YEW_SPLIT_H);
    YEW_ASSERT_NOT_NULL(nu);
    a.focus = nu;
    a.tabs.v.data[1].focus = nu;
    YEW_ASSERT(yew_pane_resize(a.tabs.v.data[1].root, 8));
    bytebuf_init(&doc);
    yew_state_emit(&a, &doc);

    (void)rs_apply(&f, &b, &doc);
    t = yew_tab_at(&b, 1);
    YEW_ASSERT_NOT_NULL(t);
    YEW_ASSERT_NOT_NULL(t->root);
    YEW_ASSERT(!t->root->is_leaf);
    YEW_ASSERT_EQ_I64(t->root->dir, YEW_SPLIT_H);
    YEW_ASSERT_EQ_U64(yew_pane_leaf_count(t->root), 2U);
    /*
     * The ratio survives as PERMILLE, so the two sides agree to within
     * a thousandth rather than exactly — which is the whole reason the
     * format has no floats.
     */
    YEW_ASSERT_EQ_I64(yew_ratio_to_permille(t->root->ratio),
                      yew_ratio_to_permille(a.tabs.v.data[1].root->ratio));
    /* Both leaves show the tab's own buffer, not the scratch. */
    YEW_ASSERT_NOT_NULL(t->root->a->win);
    YEW_ASSERT_NOT_NULL(t->root->b->win);
    YEW_ASSERT(t->root->a->win != t->root->b->win);

    bytebuf_free(&doc);
    yew_ed_free(&a);
    yew_ed_free(&b);
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
    YEW_ASSERT(yew_tab_open(&a, pa) >= 0);
    yew_tab_switch(&a, 1);
    yew_layout_compute(a.pane_root, (Rect){0U, 0U, 80U, 24U});
    /* Cursor at the top, view scrolled far down: a state follow would
     * destroy and clamp preserves. */
    a.win->cs.curs.data[a.win->cs.primary].pos = BYTEOFF(0U);
    a.win->vp.top = LINENO(100U);
    bytebuf_init(&doc);
    yew_state_emit(&a, &doc);

    (void)rs_apply(&f, &b, &doc);
    w = yew_tab_at(&b, 1)->root->win;
    YEW_ASSERT_NOT_NULL(w);
    YEW_ASSERT_EQ_U64(w->vp.top.v, 100U);
    YEW_ASSERT_EQ_U64(w->cs.curs.data[w->cs.primary].pos.v, 0U);

    bytebuf_free(&doc);
    yew_ed_free(&a);
    yew_ed_free(&b);
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
    YEW_ASSERT(yew_tab_open(&a, pa) >= 0);
    yew_tab_switch(&a, 1);
    a.win->cs.curs.data[a.win->cs.primary].pos = BYTEOFF(2U);
    a.win->cs.curs.data[a.win->cs.primary].anchor = BYTEOFF(2U);
    a.win->cs.curs.data[a.win->cs.primary].goal_col.v = YEW_GCOL_EOL;
    extra.pos = BYTEOFF(8U);
    extra.anchor = BYTEOFF(8U);
    extra.goal_col.v = 3U;
    YEW_ASSERT(yew_cset_add(&a.win->cs, extra));
    bytebuf_init(&doc);
    yew_state_emit(&a, &doc);

    (void)rs_apply(&f, &b, &doc);
    w = yew_tab_at(&b, 1)->root->win;
    YEW_ASSERT_NOT_NULL(w);
    YEW_ASSERT_EQ_U64(w->cs.curs.len, 2U);
    YEW_ASSERT_EQ_U64(w->cs.curs.data[0].pos.v, 2U);
    /* -1 in the file, YEW_GCOL_EOL in memory: the one value that does
     * not fit i64 and would otherwise force an unsigned special case
     * on every reader. */
    YEW_ASSERT_EQ_U64(w->cs.curs.data[0].goal_col.v, YEW_GCOL_EOL);
    YEW_ASSERT_EQ_U64(w->cs.curs.data[1].pos.v, 8U);
    YEW_ASSERT_EQ_U64(w->cs.curs.data[1].goal_col.v, 3U);

    bytebuf_free(&doc);
    yew_ed_free(&a);
    yew_ed_free(&b);
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
    YEW_ASSERT(yew_tab_open(&a, pa) >= 0);
    t1 = yew_tab_open(&a, pb);
    YEW_ASSERT(t1 >= 0);
    bytebuf_init(&doc);
    yew_state_emit(&a, &doc);
    /* The file goes away between the save and the restore, which is
     * the ordinary case: a branch switch, a rename, a rebuild. */
    YEW_ASSERT_EQ_I64(unlink(pb), 0);

    (void)rs_apply(&f, &b, &doc);
    YEW_ASSERT_EQ_I64(yew_tab_count(&b), 3);
    YEW_ASSERT_EQ_STR(yew_tab_at(&b, 2)->path, pb);
    YEW_ASSERT(yew_tab_at(&b, 2)->missing_at_restore);
    YEW_ASSERT(!yew_tab_at(&b, 1)->missing_at_restore);
    YEW_ASSERT_EQ_U64(b.state.missing_count, 1U);

    bytebuf_free(&doc);
    yew_ed_free(&a);
    yew_ed_free(&b);
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
    gid = yew_group_create(&a, f.work, "grp");
    yew_group_add_member(&a, gid, yew_tab_open(&a, pa));
    yew_group_add_member(&a, gid, yew_tab_open(&a, pb));
    bytebuf_init(&doc);
    yew_state_emit(&a, &doc);
    YEW_ASSERT_EQ_I64(unlink(pb), 0);

    (void)rs_apply(&f, &b, &doc);
    YEW_ASSERT_EQ_U64(b.groups.v.len, 1U);
    YEW_ASSERT_EQ_I64(yew_group_member_count(&b, b.groups.v.data[0].id), 2);
    YEW_ASSERT(yew_tab_at(&b, 2)->missing_at_restore);
    YEW_ASSERT_EQ_U64(yew_tab_at(&b, 2)->group_ordinal, 2U);

    bytebuf_free(&doc);
    yew_ed_free(&a);
    yew_ed_free(&b);
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
    YEW_ASSERT(yew_tab_open(&a, pa) >= 0);
    YEW_ASSERT(yew_tab_open(&a, pb) >= 0);
    bytebuf_init(&doc);
    yew_state_emit(&a, &doc);
    YEW_ASSERT_EQ_I64(unlink(pb), 0);
    YEW_ASSERT_EQ_I64(mkdir(pb, 0700), 0);

    (void)rs_apply(&f, &b, &doc);
    YEW_ASSERT(yew_tab_at(&b, 2)->missing_at_restore);
    YEW_ASSERT_EQ_U64(b.state.missing_count, 1U);

    bytebuf_free(&doc);
    yew_ed_free(&a);
    yew_ed_free(&b);
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
    YEW_ASSERT(yew_tab_open(&a, pa) >= 0);
    t1 = yew_tab_open(&a, pb);
    gid = yew_group_create(&a, f.work, "grp");
    yew_group_add_member(&a, gid, t1);
    yew_tab_switch(&a, t1);
    yew_layout_compute(a.pane_root, (Rect){0U, 0U, 80U, 24U});
    nu = yew_pane_split(&a, a.focus, YEW_SPLIT_V);
    YEW_ASSERT_NOT_NULL(nu);
    a.focus = nu;
    a.tabs.v.data[t1].focus = nu;
    bytebuf_init(&first);
    bytebuf_init(&second);
    yew_state_emit(&a, &first);

    (void)rs_apply(&f, &b, &first);
    yew_state_emit(&b, &second);
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
        la = yew_fl_parse(&ar, first.data, first.len, &err);
        lb = yew_fl_parse(&ar, second.data, second.len, &err);
        YEW_ASSERT_NOT_NULL(la);
        YEW_ASSERT_NOT_NULL(lb);
        YEW_ASSERT_EQ_U64(yew_fl_len(yew_fl_get(la, "tabs")),
                          yew_fl_len(yew_fl_get(lb, "tabs")));
        YEW_ASSERT_EQ_U64(yew_fl_len(yew_fl_get(la, "groups")),
                          yew_fl_len(yew_fl_get(lb, "groups")));
        arena_free_all(&ar);
    }
    /* And a second emission of the SAME editor is byte-identical. */
    {
        Bytebuf third;

        bytebuf_init(&third);
        yew_state_emit(&b, &third);
        YEW_ASSERT_EQ_U64(second.len, third.len);
        YEW_ASSERT_EQ_MEM(second.data, third.data, second.len);
        bytebuf_free(&third);
    }

    bytebuf_free(&first);
    bytebuf_free(&second);
    yew_ed_free(&a);
    yew_ed_free(&b);
    rs_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Options                                                          */
/* ---------------------------------------------------------------- */

/*
 * Keys this build has never heard of survive parse -> emit.
 *
 * The option model is Sprint 36's.  Until it exists, an older yew
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
    YEW_ASSERT_NOT_NULL(b.state.options);
    YEW_ASSERT_EQ_I64(yew_fl_int_or(yew_fl_get(b.state.options,
                                               "from.the.future"),
                                    0),
                      42);
    yew_state_emit(&b, &out);
    bytebuf_push_u8(&out, 0U);
    out.len--;
    YEW_ASSERT_NOT_NULL(strstr((const char *)out.data, "from.the.future"));
    YEW_ASSERT_NOT_NULL(strstr((const char *)out.data, "42"));

    bytebuf_free(&doc);
    bytebuf_free(&out);
    yew_ed_free(&b);
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
    YEW_ASSERT(yew_tab_open(&a, pa) >= 0);
    YEW_ASSERT(yew_tab_open(&a, pb) >= 0);
    yew_tab_switch(&a, 2);
    YEW_ASSERT(yew_ed_mark_set(&a, yew_ed_doc(&a), (u8)'q', BYTEOFF(6U)));
    yew_tab_switch(&a, 1);
    bytebuf_init(&doc);
    yew_state_emit(&a, &doc);

    yew_file_load_count_reset();
    (void)rs_apply(&f, &b, &doc);
    /* Exactly the active tab's read, even though a mark exists in the
     * other file. */
    YEW_ASSERT_EQ_U64(yew_file_load_count(), 1U);
    buf = yew_ws_buf_by_id(&b, yew_tab_at(&b, 2)->buffer_id);
    YEW_ASSERT_NOT_NULL(buf);
    YEW_ASSERT(!yew_buf_resident(buf));
    YEW_ASSERT(buf->pending_mark_set[(u32)('q' - 'a')]);
    /* The read happens on the switch, and the mark becomes real. */
    yew_tab_switch(&b, 2);
    YEW_ASSERT(yew_buf_resident(buf));
    YEW_ASSERT(!buf->pending_mark_set[(u32)('q' - 'a')]);
    YEW_ASSERT(yew_ed_mark_get(&b, buf, (u8)'q', &at));
    YEW_ASSERT_EQ_U64(at.v, 6U);

    bytebuf_free(&doc);
    yew_ed_free(&a);
    yew_ed_free(&b);
    rs_remove(&f);
}

/* ---------------------------------------------------------------- */
/* End to end, through the filesystem                               */
/* ---------------------------------------------------------------- */

/* yew_ws_restore against a document yew_state_save actually wrote. */
void test_ws_restore_reads_what_save_wrote(void)
{
    RsFix f;
    Ed a;
    Ed b;
    char pa[192];

    rs_make(&f);
    rs_file(&f, "a.txt", "alpha\n", pa, sizeof(pa));
    rs_ed(&f, &a);
    yew_state_open(&a);
    YEW_ASSERT(a.state.writer);
    YEW_ASSERT(yew_tab_open(&a, pa) >= 0);
    YEW_ASSERT(yew_state_save(&a));
    /* Close releases the lock, so the next editor is the writer. */
    yew_state_close(&a);

    rs_ed(&f, &b);
    yew_state_open(&b);
    YEW_ASSERT(b.state.writer);
    YEW_ASSERT_EQ_I64(yew_ws_restore(&b), YEW_WS_RESTORED);
    YEW_ASSERT_EQ_I64(yew_tab_count(&b), 2);
    YEW_ASSERT_EQ_STR(yew_tab_at(&b, 1)->path, pa);
    /* A restore is not a change: it must not schedule a save. */
    YEW_ASSERT(!b.state.dirty);
    yew_state_close(&b);

    yew_ed_free(&a);
    yew_ed_free(&b);
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
    yew_state_open(&b);
    YEW_ASSERT_EQ_I64(yew_ws_restore(&b), YEW_WS_FRESH);
    YEW_ASSERT_EQ_I64(yew_tab_count(&b), 1);
    yew_state_close(&b);
    yew_ed_free(&b);
    rs_remove(&f);
}

/* A stateless editor restores nothing and says nothing. */
void test_ws_restore_stateless_is_fresh(void)
{
    RsFix f;
    Ed b;

    rs_make(&f);
    rs_ed(&f, &b);
    YEW_ASSERT(!b.state.ready);
    YEW_ASSERT_EQ_I64(yew_ws_restore(&b), YEW_WS_FRESH);
    yew_ed_free(&b);
    rs_remove(&f);
}
