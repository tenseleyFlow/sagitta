#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "mod/git/git.h"
#include "mod/git/git_int.h"

typedef struct SpawnLog {
    u32 next_id;
    u32 calls;
    size_t argc;
    u32 status_calls;
    u32 ignore_calls;
    u32 comment_calls;
    u32 upstream_calls;
    u32 incoming_calls;
    u32 last_status;
    u32 last_ignore;
    u32 last_comment;
    u32 last_upstream;
    u32 last_incoming;
    bool fail_next;
    char argv[16][128];
} SpawnLog;

static u32 gitcache_spawn(Ed *ed, const GitVerb *verb, char *const *argv,
                          const GitReq *req, void *opaque,
                          char *err, size_t errsz)
{
    SpawnLog *log = opaque;
    size_t i;

    (void)ed;
    (void)verb;
    (void)req;
    if (err != NULL && errsz != 0U)
        err[0] = '\0';
    log->calls++;
    if (log->fail_next) {
        log->fail_next = false;
        return 0U;
    }
    if (strcmp(verb->name, "status") == 0) {
        log->status_calls++;
        log->last_status = log->next_id + 1U;
    }
    if (strcmp(verb->name, "ignore") == 0) {
        log->ignore_calls++;
        log->last_ignore = log->next_id + 1U;
    }
    if (strcmp(verb->name, "comment-char") == 0) {
        log->comment_calls++;
        log->last_comment = log->next_id + 1U;
    }
    if (strcmp(verb->name, "upstream-oid") == 0) {
        log->upstream_calls++;
        log->last_upstream = log->next_id + 1U;
    }
    if (strcmp(verb->name, "incoming") == 0) {
        log->incoming_calls++;
        log->last_incoming = log->next_id + 1U;
    }
    log->argc = 0U;
    for (i = 0U; argv[i] != NULL && i < YEW_ARRAY_LEN(log->argv); i++) {
        size_t n = strlen(argv[i]);

        if (n >= sizeof(log->argv[i]))
            n = sizeof(log->argv[i]) - 1U;
        (void)memcpy(log->argv[i], argv[i], n);
        log->argv[i][n] = '\0';
        log->argc++;
    }
    return ++log->next_id;
}

static void gitcache_ed(Ed *ed, SpawnLog *log)
{
    (void)memset(ed, 0, sizeof(*ed));
    yew_git_state_init(ed);
    yew_git_test_spawn_set(gitcache_spawn, log);
}

static void gitcache_done(Ed *ed)
{
    yew_git_state_free(ed);
    yew_git_test_spawn_set(NULL, NULL);
}

static void gitcache_complete_default_comment(Ed *ed, SpawnLog *log)
{
    YEW_ASSERT(log->last_comment != 0U);
    YEW_ASSERT(yew_git_test_complete_exit(ed, log->last_comment,
                                          YEW_GIT_FAILED, 1,
                                          NULL, 0U, NULL, 0U));
}

static char *gitcache_tmp_template(const char *tag)
{
    const char *base = getenv("TMPDIR");
    size_t bn;
    size_t tn = strlen(tag);
    bool slash;
    char *path;

    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    bn = strlen(base);
    slash = base[bn - 1U] != '/';
    path = malloc(bn + (slash ? 1U : 0U) + tn + sizeof("-XXXXXX"));
    YEW_ASSERT_NOT_NULL(path);
    (void)memcpy(path, base, bn);
    if (slash)
        path[bn++] = '/';
    (void)memcpy(path + bn, tag, tn);
    (void)memcpy(path + bn + tn, "-XXXXXX", sizeof("-XXXXXX"));
    return path;
}

static void gitcache_ready(Ed *ed, SpawnLog *log, const char *git_dir,
                           i64 now_ms)
{
    static const u8 version[] = "git version 2.40.1\n";
    static const u8 status[] =
        "# branch.oid 0123456789012345678901234567890123456789\0"
        "# branch.head trunk\0";
    char detect[1024];
    int n;
    u32 id;

    yew_git_test_now_set(ed, now_ms);
    YEW_ASSERT(yew_git_refresh(ed, false));
    id = log->next_id;
    YEW_ASSERT(yew_git_test_complete(ed, id, YEW_GIT_OK, version,
                                     sizeof(version) - 1U, NULL, 0U));
    id = log->next_id;
    n = snprintf(detect, sizeof(detect), "%s\ntrue\nfalse\n%s\n",
                 git_dir, git_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(detect));
    YEW_ASSERT(yew_git_test_complete(ed, id, YEW_GIT_OK,
                                     (const u8 *)detect, (u64)n, NULL, 0U));
    id = log->last_status;
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(yew_git_test_complete(ed, id, YEW_GIT_OK, status,
                                     sizeof(status) - 1U, NULL, 0U));
    id = log->last_ignore;
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(yew_git_test_complete(ed, id, YEW_GIT_OK, NULL, 0U,
                                     NULL, 0U));
    gitcache_complete_default_comment(ed, log);
}

void test_gitcache_ttl_uses_monotonic_wall_milliseconds(void)
{
    YEW_ASSERT(yew_git_cache_fresh(1000, 1499));
    YEW_ASSERT(!yew_git_cache_fresh(1000, 1500));
    YEW_ASSERT(!yew_git_cache_fresh(1000, 1501));
    /* Ten seconds of wall time must expire even when the process consumed
     * zero CPU while blocked in poll(2).  clock(3) fails this assertion. */
    YEW_ASSERT(!yew_git_cache_fresh(1000, 11000));
    YEW_ASSERT(!yew_git_cache_fresh(1000, 999));
}

void test_gitcache_state_strings_cover_taxonomy(void)
{
    static const char *const expected[] = {
        "ok", "git unavailable", "not a repository", "bare repository",
        "no HEAD", "detached HEAD", "no upstream", "conflicted",
        "merge in progress", "rebase in progress",
        "cherry-pick in progress", "revert in progress",
        "bisect in progress", "repository locked",
        "authentication required", "timed out", "git command failed",
        "malformed git output"
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(expected); i++)
        YEW_ASSERT_EQ_STR(yew_git_state_str((GitStatusCode)i), expected[i]);
    YEW_ASSERT_EQ_STR(yew_git_state_str((GitStatusCode)99),
                      "unknown git state");
}

static void gitcache_touch(const char *dir, const char *name, bool directory)
{
    size_t dn = strlen(dir);
    size_t nn = strlen(name);
    char *path = malloc(dn + nn + 2U);

    YEW_ASSERT_NOT_NULL(path);
    (void)memcpy(path, dir, dn);
    path[dn] = '/';
    (void)memcpy(path + dn + 1U, name, nn + 1U);
    if (directory) {
        YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
    } else {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

        YEW_ASSERT(fd >= 0);
        if (fd >= 0)
            (void)close(fd);
    }
    free(path);
}

static void gitcache_remove(const char *dir, const char *name, bool directory)
{
    size_t dn = strlen(dir);
    size_t nn = strlen(name);
    char *path = malloc(dn + nn + 2U);

    YEW_ASSERT_NOT_NULL(path);
    (void)memcpy(path, dir, dn);
    path[dn] = '/';
    (void)memcpy(path + dn + 1U, name, nn + 1U);
    YEW_ASSERT_EQ_I64(directory ? rmdir(path) : unlink(path), 0);
    free(path);
}

static void gitcache_write(const char *dir, const char *name,
                           const char *text)
{
    size_t dn = strlen(dir);
    size_t nn = strlen(name);
    size_t len = strlen(text);
    char *path = malloc(dn + nn + 2U);
    size_t off = 0U;
    int fd;

    YEW_ASSERT_NOT_NULL(path);
    (void)memcpy(path, dir, dn);
    path[dn] = '/';
    (void)memcpy(path + dn + 1U, name, nn + 1U);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    YEW_ASSERT(fd >= 0);
    while (fd >= 0 && off < len) {
        ssize_t wrote = write(fd, text + off, len - off);

        YEW_ASSERT(wrote > 0);
        if (wrote <= 0)
            break;
        off += (size_t)wrote;
    }
    if (fd >= 0)
        YEW_ASSERT_EQ_I64(close(fd), 0);
    free(path);
}

void test_gitcache_filesystem_taxonomy_is_message_free(void)
{
    static const struct {
        const char *name;
        bool directory;
        GitStatusCode state;
    } rows[] = {
        {"MERGE_HEAD", false, YEW_GIT_MID_MERGE},
        {"rebase-merge", true, YEW_GIT_MID_REBASE},
        {"rebase-apply", true, YEW_GIT_MID_REBASE},
        {"CHERRY_PICK_HEAD", false, YEW_GIT_MID_CHERRY_PICK},
        {"REVERT_HEAD", false, YEW_GIT_MID_REVERT},
        {"BISECT_LOG", false, YEW_GIT_MID_BISECT},
        {"index.lock", false, YEW_GIT_LOCKED}
    };
    char *tmp = gitcache_tmp_template("yew-gitcache");
    GitRepo repo = {0};
    size_t i;

    YEW_ASSERT_NOT_NULL(mkdtemp(tmp));
    repo.git_dir = tmp;
    YEW_ASSERT_EQ_I64(yew_git_probe_state(&repo), YEW_GIT_OK);
    repo.bare = true;
    YEW_ASSERT_EQ_I64(yew_git_probe_state(&repo), YEW_GIT_BARE);
    repo.bare = false;
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        gitcache_touch(tmp, rows[i].name, rows[i].directory);
        YEW_ASSERT_EQ_I64(yew_git_probe_state(&repo), rows[i].state);
        gitcache_remove(tmp, rows[i].name, rows[i].directory);
    }
    YEW_ASSERT_EQ_I64(rmdir(tmp), 0);
    free(tmp);
}

void test_gitcache_cached_accessor_and_job_ownership_are_side_effect_free(void)
{
    Ed ed;
    SpawnLog log = {0};
    const GitSnapshot *snap;

    gitcache_ed(&ed, &log);
    snap = yew_git_snapshot_cached(&ed);
    YEW_ASSERT_NULL(snap);
    YEW_ASSERT_EQ_U64(log.calls, 0U);
    snap = yew_git_snapshot(&ed);
    YEW_ASSERT_NOT_NULL(snap);
    YEW_ASSERT_EQ_U64(log.calls, 1U);
    YEW_ASSERT(yew_git_job_owned(&ed, log.next_id));
    YEW_ASSERT(!yew_git_job_owned(&ed, log.next_id + 1U));
    YEW_ASSERT_NULL(yew_git_snapshot_cached(&ed));
    YEW_ASSERT_EQ_U64(log.calls, 1U);
    gitcache_done(&ed);
}

void test_gitcache_rebase_progress_and_conflict_count_publish(void)
{
    static const u8 status[] =
        "# branch.oid 0123456789012345678901234567890123456789\0"
        "# branch.head trunk\0"
        "u UU N... 100644 100644 100644 100644 "
        "0123456789012345678901234567890123456789 "
        "1123456789012345678901234567890123456789 "
        "2123456789012345678901234567890123456789 conflict.c\0";
    Ed ed;
    SpawnLog log = {0};
    char *tmp = gitcache_tmp_template("yew-git-rebase");
    char *rebase;
    const GitSnapshot *snap;

    YEW_ASSERT_NOT_NULL(mkdtemp(tmp));
    gitcache_ed(&ed, &log);
    gitcache_ready(&ed, &log, tmp, 1000);
    rebase = malloc(strlen(tmp) + sizeof("/rebase-merge"));
    YEW_ASSERT_NOT_NULL(rebase);
    (void)sprintf(rebase, "%s/rebase-merge", tmp);
    YEW_ASSERT_EQ_I64(mkdir(rebase, 0700), 0);
    gitcache_write(rebase, "msgnum", "3\n");
    gitcache_write(rebase, "end", "7\n");

    yew_git_test_now_set(&ed, 1500);
    YEW_ASSERT(yew_git_refresh(&ed, false));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_status, YEW_GIT_OK,
                                     status, sizeof(status) - 1U,
                                     NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_ignore, YEW_GIT_OK,
                                     NULL, 0U, NULL, 0U));
    gitcache_complete_default_comment(&ed, &log);
    snap = yew_git_snapshot_cached(&ed);
    YEW_ASSERT_NOT_NULL(snap);
    YEW_ASSERT_EQ_I64(snap->state, YEW_GIT_MID_REBASE);
    YEW_ASSERT(snap->conflicted);
    YEW_ASSERT_EQ_U64(snap->conflict_count, 1U);
    YEW_ASSERT_EQ_U64(snap->rebase_step, 3U);
    YEW_ASSERT_EQ_U64(snap->rebase_total, 7U);

    gitcache_done(&ed);
    gitcache_remove(rebase, "msgnum", false);
    gitcache_remove(rebase, "end", false);
    YEW_ASSERT_EQ_I64(rmdir(rebase), 0);
    YEW_ASSERT_EQ_I64(rmdir(tmp), 0);
    free(rebase);
    free(tmp);
}

typedef struct GitCallbackCapture {
    bool completed;
    bool destroyed;
    char out[64];
} GitCallbackCapture;

static void gitcache_callback_complete(void *owner, Ed *ed,
                                       const YewJob *job)
{
    GitCallbackCapture *capture = owner;
    size_t len = job->collect.len;

    (void)ed;
    if (len >= sizeof(capture->out))
        len = sizeof(capture->out) - 1U;
    if (len != 0U)
        (void)memcpy(capture->out, job->collect.data, len);
    capture->out[len] = '\0';
    capture->completed = true;
}

static void gitcache_callback_destroy(void *owner)
{
    GitCallbackCapture *capture = owner;

    capture->destroyed = true;
}

static const YewJobCallbackOps gitcache_callback_ops = {
    gitcache_callback_complete,
    gitcache_callback_destroy
};

void test_gitcache_callback_input_copies_and_owns_stdin(void)
{
    Ed ed;
    GitCallbackCapture capture = {0};
    char input[] = "owned copy\n";
    char *argv[] = {(char *)"stripspace", NULL};
    char err[128];
    u32 id;
    i64 started;

    yew_ed_init(&ed);
    yew_git_test_spawn_set(NULL, NULL);
    id = yew_git_spawn_callback_input(&ed, yew_git_verb("version"), argv,
                                      (const u8 *)input,
                                      (u64)(sizeof(input) - 1U), &capture,
                                      &gitcache_callback_ops,
                                      err, sizeof(err));
    YEW_ASSERT(id != 0U);
    (void)memset(input, 'x', sizeof(input) - 1U);
    started = yew_now_ms();
    while (!capture.completed && yew_now_ms() - started < 5000) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        u32 n = 0U;

        yew_job_collect_fds(&ed, pfd, &n);
        if (n != 0U)
            (void)poll(pfd, (nfds_t)n, 20);
        yew_job_pump(&ed, pfd, n);
        yew_job_reap(&ed);
        yew_job_settle(&ed);
    }
    YEW_ASSERT(capture.completed);
    YEW_ASSERT(capture.destroyed);
    YEW_ASSERT_EQ_STR(capture.out, "owned copy\n");
    YEW_ASSERT_NULL(yew_job_find(&ed, id));
    yew_ed_free(&ed);
}

void test_gitcache_verb_table_and_argv_are_structural(void)
{
    Ed ed;
    SpawnLog log = {0};
    GitReq req = {0};
    char *tail[] = {(char *)"add", (char *)"--", (char *)"-n",
                    (char *)"a \"$\n", NULL};
    char err[64];
    const GitVerb *stage;
    size_t i;

    gitcache_ed(&ed, &log);
    stage = yew_git_verb("stage");
    YEW_ASSERT_NOT_NULL(stage);
    YEW_ASSERT_NOT_NULL(yew_git_verb("init"));
    YEW_ASSERT(stage->kind == YEW_GV_MUTATE);
    req.kind = YEW_GREQ_VERB;
    req.literal_paths = true;
    YEW_ASSERT(yew_git_spawn(&ed, stage, tail, &req, err, sizeof(err)) != 0U);
    YEW_ASSERT_EQ_STR(log.argv[0], "git");
    YEW_ASSERT_EQ_STR(log.argv[1], "--no-pager");
    YEW_ASSERT_EQ_STR(log.argv[2], "-c");
    YEW_ASSERT_EQ_STR(log.argv[3], "core.quotepath=false");
    YEW_ASSERT_EQ_STR(log.argv[4], "-c");
    YEW_ASSERT_EQ_STR(log.argv[5], "status.renames=true");
    YEW_ASSERT_EQ_STR(log.argv[6], "add");
    YEW_ASSERT_EQ_STR(log.argv[7], "--");
    YEW_ASSERT_EQ_STR(log.argv[8], "-n");
    YEW_ASSERT_EQ_STR(log.argv[9], "a \"$\n");
    for (i = 0U; i < yew_git_verb_count(); i++) {
        const GitVerb *verb = yew_git_verb_at(i);

        YEW_ASSERT_NOT_NULL(verb);
        YEW_ASSERT(verb->name[0] != '\0');
        YEW_ASSERT(verb->kind == YEW_GV_NET ?
                   verb->timeout_ms == YEW_GIT_NET_TIMEOUT_MS :
                   verb->timeout_ms == YEW_GIT_READ_TIMEOUT_MS);
    }
    YEW_ASSERT_NULL(yew_git_verb_at(yew_git_verb_count()));
    gitcache_done(&ed);
}

void test_gitcache_coalesces_and_caps_inflight(void)
{
    Ed ed;
    SpawnLog log = {0};
    GitReq req = {0};
    char err[64];
    char *same[] = {(char *)"status", NULL};
    char argbuf[4][2] = {{'0','\0'}, {'1','\0'}, {'2','\0'}, {'3','\0'}};
    char *different[] = {(char *)"status", NULL, NULL};
    u32 first;
    size_t i;

    gitcache_ed(&ed, &log);
    req.kind = YEW_GREQ_STATUS;
    first = yew_git_spawn(&ed, yew_git_verb("status"), same, &req, err,
                          sizeof(err));
    YEW_ASSERT(first != 0U);
    YEW_ASSERT_EQ_U64(yew_git_spawn(&ed, yew_git_verb("status"), same, &req,
                                    err, sizeof(err)), first);
    YEW_ASSERT_EQ_U64(log.calls, 1U);
    req.kind = YEW_GREQ_VERB;
    YEW_ASSERT(yew_git_spawn(&ed, yew_git_verb("status"), same, &req, err,
                             sizeof(err)) != first);
    YEW_ASSERT_EQ_U64(log.calls, 2U);
    req.kind = YEW_GREQ_STATUS;
    for (i = 0U; i < 2U; i++) {
        different[1] = argbuf[i];
        YEW_ASSERT(yew_git_spawn(&ed, yew_git_verb("status"), different,
                                 &req, err, sizeof(err)) != 0U);
    }
    different[1] = argbuf[2];
    YEW_ASSERT_EQ_U64(yew_git_spawn(&ed, yew_git_verb("status"), different,
                                    &req, err, sizeof(err)), 0U);
    YEW_ASSERT_EQ_U64(log.calls, YEW_GIT_MAX_INFLIGHT);
    gitcache_done(&ed);

    (void)memset(&log, 0, sizeof(log));
    gitcache_ed(&ed, &log);
    {
        u8 stdin_bytes[] = {'a', 'b', 'c'};
        GitReq owned = {0};
        GitReq mutate = {0};
        char *commit[] = {(char *)"commit", NULL};
        u32 owned_id;
        u32 mutate_id;

        owned.kind = YEW_GREQ_STATUS;
        owned.stdin_bytes = stdin_bytes;
        owned.stdin_len = sizeof(stdin_bytes);
        owned_id = yew_git_spawn(&ed, yew_git_verb("status"), same, &owned,
                                 err, sizeof(err));
        stdin_bytes[0] = 'x';
        YEW_ASSERT(yew_git_spawn(&ed, yew_git_verb("status"), same, &owned,
                                 err, sizeof(err)) != owned_id);
        mutate.kind = YEW_GREQ_VERB;
        mutate.stdin_bytes = stdin_bytes;
        mutate.stdin_len = sizeof(stdin_bytes);
        mutate_id = yew_git_spawn(&ed, yew_git_verb("commit"), commit,
                                  &mutate, err, sizeof(err));
        YEW_ASSERT(mutate_id != 0U);
        YEW_ASSERT(yew_git_spawn(&ed, yew_git_verb("commit"), commit,
                                 &mutate, err, sizeof(err)) != mutate_id);
    }
    gitcache_done(&ed);
}

void test_gitcache_environment_fingerprint_tracks_passthrough(void)
{
    const char *name = "GIT_NAMESPACE";
    const char *old = getenv(name);
    char *saved = old == NULL ? NULL : strdup(old);
    u64 before;
    u64 after;

    YEW_ASSERT(old == NULL || saved != NULL);
    before = yew_git_env_fingerprint();
    YEW_ASSERT_EQ_I64(setenv(name, "yew-s51-test", 1), 0);
    after = yew_git_env_fingerprint();
    YEW_ASSERT(before != after);
    if (saved == NULL)
        YEW_ASSERT_EQ_I64(unsetenv(name), 0);
    else {
        YEW_ASSERT_EQ_I64(setenv(name, saved, 1), 0);
        free(saved);
    }
    YEW_ASSERT_EQ_U64(yew_git_env_fingerprint(), before);
}

void test_gitcache_refresh_ttl_coalesces_and_pingpong_survives_failure(void)
{
    static const u8 status[] =
        "# branch.oid 0123456789012345678901234567890123456789\0"
        "# branch.head trunk\0";
    static const u8 malformed[] = "1 truncated\0";
    Ed ed;
    SpawnLog log = {0};
    char *tmp = gitcache_tmp_template("yew-git-refresh");
    const GitSnapshot *snap;
    u32 baseline;
    u32 i;

    YEW_ASSERT_NOT_NULL(mkdtemp(tmp));
    gitcache_ed(&ed, &log);
    gitcache_ready(&ed, &log, tmp, 1000);
    snap = yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(snap->gen, 1U);
    YEW_ASSERT_EQ_STR(snap->branch, "trunk");
    baseline = log.status_calls;
    yew_git_test_now_set(&ed, 1499);
    (void)yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(log.status_calls, baseline);
    yew_git_test_now_set(&ed, 1500);
    (void)yew_git_snapshot(&ed);
    for (i = 0U; i < 9U; i++)
        (void)yew_git_refresh(&ed, true);
    YEW_ASSERT_EQ_U64(log.status_calls, baseline + 1U);
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_status, YEW_GIT_OK,
                                     status, sizeof(status) - 1U, NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_ignore, YEW_GIT_OK,
                                     NULL, 0U, NULL, 0U));
    gitcache_complete_default_comment(&ed, &log);
    YEW_ASSERT_EQ_U64(log.status_calls, baseline + 2U);
    snap = yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(snap->gen, 2U);
    YEW_ASSERT_EQ_STR(snap->branch, "trunk");
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_status, YEW_GIT_OK,
                                     malformed, sizeof(malformed) - 1U,
                                     NULL, 0U));
    snap = yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(snap->gen, 2U);
    YEW_ASSERT_EQ_STR(snap->branch, "trunk");
    gitcache_done(&ed);
    YEW_ASSERT_EQ_I64(rmdir(tmp), 0);
    free(tmp);
}

void test_gitcache_comment_char_is_cached_before_snapshot_publish(void)
{
    static const u8 status[] =
        "# branch.oid 0123456789012345678901234567890123456789\0"
        "# branch.head trunk\0";
    static const u8 auto_char[] = "auto\n";
    static const u8 explicit_char[] = ";\n";
    static const u8 malformed[] = "too-long\n";
    Ed ed;
    SpawnLog log = {0};
    char *tmp = gitcache_tmp_template("yew-git-comment");
    const GitSnapshot *snap;

    YEW_ASSERT_NOT_NULL(mkdtemp(tmp));
    gitcache_ed(&ed, &log);
    gitcache_ready(&ed, &log, tmp, 1000);
    snap = yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(snap->gen, 1U);
    YEW_ASSERT_EQ_STR(snap->comment_char, "#");
    YEW_ASSERT_EQ_U64(snap->comment_char_len, 1U);

    yew_git_test_now_set(&ed, 1500);
    YEW_ASSERT(yew_git_refresh(&ed, false));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_status, YEW_GIT_OK,
                                     status, sizeof(status) - 1U,
                                     NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_ignore, YEW_GIT_OK,
                                     NULL, 0U, NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_comment, YEW_GIT_OK,
                                     auto_char, sizeof(auto_char) - 1U,
                                     NULL, 0U));
    snap = yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(snap->gen, 2U);
    YEW_ASSERT_EQ_STR(snap->comment_char, "auto");
    YEW_ASSERT_EQ_U64(snap->comment_char_len, 4U);

    yew_git_test_now_set(&ed, 2000);
    YEW_ASSERT(yew_git_refresh(&ed, false));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_status, YEW_GIT_OK,
                                     status, sizeof(status) - 1U,
                                     NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_ignore, YEW_GIT_OK,
                                     NULL, 0U, NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_comment, YEW_GIT_OK,
                                     explicit_char,
                                     sizeof(explicit_char) - 1U,
                                     NULL, 0U));
    snap = yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(snap->gen, 3U);
    YEW_ASSERT_EQ_STR(snap->comment_char, ";");
    YEW_ASSERT_EQ_U64(snap->comment_char_len, 1U);

    yew_git_test_now_set(&ed, 2500);
    YEW_ASSERT(yew_git_refresh(&ed, false));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_status, YEW_GIT_OK,
                                     status, sizeof(status) - 1U,
                                     NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_ignore, YEW_GIT_OK,
                                     NULL, 0U, NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_comment, YEW_GIT_OK,
                                     malformed, sizeof(malformed) - 1U,
                                     NULL, 0U));
    snap = yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(snap->gen, 3U);
    YEW_ASSERT_EQ_STR(snap->comment_char, ";");
    YEW_ASSERT(yew_git_result(&ed)->state == YEW_GIT_PARSE);

    gitcache_done(&ed);
    YEW_ASSERT_EQ_I64(rmdir(tmp), 0);
    free(tmp);
}

void test_gitcache_every_mutation_schedules_structural_refresh(void)
{
    Ed ed;
    SpawnLog log = {0};
    char *tmp = gitcache_tmp_template("yew-git-mutate");
    char *tail[] = {(char *)"noop", NULL};
    GitReq req = {0};
    char err[64];
    size_t i;
    u32 checked = 0U;

    YEW_ASSERT_NOT_NULL(mkdtemp(tmp));
    gitcache_ed(&ed, &log);
    gitcache_ready(&ed, &log, tmp, 1000);
    req.kind = YEW_GREQ_VERB;
    for (i = 0U; i < yew_git_verb_count(); i++) {
        const GitVerb *verb = yew_git_verb_at(i);
        u32 job;
        u32 before;

        if (verb->kind == YEW_GV_READ)
            continue;
        before = log.status_calls;
        job = yew_git_spawn(&ed, verb, tail, &req, err, sizeof(err));
        YEW_ASSERT(job != 0U);
        YEW_ASSERT(yew_git_test_complete(&ed, job, YEW_GIT_OK, NULL, 0U,
                                         NULL, 0U));
        YEW_ASSERT_EQ_U64(log.status_calls, before + 1U);
        YEW_ASSERT(yew_git_test_complete(&ed, log.last_status,
                                         YEW_GIT_FAILED, NULL, 0U,
                                         NULL, 0U));
        checked++;
    }
    YEW_ASSERT(checked >= 20U);
    gitcache_done(&ed);
    YEW_ASSERT_EQ_I64(rmdir(tmp), 0);
    free(tmp);
}

void test_gitcache_index_lock_skips_expired_refresh(void)
{
    Ed ed;
    SpawnLog log = {0};
    char *tmp = gitcache_tmp_template("yew-git-lock");
    u32 before;

    YEW_ASSERT_NOT_NULL(mkdtemp(tmp));
    gitcache_ed(&ed, &log);
    gitcache_ready(&ed, &log, tmp, 1000);
    gitcache_touch(tmp, "index.lock", false);
    before = log.status_calls;
    yew_git_test_now_set(&ed, 2000);
    (void)yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(log.status_calls, before);
    gitcache_remove(tmp, "index.lock", false);
    gitcache_done(&ed);
    YEW_ASSERT_EQ_I64(rmdir(tmp), 0);
    free(tmp);
}

void test_gitcache_log_completion_publishes_owned_records(void)
{
    static const u8 log_out[] =
        "0123456789012345678901234567890123456789\0"
        "0123456\0" "1700000000\0Jane\0jane@example.test\0\0"
        "HEAD -> trunk\0subject\0body";
    Ed ed;
    SpawnLog log = {0};
    char *tmp = gitcache_tmp_template("yew-git-log");
    char *tail[] = {(char *)"log", (char *)"-z", NULL};
    GitReq req = {0};
    const GitLogRecordList *records;
    char err[64];
    u32 job;

    YEW_ASSERT_NOT_NULL(mkdtemp(tmp));
    gitcache_ed(&ed, &log);
    gitcache_ready(&ed, &log, tmp, 1000);
    req.kind = YEW_GREQ_LOG;
    job = yew_git_spawn(&ed, yew_git_verb("log"), tail, &req, err,
                        sizeof(err));
    YEW_ASSERT(job != 0U);
    YEW_ASSERT(yew_git_test_complete(&ed, job, YEW_GIT_OK, log_out,
                                     sizeof(log_out) - 1U, NULL, 0U));
    records = yew_git_log_records(&ed);
    YEW_ASSERT_NOT_NULL(records);
    YEW_ASSERT_EQ_U64(records->len, 1U);
    YEW_ASSERT_EQ_STR(records->data[0].author, "Jane");
    YEW_ASSERT_EQ_STR(records->data[0].subject, "subject");
    YEW_ASSERT_EQ_STR(records->data[0].body, "body");
    gitcache_done(&ed);
    YEW_ASSERT_EQ_I64(rmdir(tmp), 0);
    free(tmp);
}

void test_gitcache_initial_upstream_refresh_and_oid_gating(void)
{
    static const u8 version[] = "git version 2.40.1\n";
    static const u8 status[] =
        "# branch.oid 0123456789012345678901234567890123456789\0"
        "# branch.head trunk\0"
        "# branch.upstream origin/trunk\0";
    static const u8 oid_a[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
    static const u8 oid_b[] =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n";
    static const u8 incoming[] = "src/a.c\0src/z.c\0";
    Ed ed;
    SpawnLog log = {0};
    const GitSnapshot *snap;
    char detect[256];
    int n;
    u32 first_incoming;

    gitcache_ed(&ed, &log);
    yew_git_test_now_set(&ed, 1000);
    YEW_ASSERT(yew_git_refresh(&ed, false));
    YEW_ASSERT(yew_git_test_complete(&ed, log.next_id, YEW_GIT_OK, version,
                                     sizeof(version) - 1U, NULL, 0U));
    n = snprintf(detect, sizeof(detect),
                 "/tmp/yew-git\ntrue\nfalse\n/tmp\n");
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(detect));
    YEW_ASSERT(yew_git_test_complete(&ed, log.next_id, YEW_GIT_OK,
                                     (const u8 *)detect, (u64)n, NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_status, YEW_GIT_OK,
                                     status, sizeof(status) - 1U, NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_ignore, YEW_GIT_OK,
                                     NULL, 0U, NULL, 0U));
    gitcache_complete_default_comment(&ed, &log);
    YEW_ASSERT_EQ_U64(log.upstream_calls, 1U);
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_upstream, YEW_GIT_OK,
                                     oid_a, sizeof(oid_a) - 1U, NULL, 0U));
    YEW_ASSERT_EQ_U64(log.incoming_calls, 1U);
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_incoming, YEW_GIT_OK,
                                     incoming, sizeof(incoming) - 1U,
                                     NULL, 0U));
    snap = yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(snap->gen, 1U);
    YEW_ASSERT_EQ_U64(snap->entries.len, 2U);
    YEW_ASSERT(snap->entries.data[0].incoming);
    YEW_ASSERT_EQ_STR(snap->entries.data[0].path, "src/a.c");

    first_incoming = log.incoming_calls;
    yew_git_test_now_set(&ed, 1500);
    YEW_ASSERT(yew_git_refresh(&ed, false));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_status, YEW_GIT_OK,
                                     status, sizeof(status) - 1U, NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_ignore, YEW_GIT_OK,
                                     NULL, 0U, NULL, 0U));
    gitcache_complete_default_comment(&ed, &log);
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_upstream, YEW_GIT_OK,
                                     oid_a, sizeof(oid_a) - 1U, NULL, 0U));
    YEW_ASSERT_EQ_U64(log.incoming_calls, first_incoming);
    YEW_ASSERT_EQ_U64(yew_git_snapshot(&ed)->gen, 2U);

    yew_git_test_now_set(&ed, 2000);
    YEW_ASSERT(yew_git_refresh(&ed, false));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_status, YEW_GIT_OK,
                                     status, sizeof(status) - 1U, NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_ignore, YEW_GIT_OK,
                                     NULL, 0U, NULL, 0U));
    gitcache_complete_default_comment(&ed, &log);
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_upstream, YEW_GIT_OK,
                                     oid_b, sizeof(oid_b) - 1U, NULL, 0U));
    YEW_ASSERT_EQ_U64(log.incoming_calls, first_incoming + 1U);
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_incoming, YEW_GIT_OK,
                                     incoming, sizeof(incoming) - 1U,
                                     NULL, 0U));

    {
        GitReq req = {0};
        char *fetch[] = {(char *)"fetch", NULL};
        char err[64];
        u32 job;

        req.kind = YEW_GREQ_VERB;
        job = yew_git_spawn(&ed, yew_git_verb("fetch"), fetch, &req, err,
                            sizeof(err));
        YEW_ASSERT(job != 0U);
        YEW_ASSERT(yew_git_test_complete(&ed, job, YEW_GIT_OK, NULL, 0U,
                                         NULL, 0U));
        YEW_ASSERT(yew_git_test_complete(&ed, log.last_status, YEW_GIT_OK,
                                         status, sizeof(status) - 1U,
                                         NULL, 0U));
        YEW_ASSERT(yew_git_test_complete(&ed, log.last_ignore, YEW_GIT_OK,
                                         NULL, 0U, NULL, 0U));
        gitcache_complete_default_comment(&ed, &log);
        YEW_ASSERT(yew_git_test_complete(&ed, log.last_upstream, YEW_GIT_OK,
                                         oid_b, sizeof(oid_b) - 1U,
                                         NULL, 0U));
        YEW_ASSERT_EQ_U64(log.incoming_calls, first_incoming + 2U);
        YEW_ASSERT(yew_git_test_complete(&ed, log.last_incoming, YEW_GIT_OK,
                                         incoming, sizeof(incoming) - 1U,
                                         NULL, 0U));
    }
    gitcache_done(&ed);
}

void test_gitcache_large_incoming_merge_is_sorted_and_deduplicated(void)
{
    static const u8 headers[] =
        "# branch.oid 0123456789012345678901234567890123456789\0"
        "# branch.head trunk\0"
        "# branch.upstream origin/trunk\0";
    static const u8 oid[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
    Ed ed;
    SpawnLog log = {0};
    char *tmp = gitcache_tmp_template("yew-git-large");
    Bytebuf status;
    Bytebuf incoming;
    const GitSnapshot *snap;
    int i;

    YEW_ASSERT_NOT_NULL(mkdtemp(tmp));
    gitcache_ed(&ed, &log);
    gitcache_ready(&ed, &log, tmp, 1000);
    bytebuf_init(&status);
    bytebuf_init(&incoming);
    bytebuf_append(&status, headers, sizeof(headers) - 1U);
    for (i = 0; i < 1000; i++) {
        bytebuf_printf(&status, "? file%04d", i);
        bytebuf_push_u8(&status, 0U);
    }
    for (i = 1499; i >= 500; i--) {
        bytebuf_printf(&incoming, "file%04d", i);
        bytebuf_push_u8(&incoming, 0U);
    }
    yew_git_test_now_set(&ed, 1500);
    YEW_ASSERT(yew_git_refresh(&ed, false));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_status, YEW_GIT_OK,
                                     status.data, (u64)status.len, NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_ignore, YEW_GIT_OK,
                                     NULL, 0U, NULL, 0U));
    gitcache_complete_default_comment(&ed, &log);
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_upstream, YEW_GIT_OK,
                                     oid, sizeof(oid) - 1U, NULL, 0U));
    YEW_ASSERT(yew_git_test_complete(&ed, log.last_incoming, YEW_GIT_OK,
                                     incoming.data, (u64)incoming.len,
                                     NULL, 0U));
    snap = yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(snap->entries.len, 1500U);
    YEW_ASSERT_EQ_STR(snap->entries.data[0].path, "file0000");
    YEW_ASSERT_EQ_STR(snap->entries.data[1499].path, "file1499");
    for (i = 1; i < 1500; i++)
        YEW_ASSERT(strcmp(snap->entries.data[i - 1].path,
                          snap->entries.data[i].path) < 0);
    for (i = 500; i < 1500; i++)
        YEW_ASSERT(snap->entries.data[i].incoming);
    bytebuf_free(&incoming);
    bytebuf_free(&status);
    gitcache_done(&ed);
    YEW_ASSERT_EQ_I64(rmdir(tmp), 0);
    free(tmp);
}

static void gitcache_begin_detect(Ed *ed, SpawnLog *log)
{
    static const u8 version[] = "git version 2.40.1\n";

    gitcache_ed(ed, log);
    YEW_ASSERT(yew_git_refresh(ed, false));
    YEW_ASSERT(yew_git_test_complete(ed, log->next_id, YEW_GIT_OK, version,
                                     sizeof(version) - 1U, NULL, 0U));
}

void test_gitcache_bare_exit_codes_and_negative_detection_cache(void)
{
    static const u8 bare[] = "/tmp/bare.git\nfalse\ntrue\n";
    Ed ed;
    SpawnLog log = {0};
    u32 detect_id;
    u32 calls;
    size_t i;

    gitcache_begin_detect(&ed, &log);
    detect_id = log.next_id;
    YEW_ASSERT(yew_git_test_complete_exit(&ed, detect_id, YEW_GIT_FAILED,
                                          128, bare, sizeof(bare) - 1U,
                                          NULL, 0U));
    YEW_ASSERT(yew_git_detect_state(&ed) == YEW_GIT_ASYNC_READY);
    YEW_ASSERT(yew_git_detect(&ed, NULL) == YEW_GIT_BARE);
    gitcache_done(&ed);

    (void)memset(&log, 0, sizeof(log));
    gitcache_begin_detect(&ed, &log);
    detect_id = log.next_id;
    calls = log.calls;
    YEW_ASSERT(yew_git_test_complete_exit(&ed, detect_id, YEW_GIT_FAILED,
                                          129, bare, sizeof(bare) - 1U,
                                          NULL, 0U));
    YEW_ASSERT_EQ_U64(log.calls, calls + 1U);
    YEW_ASSERT(yew_git_test_complete_exit(&ed, log.next_id, YEW_GIT_FAILED,
                                          1, bare, sizeof(bare) - 1U,
                                          NULL, 0U));
    calls = log.calls;
    for (i = 0U; i < 1000U; i++)
        (void)yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(log.calls, calls);
    ed.ws.dir = (char *)"/tmp/yew-other-root";
    (void)yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(log.calls, calls + 1U);
    YEW_ASSERT(yew_git_test_complete_exit(&ed, log.next_id, YEW_GIT_FAILED,
                                          2, bare, sizeof(bare) - 1U,
                                          NULL, 0U));
    calls = log.calls;
    YEW_ASSERT(yew_git_refresh(&ed, true));
    YEW_ASSERT_EQ_U64(log.calls, calls + 1U);
    {
        static const u8 repo[] =
            "/tmp/.git\ntrue\nfalse\n/tmp/yew-other-root\n";

        YEW_ASSERT(yew_git_test_complete(&ed, log.next_id, YEW_GIT_OK,
                                         repo, sizeof(repo) - 1U,
                                         NULL, 0U));
        YEW_ASSERT(yew_git_detect_state(&ed) == YEW_GIT_ASYNC_READY);
    }
    gitcache_done(&ed);

    (void)memset(&log, 0, sizeof(log));
    gitcache_begin_detect(&ed, &log);
    detect_id = log.next_id;
    calls = log.calls;
    YEW_ASSERT(yew_git_test_complete_exit(&ed, detect_id, YEW_GIT_FAILED,
                                          2, bare, sizeof(bare) - 1U,
                                          NULL, 0U));
    YEW_ASSERT_EQ_U64(log.calls, calls);
    YEW_ASSERT(yew_git_detect_state(&ed) == YEW_GIT_ASYNC_FAILED);
    gitcache_done(&ed);

    (void)memset(&log, 0, sizeof(log));
    gitcache_begin_detect(&ed, &log);
    detect_id = log.next_id;
    log.fail_next = true;
    YEW_ASSERT(yew_git_test_complete_exit(&ed, detect_id, YEW_GIT_FAILED,
                                          129, bare, sizeof(bare) - 1U,
                                          NULL, 0U));
    YEW_ASSERT(yew_git_detect_state(&ed) == YEW_GIT_ASYNC_FAILED);
    calls = log.calls;
    for (i = 0U; i < 1000U; i++)
        (void)yew_git_snapshot(&ed);
    YEW_ASSERT_EQ_U64(log.calls, calls);
    gitcache_done(&ed);
}

void test_gitcache_detect_completion_rechecks_captured_context(void)
{
    static const u8 detected[] =
        "/tmp/.git\ntrue\nfalse\n/tmp\n";
    Ed ed;
    SpawnLog log = {0};
    u32 old_id;
    u32 calls;

    gitcache_begin_detect(&ed, &log);
    old_id = log.next_id;
    calls = log.calls;
    ed.ws.dir = (char *)"/tmp/new-root";
    YEW_ASSERT(yew_git_test_complete(&ed, old_id, YEW_GIT_OK, detected,
                                     sizeof(detected) - 1U, NULL, 0U));
    YEW_ASSERT_EQ_U64(log.calls, calls + 1U);
    YEW_ASSERT(yew_git_detect_state(&ed) == YEW_GIT_ASYNC_PENDING);
    gitcache_done(&ed);
}

void test_gitcache_blob_validation_and_owned_binary_result(void)
{
    static const char oid40[] =
        "0123456789abcdef0123456789abcdef01234567";
    static const char oid64[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const u8 bytes[] = {'a', 0U, 'b', 0xffU};
    static const u8 stderr_bytes[] = {'e', 0U, 'r'};
    Ed ed;
    SpawnLog log = {0};
    const GitResult *result;
    char err[80];
    u32 job;

    gitcache_ed(&ed, &log);
    YEW_ASSERT_EQ_U64(yew_git_blob(&ed, "abc", err, sizeof(err)), 0U);
    YEW_ASSERT(strstr(err, "invalid") != NULL);
    YEW_ASSERT_EQ_U64(yew_git_blob(
        &ed, "0123456789abcdef0123456789abcdef0123456g", err,
        sizeof(err)), 0U);
    job = yew_git_blob(&ed, oid40, err, sizeof(err));
    YEW_ASSERT(job != 0U);
    YEW_ASSERT_EQ_STR(log.argv[6], "--no-optional-locks");
    YEW_ASSERT_EQ_STR(log.argv[7], "cat-file");
    YEW_ASSERT_EQ_STR(log.argv[8], "blob");
    YEW_ASSERT_EQ_STR(log.argv[9], oid40);
    YEW_ASSERT(yew_git_test_complete(&ed, job, YEW_GIT_OK, bytes,
                                     sizeof(bytes), stderr_bytes,
                                     sizeof(stderr_bytes)));
    result = yew_git_result(&ed);
    YEW_ASSERT_NOT_NULL(result);
    YEW_ASSERT_EQ_STR(result->verb, "blob");
    YEW_ASSERT_EQ_U64(result->job_id, job);
    YEW_ASSERT_EQ_U64(result->out_len, sizeof(bytes));
    YEW_ASSERT(memcmp(result->out, bytes, sizeof(bytes)) == 0);
    YEW_ASSERT(memcmp(result->err, stderr_bytes, sizeof(stderr_bytes)) == 0);
    job = yew_git_blob(&ed, oid64, err, sizeof(err));
    YEW_ASSERT(job != 0U);
    gitcache_done(&ed);
}

void test_gitcache_runtime_taxonomy_is_behaviorally_reachable(void)
{
    static const char *const auth_errors[] = {
        "fatal: could not read Username for 'https://host'",
        "remote: Authentication failed",
        "fatal: terminal prompts disabled",
        "git@host: Permission denied (publickey)"
    };
    static const u8 old_version[] = "git version 2.19.9\n";
    static const u8 malformed[] = "1 truncated\0";
    Ed ed;
    SpawnLog log = {0};
    GitReq req = {0};
    const GitResult *result;
    char *argv[] = {(char *)"status", NULL};
    char err[64];
    u32 job;
    GitStatusCode states[] = {
        YEW_GIT_AUTH, YEW_GIT_TIMEOUT, YEW_GIT_FAILED
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(auth_errors); i++)
        YEW_ASSERT(yew_git_test_auth_state(
                       (const u8 *)auth_errors[i], strlen(auth_errors[i])) ==
                   YEW_GIT_AUTH);
    YEW_ASSERT(yew_git_test_auth_state(
                   (const u8 *)"fatal: network is down", 22U) ==
               YEW_GIT_FAILED);

    gitcache_ed(&ed, &log);
    YEW_ASSERT(yew_git_refresh(&ed, false));
    YEW_ASSERT(yew_git_test_complete(&ed, log.next_id, YEW_GIT_NO_GIT,
                                     NULL, 0U, NULL, 0U));
    result = yew_git_result(&ed);
    YEW_ASSERT(result->state == YEW_GIT_NO_GIT);
    gitcache_done(&ed);

    (void)memset(&log, 0, sizeof(log));
    gitcache_ed(&ed, &log);
    YEW_ASSERT(yew_git_refresh(&ed, false));
    YEW_ASSERT(yew_git_test_complete(&ed, log.next_id, YEW_GIT_OK,
                                     old_version, sizeof(old_version) - 1U,
                                     NULL, 0U));
    YEW_ASSERT(yew_git_result(&ed)->state == YEW_GIT_NO_GIT);
    gitcache_done(&ed);

    (void)memset(&log, 0, sizeof(log));
    gitcache_ed(&ed, &log);
    req.kind = YEW_GREQ_VERB;
    for (i = 0U; i < YEW_ARRAY_LEN(states); i++) {
        job = yew_git_spawn(&ed, yew_git_verb("status"), argv, &req, err,
                            sizeof(err));
        YEW_ASSERT(job != 0U);
        YEW_ASSERT(yew_git_test_complete(&ed, job, states[i], NULL, 0U,
                                         NULL, 0U));
        YEW_ASSERT(yew_git_result(&ed)->state == states[i]);
    }
    gitcache_done(&ed);

    (void)memset(&log, 0, sizeof(log));
    {
        char *tmp = gitcache_tmp_template("yew-git-parse-state");

        YEW_ASSERT_NOT_NULL(mkdtemp(tmp));
        gitcache_ed(&ed, &log);
        gitcache_ready(&ed, &log, tmp, 1000);
        yew_git_test_now_set(&ed, 1500);
        YEW_ASSERT(yew_git_refresh(&ed, false));
        YEW_ASSERT(yew_git_test_complete(&ed, log.last_status, YEW_GIT_OK,
                                         malformed, sizeof(malformed) - 1U,
                                         NULL, 0U));
        YEW_ASSERT(yew_git_result(&ed)->state == YEW_GIT_PARSE);
        gitcache_done(&ed);
        YEW_ASSERT_EQ_I64(rmdir(tmp), 0);
        free(tmp);
    }
}

void test_gitcache_teardown_detaches_inflight_stdin(void)
{
    static const u8 input[] = "sensitive bytes\n";
    Ed ed;
    GitReq req = {0};
    char *argv[] = {(char *)"hash-object", (char *)"--stdin", NULL};
    char err[128];

    (void)memset(&ed, 0, sizeof(ed));
    yew_jobs_init(&ed.jobs);
    yew_git_state_init(&ed);
    yew_git_test_spawn_set(NULL, NULL);
    req.kind = YEW_GREQ_VERB;
    req.stdin_bytes = input;
    req.stdin_len = sizeof(input) - 1U;
    YEW_ASSERT(yew_git_spawn(&ed, yew_git_verb("init"), argv, &req, err,
                             sizeof(err)) != 0U);
    yew_git_state_free(&ed);
    yew_jobs_free(&ed);
}
