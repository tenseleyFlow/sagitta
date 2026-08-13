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

static u32 spawn_argv(Ed *ed, char **argv, char *err, size_t errsz)
{
    YewJobSpec spec = {0};

    spec.argv = argv;
    spec.sink = YEW_SINK_COLLECT;
    return yew_job_spawn(ed, &spec, err, errsz);
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
    YEW_ASSERT(run_to_completion(&ed, spawn_argv(&ed, argv, err,
                                                 sizeof(err))));
    release_finished(&ed);
    before = open_fd_count();
    for (i = 0U; i < 120U; i++) {
        u32 id = spawn_argv(&ed, argv, err, sizeof(err));

        YEW_ASSERT(id != 0U);
        YEW_ASSERT(run_to_completion(&ed, id));
        /* Recycle the table the way ed.job.clear_finished does. */
        release_finished(&ed);
    }
    after = open_fd_count();
    /* DoD 8: delta zero.  A missed close here is the hang nobody
     * diagnoses, because it presents as "that command is slow". */
    YEW_ASSERT_EQ_U64(after, before);
    /* And no zombies: every child was reaped. */
    YEW_ASSERT(waitpid(-1, NULL, WNOHANG) == -1);
    YEW_ASSERT_EQ_I64(errno, ECHILD);
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
    YEW_ASSERT(run_to_completion(&ed, spawn_argv(&ed, argv, err,
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
        release_finished(&ed);
    }

    after = open_fd_count();
    YEW_ASSERT_EQ_U64(after, before);
    /* Zero zombies at quiesce. */
    YEW_ASSERT(waitpid(-1, NULL, WNOHANG) == -1);
    YEW_ASSERT_EQ_I64(errno, ECHILD);
    /* The editor is still usable: one more job runs normally. */
    argv[2] = (char *)"printf 'still here\n'";
    YEW_ASSERT(run_to_completion(&ed, spawn_argv(&ed, argv, err,
                                                 sizeof(err))));
    YEW_ASSERT_EQ_U64(ed.jobs.v[0].bytes_out, 11U);
    yew_ed_free(&ed);
}
