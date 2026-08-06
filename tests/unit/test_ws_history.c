/*
 * Sprint 25 §8: per-workspace history, closing Sprint 18's deferral.
 *
 * ONE property carries this file, and it is the pitfall the contract
 * names: READS MERGE, WRITES DO NOT.
 *
 * Both files are loaded so a workspace session can still reach the
 * commands you type everywhere.  But if the merged list were written
 * back, every global entry would be copied into the workspace file, and
 * from there into the next workspace, until every workspace held
 * everybody's history and none of it meant anything.  Nothing about
 * that failure is visible in one session — it shows up as history that
 * has quietly stopped being local, weeks later.
 *
 * So every test here checks the FILES afterwards, not just the list in
 * memory.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ui/cmdhist.h"
#include "util/xdg.h"

/* ---------------------------------------------------------------- */
/* Fixture                                                          */
/* ---------------------------------------------------------------- */

typedef struct HsFix {
    char state_home[128];
    char ws_a[192];
    char ws_b[192];
    char saved[512];
    bool had_saved;
} HsFix;

static void hs_rm_rf(const char *path)
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

static void hs_make(HsFix *f)
{
    const char *prev = getenv("XDG_STATE_HOME");

    f->had_saved = prev != NULL;
    if (prev != NULL)
        (void)snprintf(f->saved, sizeof(f->saved), "%s", prev);
    (void)snprintf(f->state_home, sizeof(f->state_home),
                   "/tmp/sag-hshome-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->state_home));
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state_home, 1), 0);
    /* Two workspace state dirs, as sag_ws_ensure_dir would make them. */
    (void)snprintf(f->ws_a, sizeof(f->ws_a), "%s/wsA", f->state_home);
    (void)snprintf(f->ws_b, sizeof(f->ws_b), "%s/wsB", f->state_home);
    SAG_ASSERT(sag_mkdirs(f->ws_a, 0700U));
    SAG_ASSERT(sag_mkdirs(f->ws_b, 0700U));
}

static void hs_remove(HsFix *f)
{
    if (f->had_saved)
        (void)setenv("XDG_STATE_HOME", f->saved, 1);
    else
        (void)unsetenv("XDG_STATE_HOME");
    hs_rm_rf(f->state_home);
}

/* Counts lines in a history file; -1 when it does not exist. */
static int hs_lines(const char *path)
{
    FILE *fp = fopen(path, "rb");
    int n = 0;
    int c;

    if (fp == NULL)
        return -1;
    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n')
            n++;
    }
    (void)fclose(fp);
    return n;
}

static bool hs_file_has(const char *path, const char *needle)
{
    char buf[8192];
    FILE *fp = fopen(path, "rb");
    size_t n;

    if (fp == NULL)
        return false;
    n = fread(buf, 1U, sizeof(buf) - 1U, fp);
    buf[n] = '\0';
    (void)fclose(fp);
    return strstr(buf, needle) != NULL;
}

static void hs_paths(const HsFix *f, const char *ws, char *global,
                     size_t gcap, char *local, size_t lcap)
{
    (void)snprintf(global, gcap, "%s/sagitta/history/cmd", f->state_home);
    (void)snprintf(local, lcap, "%s/history/cmd", ws);
}

/* True when `h` holds `line` anywhere. */
static bool hs_has(const CmdHist *h, const char *line)
{
    size_t i;

    for (i = 0U; i < sag_hist_len(h); i++) {
        if (strcmp(sag_hist_at(h, i), line) == 0)
            return true;
    }
    return false;
}

/* ---------------------------------------------------------------- */
/* Where writes land                                                */
/* ---------------------------------------------------------------- */

/* Workspace scope appends to the workspace file and NOT the global. */
void test_ws_history_workspace_scope_writes_locally(void)
{
    HsFix f;
    CmdHist *h;
    char global[256];
    char local[256];

    hs_make(&f);
    hs_paths(&f, f.ws_a, global, sizeof(global), local, sizeof(local));
    h = sag_hist_open_scoped("cmd", f.ws_a, true);
    SAG_ASSERT(!sag_hist_is_memory(h));
    SAG_ASSERT_EQ_STR(sag_hist_path(h), local);
    sag_hist_add(h, "w local-one");
    sag_hist_add(h, "w local-two");
    sag_hist_close(h);

    SAG_ASSERT_EQ_I64(hs_lines(local), 2);
    SAG_ASSERT(hs_file_has(local, "local-one"));
    /* The global file was never created. */
    SAG_ASSERT_EQ_I64(hs_lines(global), -1);
    hs_remove(&f);
}

/* Global scope appends to the global file and NOT the workspace. */
void test_ws_history_global_scope_writes_globally(void)
{
    HsFix f;
    CmdHist *h;
    char global[256];
    char local[256];

    hs_make(&f);
    hs_paths(&f, f.ws_a, global, sizeof(global), local, sizeof(local));
    h = sag_hist_open_scoped("cmd", f.ws_a, false);
    SAG_ASSERT_EQ_STR(sag_hist_path(h), global);
    sag_hist_add(h, "w global-one");
    sag_hist_close(h);

    SAG_ASSERT_EQ_I64(hs_lines(global), 1);
    SAG_ASSERT(hs_file_has(global, "global-one"));
    SAG_ASSERT_EQ_I64(hs_lines(local), -1);
    hs_remove(&f);
}

/* ---------------------------------------------------------------- */
/* The merge                                                        */
/* ---------------------------------------------------------------- */

/*
 * A workspace session SEES global entries — the merge is what makes
 * scoping usable rather than a wall between you and everything you have
 * ever typed.
 */
void test_ws_history_reads_merge_both_files(void)
{
    HsFix f;
    CmdHist *g;
    CmdHist *h;

    hs_make(&f);
    g = sag_hist_open_scoped("cmd", f.ws_a, false);
    sag_hist_add(g, "w from-global");
    sag_hist_close(g);

    h = sag_hist_open_scoped("cmd", f.ws_a, true);
    sag_hist_add(h, "w from-workspace");
    SAG_ASSERT(hs_has(h, "w from-global"));
    SAG_ASSERT(hs_has(h, "w from-workspace"));
    sag_hist_close(h);

    /* Reopening still sees both. */
    h = sag_hist_open_scoped("cmd", f.ws_a, true);
    SAG_ASSERT_EQ_U64(sag_hist_len(h), 2U);
    SAG_ASSERT(hs_has(h, "w from-global"));
    SAG_ASSERT(hs_has(h, "w from-workspace"));
    sag_hist_close(h);
    hs_remove(&f);
}

/*
 * Global first, workspace second — so the most LOCAL entry is newest
 * under s18's newest-last convention, and Up reaches it first.
 */
void test_ws_history_local_entries_are_newest(void)
{
    HsFix f;
    CmdHist *g;
    CmdHist *h;

    hs_make(&f);
    g = sag_hist_open_scoped("cmd", f.ws_a, false);
    sag_hist_add(g, "w g1");
    sag_hist_add(g, "w g2");
    sag_hist_close(g);

    h = sag_hist_open_scoped("cmd", f.ws_a, true);
    sag_hist_add(h, "w L1");
    sag_hist_close(h);

    h = sag_hist_open_scoped("cmd", f.ws_a, true);
    SAG_ASSERT_EQ_U64(sag_hist_len(h), 3U);
    SAG_ASSERT_EQ_STR(sag_hist_at(h, 0U), "w g1");
    SAG_ASSERT_EQ_STR(sag_hist_at(h, 1U), "w g2");
    SAG_ASSERT_EQ_STR(sag_hist_at(h, 2U), "w L1");
    sag_hist_close(h);
    hs_remove(&f);
}

/*
 * An entry in BOTH files appears once, dated by its most local use.
 * s18's dedupe rule moves a repeat to the end, and the workspace read
 * happens last.
 */
void test_ws_history_duplicate_is_deduped_to_the_local_position(void)
{
    HsFix f;
    CmdHist *g;
    CmdHist *h;

    hs_make(&f);
    g = sag_hist_open_scoped("cmd", f.ws_a, false);
    sag_hist_add(g, "w shared");
    sag_hist_add(g, "w only-global");
    sag_hist_close(g);

    h = sag_hist_open_scoped("cmd", f.ws_a, true);
    sag_hist_add(h, "w shared");
    sag_hist_close(h);

    h = sag_hist_open_scoped("cmd", f.ws_a, true);
    SAG_ASSERT_EQ_U64(sag_hist_len(h), 2U);
    SAG_ASSERT_EQ_STR(sag_hist_at(h, 0U), "w only-global");
    /* Once, and newest — not twice, and not stuck at the global spot. */
    SAG_ASSERT_EQ_STR(sag_hist_at(h, 1U), "w shared");
    sag_hist_close(h);
    hs_remove(&f);
}

/* ---------------------------------------------------------------- */
/* THE pitfall                                                      */
/* ---------------------------------------------------------------- */

/*
 * The merged list is NEVER written back.
 *
 * After a workspace session that saw two global entries and added one
 * of its own, the workspace file holds exactly ONE line.  If the merge
 * happened at save instead of at load it would hold three, and the next
 * workspace would inherit them, and the scoping would be decorative.
 */
void test_ws_history_merge_never_migrates_into_the_workspace_file(void)
{
    HsFix f;
    CmdHist *g;
    CmdHist *h;
    char global[256];
    char local[256];

    hs_make(&f);
    hs_paths(&f, f.ws_a, global, sizeof(global), local, sizeof(local));
    g = sag_hist_open_scoped("cmd", f.ws_a, false);
    sag_hist_add(g, "w g1");
    sag_hist_add(g, "w g2");
    sag_hist_close(g);

    h = sag_hist_open_scoped("cmd", f.ws_a, true);
    SAG_ASSERT_EQ_U64(sag_hist_len(h), 2U);
    sag_hist_add(h, "w L1");
    SAG_ASSERT_EQ_U64(sag_hist_len(h), 3U);
    sag_hist_close(h);

    SAG_ASSERT_EQ_I64(hs_lines(local), 1);
    SAG_ASSERT(hs_file_has(local, "L1"));
    SAG_ASSERT(!hs_file_has(local, "g1"));
    SAG_ASSERT(!hs_file_has(local, "g2"));
    /* And the global file did not grow the local entry either. */
    SAG_ASSERT_EQ_I64(hs_lines(global), 2);
    SAG_ASSERT(!hs_file_has(global, "L1"));
    hs_remove(&f);
}

/*
 * The same, through sag_hist_flush — which COMPACTS, and is the far
 * more tempting place to write the merged list, because it already has
 * one in hand.
 */
void test_ws_history_flush_compacts_only_the_scope_file(void)
{
    HsFix f;
    CmdHist *g;
    CmdHist *h;
    char global[256];
    char local[256];

    hs_make(&f);
    hs_paths(&f, f.ws_a, global, sizeof(global), local, sizeof(local));
    g = sag_hist_open_scoped("cmd", f.ws_a, false);
    sag_hist_add(g, "w g1");
    sag_hist_close(g);

    h = sag_hist_open_scoped("cmd", f.ws_a, true);
    sag_hist_add(h, "w L1");
    sag_hist_add(h, "w L2");
    sag_hist_flush(h);
    sag_hist_close(h);

    SAG_ASSERT_EQ_I64(hs_lines(local), 2);
    SAG_ASSERT(!hs_file_has(local, "g1"));
    SAG_ASSERT_EQ_I64(hs_lines(global), 1);
    hs_remove(&f);
}

/*
 * Two workspaces do not see each other.  This is the point of the
 * feature, and the assertion that fails first if writes ever merge.
 */
void test_ws_history_workspaces_are_isolated_from_each_other(void)
{
    HsFix f;
    CmdHist *a;
    CmdHist *b;

    hs_make(&f);
    a = sag_hist_open_scoped("cmd", f.ws_a, true);
    sag_hist_add(a, "w in-A");
    sag_hist_close(a);

    b = sag_hist_open_scoped("cmd", f.ws_b, true);
    SAG_ASSERT(!hs_has(b, "w in-A"));
    sag_hist_add(b, "w in-B");
    sag_hist_close(b);

    a = sag_hist_open_scoped("cmd", f.ws_a, true);
    SAG_ASSERT(hs_has(a, "w in-A"));
    SAG_ASSERT(!hs_has(a, "w in-B"));
    sag_hist_close(a);
    hs_remove(&f);
}

/* A global-scope session sees its own entries across workspaces. */
void test_ws_history_global_scope_crosses_workspaces(void)
{
    HsFix f;
    CmdHist *a;
    CmdHist *b;

    hs_make(&f);
    a = sag_hist_open_scoped("cmd", f.ws_a, false);
    sag_hist_add(a, "w everywhere");
    sag_hist_close(a);

    b = sag_hist_open_scoped("cmd", f.ws_b, false);
    SAG_ASSERT(hs_has(b, "w everywhere"));
    sag_hist_close(b);
    hs_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Degenerate inputs                                                */
/* ---------------------------------------------------------------- */

/* A NULL workspace dir behaves exactly like the global open. */
void test_ws_history_null_workspace_is_the_global_history(void)
{
    HsFix f;
    CmdHist *h;
    char global[256];
    char local[256];

    hs_make(&f);
    hs_paths(&f, f.ws_a, global, sizeof(global), local, sizeof(local));
    h = sag_hist_open_scoped("cmd", NULL, true);
    /* Asked for workspace scope with no workspace: the only honest
     * answer is the global file, not an in-memory history that
     * silently loses everything at exit. */
    SAG_ASSERT_EQ_STR(sag_hist_path(h), global);
    sag_hist_add(h, "w somewhere");
    sag_hist_close(h);
    SAG_ASSERT_EQ_I64(hs_lines(global), 1);
    hs_remove(&f);
}

/* The search history is a separate file from the command history. */
void test_ws_history_kinds_do_not_share_a_file(void)
{
    HsFix f;
    CmdHist *c;
    CmdHist *s;
    char path[256];

    hs_make(&f);
    c = sag_hist_open_scoped("cmd", f.ws_a, true);
    sag_hist_add(c, "w a-command");
    sag_hist_close(c);
    s = sag_hist_open_scoped("search", f.ws_a, true);
    SAG_ASSERT(!hs_has(s, "w a-command"));
    sag_hist_add(s, "w a-pattern");
    sag_hist_close(s);

    (void)snprintf(path, sizeof(path), "%s/history/search", f.ws_a);
    SAG_ASSERT_EQ_I64(hs_lines(path), 1);
    SAG_ASSERT(hs_file_has(path, "a-pattern"));
    (void)snprintf(path, sizeof(path), "%s/history/cmd", f.ws_a);
    SAG_ASSERT(!hs_file_has(path, "a-pattern"));
    hs_remove(&f);
}

/* An in-memory history is what --clean and --batch keep, unchanged. */
void test_ws_history_memory_history_writes_nothing(void)
{
    HsFix f;
    CmdHist *h;
    char path[256];

    hs_make(&f);
    h = sag_hist_open_memory();
    SAG_ASSERT(sag_hist_is_memory(h));
    SAG_ASSERT_NULL(sag_hist_path(h));
    sag_hist_add(h, "w remembered");
    SAG_ASSERT(hs_has(h, "w remembered"));
    sag_hist_flush(h);
    sag_hist_close(h);
    (void)snprintf(path, sizeof(path), "%s/history/cmd", f.ws_a);
    SAG_ASSERT_EQ_I64(hs_lines(path), -1);
    hs_remove(&f);
}
