/*
 * Sprint 25 §9: the workspace-state commands.
 *
 * ed.ws.forget DELETES, which makes it the only command in this file
 * that can lose anything, and it is guarded three ways: it asks first,
 * it only ever receives a directory yew_ws_key computed, and
 * yew_ws_forget_dir independently refuses a path that is not under
 * .../workspaces/.  The third check is redundant today and is the point
 * — a future caller that builds a path some other way must not be able
 * to turn this into `rm -rf` on an argument.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "edit/ws_cmds.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "ui/message.h"
#include "util/log.h"
#include "ws/state.h"
#include "ws/workspace.h"

/* ---------------------------------------------------------------- */
/* save / restore                                                   */
/* ---------------------------------------------------------------- */

CmdStatus yew_ws_cmd_save_state(CmdCtx *cx)
{
    Ed *ed = cx == NULL ? NULL : cx->ed;

    if (ed == NULL)
        return YEW_CMD_ERR_STATE;
    if (!ed->state.ready) {
        yew_msg(ed, YEW_MSG_ERROR, "this session keeps no workspace state");
        return YEW_CMD_ERR_STATE;
    }
    if (!ed->state.writer) {
        /* Distinct from an I/O failure: nothing is wrong with the disk,
         * and telling someone to check permissions would send them
         * looking in the wrong place entirely. */
        yew_msg(ed, YEW_MSG_ERROR,
                "workspace state is owned by pid %ld; this session will not "
                "save it",
                ed->state.owner_pid);
        return YEW_CMD_ERR_STATE;
    }
    if (!yew_state_save(ed)) {
        yew_msg(ed, YEW_MSG_ERROR, "could not write workspace state");
        return YEW_CMD_ERR_IO;
    }
    yew_msg(ed, YEW_MSG_INFO, "wrote %s",
            yew_ws_state_path(&ed->state.key));
    return YEW_CMD_OK;
}

CmdStatus yew_ws_cmd_restore_state(CmdCtx *cx)
{
    Ed *ed = cx == NULL ? NULL : cx->ed;
    u32 before;
    YewWsResult r;

    if (ed == NULL)
        return YEW_CMD_ERR_STATE;
    if (!ed->state.ready) {
        yew_msg(ed, YEW_MSG_ERROR, "this session keeps no workspace state");
        return YEW_CMD_ERR_STATE;
    }
    before = (u32)yew_tab_count(ed);
    r = yew_ws_restore(ed);
    /*
     * ADDITIVE, deliberately.  Restoring into a live session opens what
     * the document names alongside what is already here; closing the
     * user's current tabs to make room would be a destructive act
     * hiding inside a command that reads like a fetch.
     */
    switch (r) {
    case YEW_WS_RESTORED:
        yew_msg(ed, YEW_MSG_INFO, "restored %u tab(s)",
                (unsigned)((u32)yew_tab_count(ed) - before));
        return YEW_CMD_OK;
    case YEW_WS_RECOVERED:
        /* yew_ws_restore already said what happened, once. */
        return YEW_CMD_ERR_IO;
    case YEW_WS_FRESH:
    default:
        yew_msg(ed, YEW_MSG_INFO, "no workspace state to restore");
        return YEW_CMD_OK;
    }
}

/* ---------------------------------------------------------------- */
/* info                                                             */
/* ---------------------------------------------------------------- */

/* The `path` record, which is what makes the hashed directory
 * auditable at all.  Empty when it cannot be read. */
static void read_path_record(const Ed *ed, char *out, size_t cap)
{
    char path[PATH_MAX];
    FILE *f;
    size_t n;

    out[0] = '\0';
    if (snprintf(path, sizeof(path), "%s/path", ed->state.key.dir) >=
        (int)sizeof(path))
        return;
    f = fopen(path, "rb");
    if (f == NULL)
        return;
    n = fread(out, 1U, cap - 1U, f);
    (void)fclose(f);
    out[n] = '\0';
    while (n > 0U && (out[n - 1U] == '\n' || out[n - 1U] == '\r'))
        out[--n] = '\0';
}

CmdStatus yew_ws_cmd_info(CmdCtx *cx)
{
    Ed *ed = cx == NULL ? NULL : cx->ed;
    char record[PATH_MAX];
    char owner[64];

    if (ed == NULL)
        return YEW_CMD_ERR_STATE;
    if (!ed->state.ready) {
        yew_msg(ed, YEW_MSG_INFO,
                "workspace %s - stateless (this session keeps no state)",
                yew_ws_root(ed));
        return YEW_CMD_OK;
    }
    read_path_record(ed, record, sizeof(record));
    if (ed->state.writer)
        (void)snprintf(owner, sizeof(owner), "this session (pid %ld)",
                       (long)getpid());
    else if (ed->state.owner_pid > 0)
        (void)snprintf(owner, sizeof(owner), "pid %ld",
                       ed->state.owner_pid);
    else
        (void)snprintf(owner, sizeof(owner), "nobody");
    /*
     * The hash in hex, because that is how it appears in the directory
     * name — printing it in decimal would mean nobody could match the
     * message to the thing on disk.
     */
    yew_msg(ed, YEW_MSG_INFO,
            "workspace %s | key %016llx probe %u | dir %s | record %s | "
            "lock %s",
            yew_ws_root(ed), (unsigned long long)ed->state.key.hash,
            (unsigned)ed->state.key.probe, ed->state.key.dir,
            record[0] == '\0' ? "(none)" : record, owner);
    return YEW_CMD_OK;
}

/* ---------------------------------------------------------------- */
/* forget                                                           */
/* ---------------------------------------------------------------- */

static bool remove_tree(const char *dir, u32 depth)
{
    DIR *d;
    struct dirent *ent;
    bool ok = true;

    /* The state tree is state.fl, path, lock, corrupt copies, history/
     * and undo/.  Anything deeper than this is not ours to walk. */
    if (depth > 4U)
        return false;
    d = opendir(dir);
    if (d == NULL)
        return errno == ENOENT;
    while ((ent = readdir(d)) != NULL) {
        char child[PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", dir, ent->d_name) >=
            (int)sizeof(child)) {
            ok = false;
            continue;
        }
        /* lstat, never stat: a symlink is unlinked, never followed —
         * following one would delete whatever it points at, which is
         * how a cache-clearing command becomes a disaster. */
        if (lstat(child, &st) != 0) {
            ok = false;
            continue;
        }
        if (S_ISDIR(st.st_mode))
            ok = remove_tree(child, depth + 1U) && ok;
        else
            ok = unlink(child) == 0 && ok;
    }
    (void)closedir(d);
    return rmdir(dir) == 0 && ok;
}

bool yew_ws_forget_dir(const char *dir)
{
    if (dir == NULL || dir[0] != '/')
        return false;
    /*
     * The independent check.  Today every caller hands us a path
     * yew_ws_key built, so this can never fire — which is exactly why
     * it is here: the next caller might not, and a recursive delete
     * that trusts its argument is one refactor away from being pointed
     * at a home directory.
     */
    if (strstr(dir, "/workspaces/") == NULL) {
        yew_log(YEW_LOG_WARN, "refusing to remove %s: not a workspace state "
                              "directory",
                dir);
        return false;
    }
    return remove_tree(dir, 0U);
}

bool yew_ws_prompt_key(Ed *ed, u8 answer)
{
    if (ed == NULL || !ed->ws_prompt.active)
        return false;
    /* Anything that is not an explicit yes cancels.  A destructive
     * default is not a default. */
    if (answer != (u8)'y') {
        ed->ws_prompt.active = false;
        ed->prompt = YEW_PROMPT_NONE;
        yew_msg(ed, YEW_MSG_INFO, "kept workspace state");
        return true;
    }
    ed->ws_prompt.active = false;
    ed->prompt = YEW_PROMPT_NONE;
    if (!ed->state.ready) {
        yew_msg(ed, YEW_MSG_ERROR, "this session keeps no workspace state");
        return true;
    }
    /*
     * Dispose FIRST.  The lock lives in the directory we are about to
     * remove, and releasing it afterwards would either fail or, worse,
     * recreate the tree we just deleted.
     */
    {
        char dir[PATH_MAX];

        (void)snprintf(dir, sizeof(dir), "%s", ed->state.key.dir);
        yew_state_dispose(ed);
        if (!yew_ws_forget_dir(dir)) {
            yew_msg(ed, YEW_MSG_ERROR, "could not remove %s", dir);
            return true;
        }
        yew_msg(ed, YEW_MSG_INFO, "removed %s", dir);
    }
    return true;
}

CmdStatus yew_ws_cmd_forget(CmdCtx *cx)
{
    Ed *ed = cx == NULL ? NULL : cx->ed;

    if (ed == NULL)
        return YEW_CMD_ERR_STATE;
    if (!ed->state.ready) {
        yew_msg(ed, YEW_MSG_ERROR, "this session keeps no workspace state");
        return YEW_CMD_ERR_STATE;
    }
    ed->ws_prompt.active = true;
    ed->prompt = YEW_PROMPT_WS_FORGET;
    yew_msg(ed, YEW_MSG_WARN, "delete %s? [y] delete [esc] keep",
            ed->state.key.dir);
    ed->msg.prompt = true;
    return YEW_CMD_OK;
}
