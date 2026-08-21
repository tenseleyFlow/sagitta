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
        if (seen && !ed->ai->call.active)
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

static int live_adapter_child(const char *kind, const char *script,
                              bool curl_transport)
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
    ok = ed.win->shadow.live &&
         ed.win->shadow.sug.prov == (u8)YEW_SHADOW_AI &&
         ed.win->shadow.sug.len == sizeof("int answer = 42;") - 1U &&
         memcmp(ed.win->shadow.sug.text, "int answer = 42;",
                sizeof("int answer = 42;") - 1U) == 0 &&
         first_ms >= 90 && first_ms <= 150;
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

void test_ai_shadow_live_streams_all_adapters_within_budget(void)
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
                                     cases[i].curl_transport));
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        YEW_ASSERT(WIFEXITED(status));
        YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 0);
    }
}
