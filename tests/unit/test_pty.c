#define _POSIX_C_SOURCE 200809L
#include "harness.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "../pty/harness.h"
#include "../pty/snapshot.h"

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
        "YEW_AI_MOCK=1",
        /* Sprint 53 keeps non-repository PTYs from discovering the source
         * checkout above their isolated workspace. */
        "GIT_CEILING_DIRECTORIES=/tmp/yew-pty-state"
    };
    char *envp[YEW_PTY_ENV_COUNT + 1U] = {0};
    size_t i;

    /* NO_COLOR and Sprint 57's two opt-in profiling variables are absent
     * from this baseline. */
    YEW_ASSERT_EQ_U64((u64)YEW_ARRAY_LEN(expected) + 3U,
                      (u64)YEW_PTY_ENV_COUNT);

    YEW_ASSERT(ptc_env_build(envp, "xterm-256color", "truecolor",
                             "/tmp/yew-pty-state",
                             NULL, "0", "/tmp/yew-runtime", "0",
                             NULL, NULL));
    for (i = 0U; i < YEW_ARRAY_LEN(expected); i++)
        YEW_ASSERT_EQ_STR(envp[i], expected[i]);
    for (; i <= YEW_PTY_ENV_COUNT; i++)
        YEW_ASSERT_NULL(envp[i]);
    ptc_env_free(envp);
    for (i = 0U; i <= YEW_PTY_ENV_COUNT; i++)
        YEW_ASSERT_NULL(envp[i]);

    YEW_ASSERT(ptc_env_build(envp, "xterm-256color", "truecolor",
                             "/tmp/yew-pty-state",
                             "", "0", "/tmp/yew-runtime", "0",
                             NULL, NULL));
    YEW_ASSERT_EQ_STR(envp[13], "NO_COLOR=");
    YEW_ASSERT_NULL(envp[YEW_PTY_ENV_COUNT]);
    ptc_env_free(envp);

    YEW_ASSERT(ptc_env_build(envp, "xterm-256color", "truecolor",
                             "/tmp/yew-pty-state",
                             "0", "0", "/tmp/yew-runtime", "0",
                             NULL, NULL));
    YEW_ASSERT_EQ_STR(envp[13], "NO_COLOR=0");
    YEW_ASSERT_NULL(envp[YEW_PTY_ENV_COUNT]);
    ptc_env_free(envp);

    YEW_ASSERT(ptc_env_build(envp, "dumb", "16", "/tmp/yew-pty-state",
                             NULL, "0", "/tmp/yew-runtime", "0",
                             "1", "/tmp/yew-rss.log"));
    YEW_ASSERT_EQ_STR(envp[0], "TERM=dumb");
    YEW_ASSERT_EQ_STR(envp[19], "YEW_PROF=1");
    YEW_ASSERT_EQ_STR(envp[20], "YEW_LOG=/tmp/yew-rss.log");
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

static bool pty_screen_contains(const PtyCtx *c, const void *arg)
{
    Bytebuf screen;
    bool found;

    bytebuf_init(&screen);
    snapshot_write(&c->vt, &screen);
    bytebuf_push_u8(&screen, 0U);
    found = strstr((const char *)screen.data, (const char *)arg) != NULL;
    bytebuf_free(&screen);
    return found;
}

/*
 * A wait NEVER fires on half of a synchronized frame.
 *
 * The editor brackets every cell-bearing frame in DECSET 2026 so that a
 * terminal shows the whole frame or none of it.  The harness VT has no such
 * buffer, so without an explicit gate a predicate could match cells written
 * early in a frame while cells the same frame overwrites later were still
 * showing the previous state — a grid no user can observe, and one whose
 * exact shape depends on where the kernel split the child's writes.  That
 * is what made fuss_group_picker disagree between two executions.
 *
 * The fixture writes `HEAD` inside a synchronized update, pauses, then
 * writes `TAIL` and closes it.  A wait for `HEAD` must not return until
 * `TAIL` is on screen too.
 */
void test_pty_wait_never_observes_a_torn_frame(void)
{
    static const PtyCase test = {
        "torn_frame", "dumb", 4U, 16U, NULL
    };
    static const char script[] =
        "printf '\\033[?1049h'; "
        "printf '\\033[?2026h\\033[1;1HHEAD'; "
        "sleep 1; "
        "printf '\\033[2;1HTAIL\\033[?2026l'; "
        "IFS= read -r line";
    PtyCtx ctx;
    i64 deadline = ptc_now_ms() + 20000;

    ptc_init(&ctx, &test, "/tmp/yew-pty-torn-frame", "/bin/sh",
             "/bin/sh", 20000, deadline);
    ptc_spawn(&ctx, "/bin/sh", "-c", script, NULL);
    YEW_ASSERT(ctx.spawned);
    ptc_wait_until(&ctx, pty_screen_contains, "HEAD",
                   "torn-frame fixture never published its frame");
    YEW_ASSERT(!ctx.failed);
    YEW_ASSERT(!ctx.vt.in_sync);
    YEW_ASSERT(pty_screen_contains(&ctx, "TAIL"));
    YEW_ASSERT_EQ_U64(ctx.vt.nsync_pairs, 1u);
    ptc_bytes(&ctx, "q\n");
    ptc_expect_exit(&ctx, 0);
    YEW_ASSERT(!ctx.failed);
    ptc_cleanup(&ctx);
    YEW_ASSERT(ctx.pty.reaped);
    YEW_ASSERT(ptc_sweep_all());
    ptc_dispose(&ctx);
}

/*
 * And a snapshot is never taken from one either.  A case that reaches its
 * snapshot with a frame still open has observed a grid that never existed;
 * saying so by name beats baking the torn cells into a golden.
 *
 * The open frame is produced by feeding the VT the same opening DECSET the
 * editor writes, rather than by a child that stalls mid-frame: every settle
 * in the harness now refuses to call an unfinished frame quiet, so a fixture
 * wedged inside one could only be reached by burning the case budget.
 */
void test_pty_snapshot_refuses_an_open_frame(void)
{
    static const PtyCase test = {
        "open_frame", "dumb", 4U, 16U, NULL
    };
    static const char script[] =
        "printf '\\033[?1049hOK'; IFS= read -r line";
    PtyCtx ctx;
    i64 deadline = ptc_now_ms() + 5000;

    ptc_init(&ctx, &test, "/tmp/yew-pty-open-frame", "/bin/sh",
             "/bin/sh", 5000, deadline);
    ptc_spawn(&ctx, "/bin/sh", "-c", script, NULL);
    YEW_ASSERT(ctx.spawned);
    ptc_settle(&ctx, 0);
    YEW_ASSERT(ctx.ready);
    YEW_ASSERT(!ctx.failed);
    vt_feed(&ctx.vt, (const u8 *)"\033[?2026h", 8U);
    YEW_ASSERT(ctx.vt.in_sync);
    ptc_snapshot(&ctx, "open_frame");
    YEW_ASSERT(ctx.failed);
    YEW_ASSERT(!ctx.snapshot_taken);
    YEW_ASSERT(strstr(ctx.failure, "unfinished synchronized frame") != NULL);
    vt_feed(&ctx.vt, (const u8 *)"\033[?2026l", 8U);
    ptc_bytes(&ctx, "q\n");
    ptc_cleanup(&ctx);
    YEW_ASSERT(ptc_sweep_all());
    ptc_dispose(&ctx);
}
