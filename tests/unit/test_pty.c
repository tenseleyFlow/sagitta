#define _POSIX_C_SOURCE 200809L
#include "harness.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "../pty/harness.h"

void test_pty_environment_exact(void)
{
    static const char *const expected[] = {
        "TERM=xterm-256color",
        "SAG_COLORS=truecolor",
        "SAG_TTY_PROBE=1",
        "SAG_PROBE_TIMEOUT_MS=500",
        "SAG_ESC_TIMEOUT_MS=25",
        "XDG_STATE_HOME=/tmp/sagitta-pty-state",
        "LANG=C.UTF-8",
        "LC_ALL=C.UTF-8",
        "SAG_LOG_LEVEL=debug",
        /* Sprint 19 pins both so job goldens are byte-stable and
         * machine-independent. */
        "SAG_JOB_ELAPSED_MS=1240",
        "SHELL=/bin/sh"
    };
    char *envp[SAG_PTY_ENV_COUNT + 1U] = {0};
    size_t i;

    /* Keeps the two in lockstep: adding a key without an expectation
     * would otherwise read past `expected` (it did, once). */
    SAG_ASSERT_EQ_U64((u64)SAG_ARRAY_LEN(expected),
                      (u64)SAG_PTY_ENV_COUNT);

    SAG_ASSERT(ptc_env_build(envp, "truecolor", "/tmp/sagitta-pty-state"));
    for (i = 0U; i < SAG_PTY_ENV_COUNT; i++)
        SAG_ASSERT_EQ_STR(envp[i], expected[i]);
    SAG_ASSERT_NULL(envp[SAG_PTY_ENV_COUNT]);
    ptc_env_free(envp);
    for (i = 0U; i <= SAG_PTY_ENV_COUNT; i++)
        SAG_ASSERT_NULL(envp[i]);
}

void test_pty_spawn_clears_signal_mask(void)
{
    static const PtyCase test = {
        "signal_mask", "dumb", 2U, 8U, NULL
    };
    sigset_t blocked;
    sigset_t saved;
    PtyCtx ctx;
    i64 deadline = ptc_now_ms() + 1000;

    SAG_ASSERT_EQ_I64(sigemptyset(&blocked), 0);
    SAG_ASSERT_EQ_I64(sigaddset(&blocked, SIGTERM), 0);
    SAG_ASSERT_EQ_I64(sigprocmask(SIG_BLOCK, &blocked, &saved), 0);
    ptc_init(&ctx, &test, "/tmp/sagitta-pty-signal-mask", "/bin/sleep",
             "/bin/sleep", 1000, deadline);
    ptc_spawn(&ctx, "/bin/sleep", "300", NULL);
    SAG_ASSERT_EQ_I64(sigprocmask(SIG_SETMASK, &saved, NULL), 0);
    SAG_ASSERT(ctx.spawned);
    SAG_ASSERT_EQ_I64(kill(ctx.pty.pid, SIGTERM), 0);
    ptc_expect_signal(&ctx, SIGTERM);
    SAG_ASSERT(!ctx.failed);
    ptc_cleanup(&ctx);
    SAG_ASSERT(ctx.pty.reaped);
    SAG_ASSERT_EQ_I64(ctx.pty.master, -1);
    SAG_ASSERT(ptc_sweep_all());
    ptc_dispose(&ctx);
}

void test_pty_timeout_reaps_child(void)
{
    static const PtyCase test = {
        "timeout_drill", "dumb", 2U, 8U, NULL
    };
    PtyCtx ctx;
    i64 deadline = ptc_now_ms() + 50;

    ptc_init(&ctx, &test, "/tmp/sagitta-pty-timeout", "/bin/sleep",
             "/bin/sleep", 50, deadline);
    ptc_spawn(&ctx, "/bin/sleep", "300", NULL);
    SAG_ASSERT(ctx.spawned);
    ptc_settle(&ctx, 0);
    SAG_ASSERT(ctx.failed);
    SAG_ASSERT(ctx.timed_out);
    ptc_cleanup(&ctx);
    SAG_ASSERT(ctx.pty.reaped);
    SAG_ASSERT_EQ_I64(ctx.pty.master, -1);
    SAG_ASSERT(WIFSIGNALED(ctx.pty.status));
    SAG_ASSERT_EQ_I64(WTERMSIG(ctx.pty.status), SIGKILL);
    SAG_ASSERT(ptc_sweep_all());
    ptc_dispose(&ctx);
}

void test_pty_post_snapshot_protocol_error_fails_cleanup(void)
{
    static const PtyCase test = {
        "post_snapshot_error", "dumb", 2U, 8U, NULL
    };
    static const char script[] =
        "printf '\\033[?1049hOK'; "
        "IFS= read -r line; "
        "printf '\\033[5L'";
    PtyCtx ctx;
    i64 deadline = ptc_now_ms() + 1000;

    ptc_init(&ctx, &test, "/tmp/sagitta-pty-post-snapshot", "/bin/sh",
             "/bin/sh", 1000, deadline);
    ptc_spawn(&ctx, "/bin/sh", "-c", script, NULL);
    ptc_settle(&ctx, 0);
    SAG_ASSERT(ctx.ready);
    ptc_snapshot(&ctx, "post_snapshot_error");
    SAG_ASSERT(ctx.snapshot_taken);
    ptc_bytes(&ctx, "q\n");
    ptc_expect_exit(&ctx, 0);
    SAG_ASSERT(!ctx.failed);
    ptc_cleanup(&ctx);
    SAG_ASSERT(ctx.failed);
    SAG_ASSERT(strstr(ctx.failure, "unknown sequence: ESC [ 5 L") != NULL);
    SAG_ASSERT(ctx.pty.reaped);
    SAG_ASSERT_EQ_I64(ctx.pty.master, -1);
    SAG_ASSERT(ptc_sweep_all());
    ptc_dispose(&ctx);
}
