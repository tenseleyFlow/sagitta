#include "mod/git/git.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "mod/git/git_int.h"
#include "ui/message.h"
#include "util/base.h"
#include "util/sort.h"

extern char **environ;

typedef struct GitPending {
    GitReq req;
    const GitVerb *verb;
    char **argv;
    size_t argc;
    bool active;
} GitPending;

typedef struct GitBlobRequest {
    struct GitBlobRequest *next;
    void *owner;
    YewGitBlobFn callback;
    char *query;
    size_t query_len;
    u32 id;
} GitBlobRequest;

typedef struct GitBlobBatch {
    Ed *ed;
    Bytebuf rx;
    Bytebuf tx;
    Bytebuf body;
    GitBlobRequest *head;
    GitBlobRequest *tail;
    u64 tx_off;
    u64 body_len;
    u64 body_got;
    u32 job_id;
    bool in_body;
    bool oversized;
    bool failed;
    bool test_only;
} GitBlobBatch;

struct GitCtx {
    GitRepo repo;
    char *detected_root;
    GitSnapshot snap[2];
    Arena log_arena;
    GitLogRecordList log_records;
    Arena result_arena;
    GitResult result;
    Arena incoming_arena;
    GitPathList incoming_paths;
    char *upstream_oid;
    char *pending_upstream_oid;
    GitVersion version;
    GitAsyncState avail_state;
    GitAsyncState detect_state;
    GitStatusCode detect_result;
    GitPending pending[YEW_GIT_MAX_INFLIGHT];
    u8 live;
    u8 inflight;
    bool refresh_inflight;
    bool refresh_again;
    bool detect_requested;
    bool log_requested;
    bool incoming_dirty;
    bool forced;
    bool refresh_failed;
    i64 refresh_failed_ms;
    i64 test_now_ms;
    bool test_now;
    GitBlobBatch *blob_batch;
    u32 blob_next_request;
    u32 blob_requests;
    u32 blob_spawns;
};

typedef struct GitJobOwner {
    u32 job_id;
} GitJobOwner;

static GitTestSpawnFn test_spawn;
static void *test_spawn_opaque;

static bool git_spawn_log(Ed *ed);
static void git_incoming_clear(GitCtx *ctx);
static void git_publish_refresh(Ed *ed);
static void git_refresh_failed(Ed *ed);

void yew_git_snapshot_init(GitSnapshot *snap)
{
    if (snap == NULL)
        return;
    (void)memset(snap, 0, sizeof(*snap));
    arena_init(&snap->a);
    snap->comment_char = arena_strdup(&snap->a, "#");
    snap->comment_char_len = 1U;
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

#define GIT_READ(name, repo, head) \
    {name, YEW_GV_READ, YEW_GIT_READ_TIMEOUT_MS, repo, head}
#define GIT_MUTATE(name, repo, head) \
    {name, YEW_GV_MUTATE, YEW_GIT_READ_TIMEOUT_MS, repo, head}
#define GIT_NET(name, repo, head) \
    {name, YEW_GV_NET, YEW_GIT_NET_TIMEOUT_MS, repo, head}

/* This is deliberately the only inventory of commands executable by the
 * module.  Callers select a descriptor and provide argv elements; there is
 * no raw command-line escape hatch. */
static const GitVerb git_verbs[] = {
    GIT_READ("version", false, false),
    GIT_READ("detect", false, false),
    GIT_READ("status", true, false),
    GIT_READ("ignore", true, false),
    GIT_READ("comment-char", true, false),
    GIT_READ("upstream-oid", true, true),
    GIT_READ("incoming", true, true),
    GIT_READ("diff", true, true),
    GIT_READ("blame", true, true),
    GIT_READ("log", true, true),
    GIT_READ("reflog", true, true),
    GIT_READ("blob", true, false),
    GIT_READ("show", true, true),
    GIT_READ("branch-list", true, false),
    GIT_READ("remote-list", true, false),
    GIT_NET("remote-check", true, false),
    GIT_MUTATE("init", false, false),
    GIT_MUTATE("stage", true, false),
    GIT_MUTATE("unstage", true, false),
    GIT_MUTATE("stage-all", true, false),
    GIT_MUTATE("unstage-all", true, false),
    GIT_MUTATE("commit", true, false),
    GIT_MUTATE("commit-amend", true, true),
    GIT_MUTATE("switch", true, true),
    GIT_MUTATE("switch-create", true, false),
    GIT_MUTATE("branch-delete", true, true),
    GIT_MUTATE("merge", true, true),
    GIT_MUTATE("reset", true, true),
    GIT_MUTATE("cherry-pick", true, true),
    GIT_MUTATE("revert", true, true),
    GIT_MUTATE("stash-push", true, false),
    GIT_MUTATE("stash-pop", true, true),
    GIT_MUTATE("tag", true, false),
    GIT_MUTATE("discard", true, true),
    GIT_MUTATE("rm", true, false),
    GIT_MUTATE("mv", true, false),
    GIT_MUTATE("apply", true, false),
    GIT_NET("fetch", true, false),
    GIT_NET("pull", true, true),
    GIT_NET("push", true, true)
};

#undef GIT_READ
#undef GIT_MUTATE
#undef GIT_NET

static const char *const git_states[] = {
    "ok", "git unavailable", "not a repository", "bare repository",
    "no HEAD", "detached HEAD", "no upstream", "conflicted",
    "merge in progress", "rebase in progress", "cherry-pick in progress",
    "revert in progress", "bisect in progress", "repository locked",
    "authentication required", "timed out", "git command failed",
    "malformed git output"
};

static i64 git_now(const Ed *ed)
{
    if (ed != NULL && ed->git != NULL && ed->git->test_now)
        return ed->git->test_now_ms;
    return ed == NULL ? 0 : ed->now_ms;
}

static void git_repo_clear(GitRepo *repo)
{
    if (repo == NULL)
        return;
    free(repo->git_dir);
    free(repo->top_level);
    (void)memset(repo, 0, sizeof(*repo));
}

static char *git_dup_bytes(const u8 *bytes, size_t len)
{
    char *copy = yew_xmalloc(len + 1U);

    if (len != 0U)
        (void)memcpy(copy, bytes, len);
    copy[len] = '\0';
    return copy;
}

static u64 git_hash_bytes(u64 hash, const void *bytes, size_t len)
{
    const u8 *p = bytes;
    size_t i;

    for (i = 0U; i < len; i++) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static u64 git_hash_cstr(u64 hash, const char *s)
{
    return git_hash_bytes(hash, s, strlen(s) + 1U);
}

u64 yew_git_env_fingerprint(void)
{
    static const char *const names[] = {
        "GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_INDEX_FILE",
        "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
        "GIT_NAMESPACE", "GIT_CEILING_DIRECTORIES", "GIT_CONFIG_GLOBAL",
        "GIT_CONFIG_SYSTEM", "GIT_CONFIG_COUNT", "GIT_SSH",
        "GIT_SSH_COMMAND", "GIT_ASKPASS", "SSH_ASKPASS", "SSH_AUTH_SOCK",
        "SSH_ASKPASS_REQUIRE"
    };
    u64 hash = UINT64_C(1469598103934665603);
    size_t i;
    char **row;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        const char *value = getenv(names[i]);

        hash = git_hash_cstr(hash, names[i]);
        hash = git_hash_cstr(hash, value == NULL ? "" : value);
    }
    /* Numbered config rows are included in their inherited environment
     * order.  Their order is semantically significant to git as well. */
    for (row = environ; row != NULL && *row != NULL; row++) {
        if (strncmp(*row, "GIT_CONFIG_KEY_", 15U) == 0 ||
            strncmp(*row, "GIT_CONFIG_VALUE_", 17U) == 0)
            hash = git_hash_cstr(hash, *row);
    }
    return hash;
}

const GitVerb *yew_git_verb(const char *name)
{
    size_t i;

    if (name == NULL)
        return NULL;
    for (i = 0U; i < YEW_ARRAY_LEN(git_verbs); i++) {
        if (strcmp(name, git_verbs[i].name) == 0)
            return &git_verbs[i];
    }
    return NULL;
}

const GitVerb *yew_git_verb_at(size_t index)
{
    return index < YEW_ARRAY_LEN(git_verbs) ? &git_verbs[index] : NULL;
}

size_t yew_git_verb_count(void)
{
    return YEW_ARRAY_LEN(git_verbs);
}

const char *yew_git_state_str(GitStatusCode code)
{
    if ((unsigned int)code >= YEW_ARRAY_LEN(git_states))
        return "unknown git state";
    return git_states[(unsigned int)code];
}

bool yew_git_cache_fresh(i64 taken_ms, i64 now_ms)
{
    return now_ms >= taken_ms && now_ms - taken_ms < YEW_GIT_TTL_MS;
}

static char *git_join(const char *dir, const char *tail)
{
    size_t dn;
    size_t tn;
    bool slash;
    char *path;

    if (dir == NULL || tail == NULL)
        return NULL;
    dn = strlen(dir);
    tn = strlen(tail);
    slash = dn != 0U && dir[dn - 1U] != '/';
    if (dn > SIZE_MAX - tn - (slash ? 2U : 1U))
        return NULL;
    path = yew_xmalloc(dn + tn + (slash ? 2U : 1U));
    (void)memcpy(path, dir, dn);
    if (slash)
        path[dn++] = '/';
    (void)memcpy(path + dn, tail, tn + 1U);
    return path;
}

static bool git_exists(const char *dir, const char *tail)
{
    char *path = git_join(dir, tail);
    bool exists = path != NULL && access(path, F_OK) == 0;

    free(path);
    return exists;
}

static bool git_read_u32(const char *dir, const char *tail, u32 *out)
{
    char bytes[32];
    char *path = git_join(dir, tail);
    size_t at = 0U;
    u64 value = 0U;
    ssize_t got;
    int fd;

    if (path == NULL || out == NULL) {
        free(path);
        return false;
    }
    fd = open(path, O_RDONLY);
    free(path);
    if (fd < 0)
        return false;
    do {
        got = read(fd, bytes, sizeof(bytes));
    } while (got < 0 && errno == EINTR);
    (void)close(fd);
    if (got <= 0 || (size_t)got == sizeof(bytes))
        return false;
    while (at < (size_t)got && bytes[at] >= '0' && bytes[at] <= '9') {
        value = value * 10U + (u64)(bytes[at] - '0');
        if (value > UINT32_MAX)
            return false;
        at++;
    }
    if (at == 0U)
        return false;
    while (at < (size_t)got &&
           (bytes[at] == '\n' || bytes[at] == '\r'))
        at++;
    if (at != (size_t)got)
        return false;
    *out = (u32)value;
    return true;
}

static GitStatusCode git_operation_state(const GitRepo *repo)
{
    const char *dir;

    if (repo == NULL || repo->git_dir == NULL)
        return YEW_GIT_OK;
    dir = repo->git_dir;
    if (git_exists(dir, "MERGE_HEAD"))
        return YEW_GIT_MID_MERGE;
    if (git_exists(dir, "rebase-merge") || git_exists(dir, "rebase-apply"))
        return YEW_GIT_MID_REBASE;
    if (git_exists(dir, "CHERRY_PICK_HEAD"))
        return YEW_GIT_MID_CHERRY_PICK;
    if (git_exists(dir, "REVERT_HEAD"))
        return YEW_GIT_MID_REVERT;
    if (git_exists(dir, "BISECT_LOG"))
        return YEW_GIT_MID_BISECT;
    return YEW_GIT_OK;
}

GitStatusCode yew_git_probe_state(const GitRepo *repo)
{
    const char *dir;

    if (repo == NULL || repo->git_dir == NULL || repo->git_dir[0] == '\0')
        return YEW_GIT_NOT_REPO;
    dir = repo->git_dir;
    if (git_exists(dir, "index.lock"))
        return YEW_GIT_LOCKED;
    if (git_exists(dir, "MERGE_HEAD"))
        return YEW_GIT_MID_MERGE;
    if (git_exists(dir, "rebase-merge") || git_exists(dir, "rebase-apply"))
        return YEW_GIT_MID_REBASE;
    if (git_exists(dir, "CHERRY_PICK_HEAD"))
        return YEW_GIT_MID_CHERRY_PICK;
    if (git_exists(dir, "REVERT_HEAD"))
        return YEW_GIT_MID_REVERT;
    if (git_exists(dir, "BISECT_LOG"))
        return YEW_GIT_MID_BISECT;
    return repo->bare ? YEW_GIT_BARE : YEW_GIT_OK;
}

static GitPending *git_pending_find(GitCtx *ctx, u32 id)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(ctx->pending); i++) {
        if (ctx->pending[i].active && ctx->pending[i].req.job_id == id)
            return &ctx->pending[i];
    }
    return NULL;
}

static GitPending *git_pending_slot(GitCtx *ctx)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(ctx->pending); i++) {
        if (!ctx->pending[i].active)
            return &ctx->pending[i];
    }
    return NULL;
}

static void git_pending_drop(GitCtx *ctx, GitPending *pending)
{
    if (pending == NULL || !pending->active)
        return;
    if (pending->req.stdin_bytes != NULL && pending->req.stdin_len != 0U)
        yew_memzero((void *)pending->req.stdin_bytes,
                    (size_t)pending->req.stdin_len);
    arena_free_all(&pending->req.arena);
    (void)memset(pending, 0, sizeof(*pending));
    if (ctx->inflight != 0U)
        ctx->inflight--;
}

static void git_pending_copy_argv(GitPending *pending, char *const *argv)
{
    size_t argc = 0U;
    size_t i;

    while (argv[argc] != NULL)
        argc++;
    pending->argv = arena_alloc(&pending->req.arena,
                                (argc + 1U) * sizeof(*pending->argv),
                                _Alignof(char *));
    for (i = 0U; i < argc; i++)
        pending->argv[i] = arena_strdup(&pending->req.arena, argv[i]);
    pending->argv[argc] = NULL;
    pending->argc = argc;
}

static bool git_pending_same(const GitPending *pending, const GitVerb *verb,
                             char *const *argv, const GitReq *req)
{
    size_t argc = 0U;
    size_t i;
    GitReqKind kind = req == NULL ? YEW_GREQ_VERB : req->kind;
    bool literal = req != NULL && req->literal_paths;
    const u8 *stdin_bytes = req == NULL ? NULL : req->stdin_bytes;
    u64 stdin_len = req == NULL ? 0U : req->stdin_len;
    bool shared = kind == YEW_GREQ_STATUS || kind == YEW_GREQ_IGNORE ||
                  kind == YEW_GREQ_UPSTREAM || kind == YEW_GREQ_INCOMING;

    if (!shared || !pending->active || pending->verb != verb ||
        verb->kind != YEW_GV_READ ||
        pending->req.kind != kind || pending->req.literal_paths != literal ||
        pending->req.stdin_len != stdin_len)
        return false;
    while (argv[argc] != NULL)
        argc++;
    if (pending->argc != argc)
        return false;
    for (i = 0U; i < argc; i++) {
        if (strcmp(pending->argv[i], argv[i]) != 0)
            return false;
    }
    return stdin_len == 0U ||
           memcmp(pending->req.stdin_bytes, stdin_bytes, (size_t)stdin_len) == 0;
}

static bool git_takes_paths(const GitVerb *verb)
{
    static const char *const names[] = {
        "stage", "unstage", "diff", "blame", "show", "switch",
        "switch-create", "merge", "discard", "rm", "mv", "apply"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        if (strcmp(verb->name, names[i]) == 0)
            return true;
    }
    return false;
}

static void git_error(char *err, size_t errsz, const char *message)
{
    size_t n;

    if (err == NULL || errsz == 0U)
        return;
    n = strlen(message);
    if (n >= errsz)
        n = errsz - 1U;
    (void)memcpy(err, message, n);
    err[n] = '\0';
}

static char **git_build_argv(const GitVerb *verb, char *const *tail)
{
    size_t n = 0U;
    size_t at = 0U;
    char **argv;

    if (tail != NULL) {
        while (tail[n] != NULL)
            n++;
    }
    argv = yew_xcalloc(n + 8U, sizeof(*argv));
    argv[at++] = (char *)"git";
    argv[at++] = (char *)"--no-pager";
    argv[at++] = (char *)"-c";
    argv[at++] = (char *)"core.quotepath=false";
    argv[at++] = (char *)"-c";
    argv[at++] = (char *)"status.renames=true";
    if (verb->kind == YEW_GV_READ)
        argv[at++] = (char *)"--no-optional-locks";
    if (n != 0U)
        (void)memcpy(argv + at, tail, n * sizeof(*argv));
    return argv;
}

static GitStatusCode git_auth_state(const u8 *bytes, u64 len)
{
    static const char *const needles[] = {
        "could not read Username", "Authentication failed",
        "terminal prompts disabled", "Permission denied (publickey)"
    };
    char *text;
    size_t i;
    GitStatusCode state = YEW_GIT_FAILED;

    if (len > (u64)SIZE_MAX - 1U)
        return state;
    text = git_dup_bytes(bytes, (size_t)len);
    for (i = 0U; i < YEW_ARRAY_LEN(needles); i++) {
        if (strstr(text, needles[i]) != NULL) {
            state = YEW_GIT_AUTH;
            break;
        }
    }
    free(text);
    return state;
}

GitStatusCode yew_git_test_auth_state(const u8 *bytes, u64 len)
{
    return git_auth_state(bytes, len);
}

static bool git_job_ok(const YewJob *job)
{
    return job->state == YEW_JOB_EXITED && job->exit_code == 0 &&
           !job->collect_capped;
}

static void git_job_complete(void *owner, Ed *ed, const YewJob *job)
{
    GitJobOwner *completion = owner;
    GitPending *pending;
    GitStatusCode state;

    if (completion == NULL || ed == NULL || ed->git == NULL)
        return;
    pending = git_pending_find(ed->git, completion->job_id);
    if (pending == NULL)
        return;
    if (job->state == YEW_JOB_TIMEOUT)
        state = YEW_GIT_TIMEOUT;
    else if (job->state == YEW_JOB_EXECFAIL && job->exec_errno == ENOENT)
        state = YEW_GIT_NO_GIT;
    else if (git_job_ok(job))
        state = YEW_GIT_OK;
    else if (pending->verb->kind == YEW_GV_NET)
        state = git_auth_state(job->collect_err.data,
                               (u64)job->collect_err.len);
    else
        state = YEW_GIT_FAILED;
    (void)yew_git_test_complete_exit(ed, completion->job_id, state,
                                     job->exit_code, job->collect.data,
                                     (u64)job->collect.len,
                                     job->collect_err.data,
                                     (u64)job->collect_err.len);
}

static void git_job_destroy(void *owner)
{
    free(owner);
}

static const YewJobCallbackOps git_job_ops = {
    git_job_complete,
    git_job_destroy
};

u32 yew_git_spawn(Ed *ed, const GitVerb *verb, char *const *argv,
                  const GitReq *req, char *err, size_t errsz)
{
    static const char *const env_base[] = {
        "GIT_TERMINAL_PROMPT=0", "GIT_EDITOR=false",
        "GIT_SEQUENCE_EDITOR=false", "GIT_FLUSH=1", "GIT_PAGER=cat",
        "PAGER=cat", "LC_ALL=C", NULL
    };
    static const char *const env_paths[] = {
        "GIT_TERMINAL_PROMPT=0", "GIT_EDITOR=false",
        "GIT_SEQUENCE_EDITOR=false", "GIT_FLUSH=1", "GIT_PAGER=cat",
        "PAGER=cat", "LC_ALL=C", "GIT_LITERAL_PATHSPECS=1", NULL
    };
    static const char *const env_unset[] = {
        "COLUMNS", "LINES", "GIT_TRACE", "GIT_TRACE_PACKET",
        "GIT_TRACE_PERFORMANCE", "GIT_CURL_VERBOSE", "GIT_TRANSFER_TRACE",
        NULL
    };
    static const char *const env_unset_prefix[] = {"GIT_TRACE2", NULL};
    GitCtx *ctx;
    GitPending *pending;
    GitJobOwner *owner = NULL;
    YewJobSpec spec = {0};
    char **final_argv;
    u32 id;
    size_t i;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (ed == NULL || ed->git == NULL || verb == NULL || argv == NULL ||
        argv[0] == NULL)
        return 0U;
    if (req != NULL && ((req->stdin_len != 0U && req->stdin_bytes == NULL) ||
                        req->stdin_len > (u64)SIZE_MAX)) {
        git_error(err, errsz, "invalid git stdin");
        return 0U;
    }
    ctx = ed->git;
    if (test_spawn == NULL && verb->needs_repo &&
        (ctx->detect_state != YEW_GIT_ASYNC_READY ||
         ctx->detect_result == YEW_GIT_NOT_REPO ||
         ctx->detect_result == YEW_GIT_NO_GIT)) {
        git_error(err, errsz, yew_git_state_str(YEW_GIT_NOT_REPO));
        return 0U;
    }
    if (verb->needs_head &&
        (ctx->snap[ctx->live].gen == 0U || ctx->snap[ctx->live].unborn)) {
        const GitSnapshot *staged = &ctx->snap[ctx->live ^ 1U];
        bool internal_head = req != NULL &&
                             (req->kind == YEW_GREQ_UPSTREAM ||
                              req->kind == YEW_GREQ_INCOMING) &&
                             staged->head_oid != NULL && !staged->unborn;

        if (!internal_head) {
            git_error(err, errsz, yew_git_state_str(YEW_GIT_NO_HEAD));
            return 0U;
        }
    }
    if (verb->kind == YEW_GV_READ) {
        for (i = 0U; i < YEW_ARRAY_LEN(ctx->pending); i++) {
            if (git_pending_same(&ctx->pending[i], verb, argv, req))
                return ctx->pending[i].req.job_id;
        }
    }
    if (ctx->inflight >= YEW_GIT_MAX_INFLIGHT)
        return 0U;
    pending = git_pending_slot(ctx);
    if (pending == NULL)
        return 0U;
    (void)memset(pending, 0, sizeof(*pending));
    arena_init(&pending->req.arena);
    if (req != NULL) {
        pending->req.kind = req->kind;
        pending->req.literal_paths = req->literal_paths;
        pending->req.retried = req->retried;
        if (req->stdin_len != 0U) {
            u8 *copy = arena_alloc(&pending->req.arena,
                                   (size_t)req->stdin_len, _Alignof(u8));

            (void)memcpy(copy, req->stdin_bytes, (size_t)req->stdin_len);
            pending->req.stdin_bytes = copy;
            pending->req.stdin_len = req->stdin_len;
        }
        pending->req.owner = req->owner;
    } else {
        pending->req.kind = YEW_GREQ_VERB;
    }
    pending->verb = verb;
    git_pending_copy_argv(pending, argv);
    if (pending->req.kind == YEW_GREQ_DETECT) {
        pending->req.detect_root = arena_strdup(&pending->req.arena,
                                                 yew_ws_root(ed));
        pending->req.detect_env_fp = yew_git_env_fingerprint();
    }
    pending->active = true;
    ctx->inflight++;
    final_argv = git_build_argv(verb, argv);
    if (test_spawn != NULL) {
        id = test_spawn(ed, verb, final_argv, &pending->req,
                        test_spawn_opaque, err, errsz);
    } else {
        owner = yew_xcalloc(1U, sizeof(*owner));
        spec.argv = final_argv;
        spec.cwd = yew_ws_root(ed);
        spec.sink = YEW_SINK_CALLBACK;
        spec.in_bytes = pending->req.stdin_bytes;
        spec.in_len = pending->req.stdin_len;
        spec.timeout_ms = verb->timeout_ms;
        spec.display = verb->name;
        spec.internal = true;
        spec.collect_max = YEW_GIT_COLLECT_MAX;
        spec.env_set = (pending->req.literal_paths || git_takes_paths(verb)) ?
                       env_paths : env_base;
        spec.env_unset = env_unset;
        spec.env_unset_prefix = env_unset_prefix;
        spec.callback_owner = owner;
        spec.callback_ops = &git_job_ops;
        id = yew_job_spawn(ed, &spec, err, errsz);
    }
    free(final_argv);
    if (id == 0U) {
        free(owner);
        git_pending_drop(ctx, pending);
        return 0U;
    }
    pending->req.job_id = id;
    if (owner != NULL)
        owner->job_id = id;
    return id;
}

typedef struct GitCallbackOwner {
    void *owner;
    const YewJobCallbackOps *ops;
    u8 *stdin_bytes;
    u64 stdin_len;
} GitCallbackOwner;

static void git_callback_complete(void *owner, Ed *ed, const YewJob *job)
{
    GitCallbackOwner *wrapped = owner;

    wrapped->ops->complete(wrapped->owner, ed, job);
}

static void git_callback_destroy(void *owner)
{
    GitCallbackOwner *wrapped = owner;

    wrapped->ops->destroy(wrapped->owner);
    if (wrapped->stdin_bytes != NULL) {
        yew_memzero(wrapped->stdin_bytes, (size_t)wrapped->stdin_len);
        free(wrapped->stdin_bytes);
    }
    free(wrapped);
}

static const YewJobCallbackOps git_callback_ops = {
    git_callback_complete,
    git_callback_destroy
};

u32 yew_git_spawn_callback_input(Ed *ed, const GitVerb *verb,
                                 char *const *argv,
                                 const u8 *stdin_bytes, u64 stdin_len,
                                 void *owner,
                                 const YewJobCallbackOps *ops,
                                 char *err, size_t errsz)
{
    static const char *const env_base[] = {
        "GIT_TERMINAL_PROMPT=0", "GIT_EDITOR=false",
        "GIT_SEQUENCE_EDITOR=false", "GIT_FLUSH=1", "GIT_PAGER=cat",
        "PAGER=cat", "LC_ALL=C", NULL
    };
    static const char *const env_unset[] = {
        "COLUMNS", "LINES", "GIT_TRACE", "GIT_TRACE_PACKET",
        "GIT_TRACE_PERFORMANCE", "GIT_CURL_VERBOSE", "GIT_TRANSFER_TRACE",
        NULL
    };
    static const char *const env_unset_prefix[] = {"GIT_TRACE2", NULL};
    YewJobSpec spec = {0};
    GitCallbackOwner *wrapped;
    char **final_argv;
    u32 id;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (ed == NULL || ed->git == NULL || verb == NULL || argv == NULL ||
        argv[0] == NULL || owner == NULL || ops == NULL ||
        ops->complete == NULL || ops->destroy == NULL ||
        (verb->kind != YEW_GV_READ && verb->kind != YEW_GV_MUTATE) ||
        (stdin_len != 0U && stdin_bytes == NULL) ||
        stdin_len > (u64)SIZE_MAX) {
        git_error(err, errsz, "invalid Git callback job");
        return 0U;
    }
    if (test_spawn != NULL) {
        git_error(err, errsz, "Git callback job unavailable under test hook");
        return 0U;
    }
    if (verb->needs_repo &&
        (ed->git->detect_state != YEW_GIT_ASYNC_READY ||
         ed->git->detect_result != YEW_GIT_OK)) {
        git_error(err, errsz, yew_git_state_str(YEW_GIT_NOT_REPO));
        return 0U;
    }
    if (verb->needs_head &&
        (ed->git->snap[ed->git->live].gen == 0U ||
         ed->git->snap[ed->git->live].unborn)) {
        git_error(err, errsz, yew_git_state_str(YEW_GIT_NO_HEAD));
        return 0U;
    }
    wrapped = yew_xcalloc(1U, sizeof(*wrapped));
    wrapped->owner = owner;
    wrapped->ops = ops;
    if (stdin_len != 0U) {
        wrapped->stdin_bytes = yew_xmalloc((size_t)stdin_len);
        (void)memcpy(wrapped->stdin_bytes, stdin_bytes, (size_t)stdin_len);
        wrapped->stdin_len = stdin_len;
    }
    final_argv = git_build_argv(verb, argv);
    spec.argv = final_argv;
    spec.cwd = yew_ws_root(ed);
    spec.sink = YEW_SINK_CALLBACK;
    spec.in_bytes = wrapped->stdin_bytes;
    spec.in_len = wrapped->stdin_len;
    spec.timeout_ms = verb->timeout_ms;
    spec.display = verb->name;
    spec.internal = true;
    spec.collect_max = YEW_GIT_COLLECT_MAX;
    spec.env_set = env_base;
    spec.env_unset = env_unset;
    spec.env_unset_prefix = env_unset_prefix;
    spec.callback_owner = wrapped;
    spec.callback_ops = &git_callback_ops;
    id = yew_job_spawn(ed, &spec, err, errsz);
    free(final_argv);
    if (id == 0U) {
        if (wrapped->stdin_bytes != NULL) {
            yew_memzero(wrapped->stdin_bytes, (size_t)wrapped->stdin_len);
            free(wrapped->stdin_bytes);
        }
        free(wrapped);
    }
    return id;
}

u32 yew_git_spawn_callback(Ed *ed, const GitVerb *verb,
                           char *const *argv, void *owner,
                           const YewJobCallbackOps *ops,
                           char *err, size_t errsz)
{
    return yew_git_spawn_callback_input(ed, verb, argv, NULL, 0U,
                                        owner, ops, err, errsz);
}

static void git_blob_rx_consume(GitBlobBatch *batch, size_t n)
{
    if (n < batch->rx.len)
        (void)memmove(batch->rx.data, batch->rx.data + n,
                      batch->rx.len - n);
    batch->rx.len -= n;
}

static void git_blob_complete(GitBlobBatch *batch, YewGitBlobState state)
{
    GitBlobRequest *request = batch->head;
    YewGitBlobResult result;

    if (request == NULL)
        return;
    batch->head = request->next;
    if (batch->head == NULL)
        batch->tail = NULL;
    result.state = state;
    result.bytes = state == YEW_GIT_BLOB_OK ? batch->body.data : NULL;
    result.len = state == YEW_GIT_BLOB_OK ? batch->body_len : 0U;
    result.request_id = request->id;
    request->callback(request->owner, &result);
    free(request->query);
    free(request);
    batch->body.len = 0U;
    batch->body_len = 0U;
    batch->body_got = 0U;
    batch->in_body = false;
    batch->oversized = false;
}

static void git_blob_fail_all(GitBlobBatch *batch, YewGitBlobState state)
{
    while (batch->head != NULL)
        git_blob_complete(batch, state);
}

static bool git_blob_hex(const u8 *s, size_t n)
{
    size_t i;

    if (n != 40U && n != 64U)
        return false;
    for (i = 0U; i < n; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'a' && s[i] <= 'f') ||
              (s[i] >= 'A' && s[i] <= 'F')))
            return false;
    }
    return true;
}

static bool git_blob_size(const u8 *s, size_t n, u64 *out)
{
    u64 value = 0U;
    size_t i;

    if (n == 0U)
        return false;
    for (i = 0U; i < n; i++) {
        u64 digit;

        if (s[i] < '0' || s[i] > '9')
            return false;
        digit = (u64)(s[i] - '0');
        if (value > (UINT64_MAX - digit) / 10U)
            return false;
        value = value * 10U + digit;
    }
    *out = value;
    return true;
}

static bool git_blob_header(GitBlobBatch *batch, const u8 *line, size_t n)
{
    static const char missing[] = " missing";
    size_t first = 0U;
    size_t second;
    u64 len;

    if (batch->head == NULL)
        return false;
    if (n > sizeof(missing) - 1U &&
        memcmp(line + n - (sizeof(missing) - 1U), missing,
               sizeof(missing) - 1U) == 0) {
        if (n != batch->head->query_len + sizeof(missing) - 1U ||
            memcmp(line, batch->head->query, batch->head->query_len) != 0)
            return false;
        git_blob_complete(batch, YEW_GIT_BLOB_MISSING);
        return true;
    }
    while (first < n && line[first] != ' ')
        first++;
    if (!git_blob_hex(line, first) || first == n)
        return false;
    second = first + 1U;
    if (n - second < 6U || memcmp(line + second, "blob ", 5U) != 0 ||
        !git_blob_size(line + second + 5U, n - second - 5U, &len))
        return false;
    batch->body_len = len;
    batch->body_got = 0U;
    batch->body.len = 0U;
    batch->oversized = len > YEW_GIT_BLOB_MAX || len > (u64)SIZE_MAX;
    if (!batch->oversized)
        bytebuf_reserve(&batch->body, (size_t)len);
    batch->in_body = true;
    return true;
}

static bool git_blob_parse(GitBlobBatch *batch)
{
    for (;;) {
        if (!batch->in_body) {
            u8 *newline = memchr(batch->rx.data, '\n', batch->rx.len);
            size_t line_len;

            if (newline == NULL)
                return batch->rx.len <= 4096U;
            line_len = (size_t)(newline - batch->rx.data);
            if (!git_blob_header(batch, batch->rx.data, line_len))
                return false;
            git_blob_rx_consume(batch, line_len + 1U);
            if (!batch->in_body)
                continue;
        }
        if (batch->body_got < batch->body_len) {
            u64 remaining = batch->body_len - batch->body_got;
            size_t take = batch->rx.len;

            if ((u64)take > remaining)
                take = (size_t)remaining;
            if (take == 0U)
                return true;
            if (!batch->oversized)
                bytebuf_append(&batch->body, batch->rx.data, take);
            batch->body_got += (u64)take;
            git_blob_rx_consume(batch, take);
            if (batch->body_got < batch->body_len)
                return true;
        }
        if (batch->rx.len == 0U)
            return true;
        if (batch->rx.data[0] != '\n')
            return false;
        git_blob_rx_consume(batch, 1U);
        git_blob_complete(batch, batch->oversized ?
                          YEW_GIT_BLOB_TOO_LARGE : YEW_GIT_BLOB_OK);
    }
}

static bool git_blob_feed(void *owner, const u8 *bytes, u64 len)
{
    GitBlobBatch *batch = owner;

    if (batch->failed || len > (u64)SIZE_MAX ||
        len > (u64)SIZE_MAX - (u64)batch->rx.len)
        return false;
    bytebuf_append(&batch->rx, bytes, (size_t)len);
    if (git_blob_parse(batch))
        return true;
    batch->failed = true;
    git_blob_fail_all(batch, YEW_GIT_BLOB_PARSE);
    return false;
}

static bool git_blob_finish(void *owner)
{
    GitBlobBatch *batch = owner;

    if (!batch->failed) {
        batch->failed = true;
        git_blob_fail_all(batch, YEW_GIT_BLOB_FAILED);
    }
    return false;
}

static u64 git_blob_tx_view(void *owner, const u8 **bytes)
{
    GitBlobBatch *batch = owner;

    if (batch->tx_off >= (u64)batch->tx.len) {
        *bytes = NULL;
        return 0U;
    }
    *bytes = batch->tx.data + (size_t)batch->tx_off;
    return (u64)batch->tx.len - batch->tx_off;
}

static void git_blob_tx_consume(void *owner, u64 len)
{
    GitBlobBatch *batch = owner;
    u64 remain = (u64)batch->tx.len - batch->tx_off;

    batch->tx_off += len < remain ? len : remain;
    if (batch->tx_off == (u64)batch->tx.len) {
        batch->tx.len = 0U;
        batch->tx_off = 0U;
    }
}

static void git_blob_destroy(void *owner)
{
    GitBlobBatch *batch = owner;

    if (!batch->failed)
        git_blob_fail_all(batch, YEW_GIT_BLOB_FAILED);
    if (batch->ed != NULL && batch->ed->git != NULL &&
        batch->ed->git->blob_batch == batch)
        batch->ed->git->blob_batch = NULL;
    bytebuf_free(&batch->rx);
    bytebuf_free(&batch->tx);
    bytebuf_free(&batch->body);
    free(batch);
}

static const YewJobFramedOps git_blob_ops = {
    git_blob_feed,
    git_blob_finish,
    git_blob_tx_view,
    git_blob_tx_consume,
    NULL,
    NULL,
    git_blob_destroy
};

static GitBlobBatch *git_blob_batch_new(Ed *ed)
{
    GitBlobBatch *batch = yew_xcalloc(1U, sizeof(*batch));

    batch->ed = ed;
    bytebuf_init(&batch->rx);
    bytebuf_init(&batch->tx);
    bytebuf_init(&batch->body);
    return batch;
}

static GitBlobBatch *git_blob_batch_ensure(Ed *ed, char *err, size_t errsz)
{
    static const char *const env_set[] = {
        "GIT_TERMINAL_PROMPT=0", "GIT_EDITOR=false",
        "GIT_SEQUENCE_EDITOR=false", "GIT_FLUSH=1", "GIT_PAGER=cat",
        "PAGER=cat", "LC_ALL=C", NULL
    };
    static const char *const env_unset[] = {
        "COLUMNS", "LINES", "GIT_TRACE", "GIT_TRACE_PACKET",
        "GIT_TRACE_PERFORMANCE", "GIT_CURL_VERBOSE", "GIT_TRANSFER_TRACE",
        NULL
    };
    static const char *const env_unset_prefix[] = {"GIT_TRACE2", NULL};
    GitCtx *ctx = ed->git;
    GitBlobBatch *batch;
    YewJobSpec spec = {0};
    const GitVerb *verb = yew_git_verb("blob");
    char *argv[] = {(char *)"cat-file", (char *)"--batch", NULL};
    char **final_argv;

    if (ctx->blob_batch != NULL)
        return ctx->blob_batch;
    if (ctx->detect_state != YEW_GIT_ASYNC_READY ||
        ctx->detect_result != YEW_GIT_OK) {
        git_error(err, errsz, yew_git_state_str(YEW_GIT_NOT_REPO));
        return NULL;
    }
    batch = git_blob_batch_new(ed);
    final_argv = git_build_argv(verb, argv);
    spec.argv = final_argv;
    spec.cwd = yew_ws_root(ed);
    spec.sink = YEW_SINK_FRAMED;
    spec.display = "blob";
    spec.internal = true;
    spec.env_set = env_set;
    spec.env_unset = env_unset;
    spec.env_unset_prefix = env_unset_prefix;
    spec.framed_owner = batch;
    spec.framed_ops = &git_blob_ops;
    batch->job_id = yew_job_spawn(ed, &spec, err, errsz);
    free(final_argv);
    if (batch->job_id == 0U) {
        git_blob_destroy(batch);
        return NULL;
    }
    ctx->blob_batch = batch;
    ctx->blob_spawns++;
    return batch;
}

static bool git_blob_path_valid(const char *path)
{
    const unsigned char *p = (const unsigned char *)path;

    if (path == NULL || path[0] == '\0')
        return false;
    for (; *p != 0U; p++)
        if (*p == '\n' || *p == '\r')
            return false;
    return true;
}

static u32 git_blob_request(Ed *ed, const char *prefix, const char *path,
                            void *owner, YewGitBlobFn callback,
                            char *err, size_t errsz)
{
    GitBlobBatch *batch;
    GitBlobRequest *request;
    size_t prefix_len;
    size_t path_len;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (ed == NULL || ed->git == NULL || callback == NULL ||
        !git_blob_path_valid(path)) {
        git_error(err, errsz, "invalid Git blob request");
        return 0U;
    }
    prefix_len = strlen(prefix);
    path_len = strlen(path);
    if (path_len > SIZE_MAX - prefix_len - 1U) {
        git_error(err, errsz, "Git blob request is too large");
        return 0U;
    }
    batch = git_blob_batch_ensure(ed, err, errsz);
    if (batch == NULL)
        return 0U;
    request = yew_xcalloc(1U, sizeof(*request));
    request->owner = owner;
    request->callback = callback;
    request->query_len = prefix_len + path_len;
    request->query = yew_xmalloc(request->query_len + 1U);
    (void)memcpy(request->query, prefix, prefix_len);
    (void)memcpy(request->query + prefix_len, path, path_len + 1U);
    request->id = ++ed->git->blob_next_request;
    if (request->id == 0U)
        request->id = ++ed->git->blob_next_request;
    if (batch->tail != NULL)
        batch->tail->next = request;
    else
        batch->head = request;
    batch->tail = request;
    if (batch->tx_off == (u64)batch->tx.len) {
        batch->tx.len = 0U;
        batch->tx_off = 0U;
    }
    bytebuf_append(&batch->tx, request->query, request->query_len);
    bytebuf_push_u8(&batch->tx, '\n');
    ed->git->blob_requests++;
    return request->id;
}

u32 yew_git_index_blob(Ed *ed, const char *path, void *owner,
                       YewGitBlobFn callback, char *err, size_t errsz)
{
    return git_blob_request(ed, ":", path, owner, callback, err, errsz);
}

u32 yew_git_head_blob(Ed *ed, const char *path, void *owner,
                      YewGitBlobFn callback, char *err, size_t errsz)
{
    return git_blob_request(ed, "HEAD:", path, owner, callback, err, errsz);
}

bool yew_git_test_blob_batch_open(Ed *ed)
{
    GitBlobBatch *batch;

    if (ed == NULL || ed->git == NULL || ed->git->blob_batch != NULL)
        return false;
    batch = git_blob_batch_new(ed);
    batch->test_only = true;
    ed->git->blob_batch = batch;
    return true;
}

bool yew_git_test_blob_batch_feed(Ed *ed, const u8 *bytes, u64 len)
{
    if (ed == NULL || ed->git == NULL || ed->git->blob_batch == NULL ||
        bytes == NULL)
        return false;
    return git_blob_feed(ed->git->blob_batch, bytes, len);
}

bool yew_git_test_blob_batch_finish(Ed *ed)
{
    if (ed == NULL || ed->git == NULL || ed->git->blob_batch == NULL)
        return false;
    return git_blob_finish(ed->git->blob_batch);
}

u64 yew_git_test_blob_batch_tx(const Ed *ed, const u8 **bytes)
{
    if (bytes != NULL)
        *bytes = NULL;
    if (ed == NULL || ed->git == NULL || ed->git->blob_batch == NULL ||
        bytes == NULL)
        return 0U;
    return git_blob_tx_view(ed->git->blob_batch, bytes);
}

u32 yew_git_test_blob_request_count(const Ed *ed)
{
    return ed == NULL || ed->git == NULL ? 0U : ed->git->blob_requests;
}

u32 yew_git_test_blob_spawn_count(const Ed *ed)
{
    return ed == NULL || ed->git == NULL ? 0U : ed->git->blob_spawns;
}

void yew_git_test_spawn_set(GitTestSpawnFn spawn, void *opaque)
{
    test_spawn = spawn;
    test_spawn_opaque = opaque;
}

void yew_git_test_now_set(Ed *ed, i64 now_ms)
{
    if (ed == NULL || ed->git == NULL)
        return;
    ed->git->test_now = true;
    ed->git->test_now_ms = now_ms;
}

void yew_git_state_init(Ed *ed)
{
    GitCtx *ctx;

    if (ed == NULL || ed->git != NULL)
        return;
    ctx = yew_xcalloc(1U, sizeof(*ctx));
    arena_init(&ctx->log_arena);
    arena_init(&ctx->result_arena);
    arena_init(&ctx->incoming_arena);
    yew_git_snapshot_init(&ctx->snap[0]);
    yew_git_snapshot_init(&ctx->snap[1]);
    ctx->snap[0].ahead = -1;
    ctx->snap[0].behind = -1;
    ctx->snap[1].ahead = -1;
    ctx->snap[1].behind = -1;
    ctx->avail_state = YEW_GIT_ASYNC_UNTRIED;
    ctx->detect_state = YEW_GIT_ASYNC_UNTRIED;
    ctx->detect_result = YEW_GIT_NOT_REPO;
    ed->git = ctx;
}

void yew_git_state_free(Ed *ed)
{
    size_t i;

    if (ed == NULL || ed->git == NULL)
        return;
    if (ed->git->blob_batch != NULL && ed->git->blob_batch->test_only)
        git_blob_destroy(ed->git->blob_batch);
    for (i = 0U; i < YEW_ARRAY_LEN(ed->git->pending); i++) {
        GitPending *pending = &ed->git->pending[i];
        YewJob *job;

        if (!pending->active)
            continue;
        job = yew_job_find(ed, pending->req.job_id);
        if (job != NULL && job->in_bytes == pending->req.stdin_bytes) {
            yew_memzero((void *)job->in_bytes, (size_t)job->in_len);
            job->in_bytes = NULL;
            job->in_len = 0U;
        }
        git_pending_drop(ed->git, &ed->git->pending[i]);
    }
    git_repo_clear(&ed->git->repo);
    free(ed->git->detected_root);
    arena_free_all(&ed->git->log_arena);
    arena_free_all(&ed->git->result_arena);
    git_incoming_clear(ed->git);
    arena_free_all(&ed->git->incoming_arena);
    yew_git_snapshot_drop(&ed->git->snap[0]);
    yew_git_snapshot_drop(&ed->git->snap[1]);
    free(ed->git);
    ed->git = NULL;
}

GitAsyncState yew_git_avail_state(const Ed *ed)
{
    return ed == NULL || ed->git == NULL ? YEW_GIT_ASYNC_FAILED :
           ed->git->avail_state;
}

GitAsyncState yew_git_detect_state(const Ed *ed)
{
    return ed == NULL || ed->git == NULL ? YEW_GIT_ASYNC_FAILED :
           ed->git->detect_state;
}

GitStatusCode yew_git_detect_result(const Ed *ed)
{
    return ed == NULL || ed->git == NULL ? YEW_GIT_FAILED :
           ed->git->detect_result;
}

static bool git_parse_version(const u8 *buf, u64 n, GitVersion *out)
{
    static const char prefix[] = "git version ";
    u64 at = sizeof(prefix) - 1U;
    unsigned long part[3] = {0UL, 0UL, 0UL};
    size_t i;

    if (n < at || memcmp(buf, prefix, (size_t)at) != 0)
        return false;
    for (i = 0U; i < 3U; i++) {
        bool digit = false;

        while (at < n && buf[at] >= '0' && buf[at] <= '9') {
            digit = true;
            if (part[i] > (ULONG_MAX - (unsigned long)(buf[at] - '0')) / 10UL)
                return false;
            part[i] = part[i] * 10UL + (unsigned long)(buf[at++] - '0');
        }
        if (!digit || part[i] > UINT16_MAX)
            return false;
        if (i != 2U) {
            if (at >= n || buf[at++] != '.')
                return false;
        }
    }
    out->major = (u16)part[0];
    out->minor = (u16)part[1];
    out->patch = (u16)part[2];
    return true;
}

static bool git_version_supported(const GitVersion *v)
{
    return v->major > 2U || (v->major == 2U && v->minor >= 20U);
}

bool yew_git_avail(Ed *ed, GitVersion *out)
{
    char *argv[] = {(char *)"--version", NULL};
    GitReq req = {0};
    char err[128];

    if (ed == NULL || ed->git == NULL)
        return false;
    if (ed->git->avail_state == YEW_GIT_ASYNC_READY) {
        if (out != NULL)
            *out = ed->git->version;
        return true;
    }
    if (ed->git->avail_state == YEW_GIT_ASYNC_UNTRIED) {
        req.kind = YEW_GREQ_VERSION;
        if (yew_git_spawn(ed, yew_git_verb("version"), argv, &req, err,
                          sizeof(err)) != 0U)
            ed->git->avail_state = YEW_GIT_ASYNC_PENDING;
        else
            ed->git->avail_state = YEW_GIT_ASYNC_FAILED;
    }
    return false;
}

static bool git_line(const u8 *buf, u64 n, u64 *at, const u8 **p, size_t *len)
{
    u64 start = *at;

    while (*at < n && buf[*at] != '\n')
        (*at)++;
    if (*at == start && *at == n)
        return false;
    *p = buf + start;
    *len = (size_t)(*at - start);
    if (*at < n)
        (*at)++;
    return true;
}

static bool git_parse_bool_line(const u8 *p, size_t n, bool *out)
{
    if (n == 4U && memcmp(p, "true", 4U) == 0) {
        *out = true;
        return true;
    }
    if (n == 5U && memcmp(p, "false", 5U) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool git_parse_detect(GitRepo *repo, const u8 *buf, u64 n,
                             const char *cwd, u64 env_fp)
{
    const u8 *line[4];
    size_t len[4];
    u64 at = 0U;
    size_t count = 0U;
    bool inside;
    bool bare;
    char *git_dir;
    char *top = NULL;

    while (count < 4U && git_line(buf, n, &at, &line[count], &len[count]))
        count++;
    if (count < 3U || !git_parse_bool_line(line[1], len[1], &inside) ||
        !git_parse_bool_line(line[2], len[2], &bare))
        return false;
    if (count == 4U)
        top = git_dup_bytes(line[3], len[3]);
    if (!bare && top == NULL)
        return false;
    git_dir = git_dup_bytes(line[0], len[0]);
    if (git_dir[0] != '/') {
        char *absolute = git_join(cwd, git_dir);

        free(git_dir);
        git_dir = absolute;
    }
    git_repo_clear(repo);
    repo->git_dir = git_dir;
    repo->top_level = top;
    repo->inside_work_tree = inside;
    repo->bare = bare;
    repo->env_fp = env_fp;
    return true;
}

GitStatusCode yew_git_detect(Ed *ed, GitRepo *out)
{
    char *argv[] = {
        (char *)"rev-parse", (char *)"--path-format=absolute",
        (char *)"--git-dir", (char *)"--is-inside-work-tree",
        (char *)"--is-bare-repository", (char *)"--show-toplevel",
        NULL
    };
    GitReq req = {0};
    char err[128];
    u64 fp;

    if (ed == NULL || ed->git == NULL)
        return YEW_GIT_FAILED;
    if (ed->git->avail_state != YEW_GIT_ASYNC_READY) {
        ed->git->detect_requested = true;
        (void)yew_git_avail(ed, NULL);
        return YEW_GIT_NO_GIT;
    }
    fp = yew_git_env_fingerprint();
    if ((ed->git->detect_state == YEW_GIT_ASYNC_READY ||
         ed->git->detect_state == YEW_GIT_ASYNC_FAILED) &&
        ed->git->repo.env_fp == fp && ed->git->detected_root != NULL &&
        strcmp(ed->git->detected_root, yew_ws_root(ed)) == 0) {
        if (out != NULL && ed->git->detect_state == YEW_GIT_ASYNC_READY)
            *out = ed->git->repo;
        return ed->git->detect_result;
    }
    if (ed->git->detect_state != YEW_GIT_ASYNC_PENDING) {
        req.kind = YEW_GREQ_DETECT;
        if (yew_git_spawn(ed, yew_git_verb("detect"), argv, &req, err,
                          sizeof(err)) != 0U)
            ed->git->detect_state = YEW_GIT_ASYNC_PENDING;
        else
            ed->git->detect_state = YEW_GIT_ASYNC_FAILED;
    }
    return ed->git->detect_result;
}

const GitRepo *yew_git_repo_cached(const Ed *ed)
{
    if (ed == NULL || ed->git == NULL ||
        ed->git->detect_state != YEW_GIT_ASYNC_READY)
        return NULL;
    return &ed->git->repo;
}

static bool git_spawn_ignore(Ed *ed)
{
    char *argv[] = {
        (char *)"ls-files", (char *)"-z", (char *)"--others",
        (char *)"--ignored", (char *)"--exclude-standard",
        (char *)"--directory", (char *)"--no-empty-directory", NULL
    };
    GitReq req = {0};
    char err[128];

    req.kind = YEW_GREQ_IGNORE;
    return yew_git_spawn(ed, yew_git_verb("ignore"), argv, &req, err,
                         sizeof(err)) != 0U;
}

static bool git_spawn_status(Ed *ed)
{
    char *argv[] = {
        (char *)"status", (char *)"--porcelain=v2", (char *)"--branch",
        (char *)"-z", (char *)"--untracked-files=normal",
        (char *)"--ignored=no", NULL
    };
    GitReq req = {0};
    char err[128];

    req.kind = YEW_GREQ_STATUS;
    return yew_git_spawn(ed, yew_git_verb("status"), argv, &req, err,
                         sizeof(err)) != 0U;
}

static bool git_spawn_comment_char(Ed *ed)
{
    char *argv[] = {
        (char *)"config", (char *)"--get", (char *)"core.commentChar",
        NULL
    };
    GitReq req = {0};
    char err[128];

    req.kind = YEW_GREQ_VERB;
    return yew_git_spawn(ed, yew_git_verb("comment-char"), argv, &req,
                         err, sizeof(err)) != 0U;
}

static bool git_spawn_incoming(Ed *ed)
{
    char *argv[] = {
        (char *)"diff", (char *)"--name-only", (char *)"-z",
        (char *)"HEAD...@{upstream}", NULL
    };
    GitReq req = {0};
    char err[128];

    req.kind = YEW_GREQ_INCOMING;
    return yew_git_spawn(ed, yew_git_verb("incoming"), argv, &req, err,
                         sizeof(err)) != 0U;
}

static bool git_spawn_upstream(Ed *ed)
{
    char *argv[] = {
        (char *)"rev-parse", (char *)"--verify", (char *)"@{upstream}", NULL
    };
    GitReq req = {0};
    char err[128];

    req.kind = YEW_GREQ_UPSTREAM;
    return yew_git_spawn(ed, yew_git_verb("upstream-oid"), argv, &req, err,
                         sizeof(err)) != 0U;
}

static int git_path_bytes_cmp(const char *ap, u32 an, const char *bp, u32 bn)
{
    size_t n = an < bn ? an : bn;
    int cmp = n == 0U ? 0 : memcmp(ap, bp, n);

    if (cmp != 0)
        return cmp;
    return an < bn ? -1 : an != bn;
}

static int git_entry_cmp(const void *av, const void *bv, void *ctx)
{
    const GitEntry *a = av;
    const GitEntry *b = bv;

    (void)ctx;
    return git_path_bytes_cmp(a->path, a->path_len, b->path, b->path_len);
}

static int git_path_cmp(const void *av, const void *bv, void *ctx)
{
    const GitPath *a = av;
    const GitPath *b = bv;

    (void)ctx;
    return git_path_bytes_cmp(a->path, a->len, b->path, b->len);
}

static void git_incoming_clear(GitCtx *ctx)
{
    arena_free_all(&ctx->incoming_arena);
    arena_init(&ctx->incoming_arena);
    ctx->incoming_paths.data = NULL;
    ctx->incoming_paths.len = 0U;
    free(ctx->upstream_oid);
    ctx->upstream_oid = NULL;
    free(ctx->pending_upstream_oid);
    ctx->pending_upstream_oid = NULL;
}

static bool git_incoming_cache(GitCtx *ctx, const u8 *out, u64 out_len)
{
    GitParseErr err;

    arena_free_all(&ctx->incoming_arena);
    arena_init(&ctx->incoming_arena);
    ctx->incoming_paths.data = NULL;
    ctx->incoming_paths.len = 0U;
    if (!yew_git_parse_z_paths(&ctx->incoming_arena, out, out_len,
                               &ctx->incoming_paths, &err))
        return false;
    yew_sort_stable(ctx->incoming_paths.data, ctx->incoming_paths.len,
                    sizeof(*ctx->incoming_paths.data), git_path_cmp, NULL);
    return true;
}

static char *git_parse_oid(const u8 *out, u64 out_len)
{
    size_t n;
    size_t i;

    while (out_len != 0U && (out[out_len - 1U] == '\n' ||
                             out[out_len - 1U] == '\r'))
        out_len--;
    if (out_len != 40U && out_len != 64U)
        return NULL;
    n = (size_t)out_len;
    for (i = 0U; i < n; i++) {
        if (!((out[i] >= '0' && out[i] <= '9') ||
              (out[i] >= 'a' && out[i] <= 'f') ||
              (out[i] >= 'A' && out[i] <= 'F')))
            return NULL;
    }
    return git_dup_bytes(out, n);
}

static bool git_cache_comment_char(GitSnapshot *snap, GitStatusCode state,
                                   int exit_code, const u8 *out, u64 out_len)
{
    size_t len;

    if (snap == NULL)
        return false;
    /* `git config --get` uses exit 1 for an absent key.  The snapshot was
     * initialized with Git's default '#', so there is nothing to copy. */
    if (state == YEW_GIT_FAILED && exit_code == 1)
        return true;
    if (state != YEW_GIT_OK || out == NULL || out_len > (u64)SIZE_MAX)
        return false;
    len = (size_t)out_len;
    if (len != 0U && out[len - 1U] == (u8)'\n') {
        len--;
        if (len != 0U && out[len - 1U] == (u8)'\r')
            len--;
    }
    if (!((len == 1U && out[0] != 0U && out[0] != (u8)'\n' &&
           out[0] != (u8)'\r') ||
          (len == 4U && memcmp(out, "auto", 4U) == 0)))
        return false;
    snap->comment_char = arena_strndup(&snap->a, (const char *)out, len);
    snap->comment_char_len = (u32)len;
    return true;
}

static bool git_continue_refresh_after_comment(Ed *ed)
{
    GitCtx *ctx = ed->git;
    GitSnapshot *next = &ctx->snap[ctx->live ^ 1U];

    if (next->upstream != NULL && next->upstream[0] != '\0')
        return git_spawn_upstream(ed);
    git_incoming_clear(ctx);
    git_publish_refresh(ed);
    return true;
}

static bool git_merge_incoming(GitSnapshot *snap, const GitPathList *paths)
{
    GitEntry *merged;
    size_t cap;
    size_t out = 0U;
    size_t ei = 0U;
    size_t pi = 0U;

    if (paths->len > SIZE_MAX - snap->entries.len)
        return false;
    cap = snap->entries.len + paths->len;
    if (cap == 0U)
        return true;
    merged = arena_alloc(&snap->a, cap * sizeof(*merged),
                         _Alignof(GitEntry));
    yew_sort_stable(snap->entries.data, snap->entries.len,
                    sizeof(*snap->entries.data), git_entry_cmp, NULL);
    while (ei < snap->entries.len || pi < paths->len) {
        int cmp;

        if (ei == snap->entries.len) {
            cmp = 1;
        } else if (pi == paths->len) {
            cmp = -1;
        } else {
            cmp = git_path_bytes_cmp(snap->entries.data[ei].path,
                                     snap->entries.data[ei].path_len,
                                     paths->data[pi].path,
                                     paths->data[pi].len);
        }
        if (cmp < 0) {
            merged[out++] = snap->entries.data[ei++];
        } else if (cmp == 0) {
            merged[out] = snap->entries.data[ei++];
            merged[out++].incoming = true;
            pi++;
        } else {
            GitEntry entry = {0};

            entry.kind = GIT_E_ORDINARY;
            entry.x = '.';
            entry.y = '.';
            entry.path = arena_strndup(&snap->a, paths->data[pi].path,
                                       paths->data[pi].len);
            entry.path_len = paths->data[pi].len;
            entry.is_dir = paths->data[pi].is_dir;
            entry.incoming = true;
            merged[out++] = entry;
            pi++;
        }
        while (pi > 0U && pi < paths->len &&
               git_path_bytes_cmp(paths->data[pi - 1U].path,
                                  paths->data[pi - 1U].len,
                                  paths->data[pi].path,
                                  paths->data[pi].len) == 0)
            pi++;
    }
    snap->entries.data = merged;
    snap->entries.len = out;
    return true;
}

static void git_publish_refresh(Ed *ed)
{
    GitCtx *ctx = ed->git;
    GitSnapshot *next = &ctx->snap[ctx->live ^ 1U];
    GitStatusCode probe = yew_git_probe_state(&ctx->repo);
    GitStatusCode operation = git_operation_state(&ctx->repo);
    bool again;
    size_t i;

    next->conflict_count = 0U;
    for (i = 0U; i < next->entries.len; i++) {
        if (next->entries.data[i].conflicted)
            next->conflict_count++;
    }
    next->rebase_step = 0U;
    next->rebase_total = 0U;
    if (operation == YEW_GIT_MID_REBASE) {
        (void)git_read_u32(ctx->repo.git_dir, "rebase-merge/msgnum",
                           &next->rebase_step);
        (void)git_read_u32(ctx->repo.git_dir, "rebase-merge/end",
                           &next->rebase_total);
    }
    if (operation != YEW_GIT_OK)
        next->state = operation;
    else if (probe != YEW_GIT_OK)
        next->state = probe;
    next->gen = ctx->snap[ctx->live].gen + 1U;
    next->taken_ms = git_now(ed);
    ctx->live ^= 1U;
    ctx->refresh_failed = false;
    /* The statusline and editor gutter are cache consumers: publishing a
     * snapshot must wake both without making either render path poll or
     * spawn.  In particular, the first paint happens before asynchronous
     * repository discovery completes, so a document repaint is what gives
     * the gutter its first chance to request the index blob. */
    ed->footer_dirty = true;
    ed->full_damage = true;
    again = ctx->refresh_again;
    ctx->refresh_inflight = false;
    ctx->refresh_again = false;
    if (ctx->log_requested) {
        ctx->log_requested = false;
        if (ctx->snap[ctx->live].unborn) {
            yew_msg(ed, YEW_MSG_ERROR, "%s",
                    yew_git_state_str(YEW_GIT_NO_HEAD));
        } else {
            (void)git_spawn_log(ed);
        }
    }
    if (again)
        (void)yew_git_refresh(ed, true);
}

bool yew_git_refresh(Ed *ed, bool force)
{
    GitCtx *ctx;
    const GitSnapshot *live;

    if (ed == NULL || ed->git == NULL)
        return false;
    ctx = ed->git;
    if (force)
        ctx->forced = true;
    if (ctx->avail_state != YEW_GIT_ASYNC_READY) {
        if (ctx->avail_state == YEW_GIT_ASYNC_FAILED && !force)
            return false;
        ctx->refresh_again = true;
        if (ctx->avail_state == YEW_GIT_ASYNC_FAILED)
            ctx->avail_state = YEW_GIT_ASYNC_UNTRIED;
        (void)yew_git_avail(ed, NULL);
        return ctx->avail_state == YEW_GIT_ASYNC_PENDING;
    }
    if (!force && ctx->detect_state == YEW_GIT_ASYNC_FAILED &&
        ctx->repo.env_fp == yew_git_env_fingerprint() &&
        ctx->detected_root != NULL &&
        strcmp(ctx->detected_root, yew_ws_root(ed)) == 0)
        return false;
    if (force && ctx->detect_state == YEW_GIT_ASYNC_FAILED)
        ctx->detect_state = YEW_GIT_ASYNC_UNTRIED;
    if (ctx->detect_state != YEW_GIT_ASYNC_READY ||
        ctx->repo.env_fp != yew_git_env_fingerprint() ||
        ctx->detected_root == NULL ||
        strcmp(ctx->detected_root, yew_ws_root(ed)) != 0) {
        ctx->refresh_again = true;
        (void)yew_git_detect(ed, NULL);
        return ctx->detect_state == YEW_GIT_ASYNC_PENDING;
    }
    if (ctx->refresh_inflight) {
        /* Rendering polls the published snapshot on every keypress.  Those
         * ordinary reads are already satisfied by the in-flight refresh;
         * only an explicit/invalidating force request earns one follow-up. */
        if (force)
            ctx->refresh_again = true;
        return false;
    }
    /* Keep a failed asynchronous refresh from becoming a tight poll-loop.
     * The published ping-pong snapshot stays untouched, but an ordinary
     * TTL poll waits one full cache interval before trying again.  Explicit
     * invalidations still bypass the backoff. */
    if (!force && ctx->refresh_failed &&
        yew_git_cache_fresh(ctx->refresh_failed_ms, git_now(ed)))
        return false;
    live = &ctx->snap[ctx->live];
    if (!ctx->forced && live->gen != 0U &&
        yew_git_cache_fresh(live->taken_ms, git_now(ed)))
        return false;
    if (yew_git_probe_state(&ctx->repo) == YEW_GIT_LOCKED)
        return false;
    ctx->refresh_inflight = true;
    ctx->forced = false;
    if (!git_spawn_status(ed)) {
        git_refresh_failed(ed);
        return false;
    }
    return true;
}

const GitSnapshot *yew_git_snapshot(Ed *ed)
{
    if (ed == NULL || ed->git == NULL)
        return NULL;
    (void)yew_git_refresh(ed, false);
    return &ed->git->snap[ed->git->live];
}

const GitSnapshot *yew_git_snapshot_cached(const Ed *ed)
{
    const GitSnapshot *snap;

    if (ed == NULL || ed->git == NULL)
        return NULL;
    snap = &ed->git->snap[ed->git->live];
    return snap->gen == 0U ? NULL : snap;
}

GitSnapshot *yew_git_test_snapshot_mut(Ed *ed)
{
    if (ed == NULL || ed->git == NULL)
        return NULL;
    return &ed->git->snap[ed->git->live];
}

const GitLogRecordList *yew_git_log_records(const Ed *ed)
{
    return ed == NULL || ed->git == NULL ? NULL : &ed->git->log_records;
}

const GitResult *yew_git_result(const Ed *ed)
{
    return ed == NULL || ed->git == NULL ? NULL : &ed->git->result;
}

static bool git_result_copy(Arena *arena, const u8 *src, u64 len,
                            const u8 **dst)
{
    u8 *copy;

    *dst = NULL;
    if (len == 0U)
        return true;
    if (src == NULL || len > (u64)SIZE_MAX)
        return false;
    copy = arena_alloc(arena, (size_t)len, _Alignof(u8));
    (void)memcpy(copy, src, (size_t)len);
    *dst = copy;
    return true;
}

static void git_publish_result(GitCtx *ctx, u32 job_id,
                               const char *verb, GitStatusCode state,
                               const u8 *out, u64 out_len,
                               const u8 *err, u64 err_len)
{
    arena_free_all(&ctx->result_arena);
    arena_init(&ctx->result_arena);
    (void)memset(&ctx->result, 0, sizeof(ctx->result));
    ctx->result.state = state;
    ctx->result.job_id = job_id;
    ctx->result.verb = arena_strdup(&ctx->result_arena, verb);
    if (!git_result_copy(&ctx->result_arena, out, out_len,
                         &ctx->result.out) ||
        !git_result_copy(&ctx->result_arena, err, err_len,
                         &ctx->result.err)) {
        arena_free_all(&ctx->result_arena);
        arena_init(&ctx->result_arena);
        (void)memset(&ctx->result, 0, sizeof(ctx->result));
        ctx->result.state = YEW_GIT_PARSE;
        ctx->result.job_id = job_id;
        ctx->result.verb = arena_strdup(&ctx->result_arena, verb);
        return;
    }
    ctx->result.out_len = out_len;
    ctx->result.err_len = err_len;
}

u32 yew_git_blob(Ed *ed, const char *oid, char *err, size_t errsz)
{
    char *argv[4];
    GitReq req = {0};
    size_t i;
    size_t len;

    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    if (oid == NULL) {
        git_error(err, errsz, "invalid git object id");
        return 0U;
    }
    len = strlen(oid);
    if (len != 40U && len != 64U) {
        git_error(err, errsz, "invalid git object id");
        return 0U;
    }
    for (i = 0U; i < len; i++) {
        if (!((oid[i] >= '0' && oid[i] <= '9') ||
              (oid[i] >= 'a' && oid[i] <= 'f') ||
              (oid[i] >= 'A' && oid[i] <= 'F'))) {
            git_error(err, errsz, "invalid git object id");
            return 0U;
        }
    }
    argv[0] = (char *)"cat-file";
    argv[1] = (char *)"blob";
    argv[2] = (char *)oid;
    argv[3] = NULL;
    req.kind = YEW_GREQ_BLOB;
    return yew_git_spawn(ed, yew_git_verb("blob"), argv, &req, err, errsz);
}

void yew_git_invalidate(Ed *ed)
{
    if (ed == NULL || ed->git == NULL)
        return;
    ed->git->forced = true;
    if (ed->git->refresh_inflight)
        ed->git->refresh_again = true;
}

bool yew_git_job_owned(const Ed *ed, u32 job_id)
{
    size_t i;

    if (ed == NULL || ed->git == NULL || job_id == 0U)
        return false;
    if (ed->git->blob_batch != NULL &&
        ed->git->blob_batch->job_id == job_id)
        return true;
    for (i = 0U; i < YEW_ARRAY_LEN(ed->git->pending); i++) {
        const GitPending *pending = &ed->git->pending[i];

        if (pending->active && pending->req.job_id == job_id)
            return true;
    }
    for (i = 0U; i < ed->jobs.len; i++) {
        const YewJob *job = &ed->jobs.v[i];

        if (job->id == job_id && job->sink == YEW_SINK_CALLBACK &&
            job->callback_ops == &git_callback_ops)
            return true;
    }
    return false;
}

static void git_refresh_failed(Ed *ed)
{
    GitCtx *ctx = ed->git;
    bool again = ctx->refresh_again;

    ctx->refresh_inflight = false;
    ctx->refresh_again = false;
    ctx->refresh_failed = true;
    ctx->refresh_failed_ms = git_now(ed);
    if (again)
        (void)yew_git_refresh(ed, true);
}

bool yew_git_test_complete_exit(Ed *ed, u32 job_id, GitStatusCode state,
                                int exit_code, const u8 *out, u64 out_len,
                                const u8 *err, u64 err_len)
{
    GitCtx *ctx;
    GitPending *pending;
    GitReqKind kind;
    GitVerbKind verb_kind;
    const char *verb_name;
    bool retried;
    char *detect_root = NULL;
    u64 detect_env_fp = 0U;

    (void)err;
    (void)err_len;
    if (ed == NULL || ed->git == NULL)
        return false;
    ctx = ed->git;
    pending = git_pending_find(ctx, job_id);
    if (pending == NULL)
        return false;
    kind = pending->req.kind;
    verb_kind = pending->verb->kind;
    verb_name = pending->verb->name;
    retried = pending->req.retried;
    if (kind == YEW_GREQ_DETECT && state == YEW_GIT_FAILED &&
        exit_code != 129)
        retried = true;
    if (kind == YEW_GREQ_DETECT) {
        detect_root = git_dup_bytes((const u8 *)pending->req.detect_root,
                                    strlen(pending->req.detect_root));
        detect_env_fp = pending->req.detect_env_fp;
    }
    git_publish_result(ctx, job_id, verb_name, state, out, out_len,
                       err, err_len);
    git_pending_drop(ctx, pending);
    if (verb_kind == YEW_GV_NET)
        ctx->incoming_dirty = true;
    if (verb_kind != YEW_GV_READ) {
        yew_git_invalidate(ed);
        (void)yew_git_refresh(ed, true);
    }

    if (kind == YEW_GREQ_VERSION) {
        GitVersion version;

        if (state == YEW_GIT_OK && git_parse_version(out, out_len, &version) &&
            git_version_supported(&version)) {
            ctx->version = version;
            ctx->avail_state = YEW_GIT_ASYNC_READY;
        } else {
            ctx->avail_state = YEW_GIT_ASYNC_FAILED;
            git_publish_result(ctx, job_id, verb_name, YEW_GIT_NO_GIT,
                               out, out_len, err, err_len);
            if (ctx->log_requested) {
                ctx->log_requested = false;
                yew_msg(ed, YEW_MSG_ERROR, "%s",
                        yew_git_state_str(YEW_GIT_NO_GIT));
            }
        }
        if (ctx->detect_requested && ctx->avail_state == YEW_GIT_ASYNC_READY) {
            ctx->detect_requested = false;
            (void)yew_git_detect(ed, NULL);
        }
        if (ctx->refresh_again && ctx->avail_state == YEW_GIT_ASYNC_READY) {
            ctx->refresh_again = false;
            (void)yew_git_refresh(ed, true);
        }
        return true;
    }
    if (kind == YEW_GREQ_DETECT) {
        bool bare_partial_ok = state == YEW_GIT_FAILED && exit_code == 128;
        bool parsed = (state == YEW_GIT_OK || bare_partial_ok) &&
                      git_parse_detect(&ctx->repo, out, out_len, detect_root,
                                       detect_env_fp);

        if (parsed && (state == YEW_GIT_OK ||
                       (bare_partial_ok && ctx->repo.bare))) {
            ctx->repo.detected_ms = git_now(ed);
            free(ctx->detected_root);
            ctx->detected_root = git_dup_bytes((const u8 *)detect_root,
                                                strlen(detect_root));
            ctx->detect_result = ctx->repo.bare ? YEW_GIT_BARE : YEW_GIT_OK;
            ctx->detect_state = YEW_GIT_ASYNC_READY;
        } else if (state == YEW_GIT_FAILED && !retried) {
            char *argv[] = {
                (char *)"rev-parse", (char *)"--git-dir",
                (char *)"--is-inside-work-tree",
                (char *)"--is-bare-repository", (char *)"--show-toplevel",
                NULL
            };
            GitReq req = {0};
            char spawn_err[128];

            req.kind = YEW_GREQ_DETECT;
            req.retried = true;
            if (yew_git_spawn(ed, yew_git_verb("detect"), argv, &req,
                              spawn_err, sizeof(spawn_err)) != 0U) {
                free(detect_root);
                return true;
            }
            git_repo_clear(&ctx->repo);
            ctx->repo.env_fp = detect_env_fp;
            free(ctx->detected_root);
            ctx->detected_root = git_dup_bytes((const u8 *)detect_root,
                                                strlen(detect_root));
            ctx->detect_result = YEW_GIT_NOT_REPO;
            ctx->detect_state = YEW_GIT_ASYNC_FAILED;
        } else {
            git_repo_clear(&ctx->repo);
            ctx->repo.env_fp = detect_env_fp;
            free(ctx->detected_root);
            ctx->detected_root = git_dup_bytes((const u8 *)detect_root,
                                                strlen(detect_root));
            ctx->detect_result = state == YEW_GIT_NO_GIT ?
                                 YEW_GIT_NO_GIT : YEW_GIT_NOT_REPO;
            ctx->detect_state = YEW_GIT_ASYNC_FAILED;
        }
        if ((ctx->detect_state == YEW_GIT_ASYNC_READY ||
             ctx->detect_state == YEW_GIT_ASYNC_FAILED) &&
            (detect_env_fp != yew_git_env_fingerprint() ||
             strcmp(detect_root, yew_ws_root(ed)) != 0)) {
            ctx->detect_state = YEW_GIT_ASYNC_UNTRIED;
            (void)yew_git_detect(ed, NULL);
            free(detect_root);
            return true;
        }
        git_publish_result(ctx, job_id, verb_name, ctx->detect_result,
                           out, out_len, err, err_len);
        if (ctx->refresh_again && ctx->detect_state == YEW_GIT_ASYNC_READY &&
            ctx->detect_result != YEW_GIT_BARE) {
            ctx->refresh_again = false;
            (void)yew_git_refresh(ed, true);
        }
        if (ctx->log_requested && ctx->detect_state == YEW_GIT_ASYNC_READY &&
            ctx->detect_result == YEW_GIT_OK) {
            (void)yew_git_refresh(ed, true);
        }
        free(detect_root);
        return true;
    }
    if (kind == YEW_GREQ_STATUS) {
        GitSnapshot *next = &ctx->snap[ctx->live ^ 1U];
        GitParseErr parse_err;

        if (state != YEW_GIT_OK) {
            git_refresh_failed(ed);
            return true;
        }
        yew_git_snapshot_drop(next);
        yew_git_snapshot_init(next);
        if (!yew_git_parse_status(next, out, out_len, &parse_err) ||
            !git_spawn_ignore(ed)) {
            if (state == YEW_GIT_OK)
                git_publish_result(ctx, job_id, verb_name, YEW_GIT_PARSE,
                                   out, out_len, err, err_len);
            yew_git_snapshot_drop(next);
            yew_git_snapshot_init(next);
            git_refresh_failed(ed);
        }
        return true;
    }
    if (kind == YEW_GREQ_IGNORE) {
        GitSnapshot *next = &ctx->snap[ctx->live ^ 1U];
        GitParseErr parse_err;

        if (state != YEW_GIT_OK ||
            !yew_git_parse_ignore(&next->a, out, out_len, &next->ignored,
                                  &parse_err)) {
            if (state == YEW_GIT_OK)
                git_publish_result(ctx, job_id, verb_name, YEW_GIT_PARSE,
                                   out, out_len, err, err_len);
            yew_git_snapshot_drop(next);
            yew_git_snapshot_init(next);
            git_refresh_failed(ed);
            return true;
        }
        if (!git_spawn_comment_char(ed)) {
            yew_git_snapshot_drop(next);
            yew_git_snapshot_init(next);
            git_refresh_failed(ed);
        }
        return true;
    }
    if (kind == YEW_GREQ_VERB &&
        strcmp(verb_name, "comment-char") == 0 && ctx->refresh_inflight) {
        GitSnapshot *next = &ctx->snap[ctx->live ^ 1U];
        bool absent = state == YEW_GIT_FAILED && exit_code == 1;

        if (!git_cache_comment_char(next, state, exit_code, out, out_len)) {
            if (state == YEW_GIT_OK)
                git_publish_result(ctx, job_id, verb_name, YEW_GIT_PARSE,
                                   out, out_len, err, err_len);
            yew_git_snapshot_drop(next);
            yew_git_snapshot_init(next);
            git_refresh_failed(ed);
            return true;
        }
        if (absent)
            git_publish_result(ctx, job_id, verb_name, YEW_GIT_OK,
                               NULL, 0U, err, err_len);
        if (!git_continue_refresh_after_comment(ed)) {
            yew_git_snapshot_drop(next);
            yew_git_snapshot_init(next);
            git_refresh_failed(ed);
        }
        return true;
    }
    if (kind == YEW_GREQ_UPSTREAM) {
        GitSnapshot *next = &ctx->snap[ctx->live ^ 1U];
        char *oid = state == YEW_GIT_OK ? git_parse_oid(out, out_len) : NULL;

        if (oid == NULL) {
            if (state == YEW_GIT_OK)
                git_publish_result(ctx, job_id, verb_name, YEW_GIT_PARSE,
                                   out, out_len, err, err_len);
            yew_git_snapshot_drop(next);
            yew_git_snapshot_init(next);
            git_refresh_failed(ed);
            return true;
        }
        if (ctx->incoming_dirty || ctx->upstream_oid == NULL ||
            strcmp(ctx->upstream_oid, oid) != 0) {
            free(ctx->pending_upstream_oid);
            ctx->pending_upstream_oid = oid;
            if (!git_spawn_incoming(ed)) {
                yew_git_snapshot_drop(next);
                yew_git_snapshot_init(next);
                git_refresh_failed(ed);
            }
        } else {
            free(oid);
            if (!git_merge_incoming(next, &ctx->incoming_paths)) {
                yew_git_snapshot_drop(next);
                yew_git_snapshot_init(next);
                git_refresh_failed(ed);
            } else {
                git_publish_refresh(ed);
            }
        }
        return true;
    }
    if (kind == YEW_GREQ_INCOMING) {
        GitSnapshot *next = &ctx->snap[ctx->live ^ 1U];

        if (state != YEW_GIT_OK || !git_incoming_cache(ctx, out, out_len) ||
            !git_merge_incoming(next, &ctx->incoming_paths)) {
            if (state == YEW_GIT_OK)
                git_publish_result(ctx, job_id, verb_name, YEW_GIT_PARSE,
                                   out, out_len, err, err_len);
            yew_git_snapshot_drop(next);
            yew_git_snapshot_init(next);
            git_refresh_failed(ed);
            return true;
        }
        free(ctx->upstream_oid);
        ctx->upstream_oid = ctx->pending_upstream_oid;
        ctx->pending_upstream_oid = NULL;
        ctx->incoming_dirty = false;
        git_publish_refresh(ed);
        return true;
    }
    if (kind == YEW_GREQ_LOG) {
        GitParseErr parse_err;

        arena_free_all(&ctx->log_arena);
        arena_init(&ctx->log_arena);
        ctx->log_records.data = NULL;
        ctx->log_records.len = 0U;
        if (state == YEW_GIT_OK &&
            !yew_git_parse_log(&ctx->log_arena, out, out_len,
                               &ctx->log_records, &parse_err))
            git_publish_result(ctx, job_id, verb_name, YEW_GIT_PARSE,
                               out, out_len, err, err_len);
        return true;
    }
    return true;
}

bool yew_git_test_complete(Ed *ed, u32 job_id, GitStatusCode state,
                           const u8 *out, u64 out_len,
                           const u8 *err, u64 err_len)
{
    int exit_code = state == YEW_GIT_OK ? 0 :
                    state == YEW_GIT_FAILED ? 129 : -1;

    return yew_git_test_complete_exit(ed, job_id, state, exit_code,
                                      out, out_len, err, err_len);
}

CmdStatus yew_git_cmd_require(CmdCtx *cx)
{
    (void)cx;
    return YEW_CMD_ERR_STATE;
}

CmdStatus yew_git_cmd_info(CmdCtx *cx)
{
    const GitSnapshot *snap;
    const char *repo;
    const char *state;
    i64 age;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    snap = yew_git_snapshot(cx->ed);
    if (snap == NULL)
        return YEW_CMD_ERR_STATE;
    repo = cx->ed->git->repo.top_level;
    if (repo == NULL)
        repo = cx->ed->git->repo.git_dir;
    if (repo == NULL)
        repo = yew_ws_root(cx->ed);
    age = snap->gen == 0U ? 0 : git_now(cx->ed) - snap->taken_ms;
    if (snap->gen != 0U)
        state = yew_git_state_str(snap->state);
    else if (cx->ed->git->avail_state == YEW_GIT_ASYNC_FAILED)
        state = yew_git_state_str(YEW_GIT_NO_GIT);
    else if (cx->ed->git->detect_state != YEW_GIT_ASYNC_UNTRIED)
        state = yew_git_state_str(cx->ed->git->detect_result);
    else
        state = "loading";
    yew_msg(cx->ed, YEW_MSG_INFO,
            "git: %s; repo %s; branch %s; age %lld ms", state, repo,
            snap->branch == NULL ? "(unknown)" : snap->branch,
            (long long)age);
    return YEW_CMD_OK;
}

CmdStatus yew_git_cmd_refresh(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    (void)yew_git_refresh(cx->ed, true);
    yew_msg(cx->ed, YEW_MSG_INFO, "git refresh scheduled");
    return YEW_CMD_OK;
}

static bool git_spawn_log(Ed *ed)
{
    char *argv[] = {
        (char *)"log", (char *)"-z", (char *)"--no-color",
        (char *)"--date=raw",
        (char *)"--pretty=format:%H%x00%h%x00%at%x00%an%x00%ae%x00%P%x00%D%x00%s%x00%b",
        NULL
    };
    GitReq req = {0};
    char err[160];

    req.kind = YEW_GREQ_LOG;
    return yew_git_spawn(ed, yew_git_verb("log"), argv, &req, err,
                         sizeof(err)) != 0U;
}

CmdStatus yew_git_cmd_log(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->ed->git == NULL)
        return YEW_CMD_ERR_STATE;
    if (cx->ed->git->avail_state != YEW_GIT_ASYNC_READY ||
        cx->ed->git->detect_state != YEW_GIT_ASYNC_READY ||
        cx->ed->git->snap[cx->ed->git->live].gen == 0U) {
        cx->ed->git->log_requested = true;
        (void)yew_git_refresh(cx->ed, true);
        yew_msg(cx->ed, YEW_MSG_INFO, "git log waiting for repository status");
        return YEW_CMD_OK;
    }
    if (!git_spawn_log(cx->ed)) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "could not start git log");
        return YEW_CMD_ERR_IO;
    }
    yew_msg(cx->ed, YEW_MSG_INFO, "git log loading");
    return YEW_CMD_OK;
}
