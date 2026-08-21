#include "mod/git/git.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "mod/mods.h"
#include "ui/message.h"
#include "util/base.h"

struct GitCtx {
    bool ready;
};

static bool git_require(Ed *ed)
{
    char err[160];

    if (!yew_mod_require(YEW_MOD_FUSS, err, sizeof(err))) {
        if (ed != NULL)
            yew_msg(ed, YEW_MSG_ERROR, "%s", err);
        return false;
    }
    return true;
}

void yew_git_snapshot_init(GitSnapshot *snap)
{
    if (snap == NULL)
        return;
    (void)memset(snap, 0, sizeof(*snap));
    arena_init(&snap->a);
    snap->ahead = -1;
    snap->behind = -1;
}

void yew_git_snapshot_drop(GitSnapshot *snap)
{
    if (snap == NULL)
        return;
    arena_free_all(&snap->a);
    (void)memset(snap, 0, sizeof(*snap));
}

bool yew_git_parse_status(GitSnapshot *snap, const u8 *buf, u64 n,
                          GitParseErr *err)
{
    (void)snap; (void)buf; (void)n; (void)err;
    return false;
}

bool yew_git_parse_z_paths(Arena *a, const u8 *buf, u64 n,
                           GitPathList *paths, GitParseErr *err)
{
    (void)a; (void)buf; (void)n; (void)paths; (void)err;
    return false;
}

bool yew_git_parse_ignore(Arena *a, const u8 *buf, u64 n,
                          GitIgnoreSet *set, GitParseErr *err)
{
    (void)a; (void)buf; (void)n; (void)set; (void)err;
    return false;
}

bool yew_git_ignored(const GitIgnoreSet *set, const char *path, u32 len)
{
    (void)set; (void)path; (void)len;
    return false;
}

u32 yew_git_parse_blame(Arena *a, const u8 *buf, u64 n,
                        GitBlameLineList *lines,
                        GitCommitMetaList *commits, GitParseErr *err)
{
    (void)a; (void)buf; (void)n; (void)lines; (void)commits; (void)err;
    return 0U;
}

bool yew_git_parse_log(Arena *a, const u8 *buf, u64 n,
                       GitLogRecordList *records, GitParseErr *err)
{
    (void)a; (void)buf; (void)n; (void)records; (void)err;
    return false;
}

bool yew_git_parse_reflog(Arena *a, const u8 *buf, u64 n,
                          GitReflogRecordList *records, GitParseErr *err)
{
    (void)a; (void)buf; (void)n; (void)records; (void)err;
    return false;
}

void yew_git_state_init(Ed *ed)
{
    if (ed == NULL || ed->git != NULL)
        return;
    ed->git = yew_xcalloc(1U, sizeof(*ed->git));
    ed->git->ready = true;
}

void yew_git_state_free(Ed *ed)
{
    if (ed == NULL)
        return;
    free(ed->git);
    ed->git = NULL;
}

GitAsyncState yew_git_avail_state(const Ed *ed)
{
    (void)ed;
    return YEW_GIT_ASYNC_FAILED;
}

GitAsyncState yew_git_detect_state(const Ed *ed)
{
    (void)ed;
    return YEW_GIT_ASYNC_FAILED;
}

bool yew_git_avail(Ed *ed, GitVersion *out)
{
    (void)out;
    return git_require(ed);
}

GitStatusCode yew_git_detect(Ed *ed, GitRepo *out)
{
    (void)out;
    (void)git_require(ed);
    return YEW_GIT_NO_GIT;
}

const GitSnapshot *yew_git_snapshot(Ed *ed)
{
    (void)git_require(ed);
    return NULL;
}

const GitLogRecordList *yew_git_log_records(const Ed *ed)
{
    (void)ed;
    return NULL;
}

const GitResult *yew_git_result(const Ed *ed)
{
    (void)ed;
    return NULL;
}

u32 yew_git_blob(Ed *ed, const char *oid, char *err, size_t errsz)
{
    (void)ed;
    (void)oid;
    (void)yew_mod_require(YEW_MOD_FUSS, err, errsz);
    return 0U;
}

bool yew_git_refresh(Ed *ed, bool force)
{
    (void)force;
    return git_require(ed);
}

void yew_git_invalidate(Ed *ed)
{
    (void)git_require(ed);
}

u64 yew_git_env_fingerprint(void)
{
    return 0U;
}

const GitVerb *yew_git_verb(const char *name)
{
    (void)name;
    return NULL;
}

const GitVerb *yew_git_verb_at(size_t index)
{
    (void)index;
    return NULL;
}

size_t yew_git_verb_count(void)
{
    return 0U;
}

u32 yew_git_spawn(Ed *ed, const GitVerb *verb, char *const *argv,
                  const GitReq *req, char *err, size_t errsz)
{
    (void)verb; (void)argv; (void)req;
    (void)yew_mod_require(YEW_MOD_FUSS, err, errsz);
    (void)git_require(ed);
    return 0U;
}

const char *yew_git_state_str(GitStatusCode code)
{
    static const char *const states[] = {
        "ok", "git unavailable", "not a repository", "bare repository",
        "no HEAD", "detached HEAD", "no upstream", "conflicted",
        "merge in progress", "rebase in progress",
        "cherry-pick in progress", "revert in progress",
        "bisect in progress", "repository locked",
        "authentication required", "timed out", "git command failed",
        "malformed git output"
    };

    return (unsigned int)code < YEW_ARRAY_LEN(states) ?
           states[(unsigned int)code] : "unknown git state";
}

CmdStatus yew_git_cmd_require(CmdCtx *cx)
{
    if (cx != NULL)
        (void)git_require(cx->ed);
    return YEW_CMD_ERR_STATE;
}

CmdStatus yew_git_cmd_info(CmdCtx *cx)
{
    return yew_git_cmd_require(cx);
}

CmdStatus yew_git_cmd_refresh(CmdCtx *cx)
{
    return yew_git_cmd_require(cx);
}

CmdStatus yew_git_cmd_log(CmdCtx *cx)
{
    return yew_git_cmd_require(cx);
}
