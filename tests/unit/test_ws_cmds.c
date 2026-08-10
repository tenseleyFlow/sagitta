/*
 * Sprint 25 §9: the workspace-state commands.
 *
 * ed.ws.forget is the only command in the tree that removes a directory
 * tree, so most of this file is about the three things standing between
 * it and a disaster: it asks first, anything but an explicit `y`
 * cancels, and yew_ws_forget_dir independently refuses a path that is
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
                   "/tmp/yew-wchome-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->state_home));
    (void)snprintf(f->work, sizeof(f->work), "/tmp/yew-wcwork-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->work));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state_home, 1), 0);

    yew_cmd_shutdown();
    yew_cmd_init();
    yew_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->work);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    yew_layout_compute(f->ed.pane_root, (Rect){0U, 0U, 80U, 24U});
}

static void wc_remove(WcFix *f)
{
    yew_ed_free(&f->ed);
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

    YEW_ASSERT(n > 0 && (size_t)n < cap);
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

    yew_cmd_shutdown();
    yew_cmd_init();
    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        CmdId id = yew_cmd_lookup(names[i], (u32)strlen(names[i]));

        YEW_ASSERT(id.v != 0U);
    }
    /* Left INITIALIZED.  Shutting the registry down here and walking
     * away crashes the next test that dispatches a command — which is
     * not the next test in this file, so the failure lands somewhere
     * that has nothing to do with workspaces. */
    yew_cmd_shutdown();
    yew_cmd_init();
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

    yew_cmd_shutdown();
    yew_cmd_init();
    id = yew_cmd_lookup("ed.ws.migrate", 13U);
    YEW_ASSERT(id.v != 0U);
    desc = yew_cmd_desc(id);
    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) != 0U);
    /* And it names the sprint that owns it. */
    YEW_ASSERT_NOT_NULL(strstr(desc->help, "Sprint 25"));
    yew_cmd_shutdown();
    yew_cmd_init();
}

/* ---------------------------------------------------------------- */
/* save_state / restore_state                                       */
/* ---------------------------------------------------------------- */

/* Writes now, without waiting out the 2 s debounce. */
void test_ws_cmds_save_state_writes_immediately(void)
{
    WcFix f;

    wc_make(&f);
    yew_state_open(&f.ed);
    YEW_ASSERT(f.ed.state.writer);
    YEW_ASSERT(!wc_exists(yew_ws_state_path(&f.ed.state.key)));
    YEW_ASSERT_EQ_I64(wc_run(&f, yew_ws_cmd_save_state), YEW_CMD_OK);
    YEW_ASSERT(wc_exists(yew_ws_state_path(&f.ed.state.key)));
    YEW_ASSERT_EQ_U64(f.ed.state.writes, 1U);
    wc_remove(&f);
}

/* A stateless session says so rather than failing as if the disk broke. */
void test_ws_cmds_save_state_refuses_when_stateless(void)
{
    WcFix f;

    wc_make(&f);
    YEW_ASSERT(!f.ed.state.ready);
    YEW_ASSERT_EQ_I64(wc_run(&f, yew_ws_cmd_save_state), YEW_CMD_ERR_STATE);
    YEW_ASSERT(f.ed.msg.active);
    wc_remove(&f);
}

/*
 * A READER refuses with ERR_STATE, not ERR_IO.
 *
 * Nothing is wrong with the disk; another yew owns the workspace.
 * Reporting an I/O error would send someone to check permissions on a
 * directory that is perfectly fine.
 */
void test_ws_cmds_save_state_refuses_for_a_reader(void)
{
    WcFix f;

    wc_make(&f);
    yew_state_open(&f.ed);
    f.ed.state.writer = false;
    f.ed.state.owner_pid = 1;
    YEW_ASSERT_EQ_I64(wc_run(&f, yew_ws_cmd_save_state), YEW_CMD_ERR_STATE);
    YEW_ASSERT(!wc_exists(yew_ws_state_path(&f.ed.state.key)));
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
    YEW_ASSERT_NOT_NULL(fp);
    (void)fputs("alpha\n", fp);
    (void)fclose(fp);
    fp = fopen(pb, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    (void)fputs("bravo\n", fp);
    (void)fclose(fp);

    yew_state_open(&f.ed);
    YEW_ASSERT(yew_tab_open(&f.ed, pa) >= 0);
    YEW_ASSERT_EQ_I64(wc_run(&f, yew_ws_cmd_save_state), YEW_CMD_OK);
    /* A tab opened after the save is NOT in the document. */
    YEW_ASSERT(yew_tab_open(&f.ed, pb) >= 0);
    YEW_ASSERT_EQ_I64(yew_tab_count(&f.ed), 3);

    YEW_ASSERT_EQ_I64(wc_run(&f, yew_ws_cmd_restore_state), YEW_CMD_OK);
    /* a.txt was already open, so yew_tab_open found it rather than
     * duplicating it — and b.txt survived the restore untouched. */
    YEW_ASSERT_EQ_I64(yew_tab_count(&f.ed), 3);
    YEW_ASSERT(yew_tab_find_by_path(&f.ed, pa) >= 0);
    YEW_ASSERT(yew_tab_find_by_path(&f.ed, pb) >= 0);
    wc_remove(&f);
}

/* Nothing saved is reported as nothing, not as a failure. */
void test_ws_cmds_restore_state_with_no_document_is_ok(void)
{
    WcFix f;

    wc_make(&f);
    yew_state_open(&f.ed);
    YEW_ASSERT_EQ_I64(wc_run(&f, yew_ws_cmd_restore_state), YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(yew_tab_count(&f.ed), 1);
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
    yew_state_open(&f.ed);
    YEW_ASSERT_EQ_I64(wc_run(&f, yew_ws_cmd_info), YEW_CMD_OK);
    YEW_ASSERT(f.ed.msg.active);
    (void)snprintf(hex, sizeof(hex), "%016llx",
                   (unsigned long long)f.ed.state.key.hash);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, hex));
    /* The path record, which is the whole collision mitigation. */
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, f.work));
    /* And who holds the lock. */
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "this session"));
    wc_remove(&f);
}

void test_ws_cmds_info_names_a_foreign_owner(void)
{
    WcFix f;

    wc_make(&f);
    yew_state_open(&f.ed);
    f.ed.state.writer = false;
    f.ed.state.owner_pid = 4242;
    YEW_ASSERT_EQ_I64(wc_run(&f, yew_ws_cmd_info), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "pid 4242"));
    wc_remove(&f);
}

/* A stateless session says stateless rather than printing a key it does
 * not have. */
void test_ws_cmds_info_says_stateless(void)
{
    WcFix f;

    wc_make(&f);
    YEW_ASSERT_EQ_I64(wc_run(&f, yew_ws_cmd_info), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "stateless"));
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
    yew_state_open(&f.ed);
    YEW_ASSERT(yew_state_save(&f.ed));
    (void)snprintf(dir, sizeof(dir), "%s", f.ed.state.key.dir);

    YEW_ASSERT_EQ_I64(wc_run(&f, yew_ws_cmd_forget), YEW_CMD_OK);
    YEW_ASSERT(f.ed.ws_prompt.active);
    YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_WS_FORGET);
    /* Still there: asking is not doing. */
    YEW_ASSERT(wc_exists(dir));
    /* The question names the directory, so nobody deletes one they
     * cannot see. */
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, dir));
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
    yew_state_open(&f.ed);
    YEW_ASSERT(yew_state_save(&f.ed));
    (void)snprintf(dir, sizeof(dir), "%s", f.ed.state.key.dir);
    /* A subdirectory with content, as history/ and undo/ are. */
    wc_join(sub, sizeof(sub), dir, "history");
    YEW_ASSERT(yew_mkdirs(sub, 0700U));
    wc_join(sub, sizeof(sub), dir, "history/cmd");
    fp = fopen(sub, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    (void)fputs("w something\n", fp);
    (void)fclose(fp);

    YEW_ASSERT_EQ_I64(wc_run(&f, yew_ws_cmd_forget), YEW_CMD_OK);
    YEW_ASSERT(yew_ws_prompt_key(&f.ed, (u8)'y'));
    YEW_ASSERT(!wc_exists(dir));
    YEW_ASSERT(!f.ed.ws_prompt.active);
    YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_NONE);
    /* The session went stateless rather than holding a key to a
     * directory that no longer exists. */
    YEW_ASSERT(!f.ed.state.ready);
    wc_remove(&f);
}

/* Anything that is not an explicit `y` keeps it.  A destructive default
 * is not a default. */
void test_ws_cmds_forget_cancels_on_anything_but_y(void)
{
    static const u8 answers[] = {0x1BU, (u8)'n', (u8)'Y', (u8)'d', 0U};
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(answers); i++) {
        WcFix f;
        char dir[PATH_MAX];

        wc_make(&f);
        yew_state_open(&f.ed);
        YEW_ASSERT(yew_state_save(&f.ed));
        (void)snprintf(dir, sizeof(dir), "%s", f.ed.state.key.dir);
        YEW_ASSERT_EQ_I64(wc_run(&f, yew_ws_cmd_forget), YEW_CMD_OK);
        YEW_ASSERT(yew_ws_prompt_key(&f.ed, answers[i]));
        YEW_ASSERT(wc_exists(dir));
        YEW_ASSERT(!f.ed.ws_prompt.active);
        YEW_ASSERT(f.ed.state.ready);
        wc_remove(&f);
    }
}

/* With no prompt up, the key is not consumed — `y` goes back to being
 * whatever it is bound to. */
void test_ws_cmds_forget_key_is_not_swallowed_when_idle(void)
{
    WcFix f;

    wc_make(&f);
    YEW_ASSERT(!yew_ws_prompt_key(&f.ed, (u8)'y'));
    wc_remove(&f);
}

/*
 * THE GUARD.  yew_ws_forget_dir refuses a path that is not under
 * .../workspaces/, regardless of who calls it.
 *
 * It cannot fire today: every caller hands it a directory yew_ws_key
 * built.  That is the point — a recursive delete that trusts its
 * argument is one refactor away from being pointed at a home
 * directory, and this test is what makes that refactor fail loudly.
 */
void test_ws_cmds_forget_dir_refuses_a_path_outside_workspaces(void)
{
    char dir[192];
    char probe[256];
    FILE *fp;

    (void)snprintf(dir, sizeof(dir), "/tmp/yew-notaws-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(probe, sizeof(probe), "%s/precious", dir);
    fp = fopen(probe, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    (void)fputs("do not delete\n", fp);
    (void)fclose(fp);

    YEW_ASSERT(!yew_ws_forget_dir(dir));
    YEW_ASSERT(wc_exists(probe));
    /* Relative paths and NULL are refused before anything is opened. */
    YEW_ASSERT(!yew_ws_forget_dir("workspaces/relative"));
    YEW_ASSERT(!yew_ws_forget_dir(NULL));
    YEW_ASSERT(!yew_ws_forget_dir(""));
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
    yew_state_open(&f.ed);
    YEW_ASSERT(yew_state_save(&f.ed));
    (void)snprintf(dir, sizeof(dir), "%s", f.ed.state.key.dir);
    (void)snprintf(outside, sizeof(outside), "/tmp/yew-target-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(outside));
    (void)snprintf(target, sizeof(target), "%s/precious", outside);
    fp = fopen(target, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    (void)fputs("do not delete\n", fp);
    (void)fclose(fp);
    wc_join(link, sizeof(link), dir, "trap");
    YEW_ASSERT_EQ_I64(symlink(outside, link), 0);

    YEW_ASSERT_EQ_I64(wc_run(&f, yew_ws_cmd_forget), YEW_CMD_OK);
    YEW_ASSERT(yew_ws_prompt_key(&f.ed, (u8)'y'));
    YEW_ASSERT(!wc_exists(dir));
    /* The link went; what it pointed at did not. */
    YEW_ASSERT(wc_exists(target));
    wc_rm_rf(outside);
    wc_remove(&f);
}
