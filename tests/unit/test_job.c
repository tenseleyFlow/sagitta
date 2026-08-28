/* Sprint 19 §1-§2 + DoD 3, 8: job lifecycle, exec-failure precision, the
 * concurrency cap, and fd/zombie accounting. */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "ws/symidx.h"

static void job_fixture(Ed *ed)
{
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
}

/*
 * Drives one job to completion off the main loop, the way yew_loop_run
 * would: poll the job fds, pump, reap, SETTLE.  Returns false on timeout.
 *
 * The settle is not decoration, and leaving it out is what this helper
 * used to do.  "Reaped" is not "finished": job.c says so in as many
 * words — a pipe can hold a bufferful past the child's exit — and the
 * old predicate, `state != RUNNING && reaped`, returned in exactly that
 * window.  `/bin/echo hi` then had bytes_out == 0 perhaps one run in
 * several, and because YEW_ASSERT longjmps out of the test, the failure
 * skipped its yew_ed_free and valgrind reported the whole Ed as leaked:
 * nineteen loss records whose real cause was one racy wait.
 *
 * `drained` is the honest condition, because only yew_job_settle sets
 * it — waiting on it proves the pipes reached EOF AND that completion
 * was delivered, rather than proving the child is merely dead.
 */
static bool run_to_completion(Ed *ed, u32 id)
{
    i64 start = yew_now_ms();

    for (;;) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        u32 n = 0U;
        YewJob *j = yew_job_find(ed, id);

        if (j == NULL)
            return false;
        if (j->drained)
            return true;
        yew_job_collect_fds(ed, pfd, &n);
        if (n != 0U)
            (void)poll(pfd, (nfds_t)n, 20);
        else
            (void)poll(NULL, 0U, 5);
        yew_job_pump(ed, pfd, n);
        yew_job_reap(ed);
        yew_job_tick(ed, yew_now_ms());
        yew_job_settle(ed);
        if (yew_now_ms() - start > 10000)
            return false;
    }
}

/* Releases every finished job through the real API.  Zeroing jobs.len
 * instead would skip job_dispose and leak the collect buffer and label —
 * ASan catches exactly that. */
static void release_finished(Ed *ed)
{
    u32 i = 0U;

    while (i < ed->jobs.len) {
        if (ed->jobs.v[i].state == YEW_JOB_RUNNING) {
            i++;
            continue;
        }
        yew_job_release(ed, &ed->jobs.v[i]);
    }
}

/* Prove that this fixture reaped its own child without making assumptions
 * about subprocesses owned by another fixture.  A preceding assertion can
 * longjmp past that fixture's cleanup; waitpid(-1) would then misattribute
 * its still-running child to whichever job test happens to run next. */
static void assert_job_child_reaped(Ed *ed, u32 id)
{
    YewJob *j = yew_job_find(ed, id);
    pid_t waited;
    int wait_errno;

    YEW_ASSERT_NOT_NULL(j);
    YEW_ASSERT(j->pid > 0);
    errno = 0;
    waited = waitpid(j->pid, NULL, WNOHANG);
    wait_errno = errno;
    YEW_ASSERT_EQ_I64(waited, -1);
    YEW_ASSERT_EQ_I64(wait_errno, ECHILD);
}

static u32 spawn_argv(Ed *ed, char **argv, char *err, size_t errsz)
{
    YewJobSpec spec = {0};

    spec.argv = argv;
    spec.sink = YEW_SINK_COLLECT;
    return yew_job_spawn(ed, &spec, err, errsz);
}

static bool run_until_released(Ed *ed, u32 id)
{
    i64 start = yew_now_ms();

    while (yew_job_find(ed, id) != NULL) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        u32 n = 0U;

        yew_job_collect_fds(ed, pfd, &n);
        if (n != 0U)
            (void)poll(pfd, (nfds_t)n, 20);
        else
            (void)poll(NULL, 0U, 5);
        yew_job_pump(ed, pfd, n);
        yew_job_reap(ed);
        yew_job_tick(ed, yew_now_ms());
        yew_job_settle(ed);
        if (yew_now_ms() - start > 10000)
            return false;
    }
    return true;
}

typedef struct JobCallbackWitness {
    Bytebuf out;
    Bytebuf err;
    YewJobState state;
    int exit_code;
    u32 completes;
    u32 destroys;
    bool reaped;
    bool pipes_eof;
    bool destroyed_after_complete;
    bool collect_capped;
} JobCallbackWitness;

static void job_callback_test_complete(void *owner, Ed *ed,
                                       const YewJob *job)
{
    JobCallbackWitness *w = owner;

    (void)ed;
    w->completes++;
    w->state = job->state;
    w->exit_code = job->exit_code;
    w->reaped = job->reaped;
    w->pipes_eof = job->out_fd == -1 && job->err_fd == -1;
    w->collect_capped = job->collect_capped;
    bytebuf_append(&w->out, job->collect.data, job->collect.len);
    bytebuf_append(&w->err, job->collect_err.data, job->collect_err.len);
}

static void job_callback_test_destroy(void *owner)
{
    JobCallbackWitness *w = owner;

    w->destroys++;
    w->destroyed_after_complete = w->completes == 1U;
}

static const YewJobCallbackOps job_callback_test_ops = {
    job_callback_test_complete,
    job_callback_test_destroy
};

static void job_callback_witness_init(JobCallbackWitness *w)
{
    (void)memset(w, 0, sizeof(*w));
    bytebuf_init(&w->out);
    bytebuf_init(&w->err);
}

static void job_callback_witness_free(JobCallbackWitness *w)
{
    bytebuf_free(&w->out);
    bytebuf_free(&w->err);
}

static char *job_test_env_copy(const char *name)
{
    const char *value = getenv(name);
    size_t len;
    char *copy;

    if (value == NULL)
        return NULL;
    len = strlen(value);
    copy = yew_xmalloc(len + 1U);
    (void)memcpy(copy, value, len + 1U);
    return copy;
}

static void job_test_env_restore(const char *name, char *saved)
{
    if (saved != NULL) {
        YEW_ASSERT_EQ_I64(setenv(name, saved, 1), 0);
        free(saved);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv(name), 0);
    }
}

void test_job_callback_waits_for_reap_and_both_eofs_then_releases(void)
{
    Ed ed;
    YewJobSpec spec = {0};
    JobCallbackWitness w;
    char *argv[] = {
        (char *)"/bin/sh", (char *)"-c",
        (char *)"printf early; (sleep 0.2; printf late; printf err >&2) & "
                 "exit 7",
        NULL
    };
    char err[256] = {0};
    u32 id;

    job_fixture(&ed);
    job_callback_witness_init(&w);
    spec.argv = argv;
    spec.sink = YEW_SINK_CALLBACK;
    spec.callback_owner = &w;
    spec.callback_ops = &job_callback_test_ops;
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(run_until_released(&ed, id));
    YEW_ASSERT(yew_job_find(&ed, id) == NULL);
    YEW_ASSERT_EQ_U64(w.completes, 1U);
    YEW_ASSERT_EQ_U64(w.destroys, 1U);
    YEW_ASSERT(w.destroyed_after_complete);
    YEW_ASSERT(w.reaped);
    YEW_ASSERT(w.pipes_eof);
    YEW_ASSERT(w.state == YEW_JOB_EXITED);
    YEW_ASSERT_EQ_I64(w.exit_code, 7);
    YEW_ASSERT_EQ_MEM(w.out.data, "earlylate", 9U);
    YEW_ASSERT_EQ_MEM(w.err.data, "err", 3U);
    yew_ed_free(&ed);
    YEW_ASSERT_EQ_U64(w.destroys, 1U);
    job_callback_witness_free(&w);
}

void test_job_callback_collect_max_and_destroy_on_teardown(void)
{
    Ed ed;
    YewJobSpec spec = {0};
    JobCallbackWitness capped;
    JobCallbackWitness teardown;
    char *output_argv[] = {(char *)"/bin/sh", (char *)"-c",
                           (char *)"printf 12345", NULL};
    char *sleep_argv[] = {(char *)"/bin/sh", (char *)"-c",
                          (char *)"sleep 30", NULL};
    char err[256] = {0};
    u32 id;

    job_fixture(&ed);
    job_callback_witness_init(&capped);
    job_callback_witness_init(&teardown);
    spec.argv = output_argv;
    spec.sink = YEW_SINK_CALLBACK;
    spec.collect_max = 4U;
    spec.callback_owner = &capped;
    spec.callback_ops = &job_callback_test_ops;
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(run_until_released(&ed, id));
    YEW_ASSERT(capped.collect_capped);
    YEW_ASSERT_EQ_U64(capped.completes, 1U);
    YEW_ASSERT_EQ_U64(capped.destroys, 1U);

    (void)memset(&spec, 0, sizeof(spec));
    spec.argv = sleep_argv;
    spec.sink = YEW_SINK_CALLBACK;
    spec.callback_owner = &teardown;
    spec.callback_ops = &job_callback_test_ops;
    YEW_ASSERT(yew_job_spawn(&ed, &spec, err, sizeof(err)) != 0U);
    yew_ed_free(&ed);
    YEW_ASSERT_EQ_U64(teardown.completes, 0U);
    YEW_ASSERT_EQ_U64(teardown.destroys, 1U);
    job_callback_witness_free(&capped);
    job_callback_witness_free(&teardown);
}

void test_job_environment_overrides_are_copied_and_name_exact(void)
{
    Ed ed;
    YewJobSpec spec = {0};
    JobCallbackWitness w;
    const char *const env_set[] = {"YEW_JOB_ENV_SET=child",
                                    "YEW_JOB_ENV_NEW=new", NULL};
    const char *const env_unset[] = {"YEW_JOB_ENV_EXACT", NULL};
    const char *const env_prefix[] = {"YEW_JOB_ENV_PREFIX_", NULL};
    char *argv[] = {
        (char *)"/bin/sh", (char *)"-c",
        (char *)"printf '%s|%s|%s|%s|%s|%s' \"$YEW_JOB_ENV_KEEP\" "
                 "\"$YEW_JOB_ENV_SET\" \"${YEW_JOB_ENV_EXACT-unset}\" "
                 "\"${YEW_JOB_ENV_EXACTLY-unset}\" "
                 "\"${YEW_JOB_ENV_PREFIX_ONE-unset}\" "
                 "\"$YEW_JOB_ENV_NEW\"",
        NULL
    };
    char err[256] = {0};
    u32 id;
    char *saved_keep = job_test_env_copy("YEW_JOB_ENV_KEEP");
    char *saved_set = job_test_env_copy("YEW_JOB_ENV_SET");
    char *saved_exact = job_test_env_copy("YEW_JOB_ENV_EXACT");
    char *saved_exactly = job_test_env_copy("YEW_JOB_ENV_EXACTLY");
    char *saved_prefix = job_test_env_copy("YEW_JOB_ENV_PREFIX_ONE");

    YEW_ASSERT_EQ_I64(setenv("YEW_JOB_ENV_KEEP", "parent", 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_JOB_ENV_SET", "parent", 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_JOB_ENV_EXACT", "gone", 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_JOB_ENV_EXACTLY", "keep", 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_JOB_ENV_PREFIX_ONE", "gone", 1), 0);
    job_fixture(&ed);
    job_callback_witness_init(&w);
    spec.argv = argv;
    spec.sink = YEW_SINK_CALLBACK;
    spec.env_set = env_set;
    spec.env_unset = env_unset;
    spec.env_unset_prefix = env_prefix;
    spec.callback_owner = &w;
    spec.callback_ops = &job_callback_test_ops;
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT_EQ_STR(getenv("YEW_JOB_ENV_SET"), "parent");
    YEW_ASSERT_EQ_STR(getenv("YEW_JOB_ENV_EXACT"), "gone");
    YEW_ASSERT_EQ_STR(getenv("YEW_JOB_ENV_PREFIX_ONE"), "gone");
    YEW_ASSERT(run_until_released(&ed, id));
    YEW_ASSERT_EQ_MEM(w.out.data,
                      "parent|child|unset|keep|unset|new", 33U);
    YEW_ASSERT_EQ_U64(w.err.len, 0U);
    yew_ed_free(&ed);
    job_callback_witness_free(&w);
    job_test_env_restore("YEW_JOB_ENV_KEEP", saved_keep);
    job_test_env_restore("YEW_JOB_ENV_SET", saved_set);
    job_test_env_restore("YEW_JOB_ENV_EXACT", saved_exact);
    job_test_env_restore("YEW_JOB_ENV_EXACTLY", saved_exactly);
    job_test_env_restore("YEW_JOB_ENV_PREFIX_ONE", saved_prefix);
}

void test_job_buffer_append_updates_syntax(void)
{
    Ed ed;
    YewJob job = {0};
    Buffer *buffer;

    job_fixture(&ed);
    buffer = ed.win->buf;
    job.buf = buffer;
    YEW_ASSERT_EQ_U64(buffer->syn.entry.len,
                      yew_textbuf_line_count(buffer->tb));

    yew_job_buffer_append(&ed, &job, (const u8 *)"one\ntwo\n", 8U,
                          false);

    YEW_ASSERT_EQ_U64(yew_textbuf_line_count(buffer->tb), 3U);
    YEW_ASSERT_EQ_U64(buffer->syn.entry.len,
                      yew_textbuf_line_count(buffer->tb));
    YEW_ASSERT(yew_symidx_pending(&ed));
    yew_ed_free(&ed);
}

/* Counts our open descriptors; /proc when available, else an fcntl sweep. */
static u32 open_fd_count(void)
{
    DIR *d = opendir("/proc/self/fd");
    u32 n = 0U;

    if (d != NULL) {
        struct dirent *e;

        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] != '.')
                n++;
        }
        (void)closedir(d);
        return n;
    }
    {
        int fd;

        for (fd = 0; fd < 256; fd++) {
            if (fcntl(fd, F_GETFD) != -1)
                n++;
        }
    }
    return n;
}

void test_job_echo_lifecycle(void)
{
    Ed ed;
    char *argv[3];
    char err[256] = {0};
    u32 id;
    YewJob *j;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/echo";
    argv[1] = (char *)"hi";
    argv[2] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT_EQ_STR(err, "");
    YEW_ASSERT(run_to_completion(&ed, id));
    j = yew_job_find(&ed, id);
    YEW_ASSERT_NOT_NULL(j);
    YEW_ASSERT(j->state == YEW_JOB_EXITED);
    YEW_ASSERT_EQ_I64(j->exit_code, 0);
    YEW_ASSERT_EQ_U64(j->bytes_out, 3U);
    YEW_ASSERT_EQ_MEM(j->collect.data, "hi\n", 3U);
    /* Every parent-side descriptor is closed by completion. */
    YEW_ASSERT_EQ_I64(j->out_fd, -1);
    YEW_ASSERT_EQ_I64(j->err_fd, -1);
    YEW_ASSERT_EQ_I64(j->in_fd, -1);
    /* The exec-status pipe closes too: closing it empty IS the success
     * signal, so a still-open exec_fd would mean we never observed it. */
    YEW_ASSERT_EQ_I64(j->exec_fd, -1);
    {
        u32 fds_before_free = open_fd_count();

        yew_ed_free(&ed);
        /* Teardown must not strand descriptors; valgrind --track-fds
         * reports the forked child's inherited copies, so this is the
         * assertion that speaks for the parent. */
        YEW_ASSERT(open_fd_count() <= fds_before_free);
    }
}

void test_job_internal_ids_do_not_advance_public_ids(void)
{
    Ed ed;
    YewJobSpec spec = {0};
    char *argv[2];
    char err[256] = {0};
    u32 internal_a;
    u32 internal_b;
    u32 public_a;
    u32 public_b;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/true";
    argv[1] = NULL;
    spec.argv = argv;
    spec.sink = YEW_SINK_DISCARD;
    spec.internal = true;
    internal_a = yew_job_spawn(&ed, &spec, err, sizeof(err));
    internal_b = yew_job_spawn(&ed, &spec, err, sizeof(err));
    spec.internal = false;
    public_a = yew_job_spawn(&ed, &spec, err, sizeof(err));
    public_b = yew_job_spawn(&ed, &spec, err, sizeof(err));

    YEW_ASSERT_EQ_U64(internal_a, UINT32_MAX);
    YEW_ASSERT_EQ_U64(internal_b, UINT32_MAX - 1U);
    YEW_ASSERT_EQ_U64(public_a, 1U);
    YEW_ASSERT_EQ_U64(public_b, 2U);
    YEW_ASSERT_NOT_NULL(yew_job_find(&ed, internal_a));
    YEW_ASSERT_NOT_NULL(yew_job_find(&ed, public_a));
    yew_ed_free(&ed);
}

void test_job_argv_resolves_path_and_preserves_explicit_paths(void)
{
    Ed ed;
    char dir[] = "/tmp/yew-job-path-XXXXXX";
    char probe[sizeof(dir) + 32U];
    char *path_argv[] = {(char *)"yew-path-probe", NULL};
    char *explicit_argv[] = {(char *)"/bin/sh", (char *)"-c",
                             (char *)"printf explicit", NULL};
    const char *old_path = getenv("PATH");
    char *saved_path = old_path != NULL ? strdup(old_path) : NULL;
    char err[256] = {0};
    u32 id;
    YewJob *j;
    bool path_ok;
    bool explicit_ok;

    job_fixture(&ed);
    if (old_path != NULL)
        YEW_ASSERT_NOT_NULL(saved_path);
    YEW_ASSERT_NOT_NULL(mkdtemp(dir));
    YEW_ASSERT(snprintf(probe, sizeof(probe), "%s/yew-path-probe", dir) > 0);
    {
        FILE *script = fopen(probe, "wb");

        YEW_ASSERT_NOT_NULL(script);
        YEW_ASSERT(fputs("#!/bin/sh\nprintf path\n", script) >= 0);
        YEW_ASSERT_EQ_I64(fclose(script), 0);
        YEW_ASSERT_EQ_I64(chmod(probe, 0700), 0);
    }
    YEW_ASSERT_EQ_I64(setenv("PATH", dir, 1), 0);

    id = spawn_argv(&ed, path_argv, err, sizeof(err));
    path_ok = id != 0U && run_to_completion(&ed, id);
    j = path_ok ? yew_job_find(&ed, id) : NULL;
    path_ok = j != NULL && j->state == YEW_JOB_EXITED &&
              j->exit_code == 0 && j->collect.len == 4U &&
              memcmp(j->collect.data, "path", 4U) == 0;

    id = spawn_argv(&ed, explicit_argv, err, sizeof(err));
    explicit_ok = id != 0U && run_to_completion(&ed, id);
    j = explicit_ok ? yew_job_find(&ed, id) : NULL;
    explicit_ok = j != NULL && j->state == YEW_JOB_EXITED &&
                  j->exit_code == 0 && j->collect.len == 8U &&
                  memcmp(j->collect.data, "explicit", 8U) == 0;

    if (saved_path != NULL)
        YEW_ASSERT_EQ_I64(setenv("PATH", saved_path, 1), 0);
    else
        YEW_ASSERT_EQ_I64(unsetenv("PATH"), 0);
    free(saved_path);
    YEW_ASSERT_EQ_I64(unlink(probe), 0);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
    yew_ed_free(&ed);

    YEW_ASSERT(path_ok);
    YEW_ASSERT(explicit_ok);
}

void test_job_exec_failure_is_not_exit_127(void)
{
    Ed ed;
    char *argv[2];
    char err[256] = {0};
    u32 id;
    YewJob *j;

    job_fixture(&ed);
    argv[0] = (char *)"/nonexistent/definitely-not-here";
    argv[1] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(run_to_completion(&ed, id));
    j = yew_job_find(&ed, id);
    /* DoD 3: without the exec-status pipe this would be indistinguishable
     * from a command that ran and exited 127. */
    YEW_ASSERT(j->state == YEW_JOB_EXECFAIL);
    YEW_ASSERT_EQ_I64(j->exec_errno, ENOENT);
    yew_ed_free(&ed);
}

void test_job_exit_127_is_not_exec_failure(void)
{
    Ed ed;
    char *argv[4];
    char err[256] = {0};
    u32 id;
    YewJob *j;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"exit 127";
    argv[3] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(run_to_completion(&ed, id));
    j = yew_job_find(&ed, id);
    /* The other half of DoD 3: a real 127 stays EXITED. */
    YEW_ASSERT(j->state == YEW_JOB_EXITED);
    YEW_ASSERT_EQ_I64(j->exit_code, 127);
    YEW_ASSERT_EQ_I64(j->exec_errno, 0);
    yew_ed_free(&ed);
}

void test_job_exit_code_and_stderr(void)
{
    Ed ed;
    char *argv[4];
    char err[256] = {0};
    u32 id;
    YewJob *j;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"echo oops >&2; exit 3";
    argv[3] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    YEW_ASSERT(run_to_completion(&ed, id));
    j = yew_job_find(&ed, id);
    YEW_ASSERT(j->state == YEW_JOB_EXITED);
    YEW_ASSERT_EQ_I64(j->exit_code, 3);
    YEW_ASSERT_EQ_U64(j->bytes_err, 5U);
    YEW_ASSERT_EQ_U64(j->bytes_out, 0U);
    yew_ed_free(&ed);
}

void test_job_killed_by_signal(void)
{
    Ed ed;
    char *argv[4];
    char err[256] = {0};
    u32 id;
    YewJob *j;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"sleep 30";
    argv[3] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(yew_job_signal(&ed, id, SIGKILL));
    YEW_ASSERT(run_to_completion(&ed, id));
    j = yew_job_find(&ed, id);
    YEW_ASSERT(j->state == YEW_JOB_SIGNALED);
    YEW_ASSERT_EQ_I64(j->termsig, SIGKILL);
    yew_ed_free(&ed);
}

void test_job_signal_reaches_the_whole_group(void)
{
    Ed ed;
    char *argv[4];
    char err[256] = {0};
    u32 id;
    YewJob *j;

    job_fixture(&ed);
    /* Killing the pid alone would leave `sleep` running and the pipeline
     * unkillable; we signal -pgid. */
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"sleep 30 | cat";
    argv[3] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    j = yew_job_find(&ed, id);
    YEW_ASSERT(j->pgid > 0);
    YEW_ASSERT(yew_job_signal(&ed, id, SIGKILL));
    YEW_ASSERT(run_to_completion(&ed, id));
    /* Nothing from that group survives. */
    YEW_ASSERT(kill(-j->pgid, 0) != 0);
    YEW_ASSERT_EQ_I64(errno, ESRCH);
    yew_ed_free(&ed);
}

void test_job_max_refuses_rather_than_queues(void)
{
    Ed ed;
    char *argv[4];
    char err[256] = {0};
    u32 i;
    u32 id;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"sleep 30";
    argv[3] = NULL;
    for (i = 0U; i < YEW_JOB_MAX; i++)
        YEW_ASSERT(spawn_argv(&ed, argv, err, sizeof(err)) != 0U);
    /* Refused with a message, never silently queued: a job that starts
     * minutes later surprises the user. */
    id = spawn_argv(&ed, argv, err, sizeof(err));
    YEW_ASSERT_EQ_U64(id, 0U);
    YEW_ASSERT_EQ_STR(err, "too many jobs (32); finish or kill one first");
    for (i = 0U; i < YEW_JOB_MAX; i++)
        (void)yew_job_signal(&ed, i + 1U, SIGKILL);
    yew_ed_free(&ed);
}

void test_job_stdin_region_is_piped(void)
{
    Ed ed;
    YewJobSpec spec = {0};
    TextBuf *tb;
    char err[256] = {0};
    u32 id;
    YewJob *j;
    char *argv[4];

    job_fixture(&ed);
    tb = yew_textbuf_from_bytes((const u8 *)"beta\nalpha\ngamma\n", 17U);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"sort";
    argv[3] = NULL;
    spec.argv = argv;
    spec.sink = YEW_SINK_COLLECT;
    spec.in_buf = tb;
    spec.in_span = (Span){0U, 17U};
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(run_to_completion(&ed, id));
    j = yew_job_find(&ed, id);
    YEW_ASSERT(j->state == YEW_JOB_EXITED);
    /* sort emits nothing until stdin EOF: this passing proves we close
     * the write end at end-of-region. */
    YEW_ASSERT_EQ_U64((u64)j->collect.len, 17U);
    YEW_ASSERT_EQ_MEM(j->collect.data, "alpha\nbeta\ngamma\n", 17U);
    yew_textbuf_free(tb);
    yew_ed_free(&ed);
}

void test_job_stdin_bytes_are_binary_exact_and_wiped(void)
{
    Ed ed;
    YewJobSpec spec = {0};
    u8 input[] = {'a', 0U, 0xFFU, '\n', 'z'};
    const u8 expect[] = {'a', 0U, 0xFFU, '\n', 'z'};
    char err[256] = {0};
    char *argv[2];
    u32 id;
    YewJob *j;
    size_t i;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/cat";
    argv[1] = NULL;
    spec.argv = argv;
    spec.sink = YEW_SINK_COLLECT;
    spec.in_bytes = input;
    spec.in_len = sizeof(input);
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT_EQ_STR(err, "");
    YEW_ASSERT(run_to_completion(&ed, id));
    j = yew_job_find(&ed, id);
    YEW_ASSERT_NOT_NULL(j);
    YEW_ASSERT(j->state == YEW_JOB_EXITED);
    YEW_ASSERT_EQ_U64((u64)j->collect.len, sizeof(expect));
    YEW_ASSERT_EQ_MEM(j->collect.data, expect, sizeof(expect));
    for (i = 0U; i < sizeof(input); i++)
        YEW_ASSERT_EQ_U64(input[i], 0U);
    yew_ed_free(&ed);
}

void test_job_stdin_bytes_are_wiped_when_child_closes_early(void)
{
    Ed ed;
    YewJobSpec spec = {0};
    struct sigaction ignored;
    struct sigaction saved;
    u8 input[128U * 1024U];
    char err[256] = {0};
    char *argv[2];
    u32 id;
    size_t i;
    bool wiped = true;

    (void)memset(&ignored, 0, sizeof(ignored));
    ignored.sa_handler = SIG_IGN;
    YEW_ASSERT_EQ_I64(sigemptyset(&ignored.sa_mask), 0);
    /* The real editor installs this process-wide before any job can run.
     * Pin it here too: Valgrind makes the child-close-before-write timing
     * deterministic, and the contract is that write(2) returns EPIPE for
     * job.c to handle rather than terminating the editor. */
    YEW_ASSERT_EQ_I64(sigaction(SIGPIPE, &ignored, &saved), 0);
    (void)memset(input, 0xA5, sizeof(input));
    job_fixture(&ed);
    argv[0] = (char *)"/bin/true";
    argv[1] = NULL;
    spec.argv = argv;
    spec.sink = YEW_SINK_COLLECT;
    spec.in_bytes = input;
    spec.in_len = sizeof(input);
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(run_to_completion(&ed, id));
    for (i = 0U; i < sizeof(input); i++)
        wiped = wiped && input[i] == 0U;
    YEW_ASSERT(wiped);
    yew_ed_free(&ed);
    YEW_ASSERT_EQ_I64(sigaction(SIGPIPE, &saved, NULL), 0);
}

typedef struct JobStreamWitness {
    Bytebuf out;
    Bytebuf err;
    u32 out_finish;
    u32 err_finish;
    u32 ticks;
    u32 destroys;
    bool reject_stdout;
    i64 deadline;
} JobStreamWitness;

static bool job_stream_test_out(void *owner, const u8 *bytes, u64 len)
{
    JobStreamWitness *w = owner;

    bytebuf_append(&w->out, bytes, (size_t)len);
    return !w->reject_stdout;
}

static bool job_stream_test_out_finish(void *owner)
{
    JobStreamWitness *w = owner;

    w->out_finish++;
    return true;
}

static bool job_stream_test_err(void *owner, const u8 *bytes, u64 len)
{
    JobStreamWitness *w = owner;

    bytebuf_append(&w->err, bytes, (size_t)len);
    return true;
}

static bool job_stream_test_err_finish(void *owner)
{
    JobStreamWitness *w = owner;

    w->err_finish++;
    return true;
}

static i64 job_stream_test_deadline(const void *owner)
{
    const JobStreamWitness *w = owner;

    return w->deadline;
}

static void job_stream_test_tick(void *owner, Ed *ed, i64 now_ms)
{
    JobStreamWitness *w = owner;

    (void)ed;
    (void)now_ms;
    w->ticks++;
}

static void job_stream_test_destroy(void *owner)
{
    JobStreamWitness *w = owner;

    w->destroys++;
}

static const YewJobStreamOps job_stream_test_ops = {
    job_stream_test_out,
    job_stream_test_out_finish,
    job_stream_test_err,
    job_stream_test_err_finish,
    job_stream_test_deadline,
    job_stream_test_tick,
    job_stream_test_destroy
};

static void job_stream_witness_init(JobStreamWitness *w)
{
    (void)memset(w, 0, sizeof(*w));
    bytebuf_init(&w->out);
    bytebuf_init(&w->err);
    w->deadline = -1;
}

static void job_stream_witness_free(JobStreamWitness *w)
{
    bytebuf_free(&w->out);
    bytebuf_free(&w->err);
}

/* Regression — the safe-prefix trap: a streaming protocol must see an
 * incomplete UTF-8 prefix immediately.  Routing this sink through
 * yew_job_safe_prefix holds all three bytes until EOF and this test fails. */
void test_job_stream_bypasses_safe_prefix(void)
{
    Ed ed;
    YewJobSpec spec = {0};
    JobStreamWitness w;
    char *argv[] = {(char *)"/bin/sh", (char *)"-c",
                    (char *)"printf '\\360\\237\\230'; sleep 30", NULL};
    char err[256] = {0};
    u32 id;
    i64 start;

    job_fixture(&ed);
    job_stream_witness_init(&w);
    spec.argv = argv;
    spec.sink = YEW_SINK_STREAM;
    spec.stream_owner = &w;
    spec.stream_ops = &job_stream_test_ops;
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    start = yew_now_ms();
    while (w.out.len < 3U && yew_now_ms() - start < 2000) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        u32 n = 0U;

        yew_job_collect_fds(&ed, pfd, &n);
        (void)poll(pfd, (nfds_t)n, 20);
        yew_job_pump(&ed, pfd, n);
    }
    YEW_ASSERT_EQ_U64((u64)w.out.len, 3U);
    YEW_ASSERT_EQ_MEM(w.out.data, "\360\237\230", 3U);
    YEW_ASSERT_EQ_U64(w.out_finish, 0U);
    YEW_ASSERT(yew_job_signal(&ed, id, SIGKILL));
    YEW_ASSERT(run_to_completion(&ed, id));
    YEW_ASSERT_EQ_U64(w.out_finish, 1U);
    YEW_ASSERT_EQ_U64(w.err_finish, 1U);
    YEW_ASSERT_EQ_U64(w.destroys, 1U);
    yew_ed_free(&ed);
    YEW_ASSERT_EQ_U64(w.destroys, 1U);
    job_stream_witness_free(&w);
}

void test_job_stream_routes_stderr_and_owns_lifecycle(void)
{
    Ed ed;
    YewJobSpec spec = {0};
    JobStreamWitness w;
    u8 input[] = "secret config\n";
    char *argv[] = {(char *)"/bin/sh", (char *)"-c",
                    (char *)"cat; printf 'yew-http-status: 201\\n' >&2",
                    NULL};
    char err[256] = {0};
    u32 id;
    YewJob *j;
    size_t i;
    i64 now;

    job_fixture(&ed);
    job_stream_witness_init(&w);
    now = yew_now_ms();
    w.deadline = now + 500;
    spec.argv = argv;
    spec.sink = YEW_SINK_STREAM;
    spec.in_bytes = input;
    spec.in_len = sizeof(input) - 1U;
    spec.stream_owner = &w;
    YEW_ASSERT_EQ_U64(yew_job_spawn(&ed, &spec, err, sizeof(err)), 0U);
    YEW_ASSERT_EQ_STR(err, "stream job has no transport");
    YEW_ASSERT_EQ_U64(w.destroys, 0U);
    spec.stream_ops = &job_stream_test_ops;
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(yew_job_deadline(&ed, now) >= 0);
    YEW_ASSERT(yew_job_deadline(&ed, now) <= 500);
    yew_job_tick(&ed, now);
    YEW_ASSERT_EQ_U64(w.ticks, 1U);
    YEW_ASSERT(run_to_completion(&ed, id));
    j = yew_job_find(&ed, id);
    YEW_ASSERT_NOT_NULL(j);
    YEW_ASSERT_EQ_MEM(w.out.data, "secret config\n", 14U);
    YEW_ASSERT_EQ_MEM(w.err.data, "yew-http-status: 201\n", 21U);
    YEW_ASSERT_EQ_MEM(j->stream_err.data, w.err.data, w.err.len);
    YEW_ASSERT_EQ_U64(w.out_finish, 1U);
    YEW_ASSERT_EQ_U64(w.err_finish, 1U);
    YEW_ASSERT_EQ_U64(w.destroys, 1U);
    for (i = 0U; i < sizeof(input); i++)
        YEW_ASSERT_EQ_U64(input[i], 0U);
    yew_ed_free(&ed);
    YEW_ASSERT_EQ_U64(w.destroys, 1U);
    job_stream_witness_free(&w);
}

void test_job_stream_read_budget_is_bounded(void)
{
    Ed ed;
    YewJobSpec spec = {0};
    JobStreamWitness w;
    char *argv[] = {
        (char *)"/bin/sh", (char *)"-c",
        (char *)"while :; do printf '0123456789abcdef'; done", NULL
    };
    char err[256] = {0};
    struct pollfd pfd[YEW_JOB_MAX * 4U];
    u32 id;
    YewJob *j;
    i64 start;
    bool ready = false;

    job_fixture(&ed);
    job_stream_witness_init(&w);
    spec.argv = argv;
    spec.sink = YEW_SINK_STREAM;
    spec.stream_owner = &w;
    spec.stream_ops = &job_stream_test_ops;
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    j = yew_job_find(&ed, id);
    YEW_ASSERT_NOT_NULL(j);
    start = yew_now_ms();
    while (!ready && yew_now_ms() - start < 2000) {
        u32 n = 0U;
        u32 i;

        yew_job_collect_fds(&ed, pfd, &n);
        (void)poll(pfd, (nfds_t)n, 20);
        for (i = 0U; i < n; i++) {
            if (pfd[i].fd == j->out_fd &&
                (pfd[i].revents & POLLIN) != 0) {
                ready = true;
                break;
            }
        }
        if (ready)
            yew_job_pump(&ed, pfd, n);
    }
    YEW_ASSERT(ready);
    YEW_ASSERT(w.out.len != 0U);
    YEW_ASSERT(w.out.len <= YEW_JOB_READ_BUDGET);
    YEW_ASSERT(yew_job_signal(&ed, id, SIGKILL));
    YEW_ASSERT(run_to_completion(&ed, id));
    YEW_ASSERT_EQ_U64(w.destroys, 1U);
    yew_ed_free(&ed);
    job_stream_witness_free(&w);
}

void test_job_stream_callback_failure_terminates_job(void)
{
    Ed ed;
    YewJobSpec spec = {0};
    JobStreamWitness w;
    char *argv[] = {
        (char *)"/bin/sh", (char *)"-c",
        /* The callback's SIGTERM kills the foreground sleep.  The shell's
         * handler then starts a new member of the same process group and
         * exits, leaving a reaped leader while the descendant still owns
         * stdout/stderr.  SIGKILL escalation must follow the group, not
         * stop merely because the leader was reaped. */
        (char *)"trap 'sleep 30 & exit 0' TERM; printf x; sleep 30", NULL
    };
    char err[256] = {0};
    u32 id;
    YewJob *j;

    job_fixture(&ed);
    job_stream_witness_init(&w);
    w.reject_stdout = true;
    spec.argv = argv;
    spec.sink = YEW_SINK_STREAM;
    spec.stream_owner = &w;
    spec.stream_ops = &job_stream_test_ops;
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(run_to_completion(&ed, id));
    j = yew_job_find(&ed, id);
    YEW_ASSERT_NOT_NULL(j);
    YEW_ASSERT(j->stream_failed);
    YEW_ASSERT(j->state == YEW_JOB_SIGNALED || j->state == YEW_JOB_EXITED);
    YEW_ASSERT_EQ_U64(w.destroys, 1U);
    yew_ed_free(&ed);
    job_stream_witness_free(&w);
}

void test_job_no_fd_or_zombie_leak_across_many_spawns(void)
{
    Ed ed;
    char *argv[3];
    char err[256] = {0};
    u32 id;
    u32 before;
    u32 after;
    u32 i;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/echo";
    argv[1] = (char *)"x";
    argv[2] = NULL;
    /* Warm up once so one-time allocations do not count as a leak. */
    id = spawn_argv(&ed, argv, err, sizeof(err));
    YEW_ASSERT(run_to_completion(&ed, id));
    assert_job_child_reaped(&ed, id);
    release_finished(&ed);
    before = open_fd_count();
    for (i = 0U; i < 120U; i++) {
        id = spawn_argv(&ed, argv, err, sizeof(err));

        YEW_ASSERT(id != 0U);
        YEW_ASSERT(run_to_completion(&ed, id));
        assert_job_child_reaped(&ed, id);
        /* Recycle the table the way ed.job.clear_finished does. */
        release_finished(&ed);
    }
    after = open_fd_count();
    /* DoD 8: delta zero.  A missed close here is the hang nobody
     * diagnoses, because it presents as "that command is slow". */
    YEW_ASSERT_EQ_U64(after, before);
    yew_ed_free(&ed);
}

void test_job_running_count_tracks_state(void)
{
    Ed ed;
    char *argv[4];
    char err[256] = {0};
    u32 id;

    job_fixture(&ed);
    YEW_ASSERT_EQ_U64(yew_job_running_count(&ed), 0U);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"sleep 30";
    argv[3] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    YEW_ASSERT_EQ_U64(yew_job_running_count(&ed), 1U);
    (void)yew_job_signal(&ed, id, SIGKILL);
    YEW_ASSERT(run_to_completion(&ed, id));
    /* The badge clears at zero. */
    YEW_ASSERT_EQ_U64(yew_job_running_count(&ed), 0U);
    yew_ed_free(&ed);
}

/*
 * Waits for a process GROUP to be gone, up to `budget_ms`.
 *
 * `kill(-pgid, 0)` keeps succeeding while any member is still a zombie,
 * and the grandchild the shell spawned is reaped by init on its own
 * schedule — so an instant assertion is a race that a loaded CI runner
 * loses and a quiet laptop wins.  The CLAIM is that the escalation ends
 * the group, not that it has already ended by the time the next line
 * runs; this waits for the claim rather than weakening it.
 */
static bool group_gone_within(pid_t pgid, i64 budget_ms)
{
    i64 start = yew_now_ms();

    for (;;) {
        if (kill(-pgid, 0) != 0)
            return true;
        if (yew_now_ms() - start > budget_ms)
            return false;
        (void)poll(NULL, 0U, 5);
    }
}

void test_job_timeout_escalates_to_kill(void)
{
    Ed ed;
    YewJobSpec spec = {0};
    char *argv[4];
    char err[256] = {0};
    u32 id;
    YewJob *j;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    /* Ignores SIGTERM, so only the SIGKILL escalation can end it. */
    argv[2] = (char *)"trap '' TERM; sleep 30";
    argv[3] = NULL;
    spec.argv = argv;
    spec.sink = YEW_SINK_COLLECT;
    spec.timeout_ms = 150;
    id = yew_job_spawn(&ed, &spec, err, sizeof(err));
    YEW_ASSERT(id != 0U);
    YEW_ASSERT(run_to_completion(&ed, id));
    j = yew_job_find(&ed, id);
    YEW_ASSERT(j->state == YEW_JOB_TIMEOUT);
    YEW_ASSERT(group_gone_within(j->pgid, 5000));
    yew_ed_free(&ed);
}

void test_job_shell_resolution_prefers_env(void)
{
    const char *saved = getenv("SHELL");

    YEW_ASSERT(setenv("SHELL", "/bin/sh", 1) == 0);
    YEW_ASSERT_EQ_STR(yew_job_shell(), "/bin/sh");
    /* An empty $SHELL falls through rather than producing "". */
    YEW_ASSERT(setenv("SHELL", "", 1) == 0);
    YEW_ASSERT(yew_job_shell()[0] == '/');
    if (saved != NULL)
        (void)setenv("SHELL", saved, 1);
    else
        (void)unsetenv("SHELL");
}

void test_job_torture_spawn_kill_cycles(void)
{
    Ed ed;
    char *argv[4];
    char err[256] = {0};
    /* Deterministic LCG: the same cycle pattern every run (invariant 3). */
    u64 seed = 0x9E3779B97F4A7C15ULL;
    u32 before;
    u32 after;
    u32 cycle;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[3] = NULL;
    /* Warm up so first-touch allocations are not counted as leaks. */
    argv[2] = (char *)"printf x";
    {
        u32 id = spawn_argv(&ed, argv, err, sizeof(err));

        YEW_ASSERT(run_to_completion(&ed, id));
        assert_job_child_reaped(&ed, id);
    }
    release_finished(&ed);
    before = open_fd_count();

    /* DoD 8: 200 spawn/kill cycles at varying timings.  The interesting
     * cases are the races — killing before the child has exec'd, while it
     * is mid-write, and after it has already exited. */
    for (cycle = 0U; cycle < 200U; cycle++) {
        u32 shape;
        u32 id;

        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        shape = (u32)((seed >> 33) % 4U);
        switch (shape) {
        case 0U:
            argv[2] = (char *)"printf 'quick\n'";
            break;
        case 1U:
            argv[2] = (char *)"sleep 30";
            break;
        case 2U:
            argv[2] = (char *)"sleep 30 | cat";  /* a whole group */
            break;
        default:
            argv[2] = (char *)"printf 'a\n'; sleep 30";
            break;
        }
        id = spawn_argv(&ed, argv, err, sizeof(err));
        YEW_ASSERT(id != 0U);
        if (shape != 0U) {
            /* Pump once so some cycles kill a child mid-flight and others
             * kill one that has not been polled at all. */
            if ((seed & 0x10000U) != 0U) {
                struct pollfd pfd[YEW_JOB_MAX * 4U];
                u32 n = 0U;

                yew_job_collect_fds(&ed, pfd, &n);
                if (n != 0U)
                    (void)poll(pfd, (nfds_t)n, 1);
                yew_job_pump(&ed, pfd, n);
            }
            (void)yew_job_signal(&ed, id, SIGKILL);
        }
        YEW_ASSERT(run_to_completion(&ed, id));
        assert_job_child_reaped(&ed, id);
        release_finished(&ed);
    }

    after = open_fd_count();
    YEW_ASSERT_EQ_U64(after, before);
    /* The editor is still usable: one more job runs normally. */
    argv[2] = (char *)"printf 'still here\n'";
    {
        u32 id = spawn_argv(&ed, argv, err, sizeof(err));

        YEW_ASSERT(run_to_completion(&ed, id));
        assert_job_child_reaped(&ed, id);
    }
    YEW_ASSERT_EQ_U64(ed.jobs.v[0].bytes_out, 11U);
    yew_ed_free(&ed);
}
