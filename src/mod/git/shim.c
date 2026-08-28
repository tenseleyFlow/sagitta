#include "mod/git/git.h"
#include "mod/git/editor.h"
#include "mod/git/fussmode.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "mod/mods.h"
#include "ui/message.h"
#include "util/base.h"

struct GitCtx {
    bool ready;
};

struct FussMode {
    bool active;
};

static bool git_require(Ed *ed);

void yew_git_editor_state_init(Ed *ed)
{
    if (ed != NULL)
        ed->git_editor = NULL;
}

void yew_git_editor_state_free(Ed *ed)
{
    if (ed != NULL)
        ed->git_editor = NULL;
}

void yew_git_editor_clock_anchor(Ed *ed, i64 monotonic_ms, i64 wall_secs)
{
    (void)ed;
    (void)monotonic_ms;
    (void)wall_secs;
}

i64 yew_git_editor_wall_now(const Ed *ed)
{
    (void)ed;
    return 0;
}

void yew_git_editor_tick(Ed *ed, i64 now_ms)
{
    (void)ed; (void)now_ms;
}

i64 yew_git_editor_deadline(const Ed *ed, i64 now_ms)
{
    (void)ed; (void)now_ms;
    return -1;
}

void yew_git_editor_stats(const Ed *ed, YewGitEditorStats *out)
{
    (void)ed;
    if (out != NULL)
        *out = (YewGitEditorStats){0};
}

void yew_git_editor_test_clock(Ed *ed, YewDiffNowUsFn now_us, void *ctx)
{
    (void)ed;
    (void)now_us;
    (void)ctx;
}

bool yew_git_editor_test_base(Ed *ed, Buffer *buf, const u8 *bytes,
                              size_t len, const char *oid,
                              bool base_is_head, i64 now_ms)
{
    (void)ed;
    (void)buf;
    (void)bytes;
    (void)len;
    (void)oid;
    (void)base_is_head;
    (void)now_ms;
    return false;
}

const HunkList *yew_git_editor_test_hunks(Ed *ed, Buffer *buf)
{
    (void)ed;
    (void)buf;
    return NULL;
}

void yew_git_editor_prepare(Ed *ed, Win *w)
{
    (void)ed; (void)w;
}

static CmdStatus git_editor_require(CmdCtx *cx)
{
    (void)git_require(cx == NULL ? NULL : cx->ed);
    return YEW_CMD_ERR_STATE;
}

CmdStatus yew_git_cmd_hunk_next(CmdCtx *cx) { return git_editor_require(cx); }
CmdStatus yew_git_cmd_hunk_prev(CmdCtx *cx) { return git_editor_require(cx); }
CmdStatus yew_git_cmd_hunk_first(CmdCtx *cx) { return git_editor_require(cx); }
CmdStatus yew_git_cmd_hunk_last(CmdCtx *cx) { return git_editor_require(cx); }
CmdStatus yew_git_cmd_hunk_stage(CmdCtx *cx) { return git_editor_require(cx); }
CmdStatus yew_git_cmd_hunk_unstage(CmdCtx *cx) { return git_editor_require(cx); }
CmdStatus yew_git_cmd_hunk_discard(CmdCtx *cx) { return git_editor_require(cx); }
CmdStatus yew_git_cmd_blame_toggle(CmdCtx *cx) { return git_editor_require(cx); }
CmdStatus yew_git_cmd_diff_view(CmdCtx *cx) { return git_editor_require(cx); }
CmdStatus yew_git_cmd_conflict_scope(CmdCtx *cx) { return git_editor_require(cx); }

const BlameLine *yew_git_blame_at(Ed *ed, Win *w, LineNo line)
{
    (void)ed; (void)w; (void)line;
    return NULL;
}

void yew_git_blame_draw(Ed *ed, Win *w, u16 lo, u16 hi)
{
    (void)ed; (void)w; (void)lo; (void)hi;
}

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

void yew_fuss_state_init(Ed *ed)
{
    if (ed != NULL)
        ed->fuss = NULL;
}

void yew_fuss_state_free(Ed *ed)
{
    if (ed != NULL)
        ed->fuss = NULL;
}

bool yew_fuss_active(const Ed *ed)
{
    (void)ed;
    return false;
}

CmdStatus yew_fuss_mode_enter(Ed *ed)
{
    (void)git_require(ed);
    return YEW_CMD_ERR_STATE;
}

void yew_fuss_mode_leave(Ed *ed)
{
    (void)ed;
}

bool yew_fuss_key(Ed *ed, const Key *key, i64 now_ms)
{
    (void)ed;
    (void)key;
    (void)now_ms;
    return false;
}

void yew_fuss_tick(Ed *ed, i64 now_ms)
{
    (void)ed;
    (void)now_ms;
}

i64 yew_fuss_deadline(const Ed *ed, i64 now_ms)
{
    (void)ed;
    (void)now_ms;
    return -1;
}

u16 yew_fuss_footer_rows(const Ed *ed)
{
    (void)ed;
    return 0U;
}

u16 yew_fuss_drawer_width(u16 content_cols)
{
    (void)content_cols;
    return 0U;
}

Rect yew_fuss_drawer_rect(const Ed *ed)
{
    (void)ed;
    return (Rect){0U, 0U, 0U, 0U};
}

Rect yew_fuss_backdrop_rect(const Ed *ed)
{
    (void)ed;
    return (Rect){0U, 0U, 0U, 0U};
}

bool yew_fuss_draw_dirty(const Ed *ed)
{
    (void)ed;
    return false;
}

void yew_fuss_draw(Ed *ed)
{
    (void)ed;
}

void yew_fuss_draw_footer(Ed *ed, Rect footer)
{
    (void)ed;
    (void)footer;
}

void yew_fuss_win_releasing(Ed *ed, u32 win_id)
{
    (void)ed;
    (void)win_id;
}

void yew_fuss_windows_changed(Ed *ed)
{
    (void)ed;
}

CmdStatus yew_fuss_commit_save(Ed *ed, Buffer *buffer, bool *handled)
{
    (void)ed;
    (void)buffer;
    if (handled != NULL)
        *handled = false;
    return YEW_CMD_ERR_STATE;
}

CmdStatus yew_fuss_commit_close(Ed *ed, Buffer *buffer, bool *handled)
{
    (void)ed;
    (void)buffer;
    if (handled != NULL)
        *handled = false;
    return YEW_CMD_ERR_STATE;
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

GitStatusCode yew_git_detect_result(const Ed *ed)
{
    (void)ed;
    return YEW_GIT_NO_GIT;
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

const GitRepo *yew_git_repo_cached(const Ed *ed)
{
    (void)ed;
    return NULL;
}

const GitSnapshot *yew_git_snapshot(Ed *ed)
{
    (void)git_require(ed);
    return NULL;
}

const GitSnapshot *yew_git_snapshot_cached(const Ed *ed)
{
    (void)ed;
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
    (void)ed;
    (void)force;
    return false;
}

void yew_git_invalidate(Ed *ed)
{
    (void)ed;
}

bool yew_git_job_owned(const Ed *ed, u32 job_id)
{
    (void)ed;
    (void)job_id;
    return false;
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

u32 yew_git_spawn_callback(Ed *ed, const GitVerb *verb,
                           char *const *argv, void *owner,
                           const YewJobCallbackOps *ops,
                           char *err, size_t errsz)
{
    (void)verb; (void)argv; (void)owner; (void)ops;
    (void)yew_mod_require(YEW_MOD_FUSS, err, errsz);
    (void)git_require(ed);
    return 0U;
}

u32 yew_git_spawn_callback_input(Ed *ed, const GitVerb *verb,
                                 char *const *argv,
                                 const u8 *stdin_bytes, u64 stdin_len,
                                 void *owner,
                                 const YewJobCallbackOps *ops,
                                 char *err, size_t errsz)
{
    (void)verb;
    (void)argv;
    (void)stdin_bytes;
    (void)stdin_len;
    (void)owner;
    (void)ops;
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

#define FUSS_SHIM(name)                 \
    CmdStatus name(CmdCtx *cx)          \
    {                                   \
        return yew_git_cmd_require(cx); \
    }

FUSS_SHIM(yew_fuss_cmd_init)
FUSS_SHIM(yew_fuss_cmd_leave)
FUSS_SHIM(yew_fuss_cmd_tree_all)
FUSS_SHIM(yew_fuss_cmd_tree_hidden)
FUSS_SHIM(yew_fuss_cmd_nav_prev)
FUSS_SHIM(yew_fuss_cmd_nav_next)
FUSS_SHIM(yew_fuss_cmd_nav_parent)
FUSS_SHIM(yew_fuss_cmd_nav_enter)
FUSS_SHIM(yew_fuss_cmd_nav_toggle)
FUSS_SHIM(yew_fuss_cmd_nav_row_prev)
FUSS_SHIM(yew_fuss_cmd_nav_row_next)
FUSS_SHIM(yew_fuss_cmd_jump_arm)
FUSS_SHIM(yew_fuss_cmd_stage)
FUSS_SHIM(yew_fuss_cmd_unstage)
FUSS_SHIM(yew_fuss_cmd_stage_all)
FUSS_SHIM(yew_fuss_cmd_unstage_all)
FUSS_SHIM(yew_fuss_cmd_commit)
FUSS_SHIM(yew_fuss_cmd_commit_amend)
FUSS_SHIM(yew_fuss_cmd_push)
FUSS_SHIM(yew_fuss_cmd_push_force)
FUSS_SHIM(yew_fuss_cmd_pull)
FUSS_SHIM(yew_fuss_cmd_fetch)
FUSS_SHIM(yew_fuss_cmd_diff)
FUSS_SHIM(yew_fuss_cmd_status)
FUSS_SHIM(yew_fuss_cmd_blame)
FUSS_SHIM(yew_fuss_cmd_history)
FUSS_SHIM(yew_fuss_cmd_reflog)
FUSS_SHIM(yew_fuss_cmd_view)
FUSS_SHIM(yew_fuss_cmd_branch_switch)
FUSS_SHIM(yew_fuss_cmd_branch_create)
FUSS_SHIM(yew_fuss_cmd_branch_delete)
FUSS_SHIM(yew_fuss_cmd_merge)
FUSS_SHIM(yew_fuss_cmd_reset)
FUSS_SHIM(yew_fuss_cmd_rebase_interactive)
FUSS_SHIM(yew_fuss_cmd_rebase_continue)
FUSS_SHIM(yew_fuss_cmd_rebase_abort)
FUSS_SHIM(yew_fuss_cmd_cherry_pick)
FUSS_SHIM(yew_fuss_cmd_revert)
FUSS_SHIM(yew_fuss_cmd_stash_push)
FUSS_SHIM(yew_fuss_cmd_stash_pop)
FUSS_SHIM(yew_fuss_cmd_tag)
FUSS_SHIM(yew_fuss_cmd_discard)
FUSS_SHIM(yew_fuss_cmd_file_delete)
FUSS_SHIM(yew_fuss_cmd_file_rename)
FUSS_SHIM(yew_fuss_cmd_open)
FUSS_SHIM(yew_fuss_cmd_open_split_h)
FUSS_SHIM(yew_fuss_cmd_open_split_v)

#undef FUSS_SHIM
