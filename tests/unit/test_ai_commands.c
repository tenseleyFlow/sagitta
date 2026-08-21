#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/option.h"
#include "ui/cmdline.h"
#include "ui/message.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#if YEW_WITH_AI
#include "edit/job.h"
#include "fl/flruntime.h"
#include "mod/ai/ai.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/key.h"

#ifndef YEW_TEST_FAKEHTTP
#define YEW_TEST_FAKEHTTP "build/tests/helpers/fakehttp"
#endif
#ifndef YEW_TEST_FAKECURL
#define YEW_TEST_FAKECURL "build/tests/helpers/fakecurl"
#endif
#endif

typedef struct {
    const char *name;
    u32 sprint;
} AiCommandRow;

#if YEW_WITH_AI
static const char *ai_message(const Ed *ed)
{
    return ed->msg.full == NULL ? ed->msg.text : ed->msg.full;
}

static bool ai_log_contains(const Ed *ed, const char *needle)
{
    size_t want = strlen(needle);
    size_t i;

    if (want == 0U)
        return true;
    for (i = 0U; i + want <= ed->ai->log.len; i++) {
        if (memcmp(ed->ai->log.data + i, needle, want) == 0)
            return true;
    }
    return false;
}

static void ai_enable(Ed *ed)
{
    OptVal enabled = {YEW_OPT_BOOL, {.b = true}};
    const char *error = NULL;

    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_GLOBAL, "ai.enable", 9U,
                           &enabled, &error));
    YEW_ASSERT_NULL(error);
}

static void ai_select(Ed *ed, const char *name)
{
    OptVal selected = {YEW_OPT_STR,
                       {.str = {name, (u32)strlen(name)}}};
    const char *error = NULL;

    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_GLOBAL, "ai.backend", 10U,
                           &selected, &error));
    YEW_ASSERT_NULL(error);
}

static CmdStatus ai_invoke(Ed *ed, const char *name)
{
    CmdCtx cx = {0};
    CmdId id = yew_cmd_lookup(name, (u32)strlen(name));

    YEW_ASSERT(id.v != 0U);
    cx.ed = ed;
    cx.win = ed->win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    return yew_ed_invoke(ed, id, &cx);
}

static void ai_define_local(Ed *ed, const char *name, u16 port)
{
    char source[384];
    int n = snprintf(
        source, sizeof(source),
        "import ai\n"
        "ai.backend(\"%s\", {kind: \"ollama\", "
        "url: \"http://127.0.0.1:%u\", model: \"qwen\"})\n",
        name, (unsigned)port);

    YEW_ASSERT(n > 0 && (size_t)n < sizeof(source));
    YEW_ASSERT_EQ_I64(yew_fl_eval(ed, source, (u32)n), YEW_CMD_OK);
    ai_select(ed, name);
}

static pid_t ai_server_start(const char *mode, u16 *port)
{
    int output[2];
    pid_t pid;
    FILE *stream;
    unsigned value = 0U;

    YEW_ASSERT_EQ_I64(pipe(output), 0);
    pid = fork();
    YEW_ASSERT(pid >= 0);
    if (pid == 0) {
        (void)close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) < 0)
            _exit(126);
        (void)close(output[1]);
        execl(YEW_TEST_FAKEHTTP, YEW_TEST_FAKEHTTP, mode, (char *)NULL);
        _exit(127);
    }
    (void)close(output[1]);
    stream = fdopen(output[0], "r");
    YEW_ASSERT_NOT_NULL(stream);
    YEW_ASSERT_EQ_I64(fscanf(stream, "%u", &value), 1);
    YEW_ASSERT(value > 0U && value <= 65535U);
    YEW_ASSERT_EQ_I64(fclose(stream), 0);
    *port = (u16)value;
    return pid;
}

static void ai_server_wait(pid_t pid)
{
    int status = 0;

    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 0);
}

static void ai_command_drive(Ed *ed)
{
    i64 started = yew_now_ms();
    bool poll_ok = true;
    bool within_deadline = true;

    while (ed->ai->command_call != NULL || ed->ai->curl.running ||
           ed->ai->curl_backends_waiting) {
        struct pollfd pfd[YEW_JOB_MAX * 4U + YEW_HTTP_POOL_MAX];
        u32 n = 0U;
        i64 deadline;
        int timeout;
        int polled;

        yew_job_collect_fds(ed, pfd, &n);
        yew_ai_collect_fds(ed, pfd, &n);
        deadline = yew_ai_deadline(ed, yew_now_ms());
        timeout = deadline < 0 || deadline > 20 ? 20 : (int)deadline;
        polled = poll(pfd, (nfds_t)n, timeout);
        if (polled < 0 && errno != EINTR) {
            poll_ok = false;
            break;
        }
        yew_job_pump(ed, pfd, n);
        yew_job_reap(ed);
        yew_job_tick(ed, yew_now_ms());
        yew_job_settle(ed);
        yew_ai_pump(ed, pfd, n);
        if (yew_now_ms() - started >= 5000) {
            within_deadline = false;
            break;
        }
    }
    YEW_ASSERT(poll_ok);
    YEW_ASSERT(within_deadline);
}

static char *ai_save_env(const char *name)
{
    const char *value = getenv(name);
    char *copy;

    if (value == NULL)
        return NULL;
    copy = malloc(strlen(value) + 1U);
    YEW_ASSERT_NOT_NULL(copy);
    (void)memcpy(copy, value, strlen(value) + 1U);
    return copy;
}

static void ai_restore_env(const char *name, char *saved)
{
    if (saved != NULL) {
        YEW_ASSERT_EQ_I64(setenv(name, saved, 1), 0);
        free(saved);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv(name), 0);
    }
}

static void ai_curl_fixture(char *dir, size_t dirsz)
{
    char link[512];

    (void)snprintf(dir, dirsz, "/tmp/yew-command-curl-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(link, sizeof(link), "%s/curl", dir);
    YEW_ASSERT_EQ_I64(symlink(YEW_TEST_FAKECURL, link), 0);
}

static void ai_curl_fixture_remove(const char *dir)
{
    char link[512];

    (void)snprintf(link, sizeof(link), "%s/curl", dir);
    YEW_ASSERT_EQ_I64(unlink(link), 0);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
}
#endif

void test_ai_commands_cross_module_boundary(void)
{
    static const AiCommandRow rows[] = {
        {"ed.ai.backends", 0U},
        {"ed.ai.models", 0U},
        {"ed.ai.ping", 0U},
        {"ed.ai.log", 0U},
        {"ed.ai.reload", 0U},
        {"ed.ai.enable", 50U},
        {"ed.ai.disable", 50U},
        {"ed.ai.stats", 0U}
    };
    Ed ed;
    CmdCtx cx = {0};
    size_t i;

    yew_ed_init(&ed);
    cx.ed = &ed;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        CmdId id = yew_cmd_lookup(rows[i].name, (u32)strlen(rows[i].name));
        const CmdDesc *desc;

        YEW_ASSERT(id.v != 0U);
        desc = yew_cmd_desc(id);
        YEW_ASSERT_NOT_NULL(desc);
#if YEW_WITH_AI
        if (rows[i].sprint != 0U) {
            char sprint[24];
            char diagnostic[96];

            (void)snprintf(sprint, sizeof(sprint), "Sprint %u",
                           rows[i].sprint);
            (void)snprintf(diagnostic, sizeof(diagnostic),
                           "%s lands in %s", rows[i].name, sprint);
            YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) != 0U);
            YEW_ASSERT_NOT_NULL(strstr(desc->help, sprint));
            yew_test_capture_log();
            YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, id, &cx),
                              YEW_CMD_ERR_DEFERRED);
            YEW_ASSERT(ed.msg.active);
            YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_INFO);
            YEW_ASSERT_EQ_STR(
                ed.msg.text,
                "AI is off; :ai enable turns it on (Sprint 50)");
            YEW_ASSERT(yew_test_log_contains(YEW_LOG_ERROR, diagnostic));
        } else {
            YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) == 0U);
            if (strcmp(rows[i].name, "ed.ai.stats") == 0) {
                YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, id, &cx),
                                  YEW_CMD_OK);
                YEW_ASSERT_NOT_NULL(strstr(ai_message(&ed), "backend"));
            } else {
                YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, id, &cx),
                                  YEW_CMD_ERR_STATE);
                YEW_ASSERT(ed.msg.active);
                YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_INFO);
                YEW_ASSERT_EQ_STR(
                    ed.msg.text,
                    "AI is off; :ai enable turns it on (Sprint 50)");
            }
        }
#else
        YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) == 0U);
        YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, id, &cx), YEW_CMD_ERR_STATE);
        YEW_ASSERT(ed.msg.active);
        YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_ERROR);
        YEW_ASSERT_EQ_STR(
            ed.msg.text,
            "this build has no ai module; rebuild with 'make MODULES=\"… ai\"'");
#endif
        yew_msg_clear(&ed);
    }
    yew_ed_free(&ed);
}

void test_ai_open_explains_the_ghost_only_surface(void)
{
    CmdId id = yew_cmd_lookup("ed.ai.open", 10U);
    const CmdDesc *desc;
    Ed ed;
    CmdCtx cx = {0};

    YEW_ASSERT(id.v != 0U);
    desc = yew_cmd_desc(id);
    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT((desc->flags & YEW_CMD_DEFERRED) == 0U);
    yew_ed_init(&ed);
    cx.ed = &ed;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_I64(yew_ed_invoke(&ed, id, &cx), YEW_CMD_ERR_STATE);
#if YEW_WITH_AI
    YEW_ASSERT(ed.msg.active);
    YEW_ASSERT_EQ_U64(ed.msg.sev, YEW_MSG_INFO);
    YEW_ASSERT_EQ_STR(
        ed.msg.text,
        "AI prompt UI is not a 1.0 feature; AI completions use ghost text");
#endif
    yew_ed_free(&ed);
}

void test_ai_commands_backends_log_and_reload(void)
{
#if YEW_WITH_AI
    static const char cloud[] =
        "import ai\n"
        "ai.backend(\"cloud\", {kind: \"openai\", transport: \"http\", "
        "url: \"http://127.0.0.1:41415\", model: \"gpt-test\", "
        "key_env: \"YEW_TEST_AI_COMMAND_KEY\"})\n";
    Ed ed;
    const AiBackendEntry *entry;
    AiErr error;
    Buffer *first_log;
    const char *shown;
    const char *local_at;
    const char *cloud_at;
    char key[64];

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    ai_enable(&ed);
    ai_define_local(&ed, "local", 11434U);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, cloud, sizeof(cloud) - 1U),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_I64(setenv("YEW_TEST_AI_COMMAND_KEY", "reload-secret", 1),
                      0);
    entry = yew_ai_backend_at(&ed, 1U);
    YEW_ASSERT_NOT_NULL(entry);
    YEW_ASSERT(yew_ai_key_cache_get(&ed, &ed.ai->keys, &entry->backend,
                                    key, sizeof(key), &error));
    YEW_ASSERT_EQ_STR(key, "reload-secret");
    yew_memzero(key, sizeof(key));
    YEW_ASSERT_EQ_U64(ed.ai->keys.len, 1U);

    /* Replacement preserves the registry slot, so address-based cache
     * identity must be invalidated before the new endpoint can use it. */
    YEW_ASSERT_EQ_I64(setenv("YEW_TEST_AI_COMMAND_KEY", "second-secret", 1),
                      0);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, cloud, sizeof(cloud) - 1U),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.ai->keys.len, 0U);
    entry = yew_ai_backend_at(&ed, 1U);
    YEW_ASSERT(yew_ai_key_cache_get(&ed, &ed.ai->keys, &entry->backend,
                                    key, sizeof(key), &error));
    YEW_ASSERT_EQ_STR(key, "second-secret");
    yew_memzero(key, sizeof(key));

    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.backends"), YEW_CMD_OK);
    shown = ai_message(&ed);
    local_at = strstr(shown, "local http cooldown=0ms");
    cloud_at = strstr(shown, "cloud http cooldown=0ms");
    YEW_ASSERT_NOT_NULL(local_at);
    YEW_ASSERT_NOT_NULL(cloud_at);
    YEW_ASSERT(local_at < cloud_at);

    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.reload"), YEW_CMD_OK);
    YEW_ASSERT_EQ_STR(ai_message(&ed),
                      "reloaded 2 AI backends; key cache cleared");
    YEW_ASSERT_EQ_U64(yew_ai_backend_count(&ed), 2U);
    YEW_ASSERT_EQ_STR(yew_ai_backend_at(&ed, 0U)->backend.name, "local");
    YEW_ASSERT_EQ_STR(yew_ai_backend_at(&ed, 1U)->backend.name, "cloud");
    YEW_ASSERT_EQ_U64(ed.ai->keys.len, 0U);

    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.log"), YEW_CMD_OK);
    first_log = yew_ws_scratch_find(&ed, "[AI Log]");
    YEW_ASSERT_NOT_NULL(first_log);
    YEW_ASSERT(yew_ed_doc(&ed) == first_log);
    YEW_ASSERT((first_log->flags & YEW_BUF_READONLY) != 0U);
    YEW_ASSERT((first_log->flags & YEW_BUF_NOUNDO) != 0U);
    YEW_ASSERT(yew_textbuf_len(first_log->tb) > 17U);
    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.log"), YEW_CMD_OK);
    YEW_ASSERT(yew_ws_scratch_find(&ed, "[AI Log]") == first_log);

    YEW_ASSERT_EQ_I64(unsetenv("YEW_TEST_AI_COMMAND_KEY"), 0);
    yew_ed_free(&ed);
#else
    YEW_ASSERT(true);
#endif
}

void test_ai_commands_models_ping_and_error_live(void)
{
#if YEW_WITH_AI
    Ed ed;
    pid_t server;
    u16 port;
    const char *shown;

    server = ai_server_start("models-close", &port);
    yew_ed_init(&ed);
    ai_enable(&ed);
    ai_define_local(&ed, "local", port);
    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.models"), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(ai_message(&ed), "request started"));
    ai_command_drive(&ed);
    YEW_ASSERT_EQ_STR(ai_message(&ed), "local models: qwen2.5, coder");
    ai_server_wait(server);
    yew_ed_free(&ed);

    server = ai_server_start("models", &port);
    yew_ed_init(&ed);
    ai_enable(&ed);
    ai_define_local(&ed, "local", port);
    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.ping"), YEW_CMD_OK);
    ai_command_drive(&ed);
    shown = ai_message(&ed);
    YEW_ASSERT(strncmp(shown, "local: ok (", 11U) == 0);
    YEW_ASSERT_NOT_NULL(strstr(shown, " ms)"));
    ai_server_wait(server);
    yew_ed_free(&ed);

    server = ai_server_start("malformed", &port);
    yew_ed_init(&ed);
    ai_enable(&ed);
    ai_define_local(&ed, "local", port);
    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.ping"), YEW_CMD_OK);
    ai_command_drive(&ed);
    shown = ai_message(&ed);
    YEW_ASSERT_NOT_NULL(strstr(shown, "[protocol, "));
    YEW_ASSERT_NOT_NULL(strstr(shown, ":ai log has the detail"));
    YEW_ASSERT(ai_log_contains(&ed, "returned a response yew could not parse"));
    ai_server_wait(server);
    yew_ed_free(&ed);

    server = ai_server_start("rate", &port);
    yew_ed_init(&ed);
    ai_enable(&ed);
    ai_define_local(&ed, "local", port);
    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.ping"), YEW_CMD_OK);
    ai_command_drive(&ed);
    shown = ai_message(&ed);
    YEW_ASSERT_NOT_NULL(strstr(shown, "local"));
    YEW_ASSERT_NOT_NULL(strstr(shown, "[rate-limit, "));
    YEW_ASSERT_NOT_NULL(strstr(shown, " ms]"));
    YEW_ASSERT(yew_ai_backend_at(&ed, 0U)->cooldown.until_ms -
               yew_now_ms() > 2500);
    ai_server_wait(server);
    yew_ed_free(&ed);

    /* A live call owns its registry entry until completion.  Runtime
     * Fletch redefinition must fail without moving or replacing it. */
    server = ai_server_start("models", &port);
    yew_ed_init(&ed);
    ai_enable(&ed);
    ai_define_local(&ed, "local", port);
    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.models"), YEW_CMD_OK);
    {
        static const char replacement[] =
            "import ai\n"
            "ai.backend(\"other\", {kind: \"ollama\", "
            "url: \"http://127.0.0.1:11434\", model: \"qwen\"})\n";

        YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, replacement,
                                      sizeof(replacement) - 1U),
                          YEW_CMD_ERR_STATE);
    }
    YEW_ASSERT_EQ_U64(yew_ai_backend_count(&ed), 1U);
    YEW_ASSERT_NOT_NULL(ed.ai->command_call);
    yew_msg_clear(&ed);
    ai_command_drive(&ed);
    YEW_ASSERT_EQ_STR(ai_message(&ed), "local models: qwen2.5, coder");
    ai_server_wait(server);
    yew_ed_free(&ed);
#else
    YEW_ASSERT(true);
#endif
}

void test_ai_commands_curl_probe_request_and_teardown(void)
{
#if YEW_WITH_AI
    static const char cloud[] =
        "import ai\n"
        "ai.backend(\"cloud\", {kind: \"openai\", transport: \"curl\", "
        "url: \"https://example.invalid/v1\", model: \"gpt-test\", "
        "key_env: \"YEW_TEST_AI_COMMAND_KEY\"})\n";
    char dir[128];
    char *old_path = ai_save_env("PATH");
    char *old_version = ai_save_env("YEW_FAKECURL_VERSION");
    char *old_stdout = ai_save_env("YEW_FAKECURL_STDOUT");
    char *old_status = ai_save_env("YEW_FAKECURL_STATUS");
    char *old_secret = ai_save_env("YEW_FAKECURL_SECRET");
    char *old_key = ai_save_env("YEW_TEST_AI_COMMAND_KEY");
    Ed ed;

    ai_curl_fixture(dir, sizeof(dir));
    YEW_ASSERT_EQ_I64(setenv("PATH", dir, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_VERSION",
                            "curl 8.5.0 fakecurl", 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_STDOUT",
                            "{\"data\":[{\"id\":\"gpt-a\"}]}", 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_STATUS", "200", 1), 0);
    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_SECRET", "command-secret", 1),
                      0);
    YEW_ASSERT_EQ_I64(setenv("YEW_TEST_AI_COMMAND_KEY", "command-secret", 1),
                      0);

    /* A foreground command that adopts :ai backends' in-flight probe owns
     * the final message.  The background refresh must not hide NO_CURL. */
    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_VERSION",
                            "curl 7.61.0 fakecurl", 1), 0);
    yew_ed_init(&ed);
    ai_enable(&ed);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, cloud, sizeof(cloud) - 1U),
                      YEW_CMD_OK);
    ai_select(&ed, "cloud");
    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.backends"), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(ai_message(&ed), "curl probing"));
    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.models"), YEW_CMD_OK);
    ai_command_drive(&ed);
    YEW_ASSERT_NOT_NULL(strstr(
        ai_message(&ed),
        "curl 7.61 is too old for the AI transport"));
    YEW_ASSERT_NOT_NULL(strstr(ai_message(&ed), "[no-curl, "));
    YEW_ASSERT_NULL(strstr(ai_message(&ed), "cooldown="));
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);
    yew_ed_free(&ed);

    YEW_ASSERT_EQ_I64(setenv("YEW_FAKECURL_VERSION",
                            "curl 8.5.0 fakecurl", 1), 0);
    yew_ed_init(&ed);
    ai_enable(&ed);
    YEW_ASSERT_EQ_I64(yew_fl_eval(&ed, cloud, sizeof(cloud) - 1U),
                      YEW_CMD_OK);
    ai_select(&ed, "cloud");
    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.backends"), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(ai_message(&ed), "curl probing"));
    ai_command_drive(&ed);
    YEW_ASSERT_EQ_STR(ai_message(&ed),
                      "cloud curl curl 8.5.0 cooldown=0ms");
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);

    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.models"), YEW_CMD_OK);
    ai_command_drive(&ed);
    YEW_ASSERT_EQ_STR(ai_message(&ed), "cloud models: gpt-a");
    YEW_ASSERT_EQ_U64(ed.jobs.len, 0U);

    /* Cancel before the poll loop writes stdin.  The job must not retain
     * the secret-bearing command buffer after AI state is freed. */
    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.models"), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(ed.ai->command_call);
    YEW_ASSERT_EQ_U64(ed.jobs.len, 1U);
    yew_ed_free(&ed);

    ai_restore_env("PATH", old_path);
    ai_restore_env("YEW_FAKECURL_VERSION", old_version);
    ai_restore_env("YEW_FAKECURL_STDOUT", old_stdout);
    ai_restore_env("YEW_FAKECURL_STATUS", old_status);
    ai_restore_env("YEW_FAKECURL_SECRET", old_secret);
    ai_restore_env("YEW_TEST_AI_COMMAND_KEY", old_key);
    ai_curl_fixture_remove(dir);
#else
    YEW_ASSERT(true);
#endif
}

void test_ai_commands_require_a_selected_backend(void)
{
#if YEW_WITH_AI
    Ed ed;

    yew_ed_init(&ed);
    ai_enable(&ed);
    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.models"), YEW_CMD_ERR_STATE);
    YEW_ASSERT_EQ_STR(
        ai_message(&ed),
        "no AI backends configured; add ai.backend(...) to init.fl");
    ai_define_local(&ed, "first", 11434U);
    ai_define_local(&ed, "second", 11435U);
    ai_select(&ed, "missing");
    YEW_ASSERT_EQ_I64(ai_invoke(&ed, "ed.ai.ping"), YEW_CMD_ERR_STATE);
    YEW_ASSERT_EQ_STR(
        ai_message(&ed),
        "select a configured backend with set({\"ai.backend\": \"name\"})");
    yew_ed_free(&ed);
#else
    YEW_ASSERT(true);
#endif
}
