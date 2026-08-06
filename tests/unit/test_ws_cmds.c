/*
 * Sprint 25 §9: the workspace-state commands.
 *
 * ed.ws.forget is the only command in the tree that removes a directory
 * tree, so most of this file is about the three things standing between
 * it and a disaster: it asks first, anything but an explicit `y`
 * cancels, and sag_ws_forget_dir independently refuses a path that is
 * not under .../workspaces/ — a guard that cannot fire today and exists
 * for the caller that does not yet exist.
 *
 * The rest asserts that each command reports the truth rather than
 * succeeding quietly: a session that is not the writer must be told
 * that, and not sent to check disk permissions.
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
#include "edit/ws_cmds.h"
#include "ui/layout.h"
#include "ui/tabs.h"
#include "util/arena.h"
#include "util/xdg.h"
#include "ws/state.h"
#include "ws/workspace.h"

/* ---------------------------------------------------------------- */
/* Fixture                                                          */
/* ---------------------------------------------------------------- */

typedef struct WcFix {
    char state_home[128];
    char work[128];
    char saved[512];
    bool had_saved;
    Ed ed;
} WcFix;

static void wc_rm_rf(const char *path)
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

static void wc_make(WcFix *f)
{
    const char *prev = getenv("XDG_STATE_HOME");

    f->had_saved = prev != NULL;
    if (prev != NULL)
        (void)snprintf(f->saved, sizeof(f->saved), "%s", prev);
    (void)snprintf(f->state_home, sizeof(f->state_home),
                   "/tmp/sag-wchome-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->state_home));
    (void)snprintf(f->work, sizeof(f->work), "/tmp/sag-wcwork-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->work));
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state_home, 1), 0);

    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->work);
    SAG_ASSERT(sag_ed_open_scratch(&f->ed));
    sag_layout_compute(f->ed.pane_root, (Rect){0U, 0U, 80U, 24U});
}

static void wc_remove(WcFix *f)
{
    sag_ed_free(&f->ed);
    if (f->had_saved)
        (void)setenv("XDG_STATE_HOME", f->saved, 1);
    else
        (void)unsetenv("XDG_STATE_HOME");
    wc_rm_rf(f->state_home);
    wc_rm_rf(f->work);
}

static bool wc_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/*
 * Joins through opaque pointers on purpose: both halves are PATH_MAX
 * buffers, so a direct snprintf lets -Wformat-truncation prove the
 * result might not fit, and the check it is asking for is the assert.
 */
static void wc_join(char *out, size_t cap, const char *dir, const char *leaf)
{
    int n = snprintf(out, cap, "%s/%s", dir, leaf);

    SAG_ASSERT(n > 0 && (size_t)n < cap);
}

static CmdStatus wc_run(WcFix *f, CmdStatus (*fn)(CmdCtx *))
{
    CmdCtx cx = {0};

    cx.ed = &f->ed;
    cx.win = f->ed.win;
    return fn(&cx);
}

/* ---------------------------------------------------------------- */
/* Registration                                                     */
/* ---------------------------------------------------------------- */

/*
 * The names resolve.  A command that is absent reads as "no such
 * command", which is indistinguishable from a typo (invariant 3).
 */
void test_ws_cmds_are_registered(void)
{
    static const char *const names[] = {
        "ed.ws.save_state", "ed.ws.restore_state", "ed.ws.info",
        "ed.ws.forget", "ed.ws.migrate"};
    u32 i;

    sag_cmd_shutdown();
    sag_cmd_init();
    for (i = 0U; i < SAG_ARRAY_LEN(names); i++) {
        CmdId id = sag_cmd_lookup(names[i], (u32)strlen(names[i]));

        SAG_ASSERT(id.v != 0U);
    }
    /* Left INITIALIZED.  Shutting the registry down here and walking
     * away crashes the next test that dispatches a command — which is
     * not the next test in this file, so the failure lands somewhere
     * that has nothing to do with workspaces. */
    sag_cmd_shutdown();
    sag_cmd_init();
}

/*
 * v1 is frozen and there is no v2, so ed.ws.migrate has nothing to
 * migrate TO.  It hard-errors naming its sprint rather than silently
 * doing nothing — a no-op here would read as "already migrated".
 */
void test_ws_cmds_migrate_is_deferred(void)
{
    CmdId id;
    const CmdDesc *desc;

    sag_cmd_shutdown();
    sag_cmd_init();
    id = sag_cmd_lookup("ed.ws.migrate", 13U);
    SAG_ASSERT(id.v != 0U);
    desc = sag_cmd_desc(id);
    SAG_ASSERT_NOT_NULL(desc);
    SAG_ASSERT((desc->flags & SAG_CMD_DEFERRED) != 0U);
    /* And it names the sprint that owns it. */
    SAG_ASSERT_NOT_NULL(strstr(desc->help, "Sprint 25"));
    sag_cmd_shutdown();
    sag_cmd_init();
}

/* ---------------------------------------------------------------- */
/* save_state / restore_state                                       */
/* ---------------------------------------------------------------- */

/* Writes now, without waiting out the 2 s debounce. */
void test_ws_cmds_save_state_writes_immediately(void)
{
    WcFix f;

    wc_make(&f);
    sag_state_open(&f.ed);
    SAG_ASSERT(f.ed.state.writer);
    SAG_ASSERT(!wc_exists(sag_ws_state_path(&f.ed.state.key)));
    SAG_ASSERT_EQ_I64(wc_run(&f, sag_ws_cmd_save_state), SAG_CMD_OK);
    SAG_ASSERT(wc_exists(sag_ws_state_path(&f.ed.state.key)));
    SAG_ASSERT_EQ_U64(f.ed.state.writes, 1U);
    wc_remove(&f);
}

/* A stateless session says so rather than failing as if the disk broke. */
void test_ws_cmds_save_state_refuses_when_stateless(void)
{
    WcFix f;

    wc_make(&f);
    SAG_ASSERT(!f.ed.state.ready);
    SAG_ASSERT_EQ_I64(wc_run(&f, sag_ws_cmd_save_state), SAG_CMD_ERR_STATE);
    SAG_ASSERT(f.ed.msg.active);
    wc_remove(&f);
}

/*
 * A READER refuses with ERR_STATE, not ERR_IO.
 *
 * Nothing is wrong with the disk; another sagitta owns the workspace.
 * Reporting an I/O error would send someone to check permissions on a
 * directory that is perfectly fine.
 */
void test_ws_cmds_save_state_refuses_for_a_reader(void)
{
    WcFix f;

    wc_make(&f);
    sag_state_open(&f.ed);
    f.ed.state.writer = false;
    f.ed.state.owner_pid = 1;
    SAG_ASSERT_EQ_I64(wc_run(&f, sag_ws_cmd_save_state), SAG_CMD_ERR_STATE);
    SAG_ASSERT(!wc_exists(sag_ws_state_path(&f.ed.state.key)));
    wc_remove(&f);
}

/*
 * restore_state is ADDITIVE.  It opens what the document names
 * alongside what is already open; closing the user's current tabs to
 * make room would be a destructive act hiding inside a fetch.
 */
void test_ws_cmds_restore_state_is_additive(void)
{
    WcFix f;
    char pa[192];
    char pb[192];
    FILE *fp;

    wc_make(&f);
    (void)snprintf(pa, sizeof(pa), "%s/a.txt", f.work);
    (void)snprintf(pb, sizeof(pb), "%s/b.txt", f.work);
    fp = fopen(pa, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fputs("alpha\n", fp);
    (void)fclose(fp);
    fp = fopen(pb, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fputs("bravo\n", fp);
    (void)fclose(fp);

    sag_state_open(&f.ed);
    SAG_ASSERT(sag_tab_open(&f.ed, pa) >= 0);
    SAG_ASSERT_EQ_I64(wc_run(&f, sag_ws_cmd_save_state), SAG_CMD_OK);
    /* A tab opened after the save is NOT in the document. */
    SAG_ASSERT(sag_tab_open(&f.ed, pb) >= 0);
    SAG_ASSERT_EQ_I64(sag_tab_count(&f.ed), 3);

    SAG_ASSERT_EQ_I64(wc_run(&f, sag_ws_cmd_restore_state), SAG_CMD_OK);
    /* a.txt was already open, so sag_tab_open found it rather than
     * duplicating it — and b.txt survived the restore untouched. */
    SAG_ASSERT_EQ_I64(sag_tab_count(&f.ed), 3);
    SAG_ASSERT(sag_tab_find_by_path(&f.ed, pa) >= 0);
    SAG_ASSERT(sag_tab_find_by_path(&f.ed, pb) >= 0);
    wc_remove(&f);
}

/* Nothing saved is reported as nothing, not as a failure. */
void test_ws_cmds_restore_state_with_no_document_is_ok(void)
{
    WcFix f;

    wc_make(&f);
    sag_state_open(&f.ed);
    SAG_ASSERT_EQ_I64(wc_run(&f, sag_ws_cmd_restore_state), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(sag_tab_count(&f.ed), 1);
    wc_remove(&f);
}

/* ---------------------------------------------------------------- */
/* info                                                             */
/* ---------------------------------------------------------------- */

/*
 * info is what makes a hashed directory auditable.  It must name the
 * key in HEX — the form the directory is named with — or nobody can
 * match the message to the thing on disk.
 */
void test_ws_cmds_info_reports_the_key_and_the_owner(void)
{
    WcFix f;
    char hex[32];

    wc_make(&f);
    sag_state_open(&f.ed);
    SAG_ASSERT_EQ_I64(wc_run(&f, sag_ws_cmd_info), SAG_CMD_OK);
    SAG_ASSERT(f.ed.msg.active);
    (void)snprintf(hex, sizeof(hex), "%016llx",
                   (unsigned long long)f.ed.state.key.hash);
    SAG_ASSERT_NOT_NULL(strstr(f.ed.msg.text, hex));
    /* The path record, which is the whole collision mitigation. */
    SAG_ASSERT_NOT_NULL(strstr(f.ed.msg.text, f.work));
    /* And who holds the lock. */
    SAG_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "this session"));
    wc_remove(&f);
}

void test_ws_cmds_info_names_a_foreign_owner(void)
{
    WcFix f;

    wc_make(&f);
    sag_state_open(&f.ed);
    f.ed.state.writer = false;
    f.ed.state.owner_pid = 4242;
    SAG_ASSERT_EQ_I64(wc_run(&f, sag_ws_cmd_info), SAG_CMD_OK);
    SAG_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "pid 4242"));
    wc_remove(&f);
}

/* A stateless session says stateless rather than printing a key it does
 * not have. */
void test_ws_cmds_info_says_stateless(void)
{
    WcFix f;

    wc_make(&f);
    SAG_ASSERT_EQ_I64(wc_run(&f, sag_ws_cmd_info), SAG_CMD_OK);
    SAG_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "stateless"));
    wc_remove(&f);
}

/* ---------------------------------------------------------------- */
/* forget                                                           */
/* ---------------------------------------------------------------- */

/* It ASKS.  Nothing is removed by the command itself. */
void test_ws_cmds_forget_asks_before_deleting(void)
{
    WcFix f;
    char dir[PATH_MAX];

    wc_make(&f);
    sag_state_open(&f.ed);
    SAG_ASSERT(sag_state_save(&f.ed));
    (void)snprintf(dir, sizeof(dir), "%s", f.ed.state.key.dir);

    SAG_ASSERT_EQ_I64(wc_run(&f, sag_ws_cmd_forget), SAG_CMD_OK);
    SAG_ASSERT(f.ed.ws_prompt.active);
    SAG_ASSERT_EQ_I64(f.ed.prompt, SAG_PROMPT_WS_FORGET);
    /* Still there: asking is not doing. */
    SAG_ASSERT(wc_exists(dir));
    /* The question names the directory, so nobody deletes one they
     * cannot see. */
    SAG_ASSERT_NOT_NULL(strstr(f.ed.msg.text, dir));
    wc_remove(&f);
}

/* `y` removes the whole tree, including its subdirectories. */
void test_ws_cmds_forget_removes_the_tree_on_yes(void)
{
    WcFix f;
    char dir[PATH_MAX];
    char sub[PATH_MAX];
    FILE *fp;

    wc_make(&f);
    sag_state_open(&f.ed);
    SAG_ASSERT(sag_state_save(&f.ed));
    (void)snprintf(dir, sizeof(dir), "%s", f.ed.state.key.dir);
    /* A subdirectory with content, as history/ and undo/ are. */
    wc_join(sub, sizeof(sub), dir, "history");
    SAG_ASSERT(sag_mkdirs(sub, 0700U));
    wc_join(sub, sizeof(sub), dir, "history/cmd");
    fp = fopen(sub, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fputs("w something\n", fp);
    (void)fclose(fp);

    SAG_ASSERT_EQ_I64(wc_run(&f, sag_ws_cmd_forget), SAG_CMD_OK);
    SAG_ASSERT(sag_ws_prompt_key(&f.ed, (u8)'y'));
    SAG_ASSERT(!wc_exists(dir));
    SAG_ASSERT(!f.ed.ws_prompt.active);
    SAG_ASSERT_EQ_I64(f.ed.prompt, SAG_PROMPT_NONE);
    /* The session went stateless rather than holding a key to a
     * directory that no longer exists. */
    SAG_ASSERT(!f.ed.state.ready);
    wc_remove(&f);
}

/* Anything that is not an explicit `y` keeps it.  A destructive default
 * is not a default. */
void test_ws_cmds_forget_cancels_on_anything_but_y(void)
{
    static const u8 answers[] = {0x1BU, (u8)'n', (u8)'Y', (u8)'d', 0U};
    u32 i;

    for (i = 0U; i < SAG_ARRAY_LEN(answers); i++) {
        WcFix f;
        char dir[PATH_MAX];

        wc_make(&f);
        sag_state_open(&f.ed);
        SAG_ASSERT(sag_state_save(&f.ed));
        (void)snprintf(dir, sizeof(dir), "%s", f.ed.state.key.dir);
        SAG_ASSERT_EQ_I64(wc_run(&f, sag_ws_cmd_forget), SAG_CMD_OK);
        SAG_ASSERT(sag_ws_prompt_key(&f.ed, answers[i]));
        SAG_ASSERT(wc_exists(dir));
        SAG_ASSERT(!f.ed.ws_prompt.active);
        SAG_ASSERT(f.ed.state.ready);
        wc_remove(&f);
    }
}

/* With no prompt up, the key is not consumed — `y` goes back to being
 * whatever it is bound to. */
void test_ws_cmds_forget_key_is_not_swallowed_when_idle(void)
{
    WcFix f;

    wc_make(&f);
    SAG_ASSERT(!sag_ws_prompt_key(&f.ed, (u8)'y'));
    wc_remove(&f);
}

/*
 * THE GUARD.  sag_ws_forget_dir refuses a path that is not under
 * .../workspaces/, regardless of who calls it.
 *
 * It cannot fire today: every caller hands it a directory sag_ws_key
 * built.  That is the point — a recursive delete that trusts its
 * argument is one refactor away from being pointed at a home
 * directory, and this test is what makes that refactor fail loudly.
 */
void test_ws_cmds_forget_dir_refuses_a_path_outside_workspaces(void)
{
    char dir[192];
    char probe[256];
    FILE *fp;

    (void)snprintf(dir, sizeof(dir), "/tmp/sag-notaws-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(probe, sizeof(probe), "%s/precious", dir);
    fp = fopen(probe, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fputs("do not delete\n", fp);
    (void)fclose(fp);

    SAG_ASSERT(!sag_ws_forget_dir(dir));
    SAG_ASSERT(wc_exists(probe));
    /* Relative paths and NULL are refused before anything is opened. */
    SAG_ASSERT(!sag_ws_forget_dir("workspaces/relative"));
    SAG_ASSERT(!sag_ws_forget_dir(NULL));
    SAG_ASSERT(!sag_ws_forget_dir(""));
    wc_rm_rf(dir);
}

/*
 * A symlink inside the tree is UNLINKED, never followed.  Following one
 * is how a cache-clearing command deletes whatever it points at.
 */
void test_ws_cmds_forget_does_not_follow_symlinks(void)
{
    WcFix f;
    char dir[PATH_MAX];
    char outside[192];
    char target[256];
    char link[PATH_MAX];
    FILE *fp;

    wc_make(&f);
    sag_state_open(&f.ed);
    SAG_ASSERT(sag_state_save(&f.ed));
    (void)snprintf(dir, sizeof(dir), "%s", f.ed.state.key.dir);
    (void)snprintf(outside, sizeof(outside), "/tmp/sag-target-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(outside));
    (void)snprintf(target, sizeof(target), "%s/precious", outside);
    fp = fopen(target, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fputs("do not delete\n", fp);
    (void)fclose(fp);
    wc_join(link, sizeof(link), dir, "trap");
    SAG_ASSERT_EQ_I64(symlink(outside, link), 0);

    SAG_ASSERT_EQ_I64(wc_run(&f, sag_ws_cmd_forget), SAG_CMD_OK);
    SAG_ASSERT(sag_ws_prompt_key(&f.ed, (u8)'y'));
    SAG_ASSERT(!wc_exists(dir));
    /* The link went; what it pointed at did not. */
    SAG_ASSERT(wc_exists(target));
    wc_rm_rf(outside);
    wc_remove(&f);
}
