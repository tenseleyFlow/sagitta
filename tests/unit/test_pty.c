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
        "YEW_COLORS=truecolor",
        "YEW_TTY_PROBE=1",
        "YEW_PROBE_TIMEOUT_MS=500",
        "YEW_ESC_TIMEOUT_MS=25",
        "XDG_STATE_HOME=/tmp/yew-pty-state",
        "XDG_CONFIG_HOME=/tmp/yew-pty-state",
        "LANG=C.UTF-8",
        "LC_ALL=C.UTF-8",
        "YEW_LOG_LEVEL=debug",
        /* Sprint 19 pins both so job goldens are byte-stable and
         * machine-independent. */
        "YEW_JOB_ELAPSED_MS=1240",
        "SHELL=/bin/sh",
        /* Sprint 26 pins the undo picker's relative timestamps, so
         * "3 minutes ago" is the same string on every run. */
        "YEW_PICKERS_NOW=1700000000",
        /* Sprint 27 §7's remaining degradation variant. NO_COLOR is
         * absent from baseline cases because even an empty value is set. */
        "YEW_ASCII=0",
        "YEW_RUNTIME_DIR=/tmp/yew-runtime",
        /* Sprint 41 cold/warm syntax-cache PTYs share one isolated cache
         * root across their two independent editor launches. */
        "XDG_CACHE_HOME=/tmp/yew-pty-state",
        /* Sprint 43's deterministic provider is opt-in per PTY case. */
        "YEW_SHADOW_TEST=0",
        /* Sprint 49's live-provider PTYs opt into deterministic mocks. */
        "YEW_AI_MOCK=1"
    };
    char *envp[YEW_PTY_ENV_COUNT + 1U] = {0};
    size_t i;

    /* Keeps the two in lockstep: adding a key without an expectation
     * would otherwise read past `expected` (it did, once). */
    YEW_ASSERT_EQ_U64((u64)YEW_ARRAY_LEN(expected) + 1U,
                      (u64)YEW_PTY_ENV_COUNT);

    YEW_ASSERT(ptc_env_build(envp, "xterm-256color", "truecolor",
                             "/tmp/yew-pty-state",
                             NULL, "0", "/tmp/yew-runtime", "0"));
    for (i = 0U; i < YEW_ARRAY_LEN(expected); i++)
        YEW_ASSERT_EQ_STR(envp[i], expected[i]);
    for (; i <= YEW_PTY_ENV_COUNT; i++)
        YEW_ASSERT_NULL(envp[i]);
    ptc_env_free(envp);
    for (i = 0U; i <= YEW_PTY_ENV_COUNT; i++)
        YEW_ASSERT_NULL(envp[i]);

    YEW_ASSERT(ptc_env_build(envp, "xterm-256color", "truecolor",
                             "/tmp/yew-pty-state",
                             "", "0", "/tmp/yew-runtime", "0"));
    YEW_ASSERT_EQ_STR(envp[13], "NO_COLOR=");
    YEW_ASSERT_NULL(envp[YEW_PTY_ENV_COUNT]);
    ptc_env_free(envp);

    YEW_ASSERT(ptc_env_build(envp, "xterm-256color", "truecolor",
                             "/tmp/yew-pty-state",
                             "0", "0", "/tmp/yew-runtime", "0"));
    YEW_ASSERT_EQ_STR(envp[13], "NO_COLOR=0");
    YEW_ASSERT_NULL(envp[YEW_PTY_ENV_COUNT]);
    ptc_env_free(envp);

    YEW_ASSERT(ptc_env_build(envp, "dumb", "16", "/tmp/yew-pty-state",
                             NULL, "0", "/tmp/yew-runtime", "0"));
    YEW_ASSERT_EQ_STR(envp[0], "TERM=dumb");
    ptc_env_free(envp);
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

    YEW_ASSERT_EQ_I64(sigemptyset(&blocked), 0);
    YEW_ASSERT_EQ_I64(sigaddset(&blocked, SIGTERM), 0);
    YEW_ASSERT_EQ_I64(sigprocmask(SIG_BLOCK, &blocked, &saved), 0);
    ptc_init(&ctx, &test, "/tmp/yew-pty-signal-mask", "/bin/sleep",
             "/bin/sleep", 1000, deadline);
    ptc_spawn(&ctx, "/bin/sleep", "300", NULL);
    YEW_ASSERT_EQ_I64(sigprocmask(SIG_SETMASK, &saved, NULL), 0);
    YEW_ASSERT(ctx.spawned);
    YEW_ASSERT_EQ_I64(kill(ctx.pty.pid, SIGTERM), 0);
    ptc_expect_signal(&ctx, SIGTERM);
    YEW_ASSERT(!ctx.failed);
    ptc_cleanup(&ctx);
    YEW_ASSERT(ctx.pty.reaped);
    YEW_ASSERT_EQ_I64(ctx.pty.master, -1);
    YEW_ASSERT(ptc_sweep_all());
    ptc_dispose(&ctx);
}

void test_pty_timeout_reaps_child(void)
{
    static const PtyCase test = {
        "timeout_drill", "dumb", 2U, 8U, NULL
    };
    PtyCtx ctx;
    i64 deadline = ptc_now_ms() + 50;

    ptc_init(&ctx, &test, "/tmp/yew-pty-timeout", "/bin/sleep",
             "/bin/sleep", 50, deadline);
    ptc_spawn(&ctx, "/bin/sleep", "300", NULL);
    YEW_ASSERT(ctx.spawned);
    ptc_settle(&ctx, 0);
    YEW_ASSERT(ctx.failed);
    YEW_ASSERT(ctx.timed_out);
    ptc_cleanup(&ctx);
    YEW_ASSERT(ctx.pty.reaped);
    YEW_ASSERT_EQ_I64(ctx.pty.master, -1);
    YEW_ASSERT(WIFSIGNALED(ctx.pty.status));
    YEW_ASSERT_EQ_I64(WTERMSIG(ctx.pty.status), SIGKILL);
    YEW_ASSERT(ptc_sweep_all());
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

    ptc_init(&ctx, &test, "/tmp/yew-pty-post-snapshot", "/bin/sh",
             "/bin/sh", 1000, deadline);
    ptc_spawn(&ctx, "/bin/sh", "-c", script, NULL);
    ptc_settle(&ctx, 0);
    YEW_ASSERT(ctx.ready);
    ptc_snapshot(&ctx, "post_snapshot_error");
    YEW_ASSERT(ctx.snapshot_taken);
    ptc_bytes(&ctx, "q\n");
    ptc_expect_exit(&ctx, 0);
    YEW_ASSERT(!ctx.failed);
    ptc_cleanup(&ctx);
    YEW_ASSERT(ctx.failed);
    YEW_ASSERT(strstr(ctx.failure, "unknown sequence: ESC [ 5 L") != NULL);
    YEW_ASSERT(ctx.pty.reaped);
    YEW_ASSERT_EQ_I64(ctx.pty.master, -1);
    YEW_ASSERT(ptc_sweep_all());
    ptc_dispose(&ctx);
}
