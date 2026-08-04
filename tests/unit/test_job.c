/* Sprint 19 §1-§2 + DoD 3, 8: job lifecycle, exec-failure precision, the
 * concurrency cap, and fd/zombie accounting. */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"

static void job_fixture(Ed *ed)
{
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
}

/* Drives one job to completion off the main loop, the way sag_loop_run
 * would: poll the job fds, pump, reap.  Returns false on timeout. */
static bool run_to_completion(Ed *ed, u32 id)
{
    i64 start = sag_now_ms();

    for (;;) {
        struct pollfd pfd[SAG_JOB_MAX * 4U];
        u32 n = 0U;
        SagJob *j = sag_job_find(ed, id);

        if (j == NULL)
            return false;
        if (j->state != SAG_JOB_RUNNING && j->reaped)
            return true;
        sag_job_collect_fds(ed, pfd, &n);
        if (n != 0U)
            (void)poll(pfd, (nfds_t)n, 20);
        else
            (void)poll(NULL, 0U, 5);
        sag_job_pump(ed, pfd, n);
        sag_job_reap(ed);
        sag_job_tick(ed, sag_now_ms());
        if (sag_now_ms() - start > 10000)
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
        if (ed->jobs.v[i].state == SAG_JOB_RUNNING) {
            i++;
            continue;
        }
        sag_job_release(ed, &ed->jobs.v[i]);
    }
}

static u32 spawn_argv(Ed *ed, char **argv, char *err, size_t errsz)
{
    SagJobSpec spec = {0};

    spec.argv = argv;
    spec.sink = SAG_SINK_COLLECT;
    return sag_job_spawn(ed, &spec, err, errsz);
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
    SagJob *j;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/echo";
    argv[1] = (char *)"hi";
    argv[2] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    SAG_ASSERT(id != 0U);
    SAG_ASSERT_EQ_STR(err, "");
    SAG_ASSERT(run_to_completion(&ed, id));
    j = sag_job_find(&ed, id);
    SAG_ASSERT_NOT_NULL(j);
    SAG_ASSERT(j->state == SAG_JOB_EXITED);
    SAG_ASSERT_EQ_I64(j->exit_code, 0);
    SAG_ASSERT_EQ_U64(j->bytes_out, 3U);
    SAG_ASSERT_EQ_MEM(j->collect.data, "hi\n", 3U);
    /* Every parent-side descriptor is closed by completion. */
    SAG_ASSERT_EQ_I64(j->out_fd, -1);
    SAG_ASSERT_EQ_I64(j->err_fd, -1);
    SAG_ASSERT_EQ_I64(j->in_fd, -1);
    /* The exec-status pipe closes too: closing it empty IS the success
     * signal, so a still-open exec_fd would mean we never observed it. */
    SAG_ASSERT_EQ_I64(j->exec_fd, -1);
    {
        u32 fds_before_free = open_fd_count();

        sag_ed_free(&ed);
        /* Teardown must not strand descriptors; valgrind --track-fds
         * reports the forked child's inherited copies, so this is the
         * assertion that speaks for the parent. */
        SAG_ASSERT(open_fd_count() <= fds_before_free);
    }
}

void test_job_exec_failure_is_not_exit_127(void)
{
    Ed ed;
    char *argv[2];
    char err[256] = {0};
    u32 id;
    SagJob *j;

    job_fixture(&ed);
    argv[0] = (char *)"/nonexistent/definitely-not-here";
    argv[1] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    SAG_ASSERT(id != 0U);
    SAG_ASSERT(run_to_completion(&ed, id));
    j = sag_job_find(&ed, id);
    /* DoD 3: without the exec-status pipe this would be indistinguishable
     * from a command that ran and exited 127. */
    SAG_ASSERT(j->state == SAG_JOB_EXECFAIL);
    SAG_ASSERT_EQ_I64(j->exec_errno, ENOENT);
    sag_ed_free(&ed);
}

void test_job_exit_127_is_not_exec_failure(void)
{
    Ed ed;
    char *argv[4];
    char err[256] = {0};
    u32 id;
    SagJob *j;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"exit 127";
    argv[3] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    SAG_ASSERT(id != 0U);
    SAG_ASSERT(run_to_completion(&ed, id));
    j = sag_job_find(&ed, id);
    /* The other half of DoD 3: a real 127 stays EXITED. */
    SAG_ASSERT(j->state == SAG_JOB_EXITED);
    SAG_ASSERT_EQ_I64(j->exit_code, 127);
    SAG_ASSERT_EQ_I64(j->exec_errno, 0);
    sag_ed_free(&ed);
}

void test_job_exit_code_and_stderr(void)
{
    Ed ed;
    char *argv[4];
    char err[256] = {0};
    u32 id;
    SagJob *j;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"echo oops >&2; exit 3";
    argv[3] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    SAG_ASSERT(run_to_completion(&ed, id));
    j = sag_job_find(&ed, id);
    SAG_ASSERT(j->state == SAG_JOB_EXITED);
    SAG_ASSERT_EQ_I64(j->exit_code, 3);
    SAG_ASSERT_EQ_U64(j->bytes_err, 5U);
    SAG_ASSERT_EQ_U64(j->bytes_out, 0U);
    sag_ed_free(&ed);
}

void test_job_killed_by_signal(void)
{
    Ed ed;
    char *argv[4];
    char err[256] = {0};
    u32 id;
    SagJob *j;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"sleep 30";
    argv[3] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    SAG_ASSERT(id != 0U);
    SAG_ASSERT(sag_job_signal(&ed, id, SIGKILL));
    SAG_ASSERT(run_to_completion(&ed, id));
    j = sag_job_find(&ed, id);
    SAG_ASSERT(j->state == SAG_JOB_SIGNALED);
    SAG_ASSERT_EQ_I64(j->termsig, SIGKILL);
    sag_ed_free(&ed);
}

void test_job_signal_reaches_the_whole_group(void)
{
    Ed ed;
    char *argv[4];
    char err[256] = {0};
    u32 id;
    SagJob *j;

    job_fixture(&ed);
    /* Killing the pid alone would leave `sleep` running and the pipeline
     * unkillable; we signal -pgid. */
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"sleep 30 | cat";
    argv[3] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    SAG_ASSERT(id != 0U);
    j = sag_job_find(&ed, id);
    SAG_ASSERT(j->pgid > 0);
    SAG_ASSERT(sag_job_signal(&ed, id, SIGKILL));
    SAG_ASSERT(run_to_completion(&ed, id));
    /* Nothing from that group survives. */
    SAG_ASSERT(kill(-j->pgid, 0) != 0);
    SAG_ASSERT_EQ_I64(errno, ESRCH);
    sag_ed_free(&ed);
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
    for (i = 0U; i < SAG_JOB_MAX; i++)
        SAG_ASSERT(spawn_argv(&ed, argv, err, sizeof(err)) != 0U);
    /* Refused with a message, never silently queued: a job that starts
     * minutes later surprises the user. */
    id = spawn_argv(&ed, argv, err, sizeof(err));
    SAG_ASSERT_EQ_U64(id, 0U);
    SAG_ASSERT_EQ_STR(err, "too many jobs (32); finish or kill one first");
    for (i = 0U; i < SAG_JOB_MAX; i++)
        (void)sag_job_signal(&ed, i + 1U, SIGKILL);
    sag_ed_free(&ed);
}

void test_job_stdin_region_is_piped(void)
{
    Ed ed;
    SagJobSpec spec = {0};
    TextBuf *tb;
    char err[256] = {0};
    u32 id;
    SagJob *j;
    char *argv[4];

    job_fixture(&ed);
    tb = sag_textbuf_from_bytes((const u8 *)"beta\nalpha\ngamma\n", 17U);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"sort";
    argv[3] = NULL;
    spec.argv = argv;
    spec.sink = SAG_SINK_COLLECT;
    spec.in_buf = tb;
    spec.in_span = (Span){0U, 17U};
    id = sag_job_spawn(&ed, &spec, err, sizeof(err));
    SAG_ASSERT(id != 0U);
    SAG_ASSERT(run_to_completion(&ed, id));
    j = sag_job_find(&ed, id);
    SAG_ASSERT(j->state == SAG_JOB_EXITED);
    /* sort emits nothing until stdin EOF: this passing proves we close
     * the write end at end-of-region. */
    SAG_ASSERT_EQ_U64((u64)j->collect.len, 17U);
    SAG_ASSERT_EQ_MEM(j->collect.data, "alpha\nbeta\ngamma\n", 17U);
    sag_textbuf_free(tb);
    sag_ed_free(&ed);
}

void test_job_no_fd_or_zombie_leak_across_many_spawns(void)
{
    Ed ed;
    char *argv[3];
    char err[256] = {0};
    u32 before;
    u32 after;
    u32 i;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/echo";
    argv[1] = (char *)"x";
    argv[2] = NULL;
    /* Warm up once so one-time allocations do not count as a leak. */
    SAG_ASSERT(run_to_completion(&ed, spawn_argv(&ed, argv, err,
                                                 sizeof(err))));
    release_finished(&ed);
    before = open_fd_count();
    for (i = 0U; i < 120U; i++) {
        u32 id = spawn_argv(&ed, argv, err, sizeof(err));

        SAG_ASSERT(id != 0U);
        SAG_ASSERT(run_to_completion(&ed, id));
        /* Recycle the table the way ed.job.clear_finished does. */
        release_finished(&ed);
    }
    after = open_fd_count();
    /* DoD 8: delta zero.  A missed close here is the hang nobody
     * diagnoses, because it presents as "that command is slow". */
    SAG_ASSERT_EQ_U64(after, before);
    /* And no zombies: every child was reaped. */
    SAG_ASSERT(waitpid(-1, NULL, WNOHANG) == -1);
    SAG_ASSERT_EQ_I64(errno, ECHILD);
    sag_ed_free(&ed);
}

void test_job_running_count_tracks_state(void)
{
    Ed ed;
    char *argv[4];
    char err[256] = {0};
    u32 id;

    job_fixture(&ed);
    SAG_ASSERT_EQ_U64(sag_job_running_count(&ed), 0U);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"sleep 30";
    argv[3] = NULL;
    id = spawn_argv(&ed, argv, err, sizeof(err));
    SAG_ASSERT_EQ_U64(sag_job_running_count(&ed), 1U);
    (void)sag_job_signal(&ed, id, SIGKILL);
    SAG_ASSERT(run_to_completion(&ed, id));
    /* The badge clears at zero. */
    SAG_ASSERT_EQ_U64(sag_job_running_count(&ed), 0U);
    sag_ed_free(&ed);
}

void test_job_timeout_escalates_to_kill(void)
{
    Ed ed;
    SagJobSpec spec = {0};
    char *argv[4];
    char err[256] = {0};
    u32 id;
    SagJob *j;

    job_fixture(&ed);
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    /* Ignores SIGTERM, so only the SIGKILL escalation can end it. */
    argv[2] = (char *)"trap '' TERM; sleep 30";
    argv[3] = NULL;
    spec.argv = argv;
    spec.sink = SAG_SINK_COLLECT;
    spec.timeout_ms = 150;
    id = sag_job_spawn(&ed, &spec, err, sizeof(err));
    SAG_ASSERT(id != 0U);
    SAG_ASSERT(run_to_completion(&ed, id));
    j = sag_job_find(&ed, id);
    SAG_ASSERT(j->state == SAG_JOB_TIMEOUT);
    SAG_ASSERT(kill(-j->pgid, 0) != 0);
    sag_ed_free(&ed);
}

void test_job_shell_resolution_prefers_env(void)
{
    const char *saved = getenv("SHELL");

    SAG_ASSERT(setenv("SHELL", "/bin/sh", 1) == 0);
    SAG_ASSERT_EQ_STR(sag_job_shell(), "/bin/sh");
    /* An empty $SHELL falls through rather than producing "". */
    SAG_ASSERT(setenv("SHELL", "", 1) == 0);
    SAG_ASSERT(sag_job_shell()[0] == '/');
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
    SAG_ASSERT(run_to_completion(&ed, spawn_argv(&ed, argv, err,
                                                 sizeof(err))));
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
        SAG_ASSERT(id != 0U);
        if (shape != 0U) {
            /* Pump once so some cycles kill a child mid-flight and others
             * kill one that has not been polled at all. */
            if ((seed & 0x10000U) != 0U) {
                struct pollfd pfd[SAG_JOB_MAX * 4U];
                u32 n = 0U;

                sag_job_collect_fds(&ed, pfd, &n);
                if (n != 0U)
                    (void)poll(pfd, (nfds_t)n, 1);
                sag_job_pump(&ed, pfd, n);
            }
            (void)sag_job_signal(&ed, id, SIGKILL);
        }
        SAG_ASSERT(run_to_completion(&ed, id));
        release_finished(&ed);
    }

    after = open_fd_count();
    SAG_ASSERT_EQ_U64(after, before);
    /* Zero zombies at quiesce. */
    SAG_ASSERT(waitpid(-1, NULL, WNOHANG) == -1);
    SAG_ASSERT_EQ_I64(errno, ECHILD);
    /* The editor is still usable: one more job runs normally. */
    argv[2] = (char *)"printf 'still here\n'";
    SAG_ASSERT(run_to_completion(&ed, spawn_argv(&ed, argv, err,
                                                 sizeof(err))));
    SAG_ASSERT_EQ_U64(ed.jobs.v[0].bytes_out, 11U);
    sag_ed_free(&ed);
}
