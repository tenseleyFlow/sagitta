#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/option.h"
#include "edit/shadow.h"
#include "fl/flruntime.h"
#include "mod/ai/ai.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/backend_curl.h"
#include "mod/git/git.h"
#include "mod/git/git_int.h"
#include "mod/lsp/client.h"
#include "text/edit.h"

#ifndef YEW_TEST_MOCKAI
#define YEW_TEST_MOCKAI "build/tests/helpers/mockai"
#endif
#ifndef YEW_TEST_MOCKCURL
#define YEW_TEST_MOCKCURL "build/tests/helpers/mockcurl"
#endif

static bool live_set_bool(Ed *ed, const char *name, bool value)
{
    OptVal option = {YEW_OPT_BOOL, {.b = value}};
    const char *error = NULL;

    return yew_opt_set(ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                       &option, &error);
}

static bool live_set_int(Ed *ed, const char *name, i64 value)
{
    OptVal option = {YEW_OPT_INT, {.i = value}};
    const char *error = NULL;

    return yew_opt_set(ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                       &option, &error);
}

static bool live_set_str(Ed *ed, const char *name, const char *value)
{
    OptVal option = {YEW_OPT_STR,
                     {.str = {value, (u32)strlen(value)}}};
    const char *error = NULL;

    return yew_opt_set(ed, YEW_OPT_GLOBAL, name, (u32)strlen(name),
                       &option, &error);
}

static pid_t live_server_start(const char *script, u16 *port)
{
    int output[2];
    pid_t pid;
    FILE *stream;
    unsigned value = 0U;

    if (pipe(output) != 0)
        return -1;
    pid = fork();
    if (pid == 0) {
        (void)close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) < 0)
            _exit(126);
        (void)close(output[1]);
        execl(YEW_TEST_MOCKAI, YEW_TEST_MOCKAI, "--port", "0",
              "--script", script, (char *)NULL);
        _exit(127);
    }
    (void)close(output[1]);
    if (pid < 0) {
        (void)close(output[0]);
        return -1;
    }
    stream = fdopen(output[0], "r");
    if (stream == NULL || fscanf(stream, "port %u", &value) != 1 ||
        value == 0U || value > 65535U) {
        if (stream != NULL)
            (void)fclose(stream);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return -1;
    }
    if (fclose(stream) != 0) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return -1;
    }
    *port = (u16)value;
    return pid;
}

static void live_server_stop(pid_t pid)
{
    int status;

    if (pid <= 0)
        return;
    (void)kill(pid, SIGTERM);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

static bool live_drive(Ed *ed, i64 *first_ms)
{
    i64 started = yew_now_ms();
    bool seen = false;

    while (yew_now_ms() - started < 3000) {
        struct pollfd fds[YEW_JOB_MAX * 4U + YEW_HTTP_POOL_MAX];
        u32 nfds = 0U;
        i64 deadline;
        int timeout;
        int rc;

        yew_job_collect_fds(ed, fds, &nfds);
        yew_ai_collect_fds(ed, fds, &nfds);
        deadline = yew_ai_deadline(ed, yew_now_ms());
        timeout = deadline < 0 || deadline > 20 ? 20 : (int)deadline;
        rc = poll(fds, (nfds_t)nfds, timeout);
        if (rc < 0 && errno != EINTR)
            return false;
        ed->now_ms = yew_now_ms();
        yew_job_pump(ed, fds, nfds);
        yew_job_reap(ed);
        yew_job_tick(ed, ed->now_ms);
        yew_job_settle(ed);
        yew_ai_pump(ed, fds, nfds);
        if (!seen && ed->win->shadow.live) {
            *first_ms = yew_now_ms() - started;
            seen = true;
        }
        /* A partial event and the peer close can coalesce into one pump.
         * The ghost is then delivered and correctly cleared before this
         * observer regains control, so requiring `seen` turns socket read
         * batching into a test result.  The caller checks the final ghost,
         * message and buffer state for both success and error cases. */
        if (!ed->ai->call.active)
            return true;
    }
    return false;
}

static bool live_probe_curl(Ed *ed)
{
    char error[192] = {0};
    i64 started = yew_now_ms();

    if (yew_ai_curl_probe(ed, &ed->ai->curl, error, sizeof(error)) ||
        error[0] != '\0')
        return false;
    while ((ed->ai->curl.running || ed->ai->curl.job_id != 0U) &&
           yew_now_ms() - started < 3000) {
        struct pollfd fds[YEW_JOB_MAX * 4U + YEW_HTTP_POOL_MAX];
        u32 nfds = 0U;
        int rc;

        yew_job_collect_fds(ed, fds, &nfds);
        yew_ai_collect_fds(ed, fds, &nfds);
        rc = poll(fds, (nfds_t)nfds, 20);
        if (rc < 0 && errno != EINTR)
            return false;
        ed->now_ms = yew_now_ms();
        yew_job_pump(ed, fds, nfds);
        yew_job_reap(ed);
        yew_job_tick(ed, ed->now_ms);
        yew_job_settle(ed);
        yew_ai_pump(ed, fds, nfds);
    }
    return yew_ai_curl_probe(ed, &ed->ai->curl, error, sizeof(error)) &&
           error[0] == '\0';
}

static bool live_text_eq(const TextBuf *tb, const u8 *want, size_t len)
{
    TextIter iter;
    u64 done = 0U;

    if (yew_textbuf_len(tb) != len)
        return false;
    if (len == 0U)
        return true;
    if (!yew_textiter_begin(&iter, tb, BYTEOFF(0U)))
        return false;
    while (done < len) {
        const u8 *bytes;
        u64 available;
        u64 take;

        if (!yew_textiter_chunk(&iter, tb, &bytes, &available))
            return false;
        take = available < len - done ? available : len - done;
        if (take == 0U || memcmp(bytes, want + done, (size_t)take) != 0)
            return false;
        done += take;
        if (done < len && !yew_textiter_advance(&iter, tb))
            return false;
    }
    return true;
}

typedef enum LiveCase {
    LIVE_STREAM,
    LIVE_ACCEPT_WORD,
    LIVE_ACCEPT_ALL,
    LIVE_ERROR_EVENT,
    LIVE_EARLY_CLOSE
} LiveCase;

static bool live_accept_and_undo(Ed *ed, LiveCase which,
                                 const u8 *input, size_t input_len)
{
    EditCtx edit;
    u32 before = yew_undo_current(ed->win->buf->undo);
    bool accepted;

    ed->mode = YEW_MODE_I;
    accepted = which == LIVE_ACCEPT_WORD ?
        yew_shadow_accept_word(ed, ed->win, false) :
        yew_shadow_accept_all(ed, ed->win);
    if (!accepted || yew_textbuf_len(ed->win->buf->tb) <= input_len ||
        yew_undo_current(ed->win->buf->undo) == before)
        return false;
    if (which == LIVE_ACCEPT_WORD &&
        (!ed->win->shadow.live || ed->win->shadow.sug.consumed == 0U))
        return false;
    if (which == LIVE_ACCEPT_ALL && ed->win->shadow.live)
        return false;
    edit = yew_ed_edit_ctx(ed);
    if (!yew_undo(&edit))
        return false;
    yew_ed_finish_edit(ed, &edit);
    return live_text_eq(ed->win->buf->tb, input, input_len);
}

static int live_adapter_child(const char *kind, const char *script,
                              bool curl_transport, LiveCase which)
{
    static const u8 input[] = "int main(void) {\n    ";
    char state_root[] = "/tmp/yew-ai-live-XXXXXX";
    char state_dir[sizeof(state_root) + sizeof("/yew")];
    char state_file[sizeof(state_dir) + sizeof("/ai_stats.fl")];
    char curl_root[] = "/tmp/yew-ai-curl-XXXXXX";
    char curl_link[sizeof(curl_root) + sizeof("/curl")];
    char source[512];
    Ed ed;
    u16 port;
    pid_t server;
    int n;
    i64 fired;
    i64 first_ms = -1;
    bool initialized = false;
    bool ok = false;
    const char *stage = "setup";

    if (mkdtemp(state_root) == NULL ||
        setenv("XDG_STATE_HOME", state_root, 1) != 0 ||
        setenv("YEW_TEST_AI_LIVE_KEY", "local-test-key", 1) != 0 ||
        setenv("YEW_AI_MOCK", "1", 1) != 0)
        return 10;
    server = 0;
    port = 443U;
    if (curl_transport) {
        if (mkdtemp(curl_root) == NULL ||
            snprintf(curl_link, sizeof(curl_link), "%s/curl", curl_root) <= 0 ||
            symlink(YEW_TEST_MOCKCURL, curl_link) != 0 ||
            setenv("PATH", curl_root, 1) != 0 ||
            setenv("YEW_AI_MOCK_SCRIPT", script, 1) != 0)
            return 11;
    } else {
        server = live_server_start(script, &port);
        if (server <= 0)
            return 11;
    }
    yew_ai_shadow_init(NULL);
    yew_ed_init(&ed);
    initialized = true;
    stage = "open";
    if (!yew_ed_open_memory(&ed, input, sizeof(input) - 1U,
                            "ai-shadow-live"))
        goto out;
    yew_ai_workspace_session_set(&ed, YEW_AI_WS_ALLOW);
    ed.win->cs.curs.data[0].pos = BYTEOFF(sizeof(input) - 1U);
    ed.win->cs.curs.data[0].anchor = BYTEOFF(sizeof(input) - 1U);
    stage = "options";
    if (!live_set_bool(&ed, "ai.enable", true) ||
        !live_set_int(&ed, "ai.frame_ms", 0) ||
        !live_set_str(&ed, "ai.backend", "local") ||
        !live_set_str(&ed, "shadow.providers", "ai"))
        goto out;
    n = snprintf(source, sizeof(source),
                 "import ai\n"
                 "ai.backend(\"local\", {kind: \"%s\", "
                 "transport: \"%s\", url: \"%s://127.0.0.1:%u/v1\", "
                 "model: \"qwen-coder\", "
                 "key_env: \"YEW_TEST_AI_LIVE_KEY\"})\n",
                 kind, curl_transport ? "curl" : "http",
                 curl_transport ? "https" : "http", (unsigned)port);
    stage = "backend";
    if (n <= 0 || (size_t)n >= sizeof(source) ||
        yew_fl_eval(&ed, source, (u32)n) != YEW_CMD_OK)
        goto out;
    if (curl_transport && !live_probe_curl(&ed)) {
        stage = "curl-probe";
        goto out;
    }
    ed.now_ms = yew_now_ms();
    stage = "arm";
    yew_shadow_arm(&ed, ed.win);
    fired = ed.now_ms + 350;
    ed.now_ms = fired;
    yew_timers_fire(&ed.timers, &ed, fired);
    stage = "drive";
    if (!ed.ai->call.active || !live_drive(&ed, &first_ms))
        goto out;
    if (which == LIVE_STREAM || which == LIVE_ACCEPT_WORD ||
        which == LIVE_ACCEPT_ALL) {
        /* The functional lane proves that the scripted 100 ms delay is
         * honored.  The 150 ms requirement is a p95 over 100 runs and is
         * owned by perf_ai_shadow; applying it to each unit-test sample
         * makes scheduler stalls indistinguishable from product latency. */
        ok = ed.win->shadow.live &&
             ed.win->shadow.sug.prov == (u8)YEW_SHADOW_AI &&
             ed.win->shadow.sug.len == sizeof("int answer = 42;") - 1U &&
             memcmp(ed.win->shadow.sug.text, "int answer = 42;",
                    sizeof("int answer = 42;") - 1U) == 0 &&
             first_ms >= 90;
        if (ok && which != LIVE_STREAM)
            ok = live_accept_and_undo(&ed, which, input,
                                      sizeof(input) - 1U);
    } else {
        const char *message = ed.msg.full == NULL ? ed.msg.text :
                                                    ed.msg.full;

        ok = !ed.win->shadow.live &&
             live_text_eq(ed.win->buf->tb, input, sizeof(input) - 1U);
        if (ok && which == LIVE_ERROR_EVENT)
            ok = strstr(message,
                        "returned a response yew could not parse") != NULL;
        else if (ok && which == LIVE_EARLY_CLOSE)
            ok = strstr(message, "response ended early") != NULL;
    }
    stage = "verify";
    if (!ok) {
        (void)fprintf(stderr,
                      "ai-shadow-live %s: live=%u len=%u first=%lld msg=%s text=%.*s\n",
                      kind, ed.win->shadow.live ? 1U : 0U,
                      ed.win->shadow.sug.len, (long long)first_ms,
                      ed.msg.full == NULL ? ed.msg.text : ed.msg.full,
                      (int)ed.win->shadow.sug.len,
                      ed.win->shadow.sug.text == NULL ? "" :
                          (const char *)ed.win->shadow.sug.text);
    }

out:
    if (!ok && initialized) {
        (void)fprintf(stderr,
                      "ai-shadow-live %s stopped at %s: active=%u msg=%s\n",
                      kind, stage, ed.ai->call.active ? 1U : 0U,
                      ed.msg.full == NULL ? ed.msg.text : ed.msg.full);
    }
    if (initialized)
        yew_ed_free(&ed);
    live_server_stop(server);
    if (curl_transport) {
        (void)unlink(curl_link);
        (void)rmdir(curl_root);
    }
    (void)snprintf(state_dir, sizeof(state_dir), "%s/yew", state_root);
    (void)snprintf(state_file, sizeof(state_file), "%s/ai_stats.fl",
                   state_dir);
    (void)unlink(state_file);
    (void)rmdir(state_dir);
    (void)rmdir(state_root);
    return ok ? 0 : 12;
}

void test_ai_shadow_live_streams_all_adapters(void)
{
    static const struct {
        const char *kind;
        const char *script;
        bool curl_transport;
    } cases[] = {
        {"ollama", "tests/fixtures/ai/ollama.script", false},
        {"openai", "tests/fixtures/ai/openai.script", false},
        {"anthropic", "tests/fixtures/ai/anthropic.script", false},
        {"openai", "tests/fixtures/ai/openai.script", true}
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(cases); i++) {
        pid_t pid = fork();
        int status = 0;

        YEW_ASSERT(pid >= 0);
        if (pid == 0)
            _exit(live_adapter_child(cases[i].kind, cases[i].script,
                                     cases[i].curl_transport, LIVE_STREAM));
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        YEW_ASSERT(WIFEXITED(status));
        YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 0);
    }
}

static void live_expect_child(LiveCase which, const char *script)
{
    pid_t pid = fork();
    int status = 0;

    YEW_ASSERT(pid >= 0);
    if (pid == 0)
        _exit(live_adapter_child("openai", script, false, which));
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 0);
}

void test_ai_shadow_live_accepts_word_and_all_as_single_undo(void)
{
    live_expect_child(LIVE_ACCEPT_WORD, "tests/fixtures/ai/openai.script");
    live_expect_child(LIVE_ACCEPT_ALL, "tests/fixtures/ai/openai.script");
}

void test_ai_shadow_live_error_event_clears_without_inserting(void)
{
    live_expect_child(LIVE_ERROR_EVENT,
                      "tests/fixtures/ai/s49-live-error-event.script");
}

void test_ai_shadow_live_early_close_clears_without_inserting(void)
{
    live_expect_child(LIVE_EARLY_CLOSE,
                      "tests/fixtures/ai/s49-live-early-close.script");
}

#if defined(__linux__) && YEW_WITH_LSP
typedef struct InvariantThreadsFix {
    u32 git_job;
    u32 git_calls;
} InvariantThreadsFix;

static u32 invariant_threads_git_spawn(Ed *ed, const GitVerb *verb,
                                        char *const *git_argv,
                                        const GitReq *req, void *opaque,
                                        char *err, size_t errsz)
{
    InvariantThreadsFix *fix = opaque;
    char *argv[] = {(char *)"/bin/sh", (char *)"-c",
                    (char *)"sleep 30", NULL};
    YewJobSpec spec = {0};

    (void)verb;
    (void)git_argv;
    (void)req;
    spec.argv = argv;
    spec.sink = YEW_SINK_DISCARD;
    fix->git_calls++;
    fix->git_job = yew_job_spawn(ed, &spec, err, errsz);
    return fix->git_job;
}

static bool invariant_threads_helper(char *out, size_t cap)
{
    const char *program = yew_test_program_path();
    const char *slash = strrchr(program, '/');
    int n;

    if (slash != NULL)
        n = snprintf(out, cap, "%.*s/tests/helpers/fakelsp",
                     (int)(slash - program), program);
    else
        n = snprintf(out, cap, "./tests/helpers/fakelsp");
    return n > 0 && (size_t)n < cap;
}

static bool invariant_threads_pump(Ed *ed, LspServer *server)
{
    i64 started = yew_now_ms();

    while (yew_now_ms() - started < 5000) {
        struct pollfd fds[YEW_JOB_MAX * 4U + YEW_HTTP_POOL_MAX];
        u32 nfds = 0U;
        int rc;

        yew_job_collect_fds(ed, fds, &nfds);
        yew_ai_collect_fds(ed, fds, &nfds);
        rc = poll(fds, (nfds_t)nfds, 20);
        if (rc < 0 && errno != EINTR)
            return false;
        ed->now_ms = yew_now_ms();
        yew_job_pump(ed, fds, nfds);
        yew_job_reap(ed);
        yew_job_tick(ed, ed->now_ms);
        yew_job_settle(ed);
        yew_lsp_client_pump(ed);
        yew_ai_pump(ed, fds, nfds);
        if (server->state == YEW_LSP_READY && ed->ai->call.active &&
            ed->win->shadow.live)
            return true;
    }
    return false;
}

static bool invariant_threads_read(u32 *threads)
{
    FILE *status = fopen("/proc/self/status", "r");
    char line[256];

    if (status == NULL)
        return false;
    while (fgets(line, sizeof(line), status) != NULL) {
        unsigned value;

        if (sscanf(line, "Threads: %u", &value) == 1) {
            *threads = (u32)value;
            (void)fclose(status);
            return true;
        }
    }
    (void)fclose(status);
    return false;
}

static int invariant_threads_child(void)
{
    static const u8 input[] = "int main(void) { return 0; }\n";
    char state_root[] = "/tmp/yew-invariant-threads-state-XXXXXX";
    char state_dir[sizeof(state_root) + sizeof("/yew")];
    char state_file[sizeof(state_dir) + sizeof("/ai_stats.fl")];
    char source_path[] = "/tmp/yew-invariant-threads-XXXXXX";
    char marker[] = "/tmp/yew-invariant-threads-lsp-XXXXXX";
    char helper[4096];
    const char *lsp_args[3];
    LspServerCfg cfg = {0};
    InvariantThreadsFix fix = {0};
    Ed ed;
    LspDoc *doc;
    LspServer *server;
    YewJobSpec spec = {0};
    char *shell_argv[] = {(char *)"/bin/sh", (char *)"-c",
                          (char *)"sleep 30", NULL};
    u32 shell_jobs[3] = {0};
    u32 threads = 0U;
    u16 port = 0U;
    pid_t mockai = -1;
    int source_fd = -1;
    int marker_fd = -1;
    int n;
    u32 i;
    bool initialized = false;
    bool git_hook = false;
    bool ok = false;
    char err[256] = {0};
    char backend[512];

    if (mkdtemp(state_root) == NULL ||
        setenv("XDG_STATE_HOME", state_root, 1) != 0 ||
        setenv("YEW_AI_MOCK", "1", 1) != 0 ||
        !invariant_threads_helper(helper, sizeof(helper)))
        goto out;
    source_fd = mkstemp(source_path);
    marker_fd = mkstemp(marker);
    if (source_fd < 0 || marker_fd < 0 ||
        write(source_fd, input, sizeof(input) - 1U) !=
            (ssize_t)(sizeof(input) - 1U) ||
        close(source_fd) != 0 || close(marker_fd) != 0)
        goto out;
    source_fd = -1;
    marker_fd = -1;
    if (unlink(marker) != 0)
        goto out;
    mockai = live_server_start(
        "tests/fixtures/ai/invariant-threads-live.script", &port);
    if (mockai <= 0)
        goto out;

    yew_ai_shadow_init(NULL);
    yew_ed_init(&ed);
    initialized = true;
    if (yew_ed_open(&ed, source_path) != YEW_LOAD_OK)
        goto out;
    ed.buffer.lang = "c";
    lsp_args[0] = "session-utf8";
    lsp_args[1] = marker;
    lsp_args[2] = NULL;
    cfg.id = "fakelsp";
    cfg.lang = "c";
    cfg.cmd = helper;
    cfg.args = lsp_args;
    cfg.init_timeout_ms = 3000;
    ed.now_ms = yew_now_ms();
    if (!yew_lsp_client_start_cfg(&ed, &ed.buffer, &cfg))
        goto out;
    doc = yew_lsp_doc_for_buffer(&ed, &ed.buffer);
    if (doc == NULL)
        goto out;
    server = yew_lsp_server_for_doc(&ed, doc);
    if (server == NULL)
        goto out;

    yew_git_test_spawn_set(invariant_threads_git_spawn, &fix);
    git_hook = true;
    if (!yew_git_refresh(&ed, false) || fix.git_calls == 0U ||
        fix.git_job == 0U)
        goto out;
    spec.argv = shell_argv;
    spec.sink = YEW_SINK_DISCARD;
    for (i = 0U; i < YEW_ARRAY_LEN(shell_jobs); i++) {
        shell_jobs[i] = yew_job_spawn(&ed, &spec, err, sizeof(err));
        if (shell_jobs[i] == 0U)
            goto out;
    }

    yew_ai_workspace_session_set(&ed, YEW_AI_WS_ALLOW);
    ed.win->cs.curs.data[0].pos = BYTEOFF(sizeof(input) - 1U);
    ed.win->cs.curs.data[0].anchor = BYTEOFF(sizeof(input) - 1U);
    if (!live_set_bool(&ed, "ai.enable", true) ||
        !live_set_int(&ed, "ai.frame_ms", 0) ||
        !live_set_str(&ed, "ai.backend", "local") ||
        !live_set_str(&ed, "shadow.providers", "ai"))
        goto out;
    n = snprintf(backend, sizeof(backend),
                 "import ai\n"
                 "ai.backend(\"local\", {kind: \"ollama\", "
                 "transport: \"http\", url: \"http://127.0.0.1:%u\", "
                 "model: \"invariant-threads\"})\n",
                 (unsigned)port);
    if (n <= 0 || (size_t)n >= sizeof(backend) ||
        yew_fl_eval(&ed, backend, (u32)n) != YEW_CMD_OK)
        goto out;
    ed.now_ms = yew_now_ms();
    yew_shadow_arm(&ed, ed.win);
    ed.now_ms += 350;
    yew_timers_fire(&ed.timers, &ed, ed.now_ms);
    if (!ed.ai->call.active || !invariant_threads_pump(&ed, server))
        goto out;

    if (yew_job_find(&ed, fix.git_job) == NULL)
        goto out;
    for (i = 0U; i < YEW_ARRAY_LEN(shell_jobs); i++)
        if (yew_job_find(&ed, shell_jobs[i]) == NULL)
            goto out;
    if (!invariant_threads_read(&threads))
        goto out;
    ok = threads == 1U;
    if (!ok)
        (void)fprintf(stderr, "invariant-8: Threads: %u\n", threads);

out:
    if (source_fd >= 0)
        (void)close(source_fd);
    if (marker_fd >= 0)
        (void)close(marker_fd);
    if (git_hook)
        yew_git_test_spawn_set(NULL, NULL);
    if (initialized)
        yew_ed_free(&ed);
    live_server_stop(mockai);
    (void)unlink(source_path);
    (void)unlink(marker);
    (void)snprintf(state_dir, sizeof(state_dir), "%s/yew", state_root);
    (void)snprintf(state_file, sizeof(state_file), "%s/ai_stats.fl",
                   state_dir);
    (void)unlink(state_file);
    (void)rmdir(state_dir);
    (void)rmdir(state_root);
    return ok ? 0 : 12;
}
#endif

void test_invariant_single_threaded_live_subsystems(void)
{
#if defined(__linux__) && YEW_WITH_LSP
    pid_t pid = fork();
    int status = 0;

    YEW_ASSERT(pid >= 0);
    if (pid == 0)
        _exit(invariant_threads_child());
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 0);
#else
    /* The invariant's normative runtime observation is Linux /proc. */
    YEW_ASSERT(true);
#endif
}
